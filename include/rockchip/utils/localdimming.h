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

#ifndef LOCAL_DIMMING_H_
#define LOCAL_DIMMING_H_

#include "utils/worker.h"

#include <stdint.h>
#include <map>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include "drmbuffer.h"

#ifdef USE_LIBPQ
#include "rkpq.h"
#endif

#ifdef USE_LIBPQ_LD

namespace android {

class LocalDimmingCtx {
  public:
    struct LocalDimmingData {
        std::shared_ptr<std::vector<uint8_t>> pData;
        int iSize;
        uint64_t uFrameNo;
    };

    LocalDimmingCtx();
    ~LocalDimmingCtx();
    int HandleBuffer(std::shared_ptr<DrmBuffer> buffer);
    LocalDimmingData GetLocalDimmingData();
    int ClearLocalDimmingData();
    void SetCrtcId(uint32_t crtc_id) { uCrtcId_ = crtc_id; }

  private:
    uint64_t uDimmingFrameNo_ = 0;
    std::recursive_mutex mMutex_;
    std::shared_ptr<std::vector<uint8_t>> pDimmingBuffer_ = NULL;
    int iDimmingBufferSize_ = -1;
    uint32_t uCrtcId_ = 0;

    Pq* swpq_ = NULL;
};
}  // namespace android
#endif  //USE_LIBPQ_LD
#endif  //LOCAL_DIMMING_H_
