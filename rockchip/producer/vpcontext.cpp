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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "hwc-sideband"

#include "rockchip/producer/vpcontext.h"
#include "rockchip/utils/drmdebug.h"
#include "utils/drmfence.h"

#include <inttypes.h>

namespace android {
VpContext::VpContext(int tunnel_fd) : mDrmGralloc_(DrmGralloc::getInstance()), iTunnelId_(tunnel_fd) {}

VpContext::~VpContext() {}

int VpContext::ReleaseResources() {
    std::lock_guard<std::mutex> lock(mtx_);
    // HWC2_LOGE("TunnelId=%d ReleaseResources:", iTunnelId_);
    mLatestR0Composition_ = NULL;

    for (auto& pair_transform_ctx : mMapVpTransformCtx_) {
        if (pair_transform_ctx.second->TryReleaseAllResource()) {
            HWC2_LOGW_IF("TunnelId=%d VpTransformCtx %s TryReleaseAllResource fail.", iTunnelId_,
                         TransformToString(pair_transform_ctx.first).c_str());
            if (LogLevel(DBG_WARN)) {
                pair_transform_ctx.second->Dump();
            }
            return -1;
        }
    }
    for (auto& transform_ctx : mNeedReleaseCtx_) {
        if (transform_ctx->TryReleaseAllResource()) {
            HWC2_LOGW_IF("TunnelId=%d NeedReleaseCtx TryReleaseAllResource fail.", iTunnelId_);
            if (LogLevel(DBG_WARN)) {
                transform_ctx->Dump();
            }
            return -1;
        }
    }
    mMapBuffer_.clear();
    return 0;
}

int VpContext::GetTunnelId() {
    return iTunnelId_;
}

// ConnectionCnt
int VpContext::ConnectionCnt() {
    std::lock_guard<std::mutex> lock(mtx_);
    return refDpyConnection_.size();
}
// AddConnectionRef
int VpContext::AddConnRef(int display_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (refDpyConnection_.count(display_id) == 0) {
        refDpyConnection_.insert(display_id);
        return 0;
    }
    return -1;
}

// ReleaseConnRef
int VpContext::ReleaseConnRef(int display_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    refDpyConnection_.erase(display_id);
    return 0;
}

std::string VpContext::TransformToString(uint32_t transform) const {
    std::string result;

    switch (transform & (DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK)) {
        case DRM_MODE_ROTATE_0:
            result += "R0";
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X:
            result += "FH";
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y:
            result += "FV";
            break;
        case DRM_MODE_ROTATE_90:
            result += "R90";
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y:
            result += "R180";
            break;
        case DRM_MODE_ROTATE_270:
            result += "R270";
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90:
            result += "FH90";
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90:
            result += "FV90";
            break;
        default:
            result += "UK";
            break;
    }

    if (transform & DRM_MODE_POST_PROCESS_SR) {
        result += "_SR";
    } else if (transform & DRM_MODE_POST_PROCESS_MEMC) {
        result += "_MM";
    }
    return result;
}
// Get Buffer cache
std::shared_ptr<DrmBuffer> VpContext::GetBufferCache(vt_buffer_t* vp_buffer, bool* use_cache) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::shared_ptr<DrmBuffer> out_buffer = NULL;

    // 利用BufferId作为键值获取CacheBuffer
    uint64_t buffer_id = vp_buffer->buffer_id;
    native_handle_t* handle = vp_buffer->handle;
    // Cache 命中
    if (mMapBuffer_.count(buffer_id)) {
        mMapBuffer_[buffer_id]->SetVpBuffer(vp_buffer);
        out_buffer = mMapBuffer_[buffer_id]->GetDrmBuffer();
        out_buffer->SetCrop(vp_buffer->crop.left, vp_buffer->crop.top, vp_buffer->crop.right, vp_buffer->crop.bottom);
        *use_cache = true;
        // HWC2_LOGD_IF("Get cache buffer-id=0x%" PRIx64 " crop=[%d,%d,%d,%d]", out_buffer->GetBufferId(),
        //                     vp_buffer->crop.left, vp_buffer->crop.top, vp_buffer->crop.right, vp_buffer->crop.bottom);
    } else {  // Cache 未命中，则说明是新Buffer,需要执行Import操作
        std::shared_ptr<DrmBuffer> drm_buffer = std::make_shared<DrmBuffer>(handle);
        if (!drm_buffer->initCheck()) {
            HWC2_LOGI("DrmBuffer import fail, handle=%p", handle);
            return NULL;
        }
        drm_buffer->SetCrop(vp_buffer->crop.left, vp_buffer->crop.top, vp_buffer->crop.right, vp_buffer->crop.bottom);
        // 保存外部唯一键值的ExternelId, 作为SidebandStream唯一键值
        drm_buffer->SetExternalId(vp_buffer->buffer_id);
        auto vp_buffer_info = std::make_shared<VpBufferInfo>(vp_buffer, drm_buffer);
        mMapBuffer_[buffer_id] = vp_buffer_info;
        out_buffer = vp_buffer_info->GetDrmBuffer();
        *use_cache = false;
        // HWC2_LOGD_IF("Get new cache buffer-id=0x%" PRIx64 " vp_buffer->buffer_id=0x%" PRIx64,
        //                     out_buffer->GetBufferId(), vp_buffer->buffer_id);
    }
    return out_buffer;
}

vt_buffer_t* VpContext::GetVpBufferInfo(uint64_t buffer_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    // 获取 BufferId
    vt_buffer_t* out_buffer = NULL;
    if (mMapBuffer_.count(buffer_id)) {
        out_buffer = mMapBuffer_[buffer_id]->GetVpBuffer();
    }
    return out_buffer;
}

int VpContext::ReleaseBufferInfo(uint64_t buffer_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (mMapBuffer_.count(buffer_id)) {
        mMapBuffer_[buffer_id]->SetVpBuffer(NULL);
    }
    return 0;
}

uint32_t VpContext::GetTransform(int display_id) {
    if (mMapTransform_.count(display_id))
        return mMapTransform_[display_id];
    else
        return DRM_MODE_ROTATE_0;
}

void VpContext::PrintReleaseFailedBuffer() {
    if (mReleaseFailedBuffer_.size() == 0) return;

    char buf[200] = {0};
    int printCount = mReleaseFailedBuffer_.size();
    if (printCount > 5) printCount = 5;
    for (int i = 0; i < printCount; i++) sprintf(buf, "%s,%" PRIx64, buf, mReleaseFailedBuffer_[i]);
    HWC2_LOGW("Tunnel_id=%d buffer:%s...(total %zu buffer(s)) pervious release failed!", iTunnelId_, buf,
              mReleaseFailedBuffer_.size());
}

void VpContext::Dump(String8& output) {
    std::unique_lock<std::mutex> lock(mtx_);
    output.appendFormat(
            "SidebandCtxInfo: Id=%d CacheBufferSize=%zu TransformSize=%zu ProducerFps=%.2f refDpyConnection=%zu "
            "DataSpace=0x%x ReleaseFailedBufferSize=%zu NeedReleaseCtxSize=%zu\n",
            iTunnelId_, mMapBuffer_.size(), mMapVpTransformCtx_.size(), mFps_, refDpyConnection_.size(), iDataSpace_,
            mReleaseFailedBuffer_.size(), mNeedReleaseCtx_.size());
    output.appendFormat("\tSidebandCtxTransformInfo: \n");
    output.appendFormat("\t[DisplayId, Transform]: ");
    for (auto pair_transform : mMapTransform_) {
        // display-id = -1/100x 均是内部控制逻辑，不需要打印
        if (pair_transform.first == -1 || pair_transform.first >= 1000) {
            continue;
        }
        output.appendFormat("[%d: %s], ", pair_transform.first, TransformToString(pair_transform.second).c_str());
    }
    output.appendFormat("\n");
    return;
}

};  // namespace android