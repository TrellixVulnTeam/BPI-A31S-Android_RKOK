/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>

#include <utils/Errors.h>

#include "SurfaceFlinger.h"
#include "SurfaceTextureLayer.h"

namespace android {
// ---------------------------------------------------------------------------


SurfaceTextureLayer::SurfaceTextureLayer(const sp<SurfaceFlinger>& flinger)
    : BufferQueue(), flinger(flinger) {
    mCompositionType[0] = -1;
    mCompositionType[1] = -1;
    mCompositionType[2] = -1;

}

SurfaceTextureLayer::~SurfaceTextureLayer() {
    // remove ourselves from SurfaceFlinger's list. We do this asynchronously
    // because we don't know where this dtor is called from, it could be
    // called with the mStateLock held, leading to a dead-lock (it actually
    // happens).
    class MessageCleanUpList : public MessageBase {
        sp<SurfaceFlinger> flinger;
        wp<IBinder> gbp;
    public:
        MessageCleanUpList(const sp<SurfaceFlinger>& flinger, const wp<IBinder>& gbp)
            : flinger(flinger), gbp(gbp) { }
        virtual bool handler() {
            Mutex::Autolock _l(flinger->mStateLock);
            flinger->mGraphicBufferProducerList.remove(gbp);
            return true;
        }
    };
    flinger->postMessageAsync(
            new MessageCleanUpList(flinger, static_cast<BnGraphicBufferProducer*>(this)) );
}

int SurfaceTextureLayer::getVideoOutputType()
{
    if(mCompositionType[0] < 0 && mCompositionType[1] < 0 && mCompositionType[2] < 0)
    {
        return 0;
    }
    else if(mCompositionType[0] == HWC_FRAMEBUFFER || mCompositionType[1] == HWC_FRAMEBUFFER || mCompositionType[2] == HWC_FRAMEBUFFER)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

status_t SurfaceTextureLayer::setCompositionType(int display, int type)
{
    if(display == 0)//init
    {
        mCompositionType[0] = -1;
        mCompositionType[1] = -1;
        mCompositionType[2] = -1;
    }

    mCompositionType[display] = type;
    return 0;
}

// ---------------------------------------------------------------------------
}; // namespace android
