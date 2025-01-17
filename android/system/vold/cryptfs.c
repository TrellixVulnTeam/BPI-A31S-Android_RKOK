/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* TO DO:
 *   1.  Perhaps keep several copies of the encrypted key, in case something
 *       goes horribly wrong?
 *
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/param.h>
#include <string.h>
#include <sys/mount.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <errno.h>
#include <ext4.h>
#include <linux/kdev_t.h>
#include <fs_mgr.h>
#include "cryptfs.h"
#define LOG_TAG "Cryptfs"
#include "cutils/log.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "hardware_legacy/power.h"
#include <logwrap/logwrap.h>
#include "VolumeManager.h"
#include "VoldUtil.h"
#include "crypto_scrypt.h"

#define DM_CRYPT_BUF_SIZE 4096
#define DATA_MNT_POINT "/data"

#define HASH_COUNT 2000
#define KEY_LEN_BYTES 16
#define IV_LEN_BYTES 16

#define KEY_IN_FOOTER  "footer"

#define EXT4_FS 1
#define FAT_FS 2

#define TABLE_LOAD_RETRIES 10

char *me = "cryptfs";

static unsigned char saved_master_key[KEY_LEN_BYTES];
static char *saved_mount_point;
static int  master_key_saved = 0;
static struct crypt_persist_data *persist_data = NULL;

extern struct fstab *fstab;

static void cryptfs_reboot(int recovery)
{
    if (recovery) {
        property_set(ANDROID_RB_PROPERTY, "reboot,recovery");
    } else {
        property_set(ANDROID_RB_PROPERTY, "reboot");
    }
    sleep(20);

    /* Shouldn't get here, reboot should happen before sleep times out */
    return;
}

static void ioctl_init(struct dm_ioctl *io, size_t dataSize, const char *name, unsigned flags)
{
    memset(io, 0, dataSize);
    io->data_size = dataSize;
    io->data_start = sizeof(struct dm_ioctl);
    io->version[0] = 4;
    io->version[1] = 0;
    io->version[2] = 0;
    io->flags = flags;
    if (name) {
        strncpy(io->name, name, sizeof(io->name));
    }
}

/**
 * Gets the default device scrypt parameters for key derivation time tuning.
 * The parameters should lead to about one second derivation time for the
 * given device.
 */
static void get_device_scrypt_params(struct crypt_mnt_ftr *ftr) {
    const int default_params[] = SCRYPT_DEFAULTS;
    int params[] = SCRYPT_DEFAULTS;
    char paramstr[PROPERTY_VALUE_MAX];
    char *token;
    char *saveptr;
    int i;

    property_get(SCRYPT_PROP, paramstr, "");
    if (paramstr[0] != '\0') {
        /*
         * The token we're looking for should be three integers separated by
         * colons (e.g., "12:8:1"). Scan the property to make sure it matches.
         */
        for (i = 0, token = strtok_r(paramstr, ":", &saveptr);
                token != NULL && i < 3;
                i++, token = strtok_r(NULL, ":", &saveptr)) {
            char *endptr;
            params[i] = strtol(token, &endptr, 10);

            /*
             * Check that there was a valid number and it's 8-bit. If not,
             * break out and the end check will take the default values.
             */
            if ((*token == '\0') || (*endptr != '\0') || params[i] < 0 || params[i] > 255) {
                break;
            }
        }

        /*
         * If there were not enough tokens or a token was malformed (not an
         * integer), it will end up here and the default parameters can be
         * taken.
         */
        if ((i != 3) || (token != NULL)) {
            SLOGW("bad scrypt parameters '%s' should be like '12:8:1'; using defaults", paramstr);
            memcpy(params, default_params, sizeof(params));
        }
    }

    ftr->N_factor = params[0];
    ftr->r_factor = params[1];
    ftr->p_factor = params[2];
}

static unsigned int get_fs_size(char *dev)
{
    int fd, block_size;
    struct ext4_super_block sb;
    off64_t len;

    if ((fd = open(dev, O_RDONLY)) < 0) {
        SLOGE("Cannot open device to get filesystem size ");
        return 0;
    }

    if (lseek64(fd, 1024, SEEK_SET) < 0) {
        SLOGE("Cannot seek to superblock");
        return 0;
    }

    if (read(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        SLOGE("Cannot read superblock");
        return 0;
    }

    close(fd);

    block_size = 1024 << sb.s_log_block_size;
    /* compute length in bytes */
    len = ( ((off64_t)sb.s_blocks_count_hi << 32) + sb.s_blocks_count_lo) * block_size;

    /* return length in sectors */
    return (unsigned int) (len / 512);
}

static int get_crypt_ftr_info(char **metadata_fname, off64_t *off)
{
  static int cached_data = 0;
  static off64_t cached_off = 0;
  static char cached_metadata_fname[PROPERTY_VALUE_MAX] = "";
  int fd;
  char key_loc[PROPERTY_VALUE_MAX];
  char real_blkdev[PROPERTY_VALUE_MAX];
  unsigned int nr_sec;
  int rc = -1;

  if (!cached_data) {
    fs_mgr_get_crypt_info(fstab, key_loc, real_blkdev, sizeof(key_loc));

    if (!strcmp(key_loc, KEY_IN_FOOTER)) {
      if ( (fd = open(real_blkdev, O_RDWR)) < 0) {
        SLOGE("Cannot open real block device %s\n", real_blkdev);
        return -1;
      }

      if ((nr_sec = get_blkdev_size(fd))) {
        /* If it's an encrypted Android partition, the last 16 Kbytes contain the
         * encryption info footer and key, and plenty of bytes to spare for future
         * growth.
         */
        strlcpy(cached_metadata_fname, real_blkdev, sizeof(cached_metadata_fname));
        cached_off = ((off64_t)nr_sec * 512) - CRYPT_FOOTER_OFFSET;
        cached_data = 1;
      } else {
        SLOGE("Cannot get size of block device %s\n", real_blkdev);
      }
      close(fd);
    } else {
      //key is save at 32768 before SEEK_END in key_loc
      off64_t key_loc_pos_to_end=-32768;
      strlcpy(cached_metadata_fname, key_loc, sizeof(cached_metadata_fname));
      int fd_key=open(key_loc,O_RDONLY);
      if (fd_key<0)
        cached_off = 0;
      else
      {
        cached_off=lseek64(fd_key,key_loc_pos_to_end,SEEK_END);
        if (cached_off<0)
            cached_off=0;
        close(fd_key);
      }
      cached_data = 1;
    }
  }

  if (cached_data) {
    if (metadata_fname) {
        *metadata_fname = cached_metadata_fname;
    }
    if (off) {
        *off = cached_off;
    }
    rc = 0;
  }

  return rc;
}

/* key or salt can be NULL, in which case just skip writing that value.  Useful to
 * update the failed mount count but not change the key.
 */
static int put_crypt_ftr_and_key(struct crypt_mnt_ftr *crypt_ftr)
{
  int fd;
  unsigned int nr_sec, cnt;
  /* starting_off is set to the SEEK_SET offset
   * where the crypto structure starts
   */
  off64_t starting_off;
  int rc = -1;
  char *fname = NULL;
  struct stat statbuf;

  if (get_crypt_ftr_info(&fname, &starting_off)) {
    SLOGE("Unable to get crypt_ftr_info\n");
    return -1;
  }
  if (fname[0] != '/') {
    SLOGE("Unexpected value for crypto key location\n");
    return -1;
  }
  if ( (fd = open(fname, O_RDWR | O_CREAT, 0600)) < 0) {
    SLOGE("Cannot open footer file %s for put\n", fname);
    return -1;
  }

  /* Seek to the start of the crypt footer */
  if (lseek64(fd, starting_off, SEEK_SET) == -1) {
    SLOGE("Cannot seek to real block device footer\n");
    goto errout;
  }

  if ((cnt = write(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
    SLOGE("Cannot write real block device footer\n");
    goto errout;
  }

  fstat(fd, &statbuf);
  /* If the keys are kept on a raw block device, do not try to truncate it. */
  if (S_ISREG(statbuf.st_mode)) {
    if (ftruncate(fd, 0x4000)) {
      SLOGE("Cannot set footer file size\n", fname);
      goto errout;
    }
  }

  /* Success! */
  rc = 0;

errout:
  close(fd);
  return rc;

}

static inline int unix_read(int  fd, void*  buff, int  len)
{
    return TEMP_FAILURE_RETRY(read(fd, buff, len));
}

static inline int unix_write(int  fd, const void*  buff, int  len)
{
    return TEMP_FAILURE_RETRY(write(fd, buff, len));
}

static void init_empty_persist_data(struct crypt_persist_data *pdata, int len)
{
    memset(pdata, 0, len);
    pdata->persist_magic = PERSIST_DATA_MAGIC;
    pdata->persist_valid_entries = 0;
}

/* A routine to update the passed in crypt_ftr to the lastest version.
 * fd is open read/write on the device that holds the crypto footer and persistent
 * data, crypt_ftr is a pointer to the struct to be updated, and offset is the
 * absolute offset to the start of the crypt_mnt_ftr on the passed in fd.
 */
static void upgrade_crypt_ftr(int fd, struct crypt_mnt_ftr *crypt_ftr, off64_t offset)
{
    int orig_major = crypt_ftr->major_version;
    int orig_minor = crypt_ftr->minor_version;

    if ((crypt_ftr->major_version == 1) && (crypt_ftr->minor_version == 0)) {
        struct crypt_persist_data *pdata;
        off64_t pdata_offset = offset + CRYPT_FOOTER_TO_PERSIST_OFFSET;

        SLOGW("upgrading crypto footer to 1.1");

        pdata = malloc(CRYPT_PERSIST_DATA_SIZE);
        if (pdata == NULL) {
            SLOGE("Cannot allocate persisent data\n");
            return;
        }
        memset(pdata, 0, CRYPT_PERSIST_DATA_SIZE);

        /* Need to initialize the persistent data area */
        if (lseek64(fd, pdata_offset, SEEK_SET) == -1) {
            SLOGE("Cannot seek to persisent data offset\n");
            return;
        }
        /* Write all zeros to the first copy, making it invalid */
        unix_write(fd, pdata, CRYPT_PERSIST_DATA_SIZE);

        /* Write a valid but empty structure to the second copy */
        init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
        unix_write(fd, pdata, CRYPT_PERSIST_DATA_SIZE);

        /* Update the footer */
        crypt_ftr->persist_data_size = CRYPT_PERSIST_DATA_SIZE;
        crypt_ftr->persist_data_offset[0] = pdata_offset;
        crypt_ftr->persist_data_offset[1] = pdata_offset + CRYPT_PERSIST_DATA_SIZE;
        crypt_ftr->minor_version = 1;
    }

    if ((crypt_ftr->major_version == 1) && (crypt_ftr->minor_version)) {
        SLOGW("upgrading crypto footer to 1.2");
        /* But keep the old kdf_type.
         * It will get updated later to KDF_SCRYPT after the password has been verified.
         */
        crypt_ftr->kdf_type = KDF_PBKDF2;
        get_device_scrypt_params(crypt_ftr);
        crypt_ftr->minor_version = 2;
    }

    if ((orig_major != crypt_ftr->major_version) || (orig_minor != crypt_ftr->minor_version)) {
        if (lseek64(fd, offset, SEEK_SET) == -1) {
            SLOGE("Cannot seek to crypt footer\n");
            return;
        }
        unix_write(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr));
    }
}


static int get_crypt_ftr_and_key(struct crypt_mnt_ftr *crypt_ftr)
{
  int fd;
  unsigned int nr_sec, cnt;
  off64_t starting_off;
  int rc = -1;
  char *fname = NULL;
  struct stat statbuf;

  if (get_crypt_ftr_info(&fname, &starting_off)) {
    SLOGE("Unable to get crypt_ftr_info\n");
    return -1;
  }
  if (fname[0] != '/') {
    SLOGE("Unexpected value for crypto key location\n");
    return -1;
  }
  if ( (fd = open(fname, O_RDWR)) < 0) {
    SLOGE("Cannot open footer file %s for get\n", fname);
    return -1;
  }

  /* Make sure it's 16 Kbytes in length */
  fstat(fd, &statbuf);
  if (S_ISREG(statbuf.st_mode) && (statbuf.st_size != 0x4000)) {
    SLOGE("footer file %s is not the expected size!\n", fname);
    goto errout;
  }

  /* Seek to the start of the crypt footer */
  if (lseek64(fd, starting_off, SEEK_SET) == -1) {
    SLOGE("Cannot seek to real block device footer\n");
    goto errout;
  }

  if ( (cnt = read(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
    SLOGE("Cannot read real block device footer\n");
    goto errout;
  }

  if (crypt_ftr->magic != CRYPT_MNT_MAGIC) {
    SLOGE("Bad magic for real block device %s\n", fname);
    goto errout;
  }

  if (crypt_ftr->major_version != CURRENT_MAJOR_VERSION) {
    SLOGE("Cannot understand major version %d real block device footer; expected %d\n",
          crypt_ftr->major_version, CURRENT_MAJOR_VERSION);
    goto errout;
  }

  if (crypt_ftr->minor_version > CURRENT_MINOR_VERSION) {
    SLOGW("Warning: crypto footer minor version %d, expected <= %d, continuing...\n",
          crypt_ftr->minor_version, CURRENT_MINOR_VERSION);
  }

  /* If this is a verion 1.0 crypt_ftr, make it a 1.1 crypt footer, and update the
   * copy on disk before returning.
   */
  if (crypt_ftr->minor_version < CURRENT_MINOR_VERSION) {
    upgrade_crypt_ftr(fd, crypt_ftr, starting_off);
  }

  /* Success! */
  rc = 0;

errout:
  close(fd);
  return rc;
}

static int validate_persistent_data_storage(struct crypt_mnt_ftr *crypt_ftr)
{
    if (crypt_ftr->persist_data_offset[0] + crypt_ftr->persist_data_size >
        crypt_ftr->persist_data_offset[1]) {
        SLOGE("Crypt_ftr persist data regions overlap");
        return -1;
    }

    if (crypt_ftr->persist_data_offset[0] >= crypt_ftr->persist_data_offset[1]) {
        SLOGE("Crypt_ftr persist data region 0 starts after region 1");
        return -1;
    }

    if (((crypt_ftr->persist_data_offset[1] + crypt_ftr->persist_data_size) -
        (crypt_ftr->persist_data_offset[0] - CRYPT_FOOTER_TO_PERSIST_OFFSET)) >
        CRYPT_FOOTER_OFFSET) {
        SLOGE("Persistent data extends past crypto footer");
        return -1;
    }

    return 0;
}

static int load_persistent_data(void)
{
    struct crypt_mnt_ftr crypt_ftr;
    struct crypt_persist_data *pdata = NULL;
    char encrypted_state[PROPERTY_VALUE_MAX];
    char *fname;
    int found = 0;
    int fd;
    int ret;
    int i;

    if (persist_data) {
        /* Nothing to do, we've already loaded or initialized it */
        return 0;
    }


    /* If not encrypted, just allocate an empty table and initialize it */
    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted") ) {
        pdata = malloc(CRYPT_PERSIST_DATA_SIZE);
        if (pdata) {
            init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
            persist_data = pdata;
            return 0;
        }
        return -1;
    }

    if(get_crypt_ftr_and_key(&crypt_ftr)) {
        return -1;
    }

    if ((crypt_ftr.major_version != 1) || (crypt_ftr.minor_version != 1)) {
        SLOGE("Crypt_ftr version doesn't support persistent data");
        return -1;
    }

    if (get_crypt_ftr_info(&fname, NULL)) {
        return -1;
    }

    ret = validate_persistent_data_storage(&crypt_ftr);
    if (ret) {
        return -1;
    }

    fd = open(fname, O_RDONLY);
    if (fd < 0) {
        SLOGE("Cannot open %s metadata file", fname);
        return -1;
    }

    if (persist_data == NULL) {
        pdata = malloc(crypt_ftr.persist_data_size);
        if (pdata == NULL) {
            SLOGE("Cannot allocate memory for persistent data");
            goto err;
        }
    }

    for (i = 0; i < 2; i++) {
        if (lseek64(fd, crypt_ftr.persist_data_offset[i], SEEK_SET) < 0) {
            SLOGE("Cannot seek to read persistent data on %s", fname);
            goto err2;
        }
        if (unix_read(fd, pdata, crypt_ftr.persist_data_size) < 0){
            SLOGE("Error reading persistent data on iteration %d", i);
            goto err2;
        }
        if (pdata->persist_magic == PERSIST_DATA_MAGIC) {
            found = 1;
            break;
        }
    }

    if (!found) {
        SLOGI("Could not find valid persistent data, creating");
        init_empty_persist_data(pdata, crypt_ftr.persist_data_size);
    }

    /* Success */
    persist_data = pdata;
    close(fd);
    return 0;

err2:
    free(pdata);

err:
    close(fd);
    return -1;
}

static int save_persistent_data(void)
{
    struct crypt_mnt_ftr crypt_ftr;
    struct crypt_persist_data *pdata;
    char *fname;
    off64_t write_offset;
    off64_t erase_offset;
    int found = 0;
    int fd;
    int ret;

    if (persist_data == NULL) {
        SLOGE("No persistent data to save");
        return -1;
    }

    if(get_crypt_ftr_and_key(&crypt_ftr)) {
        return -1;
    }

    if ((crypt_ftr.major_version != 1) || (crypt_ftr.minor_version != 1)) {
        SLOGE("Crypt_ftr version doesn't support persistent data");
        return -1;
    }

    ret = validate_persistent_data_storage(&crypt_ftr);
    if (ret) {
        return -1;
    }

    if (get_crypt_ftr_info(&fname, NULL)) {
        return -1;
    }

    fd = open(fname, O_RDWR);
    if (fd < 0) {
        SLOGE("Cannot open %s metadata file", fname);
        return -1;
    }

    pdata = malloc(crypt_ftr.persist_data_size);
    if (pdata == NULL) {
        SLOGE("Cannot allocate persistant data");
        goto err;
    }

    if (lseek64(fd, crypt_ftr.persist_data_offset[0], SEEK_SET) < 0) {
        SLOGE("Cannot seek to read persistent data on %s", fname);
        goto err2;
    }

    if (unix_read(fd, pdata, crypt_ftr.persist_data_size) < 0) {
            SLOGE("Error reading persistent data before save");
            goto err2;
    }

    if (pdata->persist_magic == PERSIST_DATA_MAGIC) {
        /* The first copy is the curent valid copy, so write to
         * the second copy and erase this one */
       write_offset = crypt_ftr.persist_data_offset[1];
       erase_offset = crypt_ftr.persist_data_offset[0];
    } else {
        /* The second copy must be the valid copy, so write to
         * the first copy, and erase the second */
       write_offset = crypt_ftr.persist_data_offset[0];
       erase_offset = crypt_ftr.persist_data_offset[1];
    }

    /* Write the new copy first, if successful, then erase the old copy */
    if (lseek(fd, write_offset, SEEK_SET) < 0) {
        SLOGE("Cannot seek to write persistent data");
        goto err2;
    }
    if (unix_write(fd, persist_data, crypt_ftr.persist_data_size) ==
        (int) crypt_ftr.persist_data_size) {
        if (lseek(fd, erase_offset, SEEK_SET) < 0) {
            SLOGE("Cannot seek to erase previous persistent data");
            goto err2;
        }
        fsync(fd);
        memset(pdata, 0, crypt_ftr.persist_data_size);
        if (unix_write(fd, pdata, crypt_ftr.persist_data_size) !=
            (int) crypt_ftr.persist_data_size) {
            SLOGE("Cannot write to erase previous persistent data");
            goto err2;
        }
        fsync(fd);
    } else {
        SLOGE("Cannot write to save persistent data");
        goto err2;
    }

    /* Success */
    free(pdata);
    close(fd);
    return 0;

err2:
    free(pdata);
err:
    close(fd);
    return -1;
}

/* Convert a binary key of specified length into an ascii hex string equivalent,
 * without the leading 0x and with null termination
 */
void convert_key_to_hex_ascii(unsigned char *master_key, unsigned int keysize,
                              char *master_key_ascii)
{
  unsigned int i, a;
  unsigned char nibble;

  for (i=0, a=0; i<keysize; i++, a+=2) {
    /* For each byte, write out two ascii hex digits */
    nibble = (master_key[i] >> 4) & 0xf;
    master_key_ascii[a] = nibble + (nibble > 9 ? 0x37 : 0x30);

    nibble = master_key[i] & 0xf;
    master_key_ascii[a+1] = nibble + (nibble > 9 ? 0x37 : 0x30);
  }

  /* Add the null termination */
  master_key_ascii[a] = '\0';

}

static int load_crypto_mapping_table(struct crypt_mnt_ftr *crypt_ftr, unsigned char *master_key,
                                     char *real_blk_name, const char *name, int fd,
                                     char *extra_params)
{
  char buffer[DM_CRYPT_BUF_SIZE];
  struct dm_ioctl *io;
  struct dm_target_spec *tgt;
  char *crypt_params;
  char master_key_ascii[129]; /* Large enough to hold 512 bit key and null */
  int i;

  io = (struct dm_ioctl *) buffer;

  /* Load the mapping table for this device */
  tgt = (struct dm_target_spec *) &buffer[sizeof(struct dm_ioctl)];

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  io->target_count = 1;
  tgt->status = 0;
  tgt->sector_start = 0;
  tgt->length = crypt_ftr->fs_size;
  strcpy(tgt->target_type, "crypt");

  crypt_params = buffer + sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec);
  convert_key_to_hex_ascii(master_key, crypt_ftr->keysize, master_key_ascii);
  sprintf(crypt_params, "%s %s 0 %s 0 %s", crypt_ftr->crypto_type_name,
          master_key_ascii, real_blk_name, extra_params);
  crypt_params += strlen(crypt_params) + 1;
  crypt_params = (char *) (((unsigned long)crypt_params + 7) & ~8); /* Align to an 8 byte boundary */
  tgt->next = crypt_params - buffer;

  for (i = 0; i < TABLE_LOAD_RETRIES; i++) {
    if (! ioctl(fd, DM_TABLE_LOAD, io)) {
      break;
    }
    usleep(500000);
  }

  if (i == TABLE_LOAD_RETRIES) {
    /* We failed to load the table, return an error */
    return -1;
  } else {
    return i + 1;
  }
}


static int get_dm_crypt_version(int fd, const char *name,  int *version)
{
    char buffer[DM_CRYPT_BUF_SIZE];
    struct dm_ioctl *io;
    struct dm_target_versions *v;
    int i;

    io = (struct dm_ioctl *) buffer;

    ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);

    if (ioctl(fd, DM_LIST_VERSIONS, io)) {
        return -1;
    }

    /* Iterate over the returned versions, looking for name of "crypt".
     * When found, get and return the version.
     */
    v = (struct dm_target_versions *) &buffer[sizeof(struct dm_ioctl)];
    while (v->next) {
        if (! strcmp(v->name, "crypt")) {
            /* We found the crypt driver, return the version, and get out */
            version[0] = v->version[0];
            version[1] = v->version[1];
            version[2] = v->version[2];
            return 0;
        }
        v = (struct dm_target_versions *)(((char *)v) + v->next);
    }

    return -1;
}

static int create_crypto_blk_dev(struct crypt_mnt_ftr *crypt_ftr, unsigned char *master_key,
                                    char *real_blk_name, char *crypto_blk_name, const char *name)
{
  char buffer[DM_CRYPT_BUF_SIZE];
  char master_key_ascii[129]; /* Large enough to hold 512 bit key and null */
  char *crypt_params;
  struct dm_ioctl *io;
  struct dm_target_spec *tgt;
  unsigned int minor;
  int fd;
  int i;
  int retval = -1;
  int version[3];
  char *extra_params;
  int load_count;

  if ((fd = open("/dev/device-mapper", O_RDWR)) < 0 ) {
    SLOGE("Cannot open device-mapper\n");
    goto errout;
  }

  io = (struct dm_ioctl *) buffer;

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  if (ioctl(fd, DM_DEV_CREATE, io)) {
    SLOGE("Cannot create dm-crypt device\n");
    goto errout;
  }

  /* Get the device status, in particular, the name of it's device file */
  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  if (ioctl(fd, DM_DEV_STATUS, io)) {
    SLOGE("Cannot retrieve dm-crypt device status\n");
    goto errout;
  }
  minor = (io->dev & 0xff) | ((io->dev >> 12) & 0xfff00);
  snprintf(crypto_blk_name, MAXPATHLEN, "/dev/block/dm-%u", minor);

  extra_params = "";
  if (! get_dm_crypt_version(fd, name, version)) {
      /* Support for allow_discards was added in version 1.11.0 */
      if ((version[0] >= 2) ||
          ((version[0] == 1) && (version[1] >= 11))) {
          extra_params = "1 allow_discards";
          SLOGI("Enabling support for allow_discards in dmcrypt.\n");
      }
  }

  load_count = load_crypto_mapping_table(crypt_ftr, master_key, real_blk_name, name,
                                         fd, extra_params);
  if (load_count < 0) {
      SLOGE("Cannot load dm-crypt mapping table.\n");
      goto errout;
  } else if (load_count > 1) {
      SLOGI("Took %d tries to load dmcrypt table.\n", load_count);
  }

  /* Resume this device to activate it */
  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);

  if (ioctl(fd, DM_DEV_SUSPEND, io)) {
    SLOGE("Cannot resume the dm-crypt device\n");
    goto errout;
  }

  /* We made it here with no errors.  Woot! */
  retval = 0;

errout:
  close(fd);   /* If fd is <0 from a failed open call, it's safe to just ignore the close error */

  return retval;
}

static int delete_crypto_blk_dev(char *name)
{
  int fd;
  char buffer[DM_CRYPT_BUF_SIZE];
  struct dm_ioctl *io;
  int retval = -1;

  if ((fd = open("/dev/device-mapper", O_RDWR)) < 0 ) {
    SLOGE("Cannot open device-mapper\n");
    goto errout;
  }

  io = (struct dm_ioctl *) buffer;

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  if (ioctl(fd, DM_DEV_REMOVE, io)) {
    SLOGE("Cannot remove dm-crypt device\n");
    goto errout;
  }

  /* We made it here with no errors.  Woot! */
  retval = 0;

errout:
  close(fd);    /* If fd is <0 from a failed open call, it's safe to just ignore the close error */

  return retval;

}

static void pbkdf2(char *passwd, unsigned char *salt, unsigned char *ikey, void *params) {
    /* Turn the password into a key and IV that can decrypt the master key */
    PKCS5_PBKDF2_HMAC_SHA1(passwd, strlen(passwd), salt, SALT_LEN,
                           HASH_COUNT, KEY_LEN_BYTES+IV_LEN_BYTES, ikey);
}

static void scrypt(char *passwd, unsigned char *salt, unsigned char *ikey, void *params) {
    struct crypt_mnt_ftr *ftr = (struct crypt_mnt_ftr *) params;

    int N = 1 << ftr->N_factor;
    int r = 1 << ftr->r_factor;
    int p = 1 << ftr->p_factor;

    /* Turn the password into a key and IV that can decrypt the master key */
    crypto_scrypt((unsigned char *) passwd, strlen(passwd), salt, SALT_LEN, N, r, p, ikey,
            KEY_LEN_BYTES + IV_LEN_BYTES);
}

static int encrypt_master_key(char *passwd, unsigned char *salt,
                              unsigned char *decrypted_master_key,
                              unsigned char *encrypted_master_key,
                              struct crypt_mnt_ftr *crypt_ftr)
{
    unsigned char ikey[32+32] = { 0 }; /* Big enough to hold a 256 bit key and 256 bit IV */
    EVP_CIPHER_CTX e_ctx;
    int encrypted_len, final_len;

    /* Turn the password into a key and IV that can decrypt the master key */
    get_device_scrypt_params(crypt_ftr);
    scrypt(passwd, salt, ikey, crypt_ftr);

    /* Initialize the decryption engine */
    if (! EVP_EncryptInit(&e_ctx, EVP_aes_128_cbc(), ikey, ikey+KEY_LEN_BYTES)) {
        SLOGE("EVP_EncryptInit failed\n");
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(&e_ctx, 0); /* Turn off padding as our data is block aligned */

    /* Encrypt the master key */
    if (! EVP_EncryptUpdate(&e_ctx, encrypted_master_key, &encrypted_len,
                              decrypted_master_key, KEY_LEN_BYTES)) {
        SLOGE("EVP_EncryptUpdate failed\n");
        return -1;
    }
    if (! EVP_EncryptFinal(&e_ctx, encrypted_master_key + encrypted_len, &final_len)) {
        SLOGE("EVP_EncryptFinal failed\n");
        return -1;
    }

    if (encrypted_len + final_len != KEY_LEN_BYTES) {
        SLOGE("EVP_Encryption length check failed with %d, %d bytes\n", encrypted_len, final_len);
        return -1;
    } else {
        return 0;
    }
}

static int decrypt_master_key_aux(char *passwd, unsigned char *salt,
                              unsigned char *encrypted_master_key,
                              unsigned char *decrypted_master_key,
                              kdf_func kdf, void *kdf_params)
{
  unsigned char ikey[32+32] = { 0 }; /* Big enough to hold a 256 bit key and 256 bit IV */
  EVP_CIPHER_CTX d_ctx;
  int decrypted_len, final_len;

  /* Turn the password into a key and IV that can decrypt the master key */
  kdf(passwd, salt, ikey, kdf_params);

  /* Initialize the decryption engine */
  if (! EVP_DecryptInit(&d_ctx, EVP_aes_128_cbc(), ikey, ikey+KEY_LEN_BYTES)) {
    return -1;
  }
  EVP_CIPHER_CTX_set_padding(&d_ctx, 0); /* Turn off padding as our data is block aligned */
  /* Decrypt the master key */
  if (! EVP_DecryptUpdate(&d_ctx, decrypted_master_key, &decrypted_len,
                            encrypted_master_key, KEY_LEN_BYTES)) {
    return -1;
  }
  if (! EVP_DecryptFinal(&d_ctx, decrypted_master_key + decrypted_len, &final_len)) {
    return -1;
  }

  if (decrypted_len + final_len != KEY_LEN_BYTES) {
    return -1;
  } else {
    return 0;
  }
}

static void get_kdf_func(struct crypt_mnt_ftr *ftr, kdf_func *kdf, void** kdf_params)
{
    if (ftr->kdf_type == KDF_SCRYPT) {
        *kdf = scrypt;
        *kdf_params = ftr;
    } else {
        *kdf = pbkdf2;
        *kdf_params = NULL;
    }
}

static int decrypt_master_key(char *passwd, unsigned char *decrypted_master_key,
        struct crypt_mnt_ftr *crypt_ftr)
{
    kdf_func kdf;
    void *kdf_params;
    int ret;

    get_kdf_func(crypt_ftr, &kdf, &kdf_params);
    ret = decrypt_master_key_aux(passwd, crypt_ftr->salt, crypt_ftr->master_key, decrypted_master_key, kdf,
            kdf_params);
    if (ret != 0) {
        SLOGW("failure decrypting master key");
    }

    return ret;
}

static int create_encrypted_random_key(char *passwd, unsigned char *master_key, unsigned char *salt,
        struct crypt_mnt_ftr *crypt_ftr) {
    int fd;
    unsigned char key_buf[KEY_LEN_BYTES];
    EVP_CIPHER_CTX e_ctx;
    int encrypted_len, final_len;

    /* Get some random bits for a key */
    fd = open("/dev/urandom", O_RDONLY);
    read(fd, key_buf, sizeof(key_buf));
    read(fd, salt, SALT_LEN);
    close(fd);

    /* Now encrypt it with the password */
    return encrypt_master_key(passwd, salt, key_buf, master_key, crypt_ftr);
}

static int wait_and_unmount(char *mountpoint)
{
    int i, rc;
#define WAIT_UNMOUNT_COUNT 20

    /*  Now umount the tmpfs filesystem */
    for (i=0; i<WAIT_UNMOUNT_COUNT; i++) {
        if (umount(mountpoint)) {
            if (errno == EINVAL) {
                /* EINVAL is returned if the directory is not a mountpoint,
                 * i.e. there is no filesystem mounted there.  So just get out.
                 */
                break;
            }
            sleep(1);
            i++;
        } else {
          break;
        }
    }

    if (i < WAIT_UNMOUNT_COUNT) {
      SLOGD("unmounting %s succeeded\n", mountpoint);
      rc = 0;
    } else {
      SLOGE("unmounting %s failed\n", mountpoint);
      rc = -1;
    }

    return rc;
}

#define DATA_PREP_TIMEOUT 200
static int prep_data_fs(void)
{
    int i;

    /* Do the prep of the /data filesystem */
    property_set("vold.post_fs_data_done", "0");
    property_set("vold.decrypt", "trigger_post_fs_data");
    SLOGD("Just triggered post_fs_data\n");

    /* Wait a max of 50 seconds, hopefully it takes much less */
    for (i=0; i<DATA_PREP_TIMEOUT; i++) {
        char p[PROPERTY_VALUE_MAX];

        property_get("vold.post_fs_data_done", p, "0");
        if (*p == '1') {
            break;
        } else {
            usleep(250000);
        }
    }
    if (i == DATA_PREP_TIMEOUT) {
        /* Ugh, we failed to prep /data in time.  Bail. */
        SLOGE("post_fs_data timed out!\n");
        return -1;
    } else {
        SLOGD("post_fs_data done\n");
        return 0;
    }
}

int cryptfs_restart(void)
{
    char fs_type[32];
    char real_blkdev[MAXPATHLEN];
    char crypto_blkdev[MAXPATHLEN];
    char fs_options[256];
    unsigned long mnt_flags;
    struct stat statbuf;
    int rc = -1, i;
    static int restart_successful = 0;

    /* Validate that it's OK to call this routine */
    if (! master_key_saved) {
        SLOGE("Encrypted filesystem not validated, aborting");
        return -1;
    }

    if (restart_successful) {
        SLOGE("System already restarted with encrypted disk, aborting");
        return -1;
    }

    /* Here is where we shut down the framework.  The init scripts
     * start all services in one of three classes: core, main or late_start.
     * On boot, we start core and main.  Now, we stop main, but not core,
     * as core includes vold and a few other really important things that
     * we need to keep running.  Once main has stopped, we should be able
     * to umount the tmpfs /data, then mount the encrypted /data.
     * We then restart the class main, and also the class late_start.
     * At the moment, I've only put a few things in late_start that I know
     * are not needed to bring up the framework, and that also cause problems
     * with unmounting the tmpfs /data, but I hope to add add more services
     * to the late_start class as we optimize this to decrease the delay
     * till the user is asked for the password to the filesystem.
     */

    /* The init files are setup to stop the class main when vold.decrypt is
     * set to trigger_reset_main.
     */
    property_set("vold.decrypt", "trigger_reset_main");
    SLOGD("Just asked init to shut down class main\n");

    /* Ugh, shutting down the framework is not synchronous, so until it
     * can be fixed, this horrible hack will wait a moment for it all to
     * shut down before proceeding.  Without it, some devices cannot
     * restart the graphics services.
     */
    sleep(2);

    /* Now that the framework is shutdown, we should be able to umount()
     * the tmpfs filesystem, and mount the real one.
     */

    property_get("ro.crypto.fs_crypto_blkdev", crypto_blkdev, "");
    if (strlen(crypto_blkdev) == 0) {
        SLOGE("fs_crypto_blkdev not set\n");
        return -1;
    }

    if (! (rc = wait_and_unmount(DATA_MNT_POINT)) ) {
        /* If that succeeded, then mount the decrypted filesystem */
        fs_mgr_do_mount(fstab, DATA_MNT_POINT, crypto_blkdev, 0);

        property_set("vold.decrypt", "trigger_load_persist_props");
        /* Create necessary paths on /data */
        if (prep_data_fs()) {
            return -1;
        }

        /* startup service classes main and late_start */
        property_set("vold.decrypt", "trigger_restart_framework");
        SLOGD("Just triggered restart_framework\n");

        /* Give it a few moments to get started */
        sleep(1);
    }

    if (rc == 0) {
        restart_successful = 1;
    }

    return rc;
}

static int do_crypto_complete(char *mount_point)
{
  struct crypt_mnt_ftr crypt_ftr;
  char encrypted_state[PROPERTY_VALUE_MAX];
  char key_loc[PROPERTY_VALUE_MAX];

  property_get("ro.crypto.state", encrypted_state, "");
  if (strcmp(encrypted_state, "encrypted") ) {
    SLOGE("not running with encryption, aborting");
    return 1;
  }

  if (get_crypt_ftr_and_key(&crypt_ftr)) {
    fs_mgr_get_crypt_info(fstab, key_loc, 0, sizeof(key_loc));

    /*
     * Only report this error if key_loc is a file and it exists.
     * If the device was never encrypted, and /data is not mountable for
     * some reason, returning 1 should prevent the UI from presenting the
     * a "enter password" screen, or worse, a "press button to wipe the
     * device" screen.
     */
    if ((key_loc[0] == '/') && (access("key_loc", F_OK) == -1)) {
      SLOGE("master key file does not exist, aborting");
      return 1;
    } else {
      SLOGE("Error getting crypt footer and key\n");
      return -1;
    }
  }

  if (crypt_ftr.flags & CRYPT_ENCRYPTION_IN_PROGRESS) {
    SLOGE("Encryption process didn't finish successfully\n");
    return -2;  /* -2 is the clue to the UI that there is no usable data on the disk,
                 * and give the user an option to wipe the disk */
  }

  /* We passed the test! We shall diminish, and return to the west */
  return 0;
}

static int test_mount_encrypted_fs(char *passwd, char *mount_point, char *label)
{
  struct crypt_mnt_ftr crypt_ftr;
  /* Allocate enough space for a 256 bit key, but we may use less */
  unsigned char decrypted_master_key[32];
  char crypto_blkdev[MAXPATHLEN];
  char real_blkdev[MAXPATHLEN];
  char tmp_mount_point[64];
  unsigned int orig_failed_decrypt_count;
  char encrypted_state[PROPERTY_VALUE_MAX];
  int rc;
  kdf_func kdf;
  void *kdf_params;

  property_get("ro.crypto.state", encrypted_state, "");
  if ( master_key_saved || strcmp(encrypted_state, "encrypted") ) {
    SLOGE("encrypted fs already validated or not running with encryption, aborting");
    return -1;
  }

  fs_mgr_get_crypt_info(fstab, 0, real_blkdev, sizeof(real_blkdev));

  if (get_crypt_ftr_and_key(&crypt_ftr)) {
    SLOGE("Error getting crypt footer and key\n");
    return -1;
  }

  SLOGD("crypt_ftr->fs_size = %lld\n", crypt_ftr.fs_size);
  orig_failed_decrypt_count = crypt_ftr.failed_decrypt_count;

  if (! (crypt_ftr.flags & CRYPT_MNT_KEY_UNENCRYPTED) ) {
    if (decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr)) {
      SLOGE("Failed to decrypt master key\n");
      return -1;
    }
  }

  if (create_crypto_blk_dev(&crypt_ftr, decrypted_master_key,
                               real_blkdev, crypto_blkdev, label)) {
    SLOGE("Error creating decrypted block device\n");
    return -1;
  }

  /* If init detects an encrypted filesystem, it writes a file for each such
   * encrypted fs into the tmpfs /data filesystem, and then the framework finds those
   * files and passes that data to me */
  /* Create a tmp mount point to try mounting the decryptd fs
   * Since we're here, the mount_point should be a tmpfs filesystem, so make
   * a directory in it to test mount the decrypted filesystem.
   */
  sprintf(tmp_mount_point, "%s/tmp_mnt", mount_point);
  mkdir(tmp_mount_point, 0755);
  if (fs_mgr_do_mount(fstab, DATA_MNT_POINT, crypto_blkdev, tmp_mount_point)) {
    SLOGE("Error temp mounting decrypted block device\n");
    delete_crypto_blk_dev(label);
    crypt_ftr.failed_decrypt_count++;
  } else {
    /* Success, so just umount and we'll mount it properly when we restart
     * the framework.
     */
    umount(tmp_mount_point);
    crypt_ftr.failed_decrypt_count  = 0;
  }

  if (orig_failed_decrypt_count != crypt_ftr.failed_decrypt_count) {
    put_crypt_ftr_and_key(&crypt_ftr);
  }

  if (crypt_ftr.failed_decrypt_count) {
    /* We failed to mount the device, so return an error */
    rc = crypt_ftr.failed_decrypt_count;

  } else {
    /* Woot!  Success!  Save the name of the crypto block device
     * so we can mount it when restarting the framework.
     */
    property_set("ro.crypto.fs_crypto_blkdev", crypto_blkdev);

    /* Also save a the master key so we can reencrypted the key
     * the key when we want to change the password on it.
     */
    memcpy(saved_master_key, decrypted_master_key, KEY_LEN_BYTES);
    saved_mount_point = strdup(mount_point);
    master_key_saved = 1;
    SLOGD("%s(): Master key saved\n", __FUNCTION__);
    rc = 0;
    /*
     * Upgrade if we're not using the latest KDF.
     */
    if (crypt_ftr.kdf_type != KDF_SCRYPT) {
        crypt_ftr.kdf_type = KDF_SCRYPT;
        rc = encrypt_master_key(passwd, crypt_ftr.salt, saved_master_key, crypt_ftr.master_key,
                &crypt_ftr);
        if (!rc) {
            rc = put_crypt_ftr_and_key(&crypt_ftr);
        }
        SLOGD("Key Derivation Function upgrade: rc=%d\n", rc);
    }
  }

  return rc;
}

/* Called by vold when it wants to undo the crypto mapping of a volume it
 * manages.  This is usually in response to a factory reset, when we want
 * to undo the crypto mapping so the volume is formatted in the clear.
 */
int cryptfs_revert_volume(const char *label)
{
    return delete_crypto_blk_dev((char *)label);
}

/*
 * Called by vold when it's asked to mount an encrypted, nonremovable volume.
 * Setup a dm-crypt mapping, use the saved master key from
 * setting up the /data mapping, and return the new device path.
 */
int cryptfs_setup_volume(const char *label, int major, int minor,
                         char *crypto_sys_path, unsigned int max_path,
                         int *new_major, int *new_minor)
{
    char real_blkdev[MAXPATHLEN], crypto_blkdev[MAXPATHLEN];
    struct crypt_mnt_ftr sd_crypt_ftr;
    struct stat statbuf;
    int nr_sec, fd;

    sprintf(real_blkdev, "/dev/block/vold/%d:%d", major, minor);

    get_crypt_ftr_and_key(&sd_crypt_ftr);

    /* Update the fs_size field to be the size of the volume */
    fd = open(real_blkdev, O_RDONLY);
    nr_sec = get_blkdev_size(fd);
    close(fd);
    if (nr_sec == 0) {
        SLOGE("Cannot get size of volume %s\n", real_blkdev);
        return -1;
    }

    sd_crypt_ftr.fs_size = nr_sec;
    create_crypto_blk_dev(&sd_crypt_ftr, saved_master_key, real_blkdev, 
                          crypto_blkdev, label);

    stat(crypto_blkdev, &statbuf);
    *new_major = MAJOR(statbuf.st_rdev);
    *new_minor = MINOR(statbuf.st_rdev);

    /* Create path to sys entry for this block device */
    snprintf(crypto_sys_path, max_path, "/devices/virtual/block/%s", strrchr(crypto_blkdev, '/')+1);

    return 0;
}

int cryptfs_crypto_complete(void)
{
  return do_crypto_complete("/data");
}

int cryptfs_check_passwd(char *passwd)
{
    int rc = -1;

    rc = test_mount_encrypted_fs(passwd, DATA_MNT_POINT, "userdata");

    return rc;
}

int cryptfs_verify_passwd(char *passwd)
{
    struct crypt_mnt_ftr crypt_ftr;
    /* Allocate enough space for a 256 bit key, but we may use less */
    unsigned char decrypted_master_key[32];
    char encrypted_state[PROPERTY_VALUE_MAX];
    int rc;

    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted") ) {
        SLOGE("device not encrypted, aborting");
        return -2;
    }

    if (!master_key_saved) {
        SLOGE("encrypted fs not yet mounted, aborting");
        return -1;
    }

    if (!saved_mount_point) {
        SLOGE("encrypted fs failed to save mount point, aborting");
        return -1;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Error getting crypt footer and key\n");
        return -1;
    }

    if (crypt_ftr.flags & CRYPT_MNT_KEY_UNENCRYPTED) {
        /* If the device has no password, then just say the password is valid */
        rc = 0;
    } else {
        decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr);
        if (!memcmp(decrypted_master_key, saved_master_key, crypt_ftr.keysize)) {
            /* They match, the password is correct */
            rc = 0;
        } else {
            /* If incorrect, sleep for a bit to prevent dictionary attacks */
            sleep(1);
            rc = 1;
        }
    }

    return rc;
}

/* Initialize a crypt_mnt_ftr structure.  The keysize is
 * defaulted to 16 bytes, and the filesystem size to 0.
 * Presumably, at a minimum, the caller will update the
 * filesystem size and crypto_type_name after calling this function.
 */
static void cryptfs_init_crypt_mnt_ftr(struct crypt_mnt_ftr *ftr)
{
    off64_t off;

    memset(ftr, 0, sizeof(struct crypt_mnt_ftr));
    ftr->magic = CRYPT_MNT_MAGIC;
    ftr->major_version = CURRENT_MAJOR_VERSION;
    ftr->minor_version = CURRENT_MINOR_VERSION;
    ftr->ftr_size = sizeof(struct crypt_mnt_ftr);
    ftr->keysize = KEY_LEN_BYTES;

    ftr->kdf_type = KDF_SCRYPT;
    get_device_scrypt_params(ftr);

    ftr->persist_data_size = CRYPT_PERSIST_DATA_SIZE;
    if (get_crypt_ftr_info(NULL, &off) == 0) {
        ftr->persist_data_offset[0] = off + CRYPT_FOOTER_TO_PERSIST_OFFSET;
        ftr->persist_data_offset[1] = off + CRYPT_FOOTER_TO_PERSIST_OFFSET +
                                    ftr->persist_data_size;
    }
}

static int cryptfs_enable_wipe(char *crypto_blkdev, off64_t size, int type)
{
    const char *args[10];
    char size_str[32]; /* Must be large enough to hold a %lld and null byte */
    int num_args;
    int status;
    int tmp;
    int rc = -1;

    if (type == EXT4_FS) {
        args[0] = "/system/bin/make_ext4fs";
        args[1] = "-a";
        args[2] = "/data";
        args[3] = "-l";
        snprintf(size_str, sizeof(size_str), "%lld", size * 512);
        args[4] = size_str;
        args[5] = crypto_blkdev;
        num_args = 6;
        SLOGI("Making empty filesystem with command %s %s %s %s %s %s\n",
              args[0], args[1], args[2], args[3], args[4], args[5]);
    } else if (type== FAT_FS) {
        args[0] = "/system/bin/newfs_msdos";
        args[1] = "-F";
        args[2] = "32";
        args[3] = "-O";
        args[4] = "android";
        args[5] = "-c";
        args[6] = "8";
        args[7] = "-s";
        snprintf(size_str, sizeof(size_str), "%lld", size);
        args[8] = size_str;
        args[9] = crypto_blkdev;
        num_args = 10;
        SLOGI("Making empty filesystem with command %s %s %s %s %s %s %s %s %s %s\n",
              args[0], args[1], args[2], args[3], args[4], args[5],
              args[6], args[7], args[8], args[9]);
    } else {
        SLOGE("cryptfs_enable_wipe(): unknown filesystem type %d\n", type);
        return -1;
    }

    tmp = android_fork_execvp(num_args, (char **)args, &status, false, true);

    if (tmp != 0) {
      SLOGE("Error creating empty filesystem on %s due to logwrap error\n", crypto_blkdev);
    } else {
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status)) {
                SLOGE("Error creating filesystem on %s, exit status %d ",
                      crypto_blkdev, WEXITSTATUS(status));
            } else {
                SLOGD("Successfully created filesystem on %s\n", crypto_blkdev);
                rc = 0;
            }
        } else {
            SLOGE("Error creating filesystem on %s, did not exit normally\n", crypto_blkdev);
       }
    }

    return rc;
}

#define CRYPT_INPLACE_BUFSIZE 4096
#define CRYPT_SECTORS_PER_BUFSIZE (CRYPT_INPLACE_BUFSIZE / 512)
static int cryptfs_enable_inplace(char *crypto_blkdev, char *real_blkdev, off64_t size,
                                  off64_t *size_already_done, off64_t tot_size)
{
    int realfd, cryptofd;
    char *buf[CRYPT_INPLACE_BUFSIZE];
    int rc = -1;
    off64_t numblocks, i, remainder;
    off64_t one_pct, cur_pct, new_pct;
    off64_t blocks_already_done, tot_numblocks;

    if ( (realfd = open(real_blkdev, O_RDONLY)) < 0) { 
        SLOGE("Error opening real_blkdev %s for inplace encrypt\n", real_blkdev);
        return -1;
    }

    if ( (cryptofd = open(crypto_blkdev, O_WRONLY)) < 0) { 
        SLOGE("Error opening crypto_blkdev %s for inplace encrypt\n", crypto_blkdev);
        close(realfd);
        return -1;
    }

    /* This is pretty much a simple loop of reading 4K, and writing 4K.
     * The size passed in is the number of 512 byte sectors in the filesystem.
     * So compute the number of whole 4K blocks we should read/write,
     * and the remainder.
     */
    numblocks = size / CRYPT_SECTORS_PER_BUFSIZE;
    remainder = size % CRYPT_SECTORS_PER_BUFSIZE;
    tot_numblocks = tot_size / CRYPT_SECTORS_PER_BUFSIZE;
    blocks_already_done = *size_already_done / CRYPT_SECTORS_PER_BUFSIZE;

    SLOGE("Encrypting filesystem in place...");

    one_pct = tot_numblocks / 100;
    cur_pct = 0;
    /* process the majority of the filesystem in blocks */
    for (i=0; i<numblocks; i++) {
        new_pct = (i + blocks_already_done) / one_pct;
        if (new_pct > cur_pct) {
            char buf[8];

            cur_pct = new_pct;
            snprintf(buf, sizeof(buf), "%lld", cur_pct);
            property_set("vold.encrypt_progress", buf);
        }
        if (unix_read(realfd, buf, CRYPT_INPLACE_BUFSIZE) <= 0) {
            SLOGE("Error reading real_blkdev %s for inplace encrypt\n", crypto_blkdev);
            goto errout;
        }
        if (unix_write(cryptofd, buf, CRYPT_INPLACE_BUFSIZE) <= 0) {
            SLOGE("Error writing crypto_blkdev %s for inplace encrypt\n", crypto_blkdev);
            goto errout;
        }
    }

    /* Do any remaining sectors */
    for (i=0; i<remainder; i++) {
        if (unix_read(realfd, buf, 512) <= 0) {
            SLOGE("Error reading rival sectors from real_blkdev %s for inplace encrypt\n", crypto_blkdev);
            goto errout;
        }
        if (unix_write(cryptofd, buf, 512) <= 0) {
            SLOGE("Error writing final sectors to crypto_blkdev %s for inplace encrypt\n", crypto_blkdev);
            goto errout;
        }
    }

    *size_already_done += size;
    rc = 0;

errout:
    close(realfd);
    close(cryptofd);

    return rc;
}

#define CRYPTO_ENABLE_WIPE 1
#define CRYPTO_ENABLE_INPLACE 2

#define FRAMEWORK_BOOT_WAIT 60

static inline int should_encrypt(struct volume_info *volume)
{
    return (volume->flags & (VOL_ENCRYPTABLE | VOL_NONREMOVABLE)) == 
            (VOL_ENCRYPTABLE | VOL_NONREMOVABLE);
}

int cryptfs_enable(char *howarg, char *passwd)
{
    int how = 0;
    char crypto_blkdev[MAXPATHLEN], real_blkdev[MAXPATHLEN], sd_crypto_blkdev[MAXPATHLEN];
    unsigned long nr_sec;
    unsigned char decrypted_master_key[KEY_LEN_BYTES];
    int rc=-1, fd, i, ret;
    struct crypt_mnt_ftr crypt_ftr, sd_crypt_ftr;;
    struct crypt_persist_data *pdata;
    char tmpfs_options[PROPERTY_VALUE_MAX];
    char encrypted_state[PROPERTY_VALUE_MAX];
    char lockid[32] = { 0 };
    char key_loc[PROPERTY_VALUE_MAX];
    char fuse_sdcard[PROPERTY_VALUE_MAX];
    char *sd_mnt_point;
    char sd_blk_dev[256] = { 0 };
    int num_vols;
    struct volume_info *vol_list = 0;
    off64_t cur_encryption_done=0, tot_encryption_size=0;

    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "unencrypted")) {
        SLOGE("Device is already running encrypted, aborting");
        goto error_unencrypted;
    }

    fs_mgr_get_crypt_info(fstab, key_loc, 0, sizeof(key_loc));

    if (!strcmp(howarg, "wipe")) {
      how = CRYPTO_ENABLE_WIPE;
    } else if (! strcmp(howarg, "inplace")) {
      how = CRYPTO_ENABLE_INPLACE;
    } else {
      /* Shouldn't happen, as CommandListener vets the args */
      goto error_unencrypted;
    }

    fs_mgr_get_crypt_info(fstab, 0, real_blkdev, sizeof(real_blkdev));

    /* Get the size of the real block device */
    fd = open(real_blkdev, O_RDONLY);
    if ( (nr_sec = get_blkdev_size(fd)) == 0) {
        SLOGE("Cannot get size of block device %s\n", real_blkdev);
        goto error_unencrypted;
    }
    close(fd);

    /* If doing inplace encryption, make sure the orig fs doesn't include the crypto footer */
    if ((how == CRYPTO_ENABLE_INPLACE) && (!strcmp(key_loc, KEY_IN_FOOTER))) {
        unsigned int fs_size_sec, max_fs_size_sec;

        fs_size_sec = get_fs_size(real_blkdev);
        max_fs_size_sec = nr_sec - (CRYPT_FOOTER_OFFSET / 512);

        if (fs_size_sec > max_fs_size_sec) {
            SLOGE("Orig filesystem overlaps crypto footer region.  Cannot encrypt in place.");
            goto error_unencrypted;
        }
    }

    /* Get a wakelock as this may take a while, and we don't want the
     * device to sleep on us.  We'll grab a partial wakelock, and if the UI
     * wants to keep the screen on, it can grab a full wakelock.
     */
    snprintf(lockid, sizeof(lockid), "enablecrypto%d", (int) getpid());
    acquire_wake_lock(PARTIAL_WAKE_LOCK, lockid);

    /* Get the sdcard mount point */
    sd_mnt_point = getenv("EMULATED_STORAGE_SOURCE");
    if (!sd_mnt_point) {
       sd_mnt_point = getenv("EXTERNAL_STORAGE");
    }
    if (!sd_mnt_point) {
        sd_mnt_point = "/mnt/sdcard";
    }

    num_vols=vold_getNumDirectVolumes();
    vol_list = malloc(sizeof(struct volume_info) * num_vols);
    vold_getDirectVolumeList(vol_list);

    for (i=0; i<num_vols; i++) {
        if (should_encrypt(&vol_list[i])) {
            fd = open(vol_list[i].blk_dev, O_RDONLY);
            if ( (vol_list[i].size = get_blkdev_size(fd)) == 0) {
                SLOGE("Cannot get size of block device %s\n", vol_list[i].blk_dev);
                goto error_unencrypted;
            }
            close(fd);

            ret=vold_disableVol(vol_list[i].label);
            if ((ret < 0) && (ret != UNMOUNT_NOT_MOUNTED_ERR)) {
                /* -2 is returned when the device exists but is not currently mounted.
                 * ignore the error and continue. */
                SLOGE("Failed to unmount volume %s\n", vol_list[i].label);
                goto error_unencrypted;
            }
        }
    }

    /* The init files are setup to stop the class main and late start when
     * vold sets trigger_shutdown_framework.
     */
    property_set("vold.decrypt", "trigger_shutdown_framework");
    SLOGD("Just asked init to shut down class main\n");

    if (vold_unmountAllAsecs()) {
        /* Just report the error.  If any are left mounted,
         * umounting /data below will fail and handle the error.
         */
        SLOGE("Error unmounting internal asecs");
    }

    property_get("ro.crypto.fuse_sdcard", fuse_sdcard, "");
    if (!strcmp(fuse_sdcard, "true")) {
        /* This is a device using the fuse layer to emulate the sdcard semantics
         * on top of the userdata partition.  vold does not manage it, it is managed
         * by the sdcard service.  The sdcard service was killed by the property trigger
         * above, so just unmount it now.  We must do this _AFTER_ killing the framework,
         * unlike the case for vold managed devices above.
         */
        if (wait_and_unmount(sd_mnt_point)) {
            goto error_shutting_down;
        }
    }

    /* Now unmount the /data partition. */
    if (wait_and_unmount(DATA_MNT_POINT)) {
        goto error_shutting_down;
    }

    /* Do extra work for a better UX when doing the long inplace encryption */
    if (how == CRYPTO_ENABLE_INPLACE) {
        /* Now that /data is unmounted, we need to mount a tmpfs
         * /data, set a property saying we're doing inplace encryption,
         * and restart the framework.
         */
        if (fs_mgr_do_tmpfs_mount(DATA_MNT_POINT)) {
            goto error_shutting_down;
        }
        /* Tells the framework that inplace encryption is starting */
        property_set("vold.encrypt_progress", "0");

        /* restart the framework. */
        /* Create necessary paths on /data */
        if (prep_data_fs()) {
            goto error_shutting_down;
        }

        /* Ugh, shutting down the framework is not synchronous, so until it
         * can be fixed, this horrible hack will wait a moment for it all to
         * shut down before proceeding.  Without it, some devices cannot
         * restart the graphics services.
         */
        sleep(2);

        /* startup service classes main and late_start */
        property_set("vold.decrypt", "trigger_restart_min_framework");
        SLOGD("Just triggered restart_min_framework\n");

        /* OK, the framework is restarted and will soon be showing a
         * progress bar.  Time to setup an encrypted mapping, and
         * either write a new filesystem, or encrypt in place updating
         * the progress bar as we work.
         */
    }

    /* Start the actual work of making an encrypted filesystem */
    /* Initialize a crypt_mnt_ftr for the partition */
    cryptfs_init_crypt_mnt_ftr(&crypt_ftr);

    if (!strcmp(key_loc, KEY_IN_FOOTER)) {
        crypt_ftr.fs_size = nr_sec - (CRYPT_FOOTER_OFFSET / 512);
    } else {
        crypt_ftr.fs_size = nr_sec;
    }
    crypt_ftr.flags |= CRYPT_ENCRYPTION_IN_PROGRESS;
    strcpy((char *)crypt_ftr.crypto_type_name, "aes-cbc-essiv:sha256");

    /* Make an encrypted master key */
    if (create_encrypted_random_key(passwd, crypt_ftr.master_key, crypt_ftr.salt, &crypt_ftr)) {
        SLOGE("Cannot create encrypted master key\n");
        goto error_unencrypted;
    }

    /* Write the key to the end of the partition */
    put_crypt_ftr_and_key(&crypt_ftr);

    /* If any persistent data has been remembered, save it.
     * If none, create a valid empty table and save that.
     */
    if (!persist_data) {
       pdata = malloc(CRYPT_PERSIST_DATA_SIZE);
       if (pdata) {
           init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
           persist_data = pdata;
       }
    }
    if (persist_data) {
        save_persistent_data();
    }

    decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr);
    create_crypto_blk_dev(&crypt_ftr, decrypted_master_key, real_blkdev, crypto_blkdev,
                          "userdata");

    /* The size of the userdata partition, and add in the vold volumes below */
    tot_encryption_size = crypt_ftr.fs_size;

    /* setup crypto mapping for all encryptable volumes handled by vold */
    for (i=0; i<num_vols; i++) {
        if (should_encrypt(&vol_list[i])) {
            vol_list[i].crypt_ftr = crypt_ftr; /* gotta love struct assign */
            vol_list[i].crypt_ftr.fs_size = vol_list[i].size;
            create_crypto_blk_dev(&vol_list[i].crypt_ftr, decrypted_master_key,
                                  vol_list[i].blk_dev, vol_list[i].crypto_blkdev,
                                  vol_list[i].label);
            tot_encryption_size += vol_list[i].size;
        }
    }

    if (how == CRYPTO_ENABLE_WIPE) {
        rc = cryptfs_enable_wipe(crypto_blkdev, crypt_ftr.fs_size, EXT4_FS);
        /* Encrypt all encryptable volumes handled by vold */
        if (!rc) {
            for (i=0; i<num_vols; i++) {
                if (should_encrypt(&vol_list[i])) {
                    rc = cryptfs_enable_wipe(vol_list[i].crypto_blkdev,
                                             vol_list[i].crypt_ftr.fs_size, FAT_FS);
                }
            }
        }
    } else if (how == CRYPTO_ENABLE_INPLACE) {
        rc = cryptfs_enable_inplace(crypto_blkdev, real_blkdev, crypt_ftr.fs_size,
                                    &cur_encryption_done, tot_encryption_size);
        /* Encrypt all encryptable volumes handled by vold */
        if (!rc) {
            for (i=0; i<num_vols; i++) {
                if (should_encrypt(&vol_list[i])) {
                    rc = cryptfs_enable_inplace(vol_list[i].crypto_blkdev,
                                                vol_list[i].blk_dev,
                                                vol_list[i].crypt_ftr.fs_size,
                                                &cur_encryption_done, tot_encryption_size);
                }
            }
        }
        if (!rc) {
            /* The inplace routine never actually sets the progress to 100%
             * due to the round down nature of integer division, so set it here */
            property_set("vold.encrypt_progress", "100");
        }
    } else {
        /* Shouldn't happen */
        SLOGE("cryptfs_enable: internal error, unknown option\n");
        goto error_unencrypted;
    }

    /* Undo the dm-crypt mapping whether we succeed or not */
    delete_crypto_blk_dev("userdata");
    for (i=0; i<num_vols; i++) {
        if (should_encrypt(&vol_list[i])) {
            delete_crypto_blk_dev(vol_list[i].label);
        }
    }

    free(vol_list);

    if (! rc) {
        /* Success */

        /* Clear the encryption in progres flag in the footer */
        crypt_ftr.flags &= ~CRYPT_ENCRYPTION_IN_PROGRESS;
        put_crypt_ftr_and_key(&crypt_ftr);

        sleep(2); /* Give the UI a chance to show 100% progress */
        cryptfs_reboot(0);
    } else {
        char value[PROPERTY_VALUE_MAX];

        property_get("ro.vold.wipe_on_crypt_fail", value, "0");
        if (!strcmp(value, "1")) {
            /* wipe data if encryption failed */
            SLOGE("encryption failed - rebooting into recovery to wipe data\n");
            mkdir("/cache/recovery", 0700);
            int fd = open("/cache/recovery/command", O_RDWR|O_CREAT|O_TRUNC, 0600);
            if (fd >= 0) {
                write(fd, "--wipe_data", strlen("--wipe_data") + 1);
                close(fd);
            } else {
                SLOGE("could not open /cache/recovery/command\n");
            }
            cryptfs_reboot(1);
        } else {
            /* set property to trigger dialog */
            property_set("vold.encrypt_progress", "error_partially_encrypted");
            release_wake_lock(lockid);
        }
        return -1;
    }

    /* hrm, the encrypt step claims success, but the reboot failed.
     * This should not happen.
     * Set the property and return.  Hope the framework can deal with it.
     */
    property_set("vold.encrypt_progress", "error_reboot_failed");
    release_wake_lock(lockid);
    return rc;

error_unencrypted:
    free(vol_list);
    property_set("vold.encrypt_progress", "error_not_encrypted");
    if (lockid[0]) {
        release_wake_lock(lockid);
    }
    return -1;

error_shutting_down:
    /* we failed, and have not encrypted anthing, so the users's data is still intact,
     * but the framework is stopped and not restarted to show the error, so it's up to
     * vold to restart the system.
     */
    SLOGE("Error enabling encryption after framework is shutdown, no data changed, restarting system");
    cryptfs_reboot(0);

    /* shouldn't get here */
    property_set("vold.encrypt_progress", "error_shutting_down");
    free(vol_list);
    if (lockid[0]) {
        release_wake_lock(lockid);
    }
    return -1;
}

int cryptfs_changepw(char *newpw)
{
    struct crypt_mnt_ftr crypt_ftr;
    unsigned char decrypted_master_key[KEY_LEN_BYTES];

    /* This is only allowed after we've successfully decrypted the master key */
    if (! master_key_saved) {
        SLOGE("Key not saved, aborting");
        return -1;
    }

    /* get key */
    if (get_crypt_ftr_and_key(&crypt_ftr)) {
      SLOGE("Error getting crypt footer and key");
      return -1;
    }

    encrypt_master_key(newpw, crypt_ftr.salt, saved_master_key, crypt_ftr.master_key, &crypt_ftr);

    /* save the key */
    put_crypt_ftr_and_key(&crypt_ftr);

    return 0;
}

static int persist_get_key(char *fieldname, char *value)
{
    unsigned int i;

    if (persist_data == NULL) {
        return -1;
    }
    for (i = 0; i < persist_data->persist_valid_entries; i++) {
        if (!strncmp(persist_data->persist_entry[i].key, fieldname, PROPERTY_KEY_MAX)) {
            /* We found it! */
            strlcpy(value, persist_data->persist_entry[i].val, PROPERTY_VALUE_MAX);
            return 0;
        }
    }

    return -1;
}

static int persist_set_key(char *fieldname, char *value, int encrypted)
{
    unsigned int i;
    unsigned int num;
    struct crypt_mnt_ftr crypt_ftr;
    unsigned int max_persistent_entries;
    unsigned int dsize;

    if (persist_data == NULL) {
        return -1;
    }

    /* If encrypted, use the values from the crypt_ftr, otherwise
     * use the values for the current spec.
     */
    if (encrypted) {
        if(get_crypt_ftr_and_key(&crypt_ftr)) {
            return -1;
        }
        dsize = crypt_ftr.persist_data_size;
    } else {
        dsize = CRYPT_PERSIST_DATA_SIZE;
    }
    max_persistent_entries = (dsize - sizeof(struct crypt_persist_data)) /
                             sizeof(struct crypt_persist_entry);

    num = persist_data->persist_valid_entries;

    for (i = 0; i < num; i++) {
        if (!strncmp(persist_data->persist_entry[i].key, fieldname, PROPERTY_KEY_MAX)) {
            /* We found an existing entry, update it! */
            memset(persist_data->persist_entry[i].val, 0, PROPERTY_VALUE_MAX);
            strlcpy(persist_data->persist_entry[i].val, value, PROPERTY_VALUE_MAX);
            return 0;
        }
    }

    /* We didn't find it, add it to the end, if there is room */
    if (persist_data->persist_valid_entries < max_persistent_entries) {
        memset(&persist_data->persist_entry[num], 0, sizeof(struct crypt_persist_entry));
        strlcpy(persist_data->persist_entry[num].key, fieldname, PROPERTY_KEY_MAX);
        strlcpy(persist_data->persist_entry[num].val, value, PROPERTY_VALUE_MAX);
        persist_data->persist_valid_entries++;
        return 0;
    }

    return -1;
}

/* Return the value of the specified field. */
int cryptfs_getfield(char *fieldname, char *value, int len)
{
    char temp_value[PROPERTY_VALUE_MAX];
    char real_blkdev[MAXPATHLEN];
    /* 0 is success, 1 is not encrypted,
     * -1 is value not set, -2 is any other error
     */
    int rc = -2;

    if (persist_data == NULL) {
        load_persistent_data();
        if (persist_data == NULL) {
            SLOGE("Getfield error, cannot load persistent data");
            goto out;
        }
    }

    if (!persist_get_key(fieldname, temp_value)) {
        /* We found it, copy it to the caller's buffer and return */
        strlcpy(value, temp_value, len);
        rc = 0;
    } else {
        /* Sadness, it's not there.  Return the error */
        rc = -1;
    }

out:
    return rc;
}

/* Set the value of the specified field. */
int cryptfs_setfield(char *fieldname, char *value)
{
    struct crypt_persist_data stored_pdata;
    struct crypt_persist_data *pdata_p;
    struct crypt_mnt_ftr crypt_ftr;
    char encrypted_state[PROPERTY_VALUE_MAX];
    /* 0 is success, -1 is an error */
    int rc = -1;
    int encrypted = 0;

    if (persist_data == NULL) {
        load_persistent_data();
        if (persist_data == NULL) {
            SLOGE("Setfield error, cannot load persistent data");
            goto out;
        }
    }

    property_get("ro.crypto.state", encrypted_state, "");
    if (!strcmp(encrypted_state, "encrypted") ) {
        encrypted = 1;
    }

    if (persist_set_key(fieldname, value, encrypted)) {
        goto out;
    }

    /* If we are running encrypted, save the persistent data now */
    if (encrypted) {
        if (save_persistent_data()) {
            SLOGE("Setfield error, cannot save persistent data");
            goto out;
        }
    }

    rc = 0;

out:
    return rc;
}
