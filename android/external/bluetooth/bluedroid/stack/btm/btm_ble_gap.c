/******************************************************************************
 *
 *  Copyright (C) 2008-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions for BLE GAP.
 *
 ******************************************************************************/

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "bt_types.h"
#include "btu.h"
#include "btm_int.h"
#include "hcimsgs.h"
#if (GAP_INCLUDED == TRUE)
#include "gap_api.h"
#endif
#if (BLE_INCLUDED == TRUE)
#include "gattdefs.h"

#define BTM_BLE_NAME_SHORT                  0x01
#define BTM_BLE_NAME_CMPL                   0x02

#define BTM_BLE_FILTER_TARGET_UNKNOWN       0xff
#define BTM_BLE_POLICY_UNKNOWN              0xff

#define BTM_EXT_BLE_RMT_NAME_TIMEOUT        30

/*******************************************************************************
**  Local functions
*******************************************************************************/
static void btm_ble_update_adv_flag(UINT8 flag);
static void btm_ble_process_adv_pkt_cont(BD_ADDR bda, UINT8 addr_type, UINT8 evt_type, UINT8 *p);
static UINT8 *btm_ble_build_adv_data(tBTM_BLE_AD_MASK *p_data_mask, UINT8 **p_dst, tBTM_BLE_ADV_DATA *p_data);
static UINT8 btm_set_conn_mode_adv_init_addr(tBTM_BLE_INQ_CB *p_cb,
                                     BD_ADDR_PTR p_addr_ptr,
                                     tBLE_ADDR_TYPE *p_init_addr_type,
                                     tBLE_ADDR_TYPE *p_own_addr_type);
static BOOLEAN btm_ble_start_adv(void);
static tBTM_STATUS btm_ble_stop_adv(void);



/*******************************************************************************
**
** Function         BTM_BleReset
**
** Description      This function is called to reset ULP controller.
**
** Parameters       None.
**
** Returns          void
**
*******************************************************************************/
void BTM_BleReset(void)
{
    btsnd_hcic_ble_reset();
}
/*******************************************************************************
**
** Function         BTM_BleUpdateAdvWhitelist
**
** Description      Add or remove device from advertising white list
**
** Returns          void
**
*******************************************************************************/
BOOLEAN BTM_BleUpdateAdvWhitelist(BOOLEAN add_remove, BD_ADDR remote_bda)
{
        return FALSE;
}

/*******************************************************************************
**
** Function         BTM_BleUpdateAdvFilterPolicy
**
** Description      This function update the filter policy of advertiser.
**
** Parameter        adv_policy: advertising filter policy
**
** Return           void
*******************************************************************************/
void BTM_BleUpdateAdvFilterPolicy(tBTM_BLE_AFP adv_policy)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    tBLE_ADDR_TYPE   init_addr_type = BLE_ADDR_PUBLIC;
    BD_ADDR          p_addr_ptr= {0};
    UINT8            adv_mode = p_cb->adv_mode;

    BTM_TRACE_EVENT0 ("BTM_BleUpdateAdvFilterPolicy");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return;

    if (p_cb->afp != adv_policy)
    {
        p_cb->afp = adv_policy;

        /* if adv active, stop and restart */
        btm_ble_stop_adv ();

        if (p_cb->connectable_mode & BTM_BLE_CONNECTABLE)
            p_cb->evt_type = btm_set_conn_mode_adv_init_addr(p_cb, p_addr_ptr, &init_addr_type, &p_cb->adv_addr_type);

        btsnd_hcic_ble_write_adv_params (p_cb->adv_interval_min,
                                         p_cb->adv_interval_max,
                                         p_cb->evt_type,
                                         p_cb->adv_addr_type,
                                         init_addr_type,
                                         p_addr_ptr,
                                         p_cb->adv_chnl_map,
                                         p_cb->afp);

        if (adv_mode == BTM_BLE_ADV_ENABLE)
            btm_ble_start_adv ();

    }
}
/*******************************************************************************
**
** Function         BTM_BleObserve
**
** Description      This procedure keep the device listening for advertising
**                  events from a broadcast device.
**
** Parameters       start: start or stop observe.
**                  white_list: use white list in observer mode or not.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleObserve(BOOLEAN start, UINT8 duration,
                           tBTM_INQ_RESULTS_CB *p_results_cb, tBTM_CMPL_CB *p_cmpl_cb)
{
    tBTM_BLE_INQ_CB *p_inq = &btm_cb.ble_ctr_cb.inq_var;
    tBTM_STATUS     status = BTM_NO_RESOURCES;

    BTM_TRACE_EVENT0 ("BTM_BleObserve ");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    if (start)
    {
        /* shared inquiry database, do not allow observe if any inquiry is active */
        if (btm_cb.btm_inq_vars.inq_active || p_inq->proc_mode != BTM_BLE_INQUIRY_NONE)
            return BTM_BUSY;

        btm_cb.btm_inq_vars.p_inq_results_cb = p_results_cb;
        btm_cb.btm_inq_vars.p_inq_cmpl_cb = p_cmpl_cb;
        p_inq->scan_type = (p_inq->scan_type == BTM_BLE_SCAN_MODE_NONE) ? BTM_BLE_SCAN_MODE_ACTI: p_inq->scan_type;

        /* allow config scanning type */
        if (btsnd_hcic_ble_set_scan_params (p_inq->scan_type,
                                            (UINT16)(!p_inq->scan_interval ? BTM_BLE_GAP_DISC_SCAN_INT : p_inq->scan_interval),
                                            (UINT16)(!p_inq->scan_window ? BTM_BLE_GAP_DISC_SCAN_WIN : p_inq->scan_window),
                                            BLE_ADDR_PUBLIC,
                                            BTM_BLE_DEFAULT_SFP)) /* assume observe always not using white list */
        {
            /* start scan, disable duplicate filtering */
            if (btsnd_hcic_ble_set_scan_enable (BTM_BLE_SCAN_ENABLE, BTM_BLE_DUPLICATE_DISABLE))
            {
                status = BTM_SUCCESS;
                p_inq->proc_mode = BTM_BLE_OBSERVE;
                btm_cb.btm_inq_vars.inq_active |= BTM_LE_OBSERVE_ACTIVE;

                if (duration != 0)
                {
                    /* start inquiry timer */
                    btu_start_timer (&p_inq->inq_timer_ent, BTU_TTYPE_BLE_INQUIRY, duration);
                }
            }
        }
    }
    else if (p_inq->proc_mode == BTM_BLE_OBSERVE)
    {
        btm_cb.btm_inq_vars.inq_active &= ~BTM_LE_OBSERVE_ACTIVE;
        btm_ble_stop_scan();
    }

    return status;
}

/*******************************************************************************
**
** Function         BTM_BleBroadcast
**
** Description      This function is to start or stop broadcasting.
**
** Parameters       start: start or stop broadcasting.
**
** Returns          status.
**
*******************************************************************************/
tBTM_STATUS BTM_BleBroadcast(BOOLEAN start)
{
    tBTM_STATUS status = BTM_NO_RESOURCES;
    tBTM_LE_RANDOM_CB *p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    UINT8 evt_type = p_cb->scan_rsp ? BTM_BLE_DISCOVER_EVT: BTM_BLE_NON_CONNECT_EVT;

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

#ifdef  BTM_BLE_PC_ADV_TEST_MODE
    if (BTM_BLE_PC_ADV_TEST_MODE)
    {
        evt_type = p_cb->scan_rsp ? BTM_BLE_CONNECT_EVT: BTM_BLE_NON_CONNECT_EVT;
    }
#endif

    if (start && p_cb->adv_mode == BTM_BLE_ADV_DISABLE)
    {
        /* update adv params */
        if (!btsnd_hcic_ble_write_adv_params ((UINT16)(p_cb->adv_interval_min ? p_cb->adv_interval_min : BTM_BLE_GAP_ADV_INT),
                                              (UINT16)(p_cb->adv_interval_max ? p_cb->adv_interval_max : BTM_BLE_GAP_ADV_INT),
                                              evt_type,
                                              p_addr_cb->own_addr_type,
                                              p_cb->direct_bda.type,
                                              p_cb->direct_bda.bda,
                                              p_cb->adv_chnl_map,
                                              p_cb->afp))

            status = BTM_NO_RESOURCES;
        else
            p_cb->evt_type = evt_type;

        status = btm_ble_start_adv ();
    }
    else if (!start && p_cb->adv_mode == BTM_BLE_ADV_ENABLE)
    {
        status = btm_ble_stop_adv();
    }
    else
    {
        status = BTM_WRONG_MODE;
        BTM_TRACE_ERROR2("Can not %s Broadcast, device %s in Broadcast mode",
            (start ? "Start" : "Stop"), (start ? "alerady" :"not"));
    }
    return status;
}

/*******************************************************************************
**
** Function         BTM_RegisterScanReqEvt
**
** Description      This function is called to register a scan request callback
**                  on the advertiser.
**
** Parameters       p_scan_req_cback: scan request callback.  If NULL, remove the
**                                    registration.
**
** Returns          void
**
*******************************************************************************/
void BTM_RegisterScanReqEvt(tBTM_BLE_SCAN_REQ_CBACK   *p_scan_req_cback)
{
#ifdef BTM_BLE_PC_ADV_TEST_MODE /* For general stack code (e.g. BTInsight testing), we simply do not define it to exclude or set it to TRUE to include */
    if (BTM_BLE_PC_ADV_TEST_MODE)   /* For stack component, it is always defined and maps to a global variable g_bDraculaAdvertisingMode */
    {
        tBTM_BLE_CB *p_cb = &btm_cb.ble_ctr_cb;
        p_cb->p_scan_req_cback = p_scan_req_cback;
    }
#endif
}

/*******************************************************************************
**
** Function         BTM_BleSetBgConnType
**
** Description      This function is called to set BLE connectable mode for a
**                  peripheral device.
**
** Parameters       bg_conn_type: it can be auto connection, or selective connection.
**                  p_select_cback: callback function when selective connection procedure
**                              is being used.
**
** Returns          void
**
*******************************************************************************/
BOOLEAN BTM_BleSetBgConnType(tBTM_BLE_CONN_TYPE   bg_conn_type,
                             tBTM_BLE_SEL_CBACK   *p_select_cback)
{
    BOOLEAN started = TRUE;

    BTM_TRACE_EVENT0 ("BTM_BleSetBgConnType ");
    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return FALSE;

    if (btm_cb.ble_ctr_cb.bg_conn_type != bg_conn_type)
    {
        switch (bg_conn_type)
        {
            case BTM_BLE_CONN_AUTO:
                btm_ble_start_auto_conn(TRUE);
                break;

            case BTM_BLE_CONN_SELECTIVE:
                if (btm_cb.ble_ctr_cb.bg_conn_type == BTM_BLE_CONN_AUTO)
                {
                    btm_ble_start_auto_conn(FALSE);
                }
                started = btm_ble_start_select_conn(TRUE, p_select_cback);
                break;

            case BTM_BLE_CONN_NONE:
                if (btm_cb.ble_ctr_cb.bg_conn_type == BTM_BLE_CONN_AUTO)
                {
                    btm_ble_start_auto_conn(FALSE);
                }
                else if (btm_cb.ble_ctr_cb.bg_conn_type == BTM_BLE_CONN_SELECTIVE)
                {
                    btm_ble_start_select_conn(FALSE, NULL);
                }
                started = TRUE;
                break;

            default:
                BTM_TRACE_ERROR1("invalid bg connection type : %d ", bg_conn_type);
                started = FALSE;
                break;
        }

        if (started)
            btm_cb.ble_ctr_cb.bg_conn_type = bg_conn_type;
    }
    return started;
}

/*******************************************************************************
**
** Function         BTM_BleUpdateBgConnDev
**
** Description      This function is called to add or remove a device into/from
**                  background connection procedure. The background connection
*                   procedure is decided by the background connection type, it can be
*                   auto connection, or selective connection.
**
** Parameters       add_remove: TRUE to add; FALSE to remove.
**                  remote_bda: device address to add/remove.
**
** Returns          void
**
*******************************************************************************/
BOOLEAN BTM_BleUpdateBgConnDev(BOOLEAN add_remove, BD_ADDR   remote_bda)
{
    BOOLEAN ret = TRUE;
    UINT8   dev_wl_type = 0;
    BTM_TRACE_EVENT0 (" BTM_BleUpdateBgConnDev");

    /* update white list */
    ret = btm_update_bg_conn_list(add_remove, remote_bda, &dev_wl_type);

    btm_update_dev_to_white_list(add_remove, remote_bda, dev_wl_type);

    return ret;
}

/*******************************************************************************
**
** Function         BTM_BleSetConnMode
**
** Description      This function is called to set BLE connectable mode for a
**                  peripheral device.
**
** Parameters       directed: is directed connectable mode, or non-directed.
**                  p_dir_bda: connectable direct initiator's LE device address
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleSetConnMode(BOOLEAN is_directed)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;

    BTM_TRACE_EVENT1 ("BTM_BleSetConnMode is_directed = %d ", is_directed);
    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    p_cb->directed_conn = is_directed;
    return btm_ble_set_connectability( p_cb->connectable_mode);

}

/*******************************************************************************
**
** Function         btm_set_conn_mode_adv_init_addr
**
** Description      set initator address type and local address type based on adv
**                  mode.
**
**
*******************************************************************************/
static UINT8 btm_set_conn_mode_adv_init_addr(tBTM_BLE_INQ_CB *p_cb,
                                     BD_ADDR_PTR p_addr_ptr,
                                     tBLE_ADDR_TYPE *p_init_addr_type,
                                     tBLE_ADDR_TYPE *p_own_addr_type)
{
    UINT8 evt_type;

    if ( p_cb->directed_conn)
    {
        /*  direct adv mode does not have privacy if privacy
            is not enabled or no reconn addr config */
        *p_own_addr_type   = BLE_ADDR_PUBLIC;
        *p_init_addr_type  = p_cb->direct_bda.type;
         memcpy(p_addr_ptr, p_cb->direct_bda.bda, BD_ADDR_LEN);
        evt_type = BTM_BLE_CONNECT_DIR_EVT;
    }
    else /* undirect adv mode */
    {
        evt_type = BTM_BLE_CONNECT_EVT;
    }

    return evt_type;

}

/*******************************************************************************
**
** Function         BTM_BleSetAdvParams
**
** Description      This function is called to set advertising parameters.
**
** Parameters       adv_int_min: minimum advertising interval
**                  adv_int_max: maximum advertising interval
**                  p_dir_bda: connectable direct initiator's LE device address
**                  chnl_map: advertising channel map.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleSetAdvParams(UINT16 adv_int_min, UINT16 adv_int_max,
                                tBLE_BD_ADDR *p_dir_bda,
                                tBTM_BLE_ADV_CHNL_MAP chnl_map)
{
    tBTM_LE_RANDOM_CB *p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    tBTM_STATUS status = BTM_SUCCESS;
    BD_ADDR     p_addr_ptr =  {0};
    tBLE_ADDR_TYPE   init_addr_type = BLE_ADDR_PUBLIC;
    tBLE_ADDR_TYPE   own_addr_type = p_addr_cb->own_addr_type;
    UINT8            adv_mode = p_cb->adv_mode;

    BTM_TRACE_EVENT0 ("BTM_BleSetAdvParams");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    if (!BTM_BLE_VALID_PRAM(adv_int_min, BTM_BLE_ADV_INT_MIN, BTM_BLE_ADV_INT_MAX) ||
        !BTM_BLE_VALID_PRAM(adv_int_max, BTM_BLE_ADV_INT_MIN, BTM_BLE_ADV_INT_MAX))
    {
        return BTM_ILLEGAL_VALUE;
    }

    p_cb->adv_interval_min = adv_int_min;
    p_cb->adv_interval_max = adv_int_max;
    p_cb->adv_chnl_map = chnl_map;

    if (p_dir_bda)
    {
        memcpy(&p_cb->direct_bda, p_dir_bda, sizeof(tBLE_BD_ADDR));
    }

    BTM_TRACE_EVENT0 ("update params for an active adv");

    btm_ble_stop_adv();

    if (p_cb->connectable_mode & BTM_BLE_CONNECTABLE)
        p_cb->evt_type = btm_set_conn_mode_adv_init_addr(p_cb, p_addr_ptr, &init_addr_type, &own_addr_type);

    /* update adv params */
    btsnd_hcic_ble_write_adv_params (p_cb->adv_interval_min,
                                     p_cb->adv_interval_max,
                                     p_cb->evt_type,
                                     own_addr_type,
                                     init_addr_type,
                                     p_addr_ptr,
                                     p_cb->adv_chnl_map,
                                     p_cb->afp);

    if (adv_mode == BTM_BLE_ADV_ENABLE)
        btm_ble_start_adv();

    return status;
}

/*******************************************************************************
**
** Function         BTM_BleReadAdvParams
**
** Description      This function is called to set advertising parameters.
**
** Parameters       adv_int_min: minimum advertising interval
**                  adv_int_max: maximum advertising interval
**                  p_dir_bda: connectable direct initiator's LE device address
**                  chnl_map: advertising channel map.
**
** Returns          void
**
*******************************************************************************/
void BTM_BleReadAdvParams (UINT16 *adv_int_min, UINT16 *adv_int_max,
                           tBLE_BD_ADDR *p_dir_bda, tBTM_BLE_ADV_CHNL_MAP *p_chnl_map)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;

    BTM_TRACE_EVENT0 ("BTM_BleReadAdvParams ");
    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return ;

    *adv_int_min = p_cb->adv_interval_min;
    *adv_int_max = p_cb->adv_interval_max;
    *p_chnl_map = p_cb->adv_chnl_map;

    if (p_dir_bda != NULL)
    {
        memcpy(p_dir_bda, &p_cb->direct_bda, sizeof(tBLE_BD_ADDR));
    }
}

/*******************************************************************************
**
** Function         BTM_BleSetScanParams
**
** Description      This function is called to set Scan parameters.
**
** Parameters       adv_int_min: minimum advertising interval
**                  adv_int_max: maximum advertising interval
**                  p_dir_bda: connectable direct initiator's LE device address
**                  chnl_map: advertising channel map.
**                  scan_type: active scan or passive scan
**
** Returns          void
**
*******************************************************************************/
void BTM_BleSetScanParams(UINT16 scan_interval, UINT16 scan_window, tBTM_BLE_SCAN_MODE scan_mode)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;

    BTM_TRACE_EVENT0 (" BTM_BleSetScanParams");
    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return ;

    if (BTM_BLE_VALID_PRAM(scan_interval, BTM_BLE_SCAN_INT_MIN, BTM_BLE_SCAN_INT_MAX) &&
        BTM_BLE_VALID_PRAM(scan_window, BTM_BLE_SCAN_WIN_MIN, BTM_BLE_SCAN_WIN_MAX) &&
        (scan_mode == BTM_BLE_SCAN_MODE_ACTI || scan_mode == BTM_BLE_SCAN_MODE_PASS))
    {
        p_cb->scan_type     = scan_mode;

        if (BTM_BLE_CONN_PARAM_UNDEF != scan_interval)
            p_cb->scan_interval = scan_interval;

        if (BTM_BLE_CONN_PARAM_UNDEF != scan_window)
            p_cb->scan_window   = scan_window;
    }
    else
    {
        BTM_TRACE_ERROR2("Illegal params: scan_interval = %d scan_window = %d",
                        scan_interval, scan_window);
    }

}

/*******************************************************************************
**
** Function         BTM_BleWriteScanRsp
**
** Description      This function is called to write LE scan response.
**
** Parameters:      p_scan_rsp: scan response information.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleWriteScanRsp(tBTM_BLE_AD_MASK data_mask, tBTM_BLE_ADV_DATA *p_data)
{
    tBTM_STATUS     status = BTM_NO_RESOURCES;
    UINT8   rsp_data[BTM_BLE_AD_DATA_LEN],
            *p = rsp_data;

    BTM_TRACE_EVENT0 (" BTM_BleWriteScanRsp");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    memset(rsp_data, 0, BTM_BLE_AD_DATA_LEN);
    btm_ble_build_adv_data(&data_mask, &p, p_data);

    if (btsnd_hcic_ble_set_scan_rsp_data((UINT8)(p - rsp_data), rsp_data))
    {
        status = BTM_SUCCESS;

        if (data_mask != 0)
            btm_cb.ble_ctr_cb.inq_var.scan_rsp = TRUE;
        else
            btm_cb.ble_ctr_cb.inq_var.scan_rsp = FALSE;
    }
    else
        status = BTM_ILLEGAL_VALUE;

    return status;
}

/*******************************************************************************
**
** Function         BTM_BleWriteAdvData
**
** Description      This function is called to write advertising data.
**
** Parameters:       None.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS BTM_BleWriteAdvData(tBTM_BLE_AD_MASK data_mask, tBTM_BLE_ADV_DATA *p_data)
{
    tBTM_BLE_LOCAL_ADV_DATA *p_cb_data = &btm_cb.ble_ctr_cb.inq_var.adv_data;
    UINT8  *p;
    UINT16   mask = data_mask;

    BTM_TRACE_EVENT0 ("BTM_BleWriteAdvData ");

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ILLEGAL_VALUE;

    memset(p_cb_data, 0, sizeof(tBTM_BLE_LOCAL_ADV_DATA));
    p = p_cb_data->ad_data;
    p_cb_data->data_mask = data_mask;

    p_cb_data->p_flags = btm_ble_build_adv_data(&mask, &p, p_data);

    p_cb_data->p_pad = p;

    if (data_mask != 0)
    {
        BTM_TRACE_ERROR0("Partial data write into ADV");
    }

    p_cb_data->data_mask &= ~mask;

    if (btsnd_hcic_ble_set_adv_data((UINT8)(p_cb_data->p_pad - p_cb_data->ad_data),
                                    p_cb_data->ad_data))
        return BTM_SUCCESS;
    else
        return BTM_NO_RESOURCES;

}

/*******************************************************************************
**
** Function         BTM_CheckAdvData
**
** Description      This function is called to get ADV data for a specific type.
**
** Parameters       p_adv - pointer of ADV data
**                  type   - finding ADV data type
**                  p_length - return the length of ADV data not including type
**
** Returns          pointer of ADV data
**
*******************************************************************************/
UINT8 *BTM_CheckAdvData( UINT8 *p_adv, UINT8 type, UINT8 *p_length)
{
    UINT8 *p = p_adv;
    UINT8 length;
    UINT8 adv_type;
    BTM_TRACE_API1("BTM_CheckAdvData type=0x%02X", type);

    STREAM_TO_UINT8(length, p);

    while ( length && (p - p_adv <= BTM_BLE_CACHE_ADV_DATA_MAX))
    {
        STREAM_TO_UINT8(adv_type, p);

        if ( adv_type == type )
        {
            /* length doesn't include itself */
            *p_length = length - 1; /* minus the length of type */
            return p;
        }
        p += length - 1; /* skip the length of data */
        STREAM_TO_UINT8(length, p);
    }

    *p_length = 0;
    return NULL;
}

/*******************************************************************************
**
** Function         btm_ble_build_adv_data
**
** Description      This function is called build the adv data and rsp data.
*******************************************************************************/
static UINT8 *btm_ble_build_adv_data(tBTM_BLE_AD_MASK *p_data_mask, UINT8 **p_dst, tBTM_BLE_ADV_DATA *p_data)
{
    UINT16 data_mask = *p_data_mask;
    UINT8   *p = *p_dst,
    *p_flag = NULL;
    UINT16  len = BTM_BLE_AD_DATA_LEN, cp_len = 0;
    UINT8   i = 0;
    tBTM_BLE_PROP_ELEM      *p_elem;

    BTM_TRACE_EVENT0 (" btm_ble_build_adv_data");

    /* build the adv data structure and build the data string */
    if (data_mask)
    {
        /* flags */
        if (data_mask & BTM_BLE_AD_BIT_FLAGS)
        {
            *p++ = 2;
            *p++ = BTM_BLE_AD_TYPE_FLAG;
            p_flag = p;
            if (p_data)
                *p++ = p_data->flag;
            else
                *p++ = 0;

            len -= 3;

            data_mask &= ~BTM_BLE_AD_BIT_FLAGS;
        }
        /* appearance data */
        if (len > 3 && data_mask & BTM_BLE_AD_BIT_APPEARANCE)
        {
            *p++ = 3; /* length */
            *p++ = BTM_BLE_AD_TYPE_APPEARANCE;
            UINT16_TO_STREAM(p, p_data->appearance);
            len -= 4;

            data_mask &= ~BTM_BLE_AD_BIT_APPEARANCE;
        }
        /* device name */
#if BTM_MAX_LOC_BD_NAME_LEN > 0
        if (len > 2 && data_mask & BTM_BLE_AD_BIT_DEV_NAME)
        {
            if (strlen(btm_cb.cfg.bd_name) > (UINT16)(len - 2))
            {
                *p++ = len - 2 + 1;
                *p++ = BTM_BLE_AD_TYPE_NAME_SHORT;
                ARRAY_TO_STREAM(p, btm_cb.cfg.bd_name, len - 2);
            }
            else
            {
                cp_len = (UINT16)strlen(btm_cb.cfg.bd_name);
                *p++ = cp_len + 1;
                *p++ = BTM_BLE_AD_TYPE_NAME_CMPL;
                ARRAY_TO_STREAM(p, btm_cb.cfg.bd_name, cp_len);
            }
            len -= (cp_len + 2);
            data_mask &= ~BTM_BLE_AD_BIT_DEV_NAME;
        }
#endif
        /* manufacturer data */
        if (len > 2 && data_mask & BTM_BLE_AD_BIT_MANU &&
            p_data && p_data->manu.len != 0 && p_data->manu.p_val)
        {
            if (p_data->manu.len > (len - 2))
                cp_len = len - 2;
            else
                cp_len = p_data->manu.len;

            *p++ = cp_len + 1;
            *p++ = BTM_BLE_AD_TYPE_MANU;
            ARRAY_TO_STREAM(p, p_data->manu.p_val, cp_len);

            len -= (cp_len + 2);
            data_mask &= ~BTM_BLE_AD_BIT_MANU;
        }
        /* TX power */
        if (len > 2 && data_mask & BTM_BLE_AD_BIT_TX_PWR)
        {
            *p++ = 2;
            *p++ = BTM_BLE_AD_TYPE_TX_PWR;
            *p++ = btm_cb.ble_ctr_cb.inq_var.tx_power;
            len -= 3;

            data_mask &= ~BTM_BLE_AD_BIT_TX_PWR;
        }
        /* services */
        if (len > 2 && data_mask & BTM_BLE_AD_BIT_SERVICE &&
            p_data && p_data->services.num_service != 0 &&
            p_data->services.p_uuid)
        {
            if (p_data->services.num_service * 2 > (len - 2))
            {
                cp_len = (len - 2)/2;
                *p ++ = 1 + cp_len * 2;
                *p++ = BTM_BLE_AD_TYPE_16SRV_PART;
            }
            else
            {
                cp_len = p_data->services.num_service;
                *p++ = 1 + cp_len * 2;
                *p++ = BTM_BLE_AD_TYPE_16SRV_CMPL;
            }
            for (i = 0; i < cp_len; i ++)
            {
                UINT16_TO_STREAM(p, *(p_data->services.p_uuid + i));
            }

            len -= (cp_len * 2 + 2);
            data_mask &= ~BTM_BLE_AD_BIT_SERVICE;
        }
        if (len >= 6 && data_mask & BTM_BLE_AD_BIT_INT_RANGE &&
            p_data)
        {
            *p++ = 5;
            *p++ = BTM_BLE_AD_TYPE_INT_RANGE;
            UINT16_TO_STREAM(p, p_data->int_range.low);
            UINT16_TO_STREAM(p, p_data->int_range.hi);
            len -= 6;
            data_mask &= ~BTM_BLE_AD_BIT_INT_RANGE;
        }
        if (data_mask & BTM_BLE_AD_BIT_PROPRIETARY && p_data && p_data->p_proprietary)
        {
            for (i = 0; i < p_data->p_proprietary->num_elem ; i ++)
            {
                p_elem = p_data->p_proprietary->p_elem  + i;

                if (len >= (2 + p_elem->len))/* len byte(1) + ATTR type(1) + Uuid len(2) + value length */
                {
                    *p ++ = p_elem->len + 1; /* Uuid len + value length */
                    *p ++ = p_elem->adv_type;
                    ARRAY_TO_STREAM(p, p_elem->p_val, p_elem->len);

                    len -= (2 + p_elem->len);
                }
                else
                {
                    BTM_TRACE_WARNING0("data exceed max adv packet length");
                    break;
                }
            }
            data_mask &= ~BTM_BLE_AD_BIT_PROPRIETARY;
        }
    }

    *p_data_mask = data_mask;
    *p_dst = p;

    return p_flag;
}

/*******************************************************************************
**
** Function         btm_ble_set_discoverability
**
** Description      This function is called to set BLE discoverable mode.
**
** Parameters:      mode: discoverability mode.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS btm_ble_set_discoverability(UINT16 combined_mode)
{
    tBTM_LE_RANDOM_CB   *p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
    tBTM_BLE_INQ_CB     *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    UINT16              mode = (combined_mode &  BTM_BLE_DISCOVERABLE_MASK);
    UINT8               flag = 0;
    UINT8               new_mode = BTM_BLE_ADV_ENABLE;
    UINT8               evt_type = (p_cb->connectable_mode == BTM_BLE_NON_CONNECTABLE) ? \
                                   ((p_cb->scan_rsp) ? BTM_BLE_DISCOVER_EVT : BTM_BLE_NON_CONNECT_EVT )\
                                   : BTM_BLE_CONNECT_EVT;
    tBTM_STATUS         status = BTM_SUCCESS;
    BD_ADDR             p_addr_ptr= {0};
    tBLE_ADDR_TYPE      init_addr_type = BLE_ADDR_PUBLIC,
                        own_addr_type = p_addr_cb->own_addr_type;;

    BTM_TRACE_EVENT2 ("btm_ble_set_discoverability mode=0x%0x combined_mode=0x%x", mode, combined_mode);

    /*** Check mode parameter ***/
    if (mode > BTM_BLE_MAX_DISCOVERABLE)
        return(BTM_ILLEGAL_VALUE);

    p_cb->br_edr_supported_flag |= (combined_mode & BTM_DISCOVERABLE_MASK);
    p_cb->discoverable_mode = mode;

    if (!p_cb->br_edr_supported_flag)
    {
        flag = BTM_BLE_BREDR_NOT_SPT;
        BTM_TRACE_DEBUG1("btm_ble_set_discoverability (BREDR not sup)flag=0x%x",flag);
    }

    BTM_TRACE_DEBUG1 ("br_edr_supported=0x%x", p_cb->br_edr_supported_flag);

    if (mode == BTM_BLE_LIMITED_DISCOVERABLE || mode == BTM_BLE_GENERAL_DISCOVERABLE)
    {
        BTM_TRACE_EVENT0 ("mode == BTM_BLE_LIMITED_DISCOVERABLE ");
        /* write ADV data with limited disc flag */
        if (mode == BTM_BLE_LIMITED_DISCOVERABLE)
            flag |= BTM_BLE_LIMIT_DISC_FLAG ;
        else
            flag |= BTM_BLE_GEN_DISC_FLAG;
    }
    else  /* non-discoverable */
    {
        BTM_TRACE_EVENT0 ("mode == BTM_BLE_NON_DISCOVERABLE ");

        if (p_cb->connectable_mode == BTM_BLE_NON_CONNECTABLE)
        {
            p_cb->br_edr_supported_flag = 0;

            BTM_TRACE_EVENT0 ("always disable adv in non-discoverable non-connectable mode if no scan rsp ");
            if (!p_cb->scan_rsp )
	            new_mode = BTM_BLE_ADV_DISABLE;

        }
        else
        {
            p_cb->evt_type = btm_set_conn_mode_adv_init_addr(p_cb, p_addr_ptr, &init_addr_type, &own_addr_type);
        }
    }
    btm_ble_update_adv_flag(flag);

    /* update adv params if start advertising */
    BTM_TRACE_EVENT2 ("evt_type=0x%x p-cb->evt_type=0x%x ", evt_type, p_cb->evt_type);
    if (new_mode == BTM_BLE_ADV_ENABLE &&
        (evt_type != p_cb->evt_type ||p_cb->adv_addr_type != own_addr_type))
    {
        btm_ble_stop_adv();

        /* update adv params */
        if (!btsnd_hcic_ble_write_adv_params ((UINT16)(p_cb->adv_interval_min ? p_cb->adv_interval_min : BTM_BLE_GAP_ADV_INT),
                                              (UINT16)(p_cb->adv_interval_max ? p_cb->adv_interval_max : BTM_BLE_GAP_ADV_INT),
                                              evt_type,
                                              own_addr_type,
                                              init_addr_type,
                                              p_addr_ptr,
                                              p_cb->adv_chnl_map,
                                              p_cb->afp))

            status = BTM_NO_RESOURCES;
        else
        {
            p_cb->evt_type = evt_type;
            p_cb->adv_addr_type = own_addr_type;
        }

    }

    if (status == BTM_SUCCESS && p_cb->adv_mode != new_mode)
    {
        if (new_mode == BTM_BLE_ADV_ENABLE)
            status = btm_ble_start_adv();
        else
            status = btm_ble_stop_adv();
    }

    /* set up stop advertising timer */
    if (status == BTM_SUCCESS && mode == BTM_BLE_LIMITED_DISCOVERABLE)
    {
        BTM_TRACE_EVENT1 ("start timer for limited disc mode duration=%d (30 secs)", BTM_BLE_GAP_LIM_TOUT);
        /* start Tgap(lim_timeout) */
        btu_start_timer (&p_cb->inq_timer_ent, BTU_TTYPE_BLE_GAP_LIM_DISC,
                         BTM_BLE_GAP_LIM_TOUT);
    }
    return status;
}

/*******************************************************************************
**
** Function         btm_ble_set_connectability
**
** Description      This function is called to set BLE connectability mode.
**
** Parameters:      mode: connectability mode.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS btm_ble_set_connectability(UINT16 combined_mode)
{
    tBTM_LE_RANDOM_CB       *p_addr_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
    tBTM_BLE_INQ_CB         *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    UINT16                  mode = (combined_mode & BTM_BLE_CONNECTABLE_MASK);
    UINT8                   cur_flag = 0;
    UINT8                   cur_br_edr_not_sup_flag;
    UINT8                   new_flag;
    UINT8                   new_mode = BTM_BLE_ADV_ENABLE;
    UINT8                   evt_type = (p_cb->scan_rsp) ? BTM_BLE_DISCOVER_EVT: BTM_BLE_NON_CONNECT_EVT;
    tBTM_STATUS             status = BTM_SUCCESS;
    BD_ADDR                 p_addr_ptr =  {0};
    tBLE_ADDR_TYPE          init_addr_type = BLE_ADDR_PUBLIC,
                            own_addr_type = p_addr_cb->own_addr_type;

    BTM_TRACE_EVENT2 ("btm_ble_set_connectability mode=0x%0x combined_mode=0x%x", mode, combined_mode);
    /*** Check mode parameter ***/
    if (mode > BTM_BLE_MAX_CONNECTABLE)
        return(BTM_ILLEGAL_VALUE);
    if (btm_cb.ble_ctr_cb.inq_var.adv_data.p_flags)
        cur_flag = *btm_cb.ble_ctr_cb.inq_var.adv_data.p_flags ;
    cur_br_edr_not_sup_flag = (cur_flag & ((UINT8) BTM_BLE_BREDR_NOT_SPT));

    p_cb->br_edr_supported_flag |= ((combined_mode & BTM_CONNECTABLE_MASK) << 4);
    if (p_cb->br_edr_supported_flag && cur_br_edr_not_sup_flag)
    {
        new_flag = cur_flag & ((UINT8) (~BTM_BLE_BREDR_NOT_SPT));
        BTM_TRACE_EVENT2 ("new flag=0x%x cur flag=0x%x",new_flag,  cur_flag);
        btm_ble_update_adv_flag(new_flag);
    }
    p_cb->connectable_mode = mode;

    if (mode == BTM_BLE_NON_CONNECTABLE)
    {
        if (p_cb->discoverable_mode == BTM_BLE_NON_DISCOVERABLE)
        {
            p_cb->br_edr_supported_flag = 0;
            BTM_TRACE_EVENT0 ("always disable adv in non-discoverable non-connectable mode with no scan rsp");
            if(!p_cb->scan_rsp)
	            new_mode = BTM_BLE_ADV_DISABLE;

        }
    }
    else /* connectable */
    {
        evt_type = btm_set_conn_mode_adv_init_addr(p_cb, p_addr_ptr, &init_addr_type, &own_addr_type);
    }

    /* update adv params if needed */
    if ((p_cb->evt_type != evt_type || p_cb->adv_addr_type != p_addr_cb->own_addr_type)
        && new_mode == BTM_BLE_ADV_ENABLE)
    {
        btm_ble_stop_adv();

        if (!btsnd_hcic_ble_write_adv_params ((UINT16)(p_cb->adv_interval_min ? p_cb->adv_interval_min : BTM_BLE_GAP_ADV_INT),
                                              (UINT16)(p_cb->adv_interval_max ? p_cb->adv_interval_max : BTM_BLE_GAP_ADV_INT),
                                              evt_type,
                                              own_addr_type,
                                              init_addr_type,
                                              p_addr_ptr,
                                              p_cb->adv_chnl_map,
                                              p_cb->afp))
            status = BTM_NO_RESOURCES;
        else
        {
            p_cb->evt_type = evt_type;
            p_cb->adv_addr_type = own_addr_type;
        }
    }

    /* update advertising mode */
    if (status == BTM_SUCCESS && new_mode != p_cb->adv_mode)
    {
        if (btsnd_hcic_ble_set_adv_enable (new_mode))
        {
            status = BTM_SUCCESS;

            p_cb->adv_mode = new_mode;

            if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE &&
                p_cb->afp != AP_SCAN_CONN_ALL)
                btm_cb.ble_ctr_cb.wl_state |= BTM_BLE_WL_ADV;
            else
                btm_cb.ble_ctr_cb.wl_state &= ~BTM_BLE_WL_ADV;

        }
    }

    return status;
}

/*******************************************************************************
**
** Function         btm_ble_start_inquiry
**
** Description      This function is called to start BLE inquiry procedure.
**                  If the duration is zero, the periodic inquiry mode is cancelled.
**
** Parameters:      mode - GENERAL or LIMITED inquiry
**                  p_inq_params - pointer to the BLE inquiry parameter.
**                  p_results_cb - callback returning pointer to results (tBTM_INQ_RESULTS)
**                  p_cmpl_cb - callback indicating the end of an inquiry
**
**
**
** Returns          BTM_CMD_STARTED if successfully started
**                  BTM_NO_RESOURCES if could not allocate a message buffer
**                  BTM_BUSY - if an inquiry is already active
**
*******************************************************************************/
tBTM_STATUS btm_ble_start_inquiry (UINT8 mode, UINT8   duration)
{
    tBTM_STATUS status = BTM_NO_RESOURCES;
    tBTM_BLE_INQ_CB *p_inq = &btm_cb.ble_ctr_cb.inq_var;

    BTM_TRACE_DEBUG2("btm_ble_start_inquiry: mode = %02x inq_active = %d", mode, btm_cb.btm_inq_vars.inq_active);

    if (p_inq->proc_mode != BTM_BLE_INQUIRY_NONE)
    {
        BTM_TRACE_ERROR0("LE scan is active, can not start inquiry");
        return(BTM_BUSY);
    }

    btm_update_scanner_filter_policy(SP_ADV_ALL);

    /* start scan, already enable duplicate filtering */
    if (btsnd_hcic_ble_set_scan_enable (BTM_BLE_SCAN_ENABLE, BTM_BLE_DUPLICATE_DISABLE))
    {
        status = BTM_CMD_STARTED;
        p_inq->proc_mode = mode;

        if (duration != 0)
        {
            /* start inquiry timer */
            btu_start_timer (&p_inq->inq_timer_ent, BTU_TTYPE_BLE_INQUIRY, duration);
        }
    }

    return status;
}

/*******************************************************************************
**
** Function         btm_ble_read_remote_name_cmpl
**
** Description      This function is called when BLE remote name is received.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_read_remote_name_cmpl(BOOLEAN status, BD_ADDR bda, UINT16 length, char *p_name)
{
    UINT8   hci_status = HCI_SUCCESS;
    BD_NAME bd_name;

    memset(bd_name, 0, (BD_NAME_LEN + 1));
    memcpy((UINT8*)bd_name, p_name, length);

    if ((!status) || (length==0))
    {
        hci_status = HCI_ERR_HOST_TIMEOUT;
    }

    btm_process_remote_name(bda, bd_name, length +1, hci_status);
    btm_sec_rmt_name_request_complete (bda, (UINT8 *)p_name, hci_status);
}

/*******************************************************************************
**
** Function         btm_ble_read_remote_name
**
** Description      This function read remote LE device name using GATT read
**                  procedure.
**
** Parameters:       None.
**
** Returns          void
**
*******************************************************************************/
tBTM_STATUS btm_ble_read_remote_name(BD_ADDR remote_bda, tBTM_INQ_INFO *p_cur, tBTM_CMPL_CB *p_cb)
{
    tBTM_INQUIRY_VAR_ST      *p_inq = &btm_cb.btm_inq_vars;

    if (!HCI_LE_HOST_SUPPORTED(btm_cb.devcb.local_lmp_features[HCI_EXT_FEATURES_PAGE_1]))
        return BTM_ERR_PROCESSING;

    if (p_cur &&
        p_cur->results.ble_evt_type != BTM_BLE_EVT_CONN_ADV &&
        p_cur->results.ble_evt_type != BTM_BLE_EVT_CONN_DIR_ADV)
    {
        BTM_TRACE_DEBUG0("name request to non-connectable device failed.");
        return BTM_ERR_PROCESSING;
    }

    /* read remote device name using GATT procedure */
    if (p_inq->remname_active)
        return BTM_BUSY;

    if (!GAP_BleReadPeerDevName(remote_bda, btm_ble_read_remote_name_cmpl))
        return BTM_BUSY;

    p_inq->p_remname_cmpl_cb = p_cb;
    p_inq->remname_active = TRUE;

    memcpy(p_inq->remname_bda, remote_bda, BD_ADDR_LEN);

    btu_start_timer (&p_inq->rmt_name_timer_ent,
                     BTU_TTYPE_BTM_RMT_NAME,
                     BTM_EXT_BLE_RMT_NAME_TIMEOUT);

    return BTM_CMD_STARTED;
}

/*******************************************************************************
**
** Function         btm_ble_cancel_remote_name
**
** Description      This function cancel read remote LE device name.
**
** Parameters:       None.
**
** Returns          void
**
*******************************************************************************/
BOOLEAN btm_ble_cancel_remote_name(BD_ADDR remote_bda)
{
    tBTM_INQUIRY_VAR_ST      *p_inq = &btm_cb.btm_inq_vars;
    BOOLEAN     status;

    status = GAP_BleCancelReadPeerDevName(remote_bda);

    p_inq->remname_active = FALSE;
    memset(p_inq->remname_bda, 0, BD_ADDR_LEN);
    btu_stop_timer(&p_inq->rmt_name_timer_ent);

    return status;
}

/*******************************************************************************
**
** Function         btm_ble_update_adv_flag
**
** Description      This function update the limited discoverable flag in the adv
**                  data.
**
** Parameters:       None.
**
** Returns          void
**
*******************************************************************************/
static void btm_ble_update_adv_flag(UINT8 flag)
{
    tBTM_BLE_LOCAL_ADV_DATA *p_adv_data = &btm_cb.ble_ctr_cb.inq_var.adv_data;
    UINT8   *p;

    BTM_TRACE_DEBUG1 ("btm_ble_update_adv_flag new=0x%x", flag);

    if (p_adv_data->p_flags != NULL)
    {
        BTM_TRACE_DEBUG1 ("btm_ble_update_adv_flag old=0x%x",   *p_adv_data->p_flags);
        *p_adv_data->p_flags = flag;
    }
    else /* no FLAGS in ADV data*/
    {
        p = (p_adv_data->p_pad == NULL) ? p_adv_data->ad_data : p_adv_data->p_pad;
        /* need 3 bytes space to stuff in the flags, if not */
        /* erase all written data, just for flags */
        if ((BTM_BLE_AD_DATA_LEN - (p - p_adv_data->ad_data)) < 3)
        {
            p = p_adv_data->p_pad = p_adv_data->ad_data;
            memset(p_adv_data->ad_data, 0, BTM_BLE_AD_DATA_LEN);
        }

        *p++ = 2;
        *p++ = BTM_BLE_AD_TYPE_FLAG;
        p_adv_data->p_flags = p;
        *p++ = flag;
        p_adv_data->p_pad = p;
    }

    if (btsnd_hcic_ble_set_adv_data((UINT8)(p_adv_data->p_pad - p_adv_data->ad_data),
                                    p_adv_data->ad_data))
        p_adv_data->data_mask |= BTM_BLE_AD_BIT_FLAGS;

}

#if 0
/*******************************************************************************
**
** Function         btm_ble_parse_adv_data
**
** Description      This function parse the adv data into a structure.
**
** Returns          pointer to entry, or NULL if not found
**
*******************************************************************************/
static void btm_ble_parse_adv_data(tBTM_INQ_INFO *p_info, UINT8 *p_data,
                                   UINT8 len, tBTM_BLE_INQ_DATA *p_adv_data, UINT8 *p_buf)
{
    UINT8   *p_cur = p_data;
    UINT8   ad_len, ad_type, ad_flag;

    BTM_TRACE_EVENT0 (" btm_ble_parse_adv_data");

    while (len > 0)
    {
        BTM_TRACE_DEBUG1("btm_ble_parse_adv_data: len = %d", len);
        if ((ad_len = *p_cur ++) == 0)
            break;

        ad_type = *p_cur ++;

        BTM_TRACE_DEBUG2("     ad_type = %02x ad_len = %d", ad_type, ad_len);

        switch (ad_type)
        {
            case BTM_BLE_AD_TYPE_NAME_SHORT:

            case BTM_BLE_AD_TYPE_NAME_CMPL:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_DEV_NAME;
                if (p_info)
                {
                    p_info->remote_name_type =(ad_type == BTM_BLE_AD_TYPE_NAME_SHORT) ?
                                              BTM_BLE_NAME_SHORT: BTM_BLE_NAME_CMPL;
                    memcpy(p_info->remote_name, p_cur, ad_len -1);
                    p_info->remote_name[ad_len] = 0;
                    p_adv_data->p_remote_name = p_info->remote_name;
                    p_info->remote_name_len = p_adv_data->remote_name_len = ad_len - 1;
                    BTM_TRACE_DEBUG1("BTM_BLE_AD_TYPE_NAME name = %s",p_adv_data->p_remote_name);
                }
                p_cur += (ad_len -1);

                break;

            case BTM_BLE_AD_TYPE_FLAG:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_FLAGS;
                ad_flag = *p_cur ++;
                p_adv_data->flag = (UINT8)(ad_flag & BTM_BLE_ADV_FLAG_MASK) ;
                BTM_TRACE_DEBUG3("BTM_BLE_AD_TYPE_FLAG flag = %s | %s | %s",
                                 (p_adv_data->flag & BTM_BLE_LIMIT_DISC_FLAG)? "LE_LIMIT_DISC" : "",
                                 (p_adv_data->flag & BTM_BLE_GEN_DISC_FLAG)? "LE_GENERAL_DISC" : "",
                                 (p_adv_data->flag & BTM_BLE_BREDR_NOT_SPT)? "LE Only device" : "");
                break;

            case BTM_BLE_AD_TYPE_TX_PWR:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_TX_PWR;
                p_adv_data->tx_power_level = (INT8)*p_cur ++;
                BTM_TRACE_DEBUG1("BTM_BLE_AD_TYPE_TX_PWR tx_level = %d", p_adv_data->tx_power_level);
                break;

            case BTM_BLE_AD_TYPE_MANU:

            case BTM_BLE_AD_TYPE_16SRV_PART:
            case BTM_BLE_AD_TYPE_16SRV_CMPL:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_SERVICE;
                /* need allocate memory to store UUID list */
                p_adv_data->service.num_service = (ad_len - 1)/2;
                BTM_TRACE_DEBUG1("service UUID list, num = %d", p_adv_data->service.num_service);
                p_cur += (ad_len - 1);
                break;

            case BTM_BLE_AD_TYPE_SOL_SRV_UUID:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_SERVICE_SOL;
                /* need allocate memory to store UUID list */
                p_adv_data->service.num_service = (ad_len - 1)/2;
                BTM_TRACE_DEBUG1("service UUID list, num = %d", p_adv_data->service.num_service);
                p_cur += (ad_len - 1);
                break;

            case BTM_BLE_AD_TYPE_128SOL_SRV_UUID:
                p_adv_data->ad_mask |= BTM_BLE_AD_BIT_SERVICE_128SOL;
                /* need allocate memory to store UUID list */
                p_adv_data->service.num_service = (ad_len - 1)/16;
                BTM_TRACE_DEBUG1("service UUID list, num = %d", p_adv_data->service.num_service);
                p_cur += (ad_len - 1);
                break;

            case BTM_BLE_AD_TYPE_APPEARANCE:
            case BTM_BLE_AD_TYPE_PUBLIC_TARGET:
            case BTM_BLE_AD_TYPE_RANDOM_TARGET:
            default:
                break;
        }
        len -= (ad_len + 1);
    }
}
#endif

/*******************************************************************************
**
** Function         btm_ble_cache_adv_data
**
** Description      Update advertising cache data.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_cache_adv_data(tBTM_INQ_RESULTS *p_cur, UINT8 data_len, UINT8 *p, UINT8 evt_type)
{
    tBTM_BLE_INQ_CB     *p_le_inq_cb = &btm_cb.ble_ctr_cb.inq_var;
    UINT8 *p_cache;
    UINT8 length;

    /* cache adv report/scan response data */
    if (evt_type != BTM_BLE_SCAN_RSP_EVT)
    {
        p_le_inq_cb->adv_len = 0;
        memset(p_le_inq_cb->adv_data_cache, 0, BTM_BLE_CACHE_ADV_DATA_MAX);
    }

    if (data_len > 0)
    {
        p_cache = &p_le_inq_cb->adv_data_cache[p_le_inq_cb->adv_len];
        STREAM_TO_UINT8(length, p);
        while ( length && ((p_le_inq_cb->adv_len + length + 1) <= BTM_BLE_CACHE_ADV_DATA_MAX))
        {
            /* copy from the length byte & data into cache */
            memcpy(p_cache, p-1, length+1);
            /* advance the cache pointer past data */
            p_cache += length+1;
            /* increment cache length */
            p_le_inq_cb->adv_len += length+1;
            /* skip the length of data */
            p += length;
            STREAM_TO_UINT8(length, p);
        }
    }

    /* parse service UUID from adv packet and save it in inq db eir_uuid */
    /* TODO */
}

/*******************************************************************************
**
** Function         btm_ble_is_discoverable
**
** Description      check ADV flag to make sure device is discoverable and match
**                  the search condition
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
BOOLEAN btm_ble_is_discoverable(BD_ADDR bda, UINT8 evt_type, UINT8 *p)
{
    BOOLEAN             is_discoverable = FALSE;
    UINT8               *p_flag, flag = 0;
    UINT8                data_len;
    tBTM_INQ_PARMS      *p_cond = &btm_cb.btm_inq_vars.inqparms;

    STREAM_TO_UINT8    (data_len, p);

    /* for observer, always "discoverable */
    if (btm_cb.ble_ctr_cb.inq_var.proc_mode == BTM_BLE_OBSERVE ||
        (btm_cb.ble_ctr_cb.inq_var.proc_mode == BTM_BLE_SELECT_SCAN &&
        btm_cb.ble_ctr_cb.bg_conn_type == BTM_BLE_CONN_SELECTIVE))
        return TRUE;

    /* does not match filter condition */
    if (p_cond->filter_cond_type == BTM_FILTER_COND_BD_ADDR &&
        memcmp(bda, p_cond->filter_cond.bdaddr_cond, BD_ADDR_LEN) != 0)
    {
        BTM_TRACE_DEBUG0("BD ADDR does not meet filter condition");
        return FALSE;
    }

    /* scan response does not include the flag */
    if (evt_type == BTM_BLE_SCAN_RSP_EVT)
        return FALSE;

    if (data_len > BTM_BLE_ADV_DATA_LEN_MAX)
    {
        BTM_TRACE_WARNING1("ADV data too long %d. discard", data_len);
        return FALSE;
    }

    if (data_len != 0)
    {
        if ((p_flag = BTM_CheckAdvData(p, BTM_BLE_AD_TYPE_FLAG, &data_len)) != NULL)
        {
            flag = * p_flag;

            if ((btm_cb.ble_ctr_cb.inq_var.proc_mode == BTM_BLE_GENERAL_INQUIRY) &&
                (flag & (BTM_BLE_LIMIT_DISC_FLAG|BTM_BLE_GEN_DISC_FLAG)) != 0)
            {
                BTM_TRACE_DEBUG0("Find Generable Discoverable device");
                is_discoverable = TRUE;
            }

            else if (btm_cb.ble_ctr_cb.inq_var.proc_mode == BTM_BLE_LIMITED_INQUIRY &&
                     (flag & BTM_BLE_LIMIT_DISC_FLAG) != 0)
            {
                BTM_TRACE_DEBUG0("Find limited discoverable device");
                is_discoverable = TRUE;
            }

        }
    }

    if (!is_discoverable)
    {
        BTM_TRACE_ERROR1("discoverable flag not desired: %d", flag);
    }

    return is_discoverable;
}

/*******************************************************************************
**
** Function         btm_ble_update_inq_result
**
** Description      Update adv packet information into inquiry result.
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
BOOLEAN btm_ble_update_inq_result(tINQ_DB_ENT *p_i, UINT8 addr_type, UINT8 evt_type, UINT8 *p)
{
    BOOLEAN             to_report = TRUE;
    tBTM_INQ_RESULTS     *p_cur = &p_i->inq_info.results;
    UINT8               len;
    UINT8               *p_flag;
    tBTM_INQUIRY_VAR_ST  *p_inq = &btm_cb.btm_inq_vars;
    UINT8                data_len, rssi;
    tBTM_BLE_INQ_CB     *p_le_inq_cb = &btm_cb.ble_ctr_cb.inq_var;
    UINT8 *p1;

    STREAM_TO_UINT8    (data_len, p);

    if (data_len > BTM_BLE_ADV_DATA_LEN_MAX)
    {
        BTM_TRACE_WARNING1("EIR data too long %d. discard", data_len);
        return FALSE;
    }
    btm_ble_cache_adv_data(p_cur, data_len, p, evt_type);

    p1 = (p + data_len);
    STREAM_TO_UINT8 (rssi, p1);

    /* Save the info */
    p_cur->inq_result_type = BTM_INQ_RESULT_BLE;
    p_cur->ble_addr_type    = addr_type;
    p_cur->rssi = rssi;

    /* active scan, always wait until get scan_rsp to report the result */
    if ((btm_cb.ble_ctr_cb.inq_var.scan_type == BTM_BLE_SCAN_MODE_ACTI &&
         (evt_type == BTM_BLE_CONNECT_EVT || evt_type == BTM_BLE_DISCOVER_EVT)))
    {
        p_i->scan_rsp = FALSE;
        to_report = FALSE;
    }
    else
        p_i->scan_rsp = TRUE;

    if (p_i->inq_count != p_inq->inq_counter)
        p_cur->device_type = BT_DEVICE_TYPE_BLE;
    else
        p_cur->device_type |= BT_DEVICE_TYPE_BLE;

    if (evt_type != BTM_BLE_SCAN_RSP_EVT)
        p_cur->ble_evt_type     = evt_type;

    p_i->inq_count = p_inq->inq_counter;   /* Mark entry for current inquiry */

    if (p_le_inq_cb->adv_len != 0)
    {
        if ((p_flag = BTM_CheckAdvData(p_le_inq_cb->adv_data_cache, BTM_BLE_AD_TYPE_FLAG, &len)) != NULL)
            p_cur->flag = * p_flag;
    }

    /* if BR/EDR not supported is not set, assume is a DUMO device */
    if ((p_cur->flag & BTM_BLE_BREDR_NOT_SPT) == 0 &&
         evt_type != BTM_BLE_CONNECT_DIR_EVT)
    {
        if (p_cur->ble_addr_type != BLE_ADDR_RANDOM)
        {
            BTM_TRACE_DEBUG0("BR/EDR NOT support bit not set, treat as DUMO");
            p_cur->device_type |= BT_DEVICE_TYPE_DUMO;
        } else {
            BTM_TRACE_DEBUG0("Random address, treating device as LE only");
        }
    }
    else
    {
        BTM_TRACE_DEBUG0("BR/EDR NOT SUPPORT bit set, LE only device");
    }

    return to_report;

}

/*******************************************************************************
**
** Function         btm_send_sel_conn_callback
**
** Description      send selection connection request callback.
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
void btm_send_sel_conn_callback(BD_ADDR remote_bda, UINT8 evt_type, UINT8 *p_data, UINT8 addr_type)
{
    UINT8   data_len, len;
    UINT8   *p_dev_name, remname[31] = {0};

    if (btm_cb.ble_ctr_cb.p_select_cback == NULL ||
        /* non-connectable device */
        (evt_type != BTM_BLE_EVT_CONN_ADV && evt_type != BTM_BLE_EVT_CONN_DIR_ADV))
        return;

    STREAM_TO_UINT8    (data_len, p_data);

    /* get the device name if exist in ADV data */
    if (data_len != 0)
    {
        p_dev_name = BTM_CheckAdvData(p_data, BTM_BLE_AD_TYPE_NAME_CMPL, &len);

        if (p_dev_name == NULL)
            p_dev_name = BTM_CheckAdvData(p_data, BTM_BLE_AD_TYPE_NAME_SHORT, &len);

        if (p_dev_name)
            memcpy(remname, p_dev_name, len);
    }
    /* allow connection */
    if ((* btm_cb.ble_ctr_cb.p_select_cback)(remote_bda, remname))
    {
        /* terminate selective connection, initiate connection */
        btm_ble_initiate_select_conn(remote_bda);
    }
}

/*******************************************************************************
**
** Function         btm_ble_process_adv_pkt
**
** Description      This function is called when adv packet report events are
**                  received from the device. It updates the inquiry database.
**                  If the inquiry database is full, the oldest entry is discarded.
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
void btm_ble_process_adv_pkt (UINT8 *p_data)
{
    BD_ADDR             bda;
    UINT8               evt_type = 0, *p = p_data;
    UINT8               addr_type = 0;

    /* always get one device at a time */
    p ++;

    /* Extract inquiry results */
    STREAM_TO_UINT8    (evt_type, p);
    STREAM_TO_UINT8    (addr_type, p);
    STREAM_TO_BDADDR   (bda, p);

#ifdef BTM_BLE_PC_ADV_TEST_MODE /* For general stack code (e.g. BTInsight testing), we simply do not define it to exclude or set it to TRUE to include */
    if (BTM_BLE_PC_ADV_TEST_MODE)   /* For stack component, it is always defined and maps to a global variable g_bDraculaAdvertisingMode */
    {
        if (btm_cb.ble_ctr_cb.p_scan_req_cback)
            (*btm_cb.ble_ctr_cb.p_scan_req_cback)(bda, addr_type, evt_type);
    }
#endif



    /* Only process the results if the inquiry is still active */
    if ((btm_cb.btm_inq_vars.inq_active & BTM_LE_SCAN_ACTIVE_MASK) == 0 &&
        (btm_cb.ble_ctr_cb.bg_conn_type != BTM_BLE_CONN_SELECTIVE ||
         /* or selective auto connection is active */
         btm_cb.ble_ctr_cb.p_select_cback == NULL))
        return;

    btm_ble_process_adv_pkt_cont(bda, addr_type, evt_type, p);
}

#ifdef RTL_8723BS_BT_USED
int btm_ble_check_cod(UINT8* data, UINT8* flag)
{
    UINT8 data_len = *data;
    UINT8* p;
    BOOLEAN isServiceExist = FALSE;
    int class_of_device = 0;
    int len, i;
    int type;
    int num_service;
    short service;
    data ++;
    if(data_len == 0)
        return class_of_device;

    BTM_TRACE_ERROR1("data_len = %d", data_len);
    for(i = 0; i<data_len; i++)
        BTM_TRACE_ERROR2("data[%d] = %x", i, data[i]);

    while( data_len != 0) {
        len = *data;
        if(len> data_len) {
            BTM_TRACE_ERROR2("data_len = %d, len = %d, data length is error", data_len
, data);
            return class_of_device;
        }

        data_len -= (len+1);

        data++;
        type = *data;
        data++;
        switch(type)
        {
            case BTM_BLE_AD_TYPE_FLAG:
                *flag = data[0];
                break;
            case BTM_BLE_AD_TYPE_DEV_CLASS:
                class_of_device = data[0] | data[1] <<8 | data [2] <<16;
                break;

            case BTM_BLE_AD_TYPE_16SRV_PART:
            case BTM_BLE_AD_TYPE_16SRV_CMPL:
                num_service = (len - 1)/2;
                service = 0;
                p = data;
                for(i=0; i<num_service; i++)
                {
                    service = p[0] | p[1] <<8;
                    p += 2;
                    if(service == 0x1812)
                    {
                        isServiceExist = TRUE;
                        break;
                    }
                }

            break;
        }
        data += len -1;
    }

    if(class_of_device != 0)
        return class_of_device;

    if(isServiceExist)
            class_of_device = 0x0500;

    return class_of_device;
}
#endif

/*******************************************************************************
**
** Function         btm_ble_process_adv_pkt_cont
**
** Description      This function is called after random address resolution is
**                  done, and proceed to process adv packet.
**
** Parameters
**
** Returns          void
**
*******************************************************************************/
static void btm_ble_process_adv_pkt_cont(BD_ADDR bda, UINT8 addr_type, UINT8 evt_type, UINT8 *p)
{
    tINQ_DB_ENT          *p_i;
    BOOLEAN              to_report = FALSE;
    tBTM_INQUIRY_VAR_ST  *p_inq = &btm_cb.btm_inq_vars;
    tBTM_INQ_RESULTS_CB  *p_inq_results_cb = p_inq->p_inq_results_cb;
    tBTM_BLE_INQ_CB      *p_le_inq_cb = &btm_cb.ble_ctr_cb.inq_var;
#ifdef RTL_8723BS_BT_USED
    UINT8* pp = p;
    UINT32 cod = 0;
    UINT8  flag = 0; 
#endif

    p_i = btm_inq_db_find (bda);

    /* Check if this address has already been processed for this inquiry */
    if (btm_inq_find_bdaddr(bda))
    {
        /* never been report as an LE device */
        if ((p_i &&
            (!(p_i->inq_info.results.device_type & BT_DEVICE_TYPE_BLE) ||
              /* scan repsonse to be updated */
              (!p_i->scan_rsp)))
            ||
            btm_cb.ble_ctr_cb.inq_var.proc_mode == BTM_BLE_OBSERVE)
        {
            BTM_TRACE_DEBUG0("update new BLE information ");
            to_report = TRUE;
        }
        else
        {
#ifndef RTL_8723BS_BT_USED
            /* if yes, skip it */
            return; /* assumption: one result per event */
#else
            BTM_TRACE_DEBUG0("LE in le_bd_db already");
            to_report = TRUE;
            //return; /* assumption: one result per event */
#endif
        }
    }
    else /* not been processed int his round */
    {
        to_report = TRUE;
    }

    /* If existing entry, use that, else get  a new one (possibly reusing the oldest) */
    if (p_i == NULL)
    {
        if (btm_ble_is_discoverable(bda, evt_type, p))
        {
            if ((p_i = btm_inq_db_new (bda)) != NULL)
            {
                p_inq->inq_cmpl_info.num_resp++;
                to_report = TRUE;
            }
            else
                return;
        }
        else
        {
            BTM_TRACE_ERROR0("discard adv pkt");
            return;
        }
    }
    else if (p_i->inq_count != p_inq->inq_counter) /* first time seen in this inquiry */
    {
        p_inq->inq_cmpl_info.num_resp++;
    }

    /* update the LE device information in inquiry database */
    if (to_report)
    {
        to_report = btm_ble_update_inq_result(p_i, addr_type, evt_type, p);
#ifdef RTL_8723BS_BT_USED
        cod = btm_ble_check_cod(pp , &flag);
        BTM_TRACE_ERROR1("cod = 0x%x", cod);
        p_i->inq_info.results.dev_class[2] = (UINT8)(cod &0xff);
        p_i->inq_info.results.dev_class[1] = (UINT8)((cod &0xff00) >>8);
        p_i->inq_info.results.dev_class[0] = (UINT8)((cod &0xff0000) >>16);
#endif
    }

#if BTM_USE_INQ_RESULTS_FILTER == TRUE
    /* If the number of responses found and limited, issue a cancel inquiry */
    if (p_inq->inqparms.max_resps &&
        p_inq->inq_cmpl_info.num_resp == p_inq->inqparms.max_resps)
    {
        /* new device */
        if (p_i == NULL ||
            (/* assume a DUMO device, BR/EDR inquiry is always active */
             p_i && p_i->inq_info.results.device_type == BT_DEVICE_TYPE_BLE && p_i->scan_rsp))
        {
            BTM_TRACE_WARNING0("INQ RES: Extra Response Received...cancelling inquiry..");

            /* if is non-periodic inquiry active, cancel now */
            if ((p_inq->inq_active & BTM_BR_INQ_ACTIVE_MASK) != 0 &&
                (p_inq->inq_active & BTM_PERIODIC_INQUIRY_ACTIVE) == 0)
                btsnd_hcic_inq_cancel();

            /* stop LE scan now */
            btm_ble_stop_scan();

#if BTM_BUSY_LEVEL_CHANGE_INCLUDED == TRUE
            btm_acl_update_busy_level (BTM_BLI_INQ_DONE_EVT);
#endif
        }
    }
#endif

    /* background connection in selective connection mode */
    if (btm_cb.ble_ctr_cb.bg_conn_type == BTM_BLE_CONN_SELECTIVE)
    {
        if (p_i->inq_info.results.device_type == BT_DEVICE_TYPE_BLE &&
            (evt_type == BTM_BLE_CONNECT_EVT || evt_type == BTM_BLE_CONNECT_DIR_EVT))
            btm_send_sel_conn_callback(bda, evt_type, p, addr_type);
        else
        {
            BTM_TRACE_DEBUG0("None LE device, can not initiate selective connection");
        }
    }
    else if (p_inq_results_cb && to_report)
    {
        (p_inq_results_cb)((tBTM_INQ_RESULTS *) &p_i->inq_info.results, p_le_inq_cb->adv_data_cache);
    }
    
#ifdef RTL_8723BS_BT_USED    
    (p_inq_results_cb)((tBTM_INQ_RESULTS *) &p_i->inq_info.results, p_le_inq_cb->adv_data_cache);
#endif    
}

/*******************************************************************************
**
** Function         btm_ble_stop_scan
**
** Description      Stop the BLE scan.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_stop_scan(void)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    tBTM_INQUIRY_VAR_ST *p_inq = &btm_cb.btm_inq_vars;

    BTM_TRACE_EVENT0 ("btm_ble_stop_scan ");

    btu_stop_timer (&p_cb->inq_timer_ent);

    /* Clear the inquiry callback if set */
    p_cb->scan_type = BTM_BLE_SCAN_MODE_NONE;
    p_cb->proc_mode = BTM_BLE_INQUIRY_NONE;

    /* stop discovery now */
    btsnd_hcic_ble_set_scan_enable (BTM_BLE_SCAN_DISABLE, BTM_BLE_DUPLICATE_ENABLE);

    /* If we have a callback registered for inquiry complete, call it */
    BTM_TRACE_DEBUG2 ("BTM Inq Compl Callback: status 0x%02x, num results %d",
                      p_inq->inq_cmpl_info.status, p_inq->inq_cmpl_info.num_resp);

    btm_update_scanner_filter_policy(SP_ADV_ALL);

    btm_process_inq_complete(HCI_SUCCESS, (UINT8)(p_inq->inqparms.mode & BTM_BLE_INQUIRY_MASK));

}

/*******************************************************************************
**
** Function         btm_ble_start_adv
**
** Description      Stop the BLE advertising.
**
** Returns          void
**
*******************************************************************************/
static tBTM_STATUS btm_ble_start_adv(void)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    tBTM_STATUS     rt = BTM_NO_RESOURCES;

    if (btsnd_hcic_ble_set_adv_enable (BTM_BLE_ADV_ENABLE))
    {
        if (p_cb->afp != AP_SCAN_CONN_ALL)
             btm_cb.ble_ctr_cb.wl_state |= BTM_BLE_WL_ADV;

         p_cb->adv_mode = BTM_BLE_ADV_ENABLE;

         rt = BTM_SUCCESS;
     }
     else
     {
         p_cb->adv_mode = BTM_BLE_ADV_DISABLE;
         btm_cb.ble_ctr_cb.wl_state &= ~BTM_BLE_WL_ADV;
     }
     return rt;
}
/*******************************************************************************
**
** Function         btm_ble_stop_adv
**
** Description      Stop the BLE advertising.
**
** Returns          void
**
*******************************************************************************/
static tBTM_STATUS btm_ble_stop_adv(void)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;
    tBTM_STATUS rt = BTM_SUCCESS;

    if (p_cb->adv_mode == BTM_BLE_ADV_ENABLE)
    {
        if (btsnd_hcic_ble_set_adv_enable (BTM_BLE_ADV_DISABLE))
        {
            p_cb->adv_mode = BTM_BLE_ADV_DISABLE;
            btm_cb.ble_ctr_cb.wl_state &= ~BTM_BLE_WL_ADV;
        }
        else
            rt = BTM_NO_RESOURCES;
    }
    return rt;

}

/*******************************************************************************
**
** Function         btm_ble_timeout
**
** Description      Called when BTM BLE inquiry timer expires
**
** Returns          void
**
*******************************************************************************/
void btm_ble_timeout(TIMER_LIST_ENT *p_tle)
{
    switch (p_tle->event)
    {
        case BTU_TTYPE_BLE_INQUIRY:
            btm_ble_stop_scan();
            break;

        case BTU_TTYPE_BLE_GAP_LIM_DISC:
            /* lim_timeout expiried, limited discovery should exit now */
            btm_ble_update_adv_flag(BTM_BLE_NON_LIMIT_DISC_FLAG);

            btm_ble_stop_adv();
            break;

        case BTU_TTYPE_BLE_RANDOM_ADDR:
            if (btm_cb.ble_ctr_cb.addr_mgnt_cb.own_addr_type == BLE_ADDR_RANDOM)
            {
                /* refresh the random addr */
                btm_gen_resolvable_private_addr();
            }
            break;

    }
}


/*******************************************************************************
**
** Function         btm_ble_read_remote_features_complete
**
** Description      This function is called when the command complete message
**                  is received from the HCI for the read LE remote feature supported
**                  complete event.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_read_remote_features_complete(UINT8 *p)
{
    tACL_CONN        *p_acl_cb = &btm_cb.acl_db[0];
    UINT8             status;
    UINT16            handle;
    int               xx;

    BTM_TRACE_EVENT0 ("btm_ble_read_remote_features_complete ");

    STREAM_TO_UINT8  (status, p);
    STREAM_TO_UINT16 (handle, p);

    /* Look up the connection by handle and copy features */
    for (xx = 0; xx < MAX_L2CAP_LINKS; xx++, p_acl_cb++)
    {
        if ((p_acl_cb->in_use) && (p_acl_cb->hci_handle == handle))
        {
            STREAM_TO_ARRAY(p_acl_cb->peer_le_features, p, BD_FEATURES_LEN);
            break;
        }
    }
}

/*******************************************************************************
**
** Function         btm_ble_write_adv_enable_complete
**
** Description      This function process the write adv enable command complete.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_write_adv_enable_complete(UINT8 * p)
{
    tBTM_BLE_INQ_CB *p_cb = &btm_cb.ble_ctr_cb.inq_var;

    /* if write adv enable/disbale not succeed */
    if (*p != HCI_SUCCESS)
    {
        /* toggle back the adv mode */
        p_cb->adv_mode = !p_cb->adv_mode;
    }


}

/*******************************************************************************
**
** Function         btm_ble_dir_adv_tout
**
** Description      when directed adv time out
**
** Returns          void
**
*******************************************************************************/
void btm_ble_dir_adv_tout(void)
{
    btm_cb.ble_ctr_cb.inq_var.adv_mode = BTM_BLE_ADV_DISABLE;

    /* make device fall back into undirected adv mode by default */
    btm_cb.ble_ctr_cb.inq_var.directed_conn = FALSE;
}

/*******************************************************************************
**
** Function         btm_ble_update_mode_operation
**
** Description      This function update the GAP role operation when a link status
**                  is updated.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_update_mode_operation(UINT8 link_role, BD_ADDR bd_addr, BOOLEAN conn_cancel)
{
    tACL_CONN   *pa = &btm_cb.acl_db[0];
    UINT16       xx;
    UINT8        dev_role = link_role;

    BTM_TRACE_DEBUG1("btm_ble_update_mode_operation adv_mode = %d", btm_cb.ble_ctr_cb.inq_var.adv_mode );

    /* update periphera role operation */
    /* If we are LE connectable, check if we need to start advertising again */
    if (link_role == HCI_ROLE_UNKNOWN)
        /* && btm_cb.ble_ctr_cb.inq_var.connectable_mode != BTM_BLE_NON_CONNECTABLE) */
    {
        for (xx = 0; xx < MAX_L2CAP_LINKS; xx++, pa++)
        {
            /* If any other LE link is up, we are still not connectable */
            if (pa->in_use && pa->is_le_link)
            {
                dev_role = pa->link_role;
                break;
            }
        }
    }

    if (btm_cb.ble_ctr_cb.inq_var.connectable_mode == BTM_BLE_CONNECTABLE  &&
        (dev_role == HCI_ROLE_UNKNOWN )) /* when device has no connection, update adv here */
        /* if already in connection, no connectable adv is allowed unless scatternet is enabled */
    {
        btm_ble_set_connectability ( btm_cb.ble_ctr_cb.inq_var.connectable_mode );
    }

    if (btm_ble_get_conn_st() == BLE_CONN_IDLE)
    {
        if (!btm_send_pending_direct_conn())
        {
            btm_ble_resume_bg_conn();
        }
    }
}

/*******************************************************************************
**
** Function         btm_ble_init
**
** Description      Initialize the control block variable values.
**
** Returns          void
**
*******************************************************************************/
void btm_ble_init (void)
{
    tBTM_BLE_CB *p_cb = &btm_cb.ble_ctr_cb;

    BTM_TRACE_EVENT0 ("btm_ble_init ");

    memset(p_cb, 0, sizeof(tBTM_BLE_CB));

    p_cb->inq_var.adv_mode = BTM_BLE_ADV_DISABLE;
    p_cb->inq_var.scan_type = BTM_BLE_SCAN_MODE_NONE;
    p_cb->inq_var.adv_chnl_map = BTM_BLE_DEFAULT_ADV_CHNL_MAP;
    p_cb->inq_var.afp = BTM_BLE_DEFAULT_AFP;
    p_cb->inq_var.sfp = BTM_BLE_DEFAULT_SFP;
    p_cb->inq_var.connectable_mode = BTM_BLE_NON_CONNECTABLE;
    p_cb->inq_var.discoverable_mode = BTM_BLE_NON_DISCOVERABLE;

    /* for background connection, reset connection params to be undefined */
    p_cb->scan_int = p_cb->scan_win = BTM_BLE_CONN_PARAM_UNDEF;

    p_cb->inq_var.evt_type = BTM_BLE_NON_CONNECT_EVT;
}

#endif  /* BLE_INCLUDED */
