/*
 * Copyright (C) 2025 Fuzhou Rockchip Electronics Co.Ltd.
 *
 * Modification based on code covered by the Apache License, Version 2.0 (the "License").
 * You may not use this software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS TO YOU ON AN "AS IS" BASIS
 * AND ANY AND ALL WARRANTIES AND REPRESENTATIONS WITH RESPECT TO SUCH SOFTWARE, WHETHER EXPRESS,
 * IMPLIED, STATUTORY OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY IMPLIED WARRANTIES OF TITLE,
 * NON-INFRINGEMENT, MERCHANTABILITY, SATISFACTROY QUALITY, ACCURACY OR FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.
 *
 * IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
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

#ifndef _DRM_PQ_H_
#define _DRM_PQ_H_

#include "Pq.h"
#include "drmbufferqueue.h"
#include "drmlayer.h"

#include <mutex>

class DrmPqBufferInfo {
  public:
    hwc_frect_t source_crop;
    hwc_rect_t display_frame;
    uint64_t buffer_id = 0;
    bool match(DrmHwcLayer* layer) {
        if (!layer)
            return false;
        else
            return (layer->uBufferId_ == buffer_id &&
                    memcmp(&layer->source_crop, &source_crop, sizeof(source_crop)) == 0 &&
                    memcmp(&layer->display_frame, &display_frame, sizeof(display_frame)) == 0);
    }

    bool match(DrmHwcLayer& layer) {
        return (layer.uBufferId_ == buffer_id && memcmp(&layer.source_crop, &source_crop, sizeof(source_crop)) == 0 &&
                memcmp(&layer.display_frame, &display_frame, sizeof(display_frame)) == 0);
    }

    void set(DrmHwcLayer* layer) {
        if (!layer) return;
        buffer_id = layer->uBufferId_;
        source_crop = layer->source_crop;
        display_frame = layer->display_frame;
    }

    void set(DrmHwcLayer& layer) {
        buffer_id = layer.uBufferId_;
        source_crop = layer.source_crop;
        display_frame = layer.display_frame;
    }

    void clear() {
        buffer_id = 0;
        memset(&source_crop, 0, sizeof(source_crop));
        memset(&display_frame, 0, sizeof(display_frame));
    }
};

class DrmPq {
  public:
    DrmPq();
    ~DrmPq();
    int Init();

    int RunHwPqUiMode(std::vector<DrmHwcLayer>& layers);
    int CheckHwPqDstScale(DrmHwcLayer* drmLayer);
    int RunHwPqVideoMode(DrmHwcLayer* drmLayer);
    std::shared_ptr<DrmBuffer> RunHwPqSidebandMode(std::shared_ptr<DrmBuffer> src_buffer,
                                                   std::shared_ptr<android::HwpqDisplayStatus> display_state);
    int RunHwPqGetReg(DrmHwcLayer* drmLayer);
    int RunHwPqGetReg(std::shared_ptr<DrmBuffer> src_buffer);
    void ClearCache() { mLastHwPqBufInfo_.clear(); }

    void SetLumaAvgParam(int display_id, int interval) {
        mPqLumaAvgDisplayId_ = display_id;
        mPqLumaAvgInterval_ = interval;
    }
    bool CheckDisplayRunLumaAvg(int display_id);
    int RunGetLumaAvg(DrmHwcLayer* drmLayer);
    int RunGetLumaAvg(std::shared_ptr<DrmBuffer> src_buffer, std::shared_ptr<android::HwpqDisplayStatus> display_state);
    int ResetLumaAvg();

  private:
    bool bInit_;
    std::shared_ptr<Pq> mPq_;
    std::shared_ptr<DrmBufferQueue> mBufferQueue_;
    // SWPQ 上下文
    PqContext mSwpqCtx_;

    // HWPQ
    sp<AcquireFence> mLastAcquireFence_;
    bool mLastHwPqUseNewBuffer_;
    DrmPqBufferInfo mLastHwPqBufInfo_;
    std::shared_ptr<rk_hwpq_reg> mLastHwPqReg_;
    mutable std::recursive_mutex mRecursiveMutex;

    //Pq->GetLumaAvg
    int mPqLumaAvgDisplayId_ = 0;
    // Interval of Callings of Pq->GetLumaAvg
    int mPqLumaAvgInterval_ = -1;
    int64_t mLastPqLumaAvgTimeStamp_;
    bool bPqLumaAvgReset_ = false;
};

#endif  // _DRM_PQ_H_
