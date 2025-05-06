/*
 * Copyright (C) 2016 The Android Open Source Project
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
#ifndef _VP_CONTEXT_H_
#define _VP_CONTEXT_H_

#include <inttypes.h>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <drm_fourcc.h>
#include <drmbufferqueue.h>
#include "drmbuffer.h"
#include "rockchip/drmgralloc.h"
#include "rockchip/producer/VpTransformContext.h"
#include "rockchip/producer/videotunnel/video_tunnel.h"
#include "utils/drmfence.h"

namespace android {

class VpBufferInfo {
  public:
    VpBufferInfo(vt_buffer_t* vp_buffer, std::shared_ptr<DrmBuffer> drm_buffer)
        : pVpBuffer_(vp_buffer), mDrmBuffer_(drm_buffer) {};

    ~VpBufferInfo() {};
    VpBufferInfo(const VpBufferInfo& rhs) = delete;
    VpBufferInfo& operator=(const VpBufferInfo& rhs) = delete;

    void SetVpBuffer(vt_buffer_t* vp_buffer) {
        std::lock_guard<std::mutex> lock(mtx_);
        pVpBuffer_ = vp_buffer;
    }

    // Get VpBuffer
    vt_buffer_t* GetVpBuffer() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return pVpBuffer_;
    }
    // Get VpBuffer
    std::shared_ptr<DrmBuffer> GetDrmBuffer() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return mDrmBuffer_;
    }

  private:
    vt_buffer_t* pVpBuffer_;
    std::shared_ptr<DrmBuffer> mDrmBuffer_;
    mutable std::mutex mtx_;
};

// DrmVideoProducer Context
class VpContext {
  public:
    VpContext(int tunnel_id);
    ~VpContext();
    int ReleaseResources();

    // Get tunnel fd
    int GetTunnelId();
    // AddConnectionRef
    int AddConnRef(int display_id);
    // ReleaseConnRef
    int ReleaseConnRef(int display_id);
    // ConnectionCnt
    int ConnectionCnt();
    // Get Buffer cache
    std::shared_ptr<DrmBuffer> GetBufferCache(vt_buffer_t* buffer, bool* use_cache);
    // Get VpBuffer
    vt_buffer_t* GetVpBufferInfo(uint64_t buffer_id);
    // Release VpBuffer
    int ReleaseBufferInfo(uint64_t buffer_id);

    // 设置/获取变换配置
    void SetTransform(int display_id, uint32_t transform) { mMapTransform_[display_id] = transform; }
    uint32_t GetTransform(int display_id);
    void SetProducerFps(float fps) { mFps_ = fps; }
    float GetProducerFps() { return mFps_; }

    void SetDataspace(android_dataspace_t dataspace) { iDataSpace_ = dataspace; }
    android_dataspace_t GetDataspace() { return iDataSpace_; }

    std::string TransformToString(uint32_t transform) const;

    std::vector<uint64_t> mReleaseFailedBuffer_;

    void PrintReleaseFailedBuffer();
    void Dump(String8& output);

    // 顺序存储从驱动获取的图像数据
    std::shared_ptr<VpComposition> mLatestR0Composition_;
    // Transform of displays, must lock before read/write.
    std::map<int /*display_id*/, uint32_t /*DRM_MODE_ROTATE*/> mMapTransform_;
    // 保存 VpTransformContext map结构
    std::map<uint32_t /*DRM_MODE_ROTATE*/, std::shared_ptr<VpTransformContext>> mMapVpTransformCtx_;
    // 需要释放的 VpTransformContext
    std::vector<std::shared_ptr<VpTransformContext>> mNeedReleaseCtx_;

  private:
    DrmGralloc* mDrmGralloc_;
    int iTunnelId_;
    std::map<int, std::shared_ptr<VpBufferInfo>> mMapBuffer_;
    std::set<int> refDpyConnection_;
    float mFps_ = 60;
    android_dataspace_t iDataSpace_ = HAL_DATASPACE_UNKNOWN;
    mutable std::mutex mtx_;
};
};  // namespace android

#endif  // _VP_CONTEXT_H_
