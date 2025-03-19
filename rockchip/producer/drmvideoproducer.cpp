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

#include "rockchip/producer/drmvideoproducer.h"
#include <rockchip/utils/rgautils.h>
#include <utils/Trace.h>
#include <im2d.hpp>
#include "resources/resourcemanager.h"
#include "rockchip/utils/drmdebug.h"

#include <dlfcn.h>
#include <fcntl.h>
#ifdef USE_LIBPQ_HWPQ
#include <Pq.h>
#include <hardware/hwcomposer_defs.h>
#endif
namespace android {

#if defined(__arm64__) || defined(__aarch64__)
#define RK_LIB_VT_PATH "/vendor/lib64/librkvt.so"
#else
#define RK_LIB_VT_PATH "/vendor/lib/librkvt.so"
#endif

#define ALIGN_DOWN(value, base) (value & (~(base - 1)))
#ifndef ALIGN
#define ALIGN(value, base) (((value) + ((base) - 1)) & ~((base) - 1))
#endif

#ifndef RK_GRALLOC_USAGE_STRIDE_ALIGN_64
#define RK_GRALLOC_USAGE_STRIDE_ALIGN_64 (1ULL << 60)
#endif

#ifndef RK_GRALLOC_USAGE_WITHIN_4G
#define RK_GRALLOC_USAGE_WITHIN_4G (1ULL << 56)
#endif

#ifndef MALI_GRALLOC_USAGE_NO_AFBC
#define MALI_GRALLOC_USAGE_NO_AFBC (1ULL << 29)
#endif

// Next Hdr
typedef int (*rk_vt_open_func)(void);
typedef int (*rk_vt_close_func)(int fd);
typedef int (*rk_vt_connect_func)(int fd, int tunnel_id, int role);
typedef int (*rk_vt_disconnect_func)(int fd, int tunnel_id, int role);
typedef int (*rk_vt_acquire_buffer_func)(int fd, int tunnel_id, int timeout_ms, vt_buffer_t** buffer,
                                         int64_t* expected_present_time);
typedef int (*rk_vt_release_buffer_func)(int fd, int tunnel_id, vt_buffer_t* buffer);

struct rkvt_ops {
    int (*rk_vt_open)(void);
    int (*rk_vt_close)(int fd);
    int (*rk_vt_connect)(int fd, int tunnel_id, int role);
    int (*rk_vt_disconnect)(int fd, int tunnel_id, int role);
    int (*rk_vt_acquire_buffer)(int fd, int tunnel_id, int timeout_ms, vt_buffer_t** buffer,
                                int64_t* expected_present_time);
    int (*rk_vt_release_buffer)(int fd, int tunnel_id, vt_buffer_t* buffer);
};

static struct rkvt_ops g_rkvt_ops;
static void* g_rkvt_lib_handle = NULL;

DrmVideoProducer::DrmVideoProducer()
    : Worker("DVPWorker", HAL_PRIORITY_URGENT_DISPLAY), bInit_(false), iTunnelFd_(-1) {}

DrmVideoProducer::~DrmVideoProducer() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (iTunnelFd_ > 0) {
        int ret = g_rkvt_ops.rk_vt_close(iTunnelFd_);
        if (ret < 0) {
            HWC2_LOGE("rk_vt_close fail ret=%d", ret);
        }
    }
}

// Init video tunel.
int DrmVideoProducer::Init() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (bInit_) return 0;

    if (InitLibHandle()) {
        HWC2_LOGE("init fail, disable VideoProducer function.");
        return -1;
    }

    if (iTunnelFd_ < 0) {
        iTunnelFd_ = g_rkvt_ops.rk_vt_open();
        if (iTunnelFd_ < 0) {
            HWC2_LOGE("rk_vt_open fail ret=%d", iTunnelFd_);
            return -1;
        }
    }

    HWC2_LOGI("Init success fd=%d", iTunnelFd_);
    bInit_ = true;
    return InitWorker();
}

int DrmVideoProducer::InitLibHandle() {
    g_rkvt_lib_handle = dlopen(RK_LIB_VT_PATH, RTLD_NOW);
    if (g_rkvt_lib_handle == NULL) {
        HWC2_LOGE("cat not open %s\n", RK_LIB_VT_PATH);
        return -1;
    } else {
        g_rkvt_ops.rk_vt_open = (rk_vt_open_func)dlsym(g_rkvt_lib_handle, "rk_vt_open");
        g_rkvt_ops.rk_vt_close = (rk_vt_close_func)dlsym(g_rkvt_lib_handle, "rk_vt_close");
        g_rkvt_ops.rk_vt_connect = (rk_vt_connect_func)dlsym(g_rkvt_lib_handle, "rk_vt_connect");
        g_rkvt_ops.rk_vt_disconnect = (rk_vt_disconnect_func)dlsym(g_rkvt_lib_handle, "rk_vt_disconnect");
        g_rkvt_ops.rk_vt_acquire_buffer = (rk_vt_acquire_buffer_func)dlsym(g_rkvt_lib_handle, "rk_vt_acquire_buffer");
        g_rkvt_ops.rk_vt_release_buffer = (rk_vt_release_buffer_func)dlsym(g_rkvt_lib_handle, "rk_vt_release_buffer");

        if (g_rkvt_ops.rk_vt_open == NULL || g_rkvt_ops.rk_vt_close == NULL || g_rkvt_ops.rk_vt_connect == NULL ||
            g_rkvt_ops.rk_vt_disconnect == NULL || g_rkvt_ops.rk_vt_acquire_buffer == NULL ||
            g_rkvt_ops.rk_vt_release_buffer == NULL) {
            HWC2_LOGE_IF(
                    "cat not dlsym open=%p close=%p connect=%p disconnect=%p acquire_buffer=%p release_buffer=%p\n",
                    g_rkvt_ops.rk_vt_open, g_rkvt_ops.rk_vt_close, g_rkvt_ops.rk_vt_connect,
                    g_rkvt_ops.rk_vt_disconnect, g_rkvt_ops.rk_vt_acquire_buffer, g_rkvt_ops.rk_vt_release_buffer);
            return -1;
        }
    }
    HWC2_LOGI("InitLibHandle %s success!\n", RK_LIB_VT_PATH);
    return 0;
}

bool DrmVideoProducer::IsValid() {
    return bInit_;
}

std::string DrmVideoProducer::TransformToString(uint32_t transform) const {
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
// Create tunnel connection.
int DrmVideoProducer::CreateConnection(int display_id, int tunnel_id, android_dataspace_t dataspace,
                                       uint32_t transform) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_LOGE(" fail, display-id=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return -1;
    }

    if (mMapCtx_.count(tunnel_id)) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        if (!ctx->AddConnRef(display_id)) {
            HWC2_LOGD_IF("display-id=%d tunnel_id=%d success, transform=%s, connections size=%d", display_id, tunnel_id,
                      TransformToString(transform).c_str(), ctx->ConnectionCnt());
        }
        ctx->SetDataspace(dataspace);
        ctx->SetTransform(display_id, transform);
        return 0;
    }

    int ret = g_rkvt_ops.rk_vt_connect(iTunnelFd_, tunnel_id, RKVT_ROLE_CONSUMER);
    if (ret < 0) {
        return ret;
    }

    HWC2_LOGD_IF("display-id=%d tunnel_id=%d success, transfrom=%s", display_id, tunnel_id,
              TransformToString(transform).c_str());
    mMapCtx_[tunnel_id] = std::make_shared<VpContext>(tunnel_id);
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->AddConnRef(display_id);
    ctx->SetDataspace(dataspace);
    ctx->SetTransform(display_id, transform);
    // 默认需要创建 transform = 0 的 transform
    ctx->SetTransform(-1, DRM_MODE_ROTATE_0);
    Signal();
    return 0;
}

// Destory Connection
int DrmVideoProducer::DestoryConnection(int display_id, int tunnel_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_LOGE("fail, display=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return -1;
    }

    if (mMapCtx_.count(tunnel_id) == 0) {
        HWC2_LOGE("display_id=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return -1;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->ReleaseConnRef(display_id);
    if (ctx->ConnectionCnt() > 0) {
        HWC2_LOGD_IF("display=%d tunnel_id=%d connection cnt=%d, no need to destory. ", display_id, tunnel_id,
                     ctx->ConnectionCnt());
        return 0;
    }
    HWC2_LOGD_IF("display=%d tunnel_id=%d connection cnt=%d, going to destory. ", display_id, tunnel_id,
                 ctx->ConnectionCnt());
    return 0;
}

#ifdef USE_LIBPQ_HWPQ

static bool IsYuvFormat(int format, uint32_t fourcc_format) {
    switch (fourcc_format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV12_10:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_NV16:
        case DRM_FORMAT_NV61:
        case DRM_FORMAT_YUV420:
        case DRM_FORMAT_YVU420:
        case DRM_FORMAT_YUV422:
        case DRM_FORMAT_YVU422:
        case DRM_FORMAT_YUV444:
        case DRM_FORMAT_YVU444:
        case DRM_FORMAT_UYVY:
        case DRM_FORMAT_VYUY:
        case DRM_FORMAT_YUYV:
        case DRM_FORMAT_YVYU:
        case DRM_FORMAT_YUV420_8BIT:
        case DRM_FORMAT_YUV420_10BIT:
        case DRM_FORMAT_NV24:
        case DRM_FORMAT_NV42:
        case DRM_FORMAT_NV15:
        case DRM_FORMAT_NV20:
        case DRM_FORMAT_NV30:
        case DRM_FORMAT_Y210:
        case DRM_FORMAT_VUY888:
        case DRM_FORMAT_VUY101010:
            return true;
        default:
            break;
    }

    switch (format) {
        case HAL_PIXEL_FORMAT_YCrCb_NV12:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
        case HAL_PIXEL_FORMAT_YCrCb_NV12_VIDEO:
        case HAL_PIXEL_FORMAT_YCbCr_422_SP_10:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP_10:
        case HAL_PIXEL_FORMAT_YCBCR_422_I:
        case HAL_PIXEL_FORMAT_YUV420_8BIT_I:
        case HAL_PIXEL_FORMAT_YUV420_10BIT_I:
        case HAL_PIXEL_FORMAT_Y210:
            return true;
        default:
            return false;
    }
}

static bool Is10bitYuv(int format, uint32_t fourcc_format) {
    switch (fourcc_format) {
        case DRM_FORMAT_NV12_10:
        case DRM_FORMAT_YUV420_10BIT:
        case DRM_FORMAT_VUY101010:
        case DRM_FORMAT_Y210:
        case DRM_FORMAT_NV30:
        case DRM_FORMAT_NV20:
        case DRM_FORMAT_NV15:
            return true;
        default:
            break;
    }

    switch (format) {
        case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
        case HAL_PIXEL_FORMAT_YCbCr_422_SP_10:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP_10:
        case HAL_PIXEL_FORMAT_YUV420_10BIT_I:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<DrmBuffer> DrmVideoProducer::DoHwPq(std::shared_ptr<VpContext> ctx, std::shared_ptr<DrmBuffer> buffer) {
    int left, top, right, bottom;
    buffer->GetCrop(&left, &top, &right, &bottom);

    int act_w = right - left;
    int act_h = bottom - top;

    int ret = 0;
    // 0. Check if HwPq is Ready
    if (hwpq_ == NULL) {
        hwpq_ = std::make_shared<Pq>();
        if (hwpq_ != NULL) {
            ret = hwpq_->Init(PQ_VERSION);
            if (ret != 0) {
                HWC2_LOGE("Pq module Init Failed, ret=%d", ret);
                hwpq_ = NULL;
            }
        }
    }
    if (hwpq_ == NULL) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq module not ready! Pq::Get() return NULL",
                  ctx->GetTunnelId(), buffer->GetExternalId());
        return NULL;
    }
    // 1. 初始化Ctx
    // 2. Fill buffer Info
    HwPqImageInfo src;
    src.mBufferInfo_.iFd_ = buffer->GetFd();
    src.mBufferInfo_.iWidth_ = buffer->GetWidth();
    src.mBufferInfo_.iHeight_ = buffer->GetHeight();
    src.mBufferInfo_.iFormat_ = buffer->GetFormat();
    src.mBufferInfo_.iStride_ = buffer->GetStride();
    src.mBufferInfo_.iHeightStride_ = buffer->GetHeightStride();
    src.mBufferInfo_.uBufferId_ = buffer->GetBufferId();
    src.mBufferInfo_.uDataSpace_ = (uint64_t)ctx->GetDataspace();

    src.mIsFbcdFormat_ = (buffer->IsAfbc() || buffer->IsRfbc());

    src.mCrop_.iLeft_ = (int)left;
    src.mCrop_.iTop_ = (int)top;
    src.mCrop_.iRight_ = (int)right;
    src.mCrop_.iBottom_ = (int)bottom;

    src.iMetaDataFd_ = -1;
    src.iMetaDataSize_ = 0;
    src.iMetaDataOffset_ = 0;

    // src.mVirtualAddress_ = buffer->Lock();
    DrmGralloc* gralloc = DrmGralloc::getInstance();
    if (gralloc == NULL) {
        HWC2_LOGI_IF("DrmGralloc is null, Can not get PQ Metadata");
    } else {
        src.iMetaDataOffset_ = gralloc->hwc_get_offset_of_pq_metadata(buffer->GetHandle());
        if (src.iMetaDataOffset_ > 0) {
            src.iMetaDataFd_ = buffer->GetFd();
            src.iMetaDataSize_ = buffer->GetSize();
        }
    }

    HwPqImageInfo dst;
    dst.mBufferInfo_.iFd_ = -1;
    std::shared_ptr<DrmBuffer> dst_buffer;

    // 3. Set buffer Info
    bool needAllocNewBuffer = false;
    ret = hwpq_->SetHwPqSrcImage(src, dst, &needAllocNewBuffer);
    if (ret) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq SetSrcImage fail ret = %d", ctx->GetTunnelId(),
                  buffer->GetExternalId(), ret);
        // buffer->Unlock();
        return NULL;
    }

    //判断是否需要新buffer的条件：宽高format存在差异
    if (needAllocNewBuffer) {
        dst_buffer = std::make_shared<DrmBuffer>(
                dst.mBufferInfo_.iWidth_, dst.mBufferInfo_.iHeight_, dst.mBufferInfo_.iFormat_,
                RK_GRALLOC_USAGE_STRIDE_ALIGN_64 | MALI_GRALLOC_USAGE_NO_AFBC, "HWPQ-target");
        ret = dst_buffer->Init();
        if (ret) {
            HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Create DstBuffer fail! ret = %d", ctx->GetTunnelId(),
                      buffer->GetExternalId(), ret);
            return NULL;
        }
        //更新buffer参数
        dst.mBufferInfo_.iFd_ = dst_buffer->GetFd();
        dst.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
        dst.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
        dst.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
        dst.mBufferInfo_.iStride_ = dst_buffer->GetStride();
        dst.mBufferInfo_.iHeightStride_ = dst_buffer->GetStride();
        dst.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
    } else {
        dst.mBufferInfo_ = src.mBufferInfo_;
    }

    if (dst.mBufferInfo_.iFd_ < 0) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " dst buffer not set, dst.mBufferInfo_.iFd_=%d",
                  ctx->GetTunnelId(), buffer->GetExternalId(), dst.mBufferInfo_.iFd_);
        return NULL;
    }

    //Alloc rk_hwpq_reg变量
    if (dst_buffer == NULL)
        dst.mRkHwpqReg_ = buffer->GetHwPqRegs().get();
    else
        dst.mRkHwpqReg_ = dst_buffer->GetHwPqRegs().get();

    //设置目标属性
    ret = hwpq_->SetHwPqDstImage(dst);
    if (ret) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq SetHwPqDstImage fail, ret = %d", ctx->GetTunnelId(),
                  buffer->GetExternalId(), ret);
        return NULL;
    }

    //执行PQ
    int output_fence = -1;
    ret = hwpq_->RunHwPqAsync(&output_fence, ctx->DupAcquireFence(buffer->GetExternalId()));
    if (ret) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " .hwPq Run fail ret = %d", ctx->GetTunnelId(),
                  buffer->GetExternalId(), ret);
        return NULL;
    }
    ctx->SetPqAcquireFence(buffer->GetExternalId(), output_fence);

    if (dst_buffer) {
        char value[PROPERTY_VALUE_MAX];
        property_get("vendor.dump", value, "false");
        if (!strcmp(value, "true")) {
            ctx->WaitAcquireFence(buffer->GetExternalId(), 3000);
            ctx->WaitPqAcquireFence(buffer->GetExternalId(), 3000);
            dst_buffer->DumpData(ctx->GetFrameNo(), 0, "S");
        }
        //Update Crop form Query Result
        dst_buffer->SetCrop(dst.mCrop_.iLeft_, dst.mCrop_.iTop_, dst.mCrop_.iRight_, dst.mCrop_.iBottom_);
        return dst_buffer;
    } else {
        return NULL;
    }
}
#endif

// Get Last video buffer
std::shared_ptr<VpComposition> DrmVideoProducer::AcquireVpCompositon(int display_id, int tunnel_id, vt_rect_t* dis_rect,
                                                                     int timeout_ms, bool wait_fence) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mtx_);
    // 更新调试日志
    UpdateLogLevel();

    if (!bInit_) {
        HWC2_LOGE("fail, display=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return NULL;
    }

    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_LOGE("display=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return NULL;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];

    std::shared_ptr<DrmBuffer> acquired_buffer = NULL;

    uint32_t transform = ctx->GetTransform(display_id);

    if (ctx->mMapVpTransformCtx_.count(transform)) {
        std::shared_ptr<VpTransformContext> transform_ctx = ctx->mMapVpTransformCtx_[transform];
        transform_ctx->SetDisplayFrameInfo(display_id, dis_rect);
        std::shared_ptr<VpComposition> composition = transform_ctx->GetVpComposition(wait_fence);
        if (composition != nullptr) {
            ALOGD_IF(LogLevel(DBG_DEBUG), "AcquireVpCompositon display-id=%d Name=%s success! transform=%s", display_id,
                     composition->Name().c_str(), TransformToString(transform).c_str());
            return composition;
        }
    }
    return nullptr;
}

// Get Last video buffer
void DrmVideoProducer::SignalVpCompositon(int display_id, int tunnel_id) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_LOGE("fail, display=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return;
    }

    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_LOGE("display=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];

    std::shared_ptr<DrmBuffer> acquired_buffer = NULL;

    // uint32_t transform = ctx->GetTransform(display_id);
    // if (ctx->mMapVpTransformCtx_.count(transform)) {
    //     std::shared_ptr<VpTransformContext> transform_ctx = ctx->mMapVpTransformCtx_[transform];
    //     transform_ctx->ReleaseUnuseComposition();
    // }

    for (auto& pair_transform_ctx : ctx->mMapVpTransformCtx_) {
        pair_transform_ctx.second->ReleaseUnuseComposition();
    }
    return;
}

int DrmVideoProducer::ReleaseUnuseTunnel() {
    std::unique_lock<std::mutex> lock(mtx_);
    std::vector<int> tunnelShouldRelease;
    //打印释放失败的tunnel，以警告可能存在的内存泄露
    PrintPendingReleaseTunnel();

    //检查当前tunnel状态
    for (auto& map_ctx : mMapCtx_) {
        //如果引用计数为0，则释放tunnel
        if (map_ctx.second->ConnectionCnt() == 0) {
            tunnelShouldRelease.push_back(map_ctx.first);
        }
    }
    //释放需要释放的tunnel
    for (auto tunnel_id : tunnelShouldRelease) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        if (ctx->ReleaseResources() == 0) {
            // HWC2_LOGD_IF("tunnel_id=%d is not connected, release tunnel", tunnel_id);
            int ret = g_rkvt_ops.rk_vt_disconnect(iTunnelFd_, ctx->GetTunnelId(), RKVT_ROLE_CONSUMER);
            if (ret < 0) {
                HWC2_LOGE("rk_vt_disconnect fail tunnel_id=%d, ret=%d.", ctx->GetTunnelId(), ret);
                mPendingReleaseTunnel_.push_back(tunnel_id);
                mMapCtx_.erase(tunnel_id);
                continue;
            }
            mMapCtx_.erase(tunnel_id);
            HWC2_LOGI_IF("tunnel_id=%d relase all buffer and disconnect success!", tunnel_id);
        }
    }
    return 0;
}

std::vector<int> DrmVideoProducer::GetActiveTunnelIdVector() {
    std::unique_lock<std::mutex> lock(mtx_);
    std::vector<int> tunnelShouldAcquire;

    //检查当前tunnel状态
    for (auto& map_ctx : mMapCtx_) {
        //如果引用计数为0，则释放tunnel
        if (map_ctx.second->ConnectionCnt() != 0) {
            tunnelShouldAcquire.push_back(map_ctx.first);
        }
    }
    return tunnelShouldAcquire;
}

// 更新 VpTransformCtx 状态，添加或者删除对应ctx信息
int DrmVideoProducer::UpdateTransformCtx(int tunnel_id) {
    ATRACE_CALL();
    std::unique_lock<std::mutex> lock(mtx_);
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    // 创建新 VpTransformContext
    for (auto pair_transform : ctx->mMapTransform_) {
        // 0 是非法的值
        if (pair_transform.second == 0) {
            continue;
        }
        // transform 是有效的值
        if (pair_transform.second) {
            // 如果未创建 VpTransformContext ，则创建
            if (ctx->mMapVpTransformCtx_.count(pair_transform.second) == 0) {
                ctx->mMapVpTransformCtx_[pair_transform.second] =
                        std::make_shared<VpTransformContext>(tunnel_id, pair_transform.second);
            }
        }
    }

    // 收集需要释放的 transform_ctx
    std::set<uint32_t> unuse_transform_ctx;
    for (auto& pair_transform_ctx : ctx->mMapVpTransformCtx_) {
        bool unuse = true;
        // 如果 mMapVpTransformCtx_ 存在当前 mMapTransform_ 未存在的 transform
        for (auto& pair_transform : ctx->mMapTransform_) {
            if (pair_transform.second == pair_transform_ctx.first) {
                unuse = false;
                break;
            }
        }

        if (unuse) {
            unuse_transform_ctx.insert(pair_transform_ctx.first);
            // 将需要释放的 transform_ctx 存储到 mNeedReleaseCtx_ 结构中
            ctx->mNeedReleaseCtx_.push_back(pair_transform_ctx.second);
        }
    }

    // mMapVpTransformCtx_ 从删除需要释放的 transform_ctx
    for (auto& unuse_transform : unuse_transform_ctx) {
        ctx->mMapVpTransformCtx_.erase(unuse_transform);
    }

    // 依次尝试删除 transform_ctx
    if (ctx->mNeedReleaseCtx_.size() > 0) {
        for (auto it = ctx->mNeedReleaseCtx_.begin(); it != ctx->mNeedReleaseCtx_.end();) {
            // String8 output;
            // (*it)->Dump(output);
            // HWC2_LOGI("%s", output.c_str());
            // 尝试释放所有资源，若失败则下次继续尝试释放
            if ((*it)->TryReleaseAllResource() == 0) {
                it = ctx->mNeedReleaseCtx_.erase(it);
            } else {
                it++;
            }
        }
    }
    return 0;
}
int DrmVideoProducer::DoTransform(int tunnel_id) {
    ATRACE_CALL();
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    for (auto& transform_ctx : ctx->mMapVpTransformCtx_) {
        if (transform_ctx.first == DRM_MODE_ROTATE_0) {
            continue;
        }
        int ret = transform_ctx.second->Handle(ctx->mLatestR0Composition_);
        if (ret) {
            HWC2_LOGI("%s handle fail, ret=%d", TransformToString(transform_ctx.first).c_str(), ret);
        }
    }

    return 0;
}

// 更新 VpTransformCtx 状态，添加或者删除对应ctx信息
int DrmVideoProducer::AcqureBuffer(int tunnel_id) {
    ATRACE_CALL();
    std::unique_lock<std::mutex> lock(mtx_);
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->mLatestR0Composition_ = nullptr;
    //解开锁等待vtunnl送buffer
    lock.unlock();

    // 防止 rk_vt_acquire_buffer 失败进入死锁状态
    if (bLastAcquireSucceed == false) {
        usleep(10 * 1000);
    } else {
        bLastAcquireSucceed = false;
    }

    vt_buffer_t* acquire_buffer = NULL;
    int64_t queue_timestamp = 0;
    //固定等待时间为50ms，如果存在fps<20的情况再另做修改
    int waitTime_ms = 50;
    // 请求最新帧
    int ret = g_rkvt_ops.rk_vt_acquire_buffer(iTunnelFd_, tunnel_id, waitTime_ms, &acquire_buffer, &queue_timestamp);
    //不管是否获取成功均需要重新锁定不然有重复unlock风险
    lock.lock();
    if (ret != 0) {
        HWC2_LOGE("tunnel_id=%d rk_vt_acquire_buffer failed! ret:%d", tunnel_id, ret);
        return -1;
    }
    bLastAcquireSucceed = true;
    uint64_t hnd_buffer_id = acquire_buffer->buffer_id;

    // 获取 buffer cache信息,如果失败直接跳过当前 tunnel-id 后续处理
    bool use_cache = false;
    std::shared_ptr<DrmBuffer> buffer = ctx->GetBufferCache(acquire_buffer, &use_cache);
    if (buffer != NULL && !buffer->initCheck()) {
        HWC2_LOGE("tunnel_id=%d hnd_buffer_id=0x%" PRIx64
                  " DrmBuffer import fail, acquire_buffer=%p queue_timestamp=%" PRIi64 " need to release.",
                  tunnel_id, hnd_buffer_id, acquire_buffer, queue_timestamp);
        // 关闭 AcqureFenceFd
        if (acquire_buffer->rdy_render_fence_fd > 0) {
            close(acquire_buffer->rdy_render_fence_fd);
            acquire_buffer->rdy_render_fence_fd = -1;
        }

        ret = g_rkvt_ops.rk_vt_release_buffer(iTunnelFd_, tunnel_id, acquire_buffer);
        if (ret) {
            HWC2_LOGE("tunnel_id=%d hnd_buffer_id=0x%" PRIx64 " buffer_id=0x%" PRIx64 " release buffer failed, ret=%d.",
                      tunnel_id, hnd_buffer_id, buffer->GetBufferId(), ret);
            ctx->mReleaseFailedBuffer_.push_back(hnd_buffer_id);
        }
        return -1;
    }

    // Setup Acquire Fence
    if (acquire_buffer->rdy_render_fence_fd > 0) {
        buffer->SetFinishFence(acquire_buffer->rdy_render_fence_fd);
        acquire_buffer->rdy_render_fence_fd = -1;
    }

    //将当前buffer的生产者queue buffer时间戳和hwc acquire buffer时间戳记录下来
    std::shared_ptr<VpTransformContext> transform_ctx = ctx->mMapVpTransformCtx_[DRM_MODE_ROTATE_0];
    std::shared_ptr<VpComposition> composition = std::make_shared<VpComposition>(
            tunnel_id, DRM_MODE_ROTATE_0, buffer, ReleaseFence::NO_FENCE, queue_timestamp);
    ret = transform_ctx->Handle(composition);

    // 判断是否需要关闭ReleaseFence
    int disableReleaseFence = hwc_get_int_property("vendor.hwc.disable_releaseFence", "0");

    //If Release Fence is Enabled,
    if (disableReleaseFence > 0) {
        acquire_buffer->fence_fd = -1;
    } else {
        sp<ReleaseFence> release_fence = composition->GetReleaseFence();
        if (release_fence != NULL) {
            acquire_buffer->fence_fd = dup(release_fence->getFd());
        }
    }

    //Now we can release Buffer
    ret = g_rkvt_ops.rk_vt_release_buffer(iTunnelFd_, tunnel_id, acquire_buffer);
    if (ret) {
        HWC2_LOGE("tunnel_id=%d, hnd_buffer_id=0x%" PRIx64 " buffer_id=0x%" PRIx64 " Buffer release failed, ret=%d.",
                  tunnel_id, hnd_buffer_id, buffer->GetBufferId(), ret);
        ctx->mReleaseFailedBuffer_.push_back(hnd_buffer_id);
    }
    ctx->ReleaseBufferInfo(hnd_buffer_id);
    ctx->PrintReleaseFailedBuffer();

    // Add buff to list
    ctx->mLatestR0Composition_ = composition;

    HWC2_LOGD_IF("AcquireBuffer Name=%s success! buffer_id=0x%" PRIx64
                 " use_cache=%d finish=%s  queue_timestamp=%" PRIi64,
                 composition->Name().c_str(), buffer->GetBufferId(), use_cache,
                 buffer->WaitFinishFence(0) == 0 ? "Yes" : "No", queue_timestamp);
    return 0;
}

void DrmVideoProducer::Routine() {
    ATRACE_CALL();
    int ret;

    //Check tunnel status。
    if (!bInit_ || mMapCtx_.size() == 0) {
        Lock();
        WaitForSignalOrExitLocked(-1);
        Unlock();
    }

    // 释放未使用的tunnel
    ReleaseUnuseTunnel();

    // 获取当前激活状态的 tunnel_id vector
    std::vector<int> active_tunnel_id = GetActiveTunnelIdVector();

    // 如果激活状态的 tunnel 为空，则进入50ms sleep，避免死循环
    if (active_tunnel_id.size() == 0) {
        usleep(10 * 1000);
        return;
    }

    bool update = false;
    for (auto tunnel_id : active_tunnel_id) {
        // 更新 VpTransformCtx 状态，添加或者删除对应ctx信息
        UpdateTransformCtx(tunnel_id);
        // 尝试获取下一帧新图像
        if (AcqureBuffer(tunnel_id) == 0) {
            update = true;
            // 执行 DoTransform 操作
            DoTransform(tunnel_id);
        }
    }

    // 如果存在图像更新则输出日志打印
    if (update) {
        Dump();
    }
}

int DrmVideoProducer::SetProducerFps(int tunnel_id, float fps) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock{mtx_};

    //Update tunnel fps for
    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_LOGE("mMapCtx_ can't find tunnel_id=%d", tunnel_id);
        return -1;
    }
    mMapCtx_[tunnel_id]->SetProducerFps(fps);

    return 0;
}

float DrmVideoProducer::GetProducerFps(int tunnel_id) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock{mtx_};

    if (!bInit_) {
        HWC2_LOGE(" fail, bInit_=%d tunnel_id=%d", bInit_, tunnel_id);
        return 60;
    }
    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_LOGE("mMapCtx_ can't find tunnel_id=%d", tunnel_id);
        return 60;
    }

    // 获取 VideoProducer 上下文
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    return ctx->GetProducerFps();
}

void DrmVideoProducer::PrintPendingReleaseTunnel() {
    if (mPendingReleaseTunnel_.size() != 0) {
        char buf[200] = {0};
        int printCount = mPendingReleaseTunnel_.size();

        if (printCount > 5) printCount = 5;

        for (int i = 0; i < printCount; i++) sprintf(buf, "%s,%d", buf, mPendingReleaseTunnel_[i]);

        HWC2_LOGW("Tunnel_id=%s...(total %zu tunnels) pervious disconnect failed! please check!", buf,
                  mPendingReleaseTunnel_.size());
    }
}

void DrmVideoProducer::Dump(String8& output) {
    output.appendFormat("Sideband-2.0 Info Dump: \n");
    output.appendFormat("%s", bInit_ ? "InitSuccess " : "UnInit ");
    if (!bInit_) {
        output.appendFormat("\n");
        return;
    }
    std::vector<int> active_tunnel_id = GetActiveTunnelIdVector();
    std::unique_lock<std::mutex> lock(mtx_);

    output.appendFormat("TunnelFd=%d CtxSize=%zu ActiveCtxSize=%zu \n", iTunnelFd_, mMapCtx_.size(),
                        active_tunnel_id.size());
    for (auto tunnel_id : active_tunnel_id) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        ctx->Dump(output);
        for (auto& pair_transform_ctx : ctx->mMapVpTransformCtx_) {
            if (pair_transform_ctx.second != NULL) {
                pair_transform_ctx.second->Dump(output);
            }
        }
    }
    return;
}

void DrmVideoProducer::Dump() {
    if (LogLevel(DBG_DEBUG) == 0) {
        return;
    }

    std::vector<int> active_tunnel_id = GetActiveTunnelIdVector();
    std::unique_lock<std::mutex> lock(mtx_);
    for (auto tunnel_id : active_tunnel_id) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        String8 output;
        ctx->Dump(output);
        ALOGD("%s", output.c_str());
        output.clear();
        for (auto& pair_transform_ctx : ctx->mMapVpTransformCtx_) {
            if (pair_transform_ctx.second != NULL) {
                pair_transform_ctx.second->Dump(output);
                ALOGD("%s", output.c_str());
                output.clear();
            }
        }
    }
    return;
}

};  // namespace android