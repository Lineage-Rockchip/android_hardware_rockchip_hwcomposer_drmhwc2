/*
 * Copyright (C) 2015 The Android Open Source Project
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
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "hwc-localdimming"

#include <rockchip/utils/localdimming.h>

#include <stdlib.h>
#include <time.h>
#include <mutex>
#include <vector>

#include <log/log.h>
#include <sync/sync.h>
#include <utils/Trace.h>
// System property
#include <cutils/properties.h>

#include "drm/drm_mode.h"
#include "drmcrtc.h"
#include "drmdevice.h"
#include "drmplane.h"
#include "resources/resourcemanager.h"
#include "rockchip/drmtype.h"
#include "rockchip/producer/drmvideoproducer.h"
#include "rockchip/utils/drmdebug.h"
#include "rockchip/utils/pqutils.h"
#include "utils/autolock.h"

#include <rga.h>

#ifdef USE_LIBPQ_LD

#include "rkpq.h"

namespace android {

LocalDimmingCtx::LocalDimmingCtx() {
    return;
}

LocalDimmingCtx::~LocalDimmingCtx() {
    //等待完成
    std::lock_guard<std::recursive_mutex> lock(mMutex_);

    return;
}

int LocalDimmingCtx::HandleBuffer(std::shared_ptr<DrmBuffer> buffer) {
    ATRACE_CALL();

    std::lock_guard<std::recursive_mutex> pq_lock(ResourceManager::getInstance()->GetSwpqMutex());
    if (ResourceManager::getInstance()->NeedReinitSwpqForMode(ResourceManager::SWPQ_MODE_WBLD)) {
        if (swpq_ == NULL) {
            swpq_ = Pq::Get();
        }
        if (swpq_) {
            HWC2_LOGI("SWPQ: Swap form PQ mode to LD mode! Deinit swpq!");
            swpq_->DeInit();
            swpq_ = NULL;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(mMutex_);
    uDimmingFrameNo_ = buffer->GetFrameNumber();

    //Clear DimmingBuffer
    iDimmingBufferSize_ = -1;
    pDimmingBuffer_ = nullptr;

    if (swpq_ == NULL) {
        swpq_ = Pq::Get();
        if (swpq_ == NULL) {
            HWC2_LOGE("local_dimm: Pq module not ready.");
            return -1;
        }
    }

    // 1. Set buffer Info
    PqImageInfo src;
    uint32_t uDimmingBufferSize = 0;
    std::shared_ptr<std::vector<uint8_t>> pDimmingBuffer = NULL;

    src.mBufferInfo_.iFd_ = buffer->GetFd();
    src.mBufferInfo_.iWidth_ = buffer->GetWidth();
    src.mBufferInfo_.iHeight_ = buffer->GetHeight();
    src.mBufferInfo_.iFormat_ = buffer->GetFormat();
    src.mBufferInfo_.iStride_ = buffer->GetStride();
    // src.mBufferInfo_.iSize_   = buffer->GetSize();;
    src.mBufferInfo_.uBufferId_ = buffer->GetBufferId();
    src.mBufferInfo_.uDataSpace_ = buffer->GetDataspace();

    src.mCrop_.iLeft_ = 0;
    src.mCrop_.iTop_ = 0;
    src.mCrop_.iRight_ = buffer->GetWidth();
    src.mCrop_.iBottom_ = buffer->GetHeight();
    // wbBuffer->GetCrop(&src.mCrop_.iLeft_, &src.mCrop_.iTop_, &src.mCrop_.iRight_, &src.mCrop_.iBottom_);

    src.uCrtcId = uCrtcId_;
    src.uSpiProtLength = &uDimmingBufferSize;

    // 2. 获取DimmingData
    uint32_t dimm_size = swpq_->getDimmingSize(uCrtcId_);
    pDimmingBuffer = std::make_shared<std::vector<uint8_t>>(dimm_size);

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    int64_t begin_us = (int64_t)current_time.tv_sec * 1000000L + current_time.tv_nsec / 1000;

    int ret = swpq_->getDimmingConfig(src, pDimmingBuffer->data(), &uDimmingBufferSize);

    clock_gettime(CLOCK_MONOTONIC, &current_time);
    int64_t end_us = (int64_t)current_time.tv_sec * 1000000L + current_time.tv_nsec / 1000;
    if (ret) {
        HWC2_LOGE("local_dimm: Run getDimmingConfig fail! FrameNo %" PRIu64 ", dimming size = %" PRIu32
                  ", getDimmingConfig time=%" PRIi64 "us",
                  uDimmingFrameNo_, dimm_size, end_us - begin_us);
        return ret;
    }
    HWC2_LOGD_IF("local_dimm: Run getDimmingConfig OK, FrameNo %" PRIu64 ", dimming size = %" PRIu32
                 ", getDimmingConfig time=%" PRIi64 "us",
                 uDimmingFrameNo_, dimm_size, end_us - begin_us);

    pDimmingBuffer_ = pDimmingBuffer;
    iDimmingBufferSize_ = uDimmingBufferSize;

    return 0;
}

LocalDimmingCtx::LocalDimmingData LocalDimmingCtx::GetLocalDimmingData() {
    std::lock_guard<std::recursive_mutex> lock(mMutex_);
    HWC2_LOGV_IF("local_dimm: Take LocalDimming Data %" PRIu64, uDimmingFrameNo_);
    return {pDimmingBuffer_, iDimmingBufferSize_, uDimmingFrameNo_};
}

int LocalDimmingCtx::ClearLocalDimmingData() {
    std::lock_guard<std::recursive_mutex> lock(mMutex_);
    HWC2_LOGV_IF("local_dimm: Clear LocalDimming Data %" PRIu64, uDimmingFrameNo_);
    iDimmingBufferSize_ = -1;
    pDimmingBuffer_ = nullptr;
    return 0;
}

}  // namespace android

#endif