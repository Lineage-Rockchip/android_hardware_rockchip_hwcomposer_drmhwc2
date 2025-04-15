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
#ifndef _VP_TRANSFORM_CONTEXT_H_
#define _VP_TRANSFORM_CONTEXT_H_

#include <inttypes.h>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <drm_fourcc.h>
#include <drmbufferqueue.h>
#include "drmbuffer.h"
#include "rockchip/drmgralloc.h"
#include "rockchip/producer/videotunnel/video_tunnel.h"
#include "utils/drmfence.h"

namespace android {
// 用于外部获取 vp composition 结构
class VpComposition {
  public:
    VpComposition(int tunnel_id, uint32_t transform, std::shared_ptr<DrmBuffer> buffer, sp<ReleaseFence> release_fence,
                  int64_t queue_timestamp_us);
    ~VpComposition();

    void SetName(std::string name) { mName_ = name; };
    std::string Name() { return mName_; };

    int TunnelId() { return mTunnelId_; };

    uint32_t Transform() { return mTransform_; };

    void SetFrameNo(uint64_t frame_no) { mFrameNo_ = frame_no; };
    uint64_t FrameNo() { return mFrameNo_; };

    std::shared_ptr<DrmBuffer> GetBuffer() { return mBuffer_; };
    void SetBuffer(std::shared_ptr<DrmBuffer> buffer) { mBuffer_ = buffer; };

    void SetReleaseFence(sp<ReleaseFence> release_fence) { mReleaseFence_ = release_fence; };
    sp<ReleaseFence> GetReleaseFence() { return mReleaseFence_; }

    int64_t GetQueueTimestamp() { return mQueueTimestampUs_; }
    void MarkCommitTimestamp();

    void Dump(String8& output);

  private:
    std::string mName_;
    int mTunnelId_;
    uint32_t mTransform_;
    uint64_t mFrameNo_;
    std::shared_ptr<DrmBuffer> mBuffer_;
    sp<AcquireFence> mAcquireFence_;
    sp<ReleaseFence> mReleaseFence_;
    int64_t mQueueTimestampUs_;
    int64_t mCreateTimestampUs_;
    int64_t mCommitTimestampUs_;
};

/*
    VP_POST_PROCESS_TYPE 表示后处理的类型，通常为旋转/SR/MEMC/HWPQ等后处理
    #define DRM_MODE_ROTATE_0       (1<<0)
    #define DRM_MODE_ROTATE_90      (1<<1)
    #define DRM_MODE_ROTATE_180     (1<<2)
    #define DRM_MODE_ROTATE_270     (1<<3)
    #define DRM_MODE_REFLECT_X      (1<<4)
    #define DRM_MODE_REFLECT_Y      (1<<5)
 */

#define DRM_MODE_POST_PROCESS_SR (1 << 6)
#define DRM_MODE_POST_PROCESS_MEMC (1 << 7)
class VpTransformContext {
  public:
    VpTransformContext(int tunnel_id, uint32_t transform);
    // 处理 VpComposition 对外接口
    int Handle(std::shared_ptr<VpComposition> input);
    // 获取最新的 VpComposition
    std::shared_ptr<VpComposition> GetVpComposition(bool wait_finish);
    // 设置外部display获取的目标图像参数，主要用于调整SR/RGA输出结果
    // 比如：显示区域只有480p，则RGA/SR策略则直接输出按480p输出
    // 减少VOP后级的带宽访问压力
    void SetDisplayFrameInfo(int display_id, vt_rect_t* dis_rect);
    // 释放无用的 VpComposition，基本规则为 次旧VpComposition渲染完成，最旧无后级引用
    // 则可释放最旧VpComposition
    int ReleaseUnuseComposition();
    // 尝试释放所有资源
    int TryReleaseAllResource();

    uint64_t GetFrameNo() { return uFrameNo_; }

    void Dump(String8& output);
    void Dump();

  private:
    sp<ReleaseFence> AddReleaseFence(std::shared_ptr<VpComposition> input);
    int DoStore(std::shared_ptr<VpComposition> input);
    int DoTransformByRga(std::shared_ptr<VpComposition> input);

#ifdef USE_LIBSR
    int DoTransformBySvepSr(std::shared_ptr<VpComposition> input);
#endif
    std::string TransformToString(uint32_t transform) const;
    void CalculateFPS();

    int mTunnelId_;
    uint32_t mTransform_;
    std::string mFenceTag_;
    SyncTimeline mTimeLine_;
    uint64_t uFrameNo_;
    std::list<std::shared_ptr<VpComposition>> mListVpCompositions_;
    std::shared_ptr<DrmBufferQueue> mBufferQueue_;
    std::map<int /*display-id*/, vt_rect_t> mMapDisplayFrame_;
    bool mEnableOnelineMode_ = false;
    int mSrAuthTimeline_ = 0;
    // 用于计算帧率
    uint64_t mLastFrameNoCnt_;
    int64_t mLastTimestampUs_;
    float mFps_;
    bool bNeedRelease_;

    mutable std::mutex mMutex_;
};

};  // namespace android

#endif  // _VP_TRANSFORM_CONTEXT_H_
