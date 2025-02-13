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
#define LOG_TAG "hwc-video-producer"

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
            HWC2_ALOGE("rk_vt_close fail ret=%d", ret);
        }
    }
}

// Init video tunel.
int DrmVideoProducer::Init() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (bInit_) return 0;

    if (InitLibHandle()) {
        HWC2_ALOGE("init fail, disable VideoProducer function.");
        return -1;
    }

    if (iTunnelFd_ < 0) {
        iTunnelFd_ = g_rkvt_ops.rk_vt_open();
        if (iTunnelFd_ < 0) {
            HWC2_ALOGE("rk_vt_open fail ret=%d", iTunnelFd_);
            return -1;
        }
    }

    HWC2_ALOGI("Init success fd=%d", iTunnelFd_);
    bInit_ = true;
    return InitWorker();
}

int DrmVideoProducer::InitLibHandle() {
    g_rkvt_lib_handle = dlopen(RK_LIB_VT_PATH, RTLD_NOW);
    if (g_rkvt_lib_handle == NULL) {
        HWC2_ALOGE("cat not open %s\n", RK_LIB_VT_PATH);
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
            HWC2_ALOGD_IF_ERR(
                    "cat not dlsym open=%p close=%p connect=%p disconnect=%p acquire_buffer=%p release_buffer=%p\n",
                    g_rkvt_ops.rk_vt_open, g_rkvt_ops.rk_vt_close, g_rkvt_ops.rk_vt_connect,
                    g_rkvt_ops.rk_vt_disconnect, g_rkvt_ops.rk_vt_acquire_buffer, g_rkvt_ops.rk_vt_release_buffer);
            return -1;
        }
    }
    HWC2_ALOGI("InitLibHandle %s success!\n", RK_LIB_VT_PATH);
    return 0;
}

bool DrmVideoProducer::IsValid() {
    return bInit_;
}

// Create tunnel connection.
int DrmVideoProducer::CreateConnection(int display_id, int tunnel_id, android_dataspace_t dataspace,
                                       uint32_t transform) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_ALOGE(" fail, display-id=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return -1;
    }

    if (mMapCtx_.count(tunnel_id)) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        ctx->iDataSpace_ = dataspace;
        if (!ctx->AddConnRef(display_id)) {
            HWC2_ALOGI("display-id=%d tunnel_id=%d success, transform = 0x%" PRIx32 ", connections size=%d", display_id,
                       tunnel_id, transform, ctx->ConnectionCnt());
        }
        ctx->SetTransform(display_id, transform);
        return 0;
    }

    int ret = g_rkvt_ops.rk_vt_connect(iTunnelFd_, tunnel_id, RKVT_ROLE_CONSUMER);
    if (ret < 0) {
        return ret;
    }

    HWC2_ALOGI("display-id=%d tunnel_id=%d success, transfrom = 0x%" PRIx32, display_id, tunnel_id, transform);
    mMapCtx_[tunnel_id] = std::make_shared<VpContext>(tunnel_id);
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->AddConnRef(display_id);
    ctx->iDataSpace_ = dataspace;
    ctx->SetTransform(display_id, transform);
    Signal();
    return 0;
}

// Destory Connection
int DrmVideoProducer::DestoryConnection(int display_id, int tunnel_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_ALOGE("fail, display=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return -1;
    }

    if (mMapCtx_.count(tunnel_id) == 0) {
        HWC2_ALOGE("display_id=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return -1;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->ReleaseConnRef(display_id);
    if (ctx->ConnectionCnt() > 0) {
        HWC2_ALOGD_IF_DEBUG("display=%d tunnel_id=%d connection cnt=%d, no need to destory. ", display_id, tunnel_id,
                            ctx->ConnectionCnt());
        return 0;
    }
    HWC2_ALOGD_IF_DEBUG("display=%d tunnel_id=%d connection cnt=%d, going to destory. ", display_id, tunnel_id,
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
                HWC2_ALOGE("Pq module Init Failed, ret=%d", ret);
                hwpq_ = NULL;
            }
        }
    }
    if (hwpq_ == NULL) {
        HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq module not ready! Pq::Get() return NULL",
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
    src.mBufferInfo_.uDataSpace_ = (uint64_t)ctx->iDataSpace_;

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
        HWC2_ALOGD_IF_INFO("DrmGralloc is null, Can not get PQ Metadata");
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
        HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq SetSrcImage fail ret = %d", ctx->GetTunnelId(),
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
            HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Create DstBuffer fail! ret = %d", ctx->GetTunnelId(),
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
        HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " dst buffer not set, dst.mBufferInfo_.iFd_=%d",
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
        HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Pq SetHwPqDstImage fail, ret = %d", ctx->GetTunnelId(),
                   buffer->GetExternalId(), ret);
        return NULL;
    }

    //执行PQ
    int output_fence = -1;
    ret = hwpq_->RunHwPqAsync(&output_fence, ctx->DupAcquireFence(buffer->GetExternalId()));
    if (ret) {
        HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " .hwPq Run fail ret = %d", ctx->GetTunnelId(),
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
            dst_buffer->DumpData();
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
std::shared_ptr<DrmBuffer> DrmVideoProducer::AcquireBuffer(int display_id, int tunnel_id, vt_rect_t* dis_rect,
                                                           int timeout_ms, bool wait_fence) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock(mtx_);

    if (!bInit_) {
        HWC2_ALOGE("fail, display=%d bInit_=%d tunnel-fd=%d", display_id, bInit_, iTunnelFd_);
        return NULL;
    }

    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_ALOGE("display=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return NULL;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];

    std::shared_ptr<DrmBuffer> acquired_buffer = NULL;

    uint32_t transform = ctx->GetTransform(display_id);
    if (transform != DRM_MODE_ROTATE_0) {
        //检查是否有符合旋转的buffer
        if (ctx->mTransfromBuffers_.count(transform) && ctx->mTransfromBuffers_[transform].size() > 0) {
            if (ctx->mTransfromBuffers_[transform].back()) {
                //优先使用最新的buffer，dup出Fence检查是否signal
                acquired_buffer = ctx->mTransfromBuffers_[transform].back();
                int fence_fd = acquired_buffer->GetFinishFence();
                if (fence_fd > 0) {
                    int wait_ret = sync_wait(fence_fd, 0);
                    close(fence_fd);
                    fence_fd = -1;
                    //如果还没signal，则改用旧buffer，并等待fence
                    if (wait_ret) {
                        acquired_buffer = ctx->mTransfromBuffers_[transform].front();
                        int ret = acquired_buffer->WaitFinishFence();
                        if (ret) {
                            HWC2_ALOGE("tunnel_id=%d, display=%d, transform=%" PRIx32
                                       " wait transform fence failed in both back and front buffer! "
                                       "back buffer id=%" PRIx64 ", front buffer id=%" PRIx64,
                                       tunnel_id, display_id, transform,
                                       ctx->mTransfromBuffers_[transform].back()->GetExternalId(),
                                       ctx->mTransfromBuffers_[transform].front()->GetExternalId());
                        }
                    }
                }
            }
            return acquired_buffer;
        } else {
            return nullptr;
        }
    } else {
        //if there is any buffer in list
        if (ctx->lBuffer_.size() > 0) {
            for (auto& b : ctx->lBuffer_) {
                HWC2_ALOGD_IF_VERBOSE("tunnel_id=%d, display=%d, lBuffer_ have buffer:%" PRIu64, tunnel_id, display_id,
                                      b->GetExternalId());
            }

            acquired_buffer = ctx->lBuffer_.back();

#ifdef USE_LIBPQ_HWPQ
#define HWPQ_WAIT_FENCE_MIN_FPS (24)
#define HWPQ_WAIT_FENCE_MAX_FPS (120)
            //获取tunnel帧率
            float fps = ctx->GetProducerFps();
            //限制帧率范围，避免tunnel参数错误导致等待时间计算错误
            if (fps < HWPQ_WAIT_FENCE_MIN_FPS) fps = HWPQ_WAIT_FENCE_MIN_FPS;
            if (fps > HWPQ_WAIT_FENCE_MAX_FPS) fps = HWPQ_WAIT_FENCE_MAX_FPS;

            //默认等待两倍VSYNC时间
            int pq_wait_time = (1000.0f / fps * 2.0) + 0.5;

            //PQ等待逻辑：
            //检查最新buffer是否被signal -> 如果未signal，则更换为上一帧 -> 等待上一帧signal
            //wait 2VSYNC后未signal打警告 -> wait 3000ms ->仍未signal打错误返回NULL；
            if (acquired_buffer->HasHwPqRegs() && !wait_fence) {
                int ret = ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), 0);
                if (ret) {
                    acquired_buffer = ctx->lBuffer_.front();
                    ret = ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), pq_wait_time);
                    if (ret) {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        int64_t time_begin_wait = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
                        HWC2_ALOGW("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64 " HWPQ is not signaled after %dms!!!",
                                   display_id, tunnel_id, acquired_buffer->GetExternalId(), pq_wait_time);
                        ret = ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), 3000);
                        if (ret) {
                            HWC2_ALOGE("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64
                                       " HWPQ is not signaled after 3000ms!!!",
                                       display_id, tunnel_id, acquired_buffer->GetExternalId());
                            return NULL;
                        } else {
                            clock_gettime(CLOCK_MONOTONIC, &ts);
                            int64_t time_end_wait = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
                            HWC2_ALOGW("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64 " HWPQ is signaled after %" PRIi64
                                       "ms!!!",
                                       display_id, tunnel_id, acquired_buffer->GetExternalId(),
                                       pq_wait_time + (time_end_wait - time_begin_wait) / 1000);
                        }
                    }
                }
            }
#endif

            //if wait fence is acquired
            if (wait_fence) {
                int ret = 0;
                //if we have two buffer, try new one first.
                if (ctx->lBuffer_.size() == 2) {
                    //check if new buffer is signaled
                    ret = ctx->WaitAcquireFence(acquired_buffer->GetExternalId(), 0);
#ifdef USE_LIBPQ_HWPQ
                    ret |= ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), 0);
#endif
                    //if not, use old buffer.
                    if (ret) acquired_buffer = ctx->lBuffer_.front();
                }
                //wait acquire fence
                ret = ctx->WaitAcquireFence(acquired_buffer->GetExternalId(), 3000);
                if (ret) {
                    HWC2_ALOGE("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64 " is not signaled after 3000ms!!!",
                               display_id, tunnel_id, acquired_buffer->GetExternalId());
                    return NULL;
                }
#ifdef USE_LIBPQ_HWPQ
                //PQ等待逻辑：
                //wait 2VSYNC后未signal打警告 -> wait 3000ms ->仍未signal打错误返回NULL；
                ret = ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), pq_wait_time);
                if (ret) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    int64_t time_begin_wait = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
                    HWC2_ALOGW("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64 " HWPQ is not signaled after %dms!!!",
                               display_id, tunnel_id, acquired_buffer->GetExternalId(), pq_wait_time);
                    ret = ctx->WaitPqAcquireFence(acquired_buffer->GetExternalId(), 3000);
                    if (ret) {
                        HWC2_ALOGE("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64
                                   " HWPQ is not signaled after 3000ms!!!",
                                   display_id, tunnel_id, acquired_buffer->GetExternalId());
                        return NULL;
                    } else {
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        int64_t time_end_wait = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
                        HWC2_ALOGW("display-id=%d tunnel_id=%d Buffer 0x%" PRIx64 " HWPQ is signaled after %" PRIi64
                                   "ms!!!",
                                   display_id, tunnel_id, acquired_buffer->GetExternalId(),
                                   pq_wait_time + (time_end_wait - time_begin_wait) / 1000);
                    }
                }
#endif
            }
#ifdef USE_LIBPQ_HWPQ
            HWC2_ALOGD_IF_DEBUG("tunnel_id=%d, display=%d, acquired buffer:%" PRIu64
                                " , %s Hwpq Regs, add Release fence Reference",
                                tunnel_id, display_id, acquired_buffer->GetExternalId(),
                                acquired_buffer->HasHwPqRegs() ? "with" : "without");
#else
            HWC2_ALOGD_IF_VERBOSE("tunnel_id=%d, display=%d, acquired buffer:%" PRIu64 " ,add Release fence Reference",
                                  tunnel_id, display_id, acquired_buffer->GetExternalId());
#endif
            //release fence add refCount
            ctx->AddReleaseFenceRefCnt(display_id, acquired_buffer->GetExternalId());

            return acquired_buffer;
        } else {
            return NULL;
        }
    }
}

// Release video buffer
int DrmVideoProducer::SignalReleaseFence(int display_id, int tunnel_id, uint64_t buffer_id) {
    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mtx_);
    if (!bInit_) {
        HWC2_ALOGE(" fail, display=%d bInit_=%d tunnel_id=%d", display_id, bInit_, tunnel_id);
        return -1;
    }

    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_ALOGE("display=%d mMapCtx_ can't find tunnel_id=%d", display_id, tunnel_id);
        return -1;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    return ctx->SignalReleaseFence(display_id, buffer_id);
    ;
}

int DrmVideoProducer::DoTransform(std::shared_ptr<VpContext> ctx, std::shared_ptr<DrmBuffer> src_buffer,
                                  std::set<uint32_t> transforms) {
    ATRACE_CALL();
    int ret = 0;

    // 1. 初始化RGA变量
    rga_buffer_t src;
    rga_buffer_t dst;
    rga_buffer_t pat;
    im_rect src_rect;
    im_rect dst_rect;
    im_rect pat_rect;
    memset(&src, 0, sizeof(rga_buffer_t));
    memset(&dst, 0, sizeof(rga_buffer_t));
    memset(&pat, 0, sizeof(rga_buffer_t));
    memset(&src_rect, 0, sizeof(im_rect));
    memset(&dst_rect, 0, sizeof(im_rect));
    memset(&pat_rect, 0, sizeof(im_rect));

    int mergedReleaseFence = -1;

    std::set<uint32_t> unused_tf;
    for (auto& tfbq : ctx->mapTransformBufferQueue_) {
        if (transforms.count(tfbq.first) == 0) {
            unused_tf.emplace(tfbq.first);
        }
    }
    for (auto tf : unused_tf) ctx->mapTransformBufferQueue_.erase(tf);

    // 3.针对每个旋转类型进行旋转
    for (uint32_t transform : transforms) {
        // 3.1 查找或创建BufferQueue
        std::shared_ptr<DrmBufferQueue> bufferQueue;
        if (ctx->mapTransformBufferQueue_.count(transform) && ctx->mapTransformBufferQueue_[transform]) {
            bufferQueue = ctx->mapTransformBufferQueue_[transform];
        } else {
            bufferQueue = std::make_shared<DrmBufferQueue>(4);
            if (bufferQueue) {
                ctx->mapTransformBufferQueue_[transform] = bufferQueue;
            } else {
                HWC2_ALOGE("DVP_Transform: BufferQueue create failed!");
                continue;
            }
        }

        // 3.2 Set src buffer info
        int src_format = -1;
        if (gIsRK3588()) {
            if (!hwc_rga_utils::isRK3588RGA3SupportFormat(src_buffer->GetFormat())) {
                HWC2_ALOGE("RK3588 RGA3 do not support this format, transform failed!");
                return -1;
            } else {
                src_format = hwc_rga_utils::HwcGetRgaFormat(
                        hwc_rga_utils::UnifyAndroidFormatForRK3588(src_buffer->GetFormat()));
            }
        } else if (gIsRK3576()) {
            if (!hwc_rga_utils::isRK3576RGA2SupportFormat(src_buffer->GetFormat())) {
                HWC2_ALOGE("RK3576 RGA2.5 do not support this format, transform failed!");
                return -1;
            } else {
                src_format = hwc_rga_utils::HwcGetRgaFormat(
                        hwc_rga_utils::UnifyAndroidFormatForRK3576(src_buffer->GetFormat()));
            }
        } else {
            //目前RGA格式匹配，3576格式最全，默认使用此转换函数
            src_format =
                    hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3576(src_buffer->GetFormat()));
        }
        if (src_format == -1) src_format = src_buffer->GetFormat();

        // RGA 的特殊修改，需要通过 wstride
        int src_stride;
        if (src_buffer->GetFourccFormat() == DRM_FORMAT_NV15)
            src_stride = src_buffer->GetByteStride();
        else
            src_stride = src_buffer->GetStride();

        src = wrapbuffer_handle(src_buffer->GetRgaHandle(), src_buffer->GetWidth(), src_buffer->GetHeight(), src_format,
                                src_stride, src_buffer->GetHeightStride());
        // AFBC format
        src.rd_mode = 0;
        if (fourcc_mod_is_vendor(src_buffer->GetModifier(), ARM)) {
            if (gIsRK3576()) {
                src.rd_mode = IM_AFBC32x8_MODE;
            } else {
                src.rd_mode = IM_FBC_MODE;
            }
        } else if (IS_ROCKCHIP_RFBC_MOD(src_buffer->GetModifier())) {
            src.rd_mode = IM_RKFBC64x4_MODE;
        }

        // Set src rect info
        int left, top, right, bottom;
        src_buffer->GetCrop(&left, &top, &right, &bottom);

        src_rect.x = ALIGN_DOWN((int)left, 2);
        src_rect.y = ALIGN_DOWN((int)top, 2);
        src_rect.width = ALIGN_DOWN((int)(right - left), 2);
        src_rect.height = ALIGN_DOWN((int)(bottom - top), 2);

        // 3.3 设置目标buffer属性
        int dst_width = src_buffer->GetWidth();
        int dst_height = src_buffer->GetHeight();
        if ((transform == DRM_MODE_ROTATE_90) || (transform & DRM_MODE_ROTATE_270)) {
            std::swap(dst_height, dst_width);
        }

        int dst_buf_format = src_buffer->GetFormat();
        if (dst_buf_format == HAL_PIXEL_FORMAT_YUV420_8BIT_RFBC ||
            dst_buf_format == HAL_PIXEL_FORMAT_YUV422_8BIT_RFBC ||
            dst_buf_format == HAL_PIXEL_FORMAT_YUV444_8BIT_RFBC || dst_buf_format == HAL_PIXEL_FORMAT_YUV420_8BIT_I ||
            dst_buf_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
            dst_buf_format = HAL_PIXEL_FORMAT_YCrCb_NV12;
        } else if (dst_buf_format == HAL_PIXEL_FORMAT_YUV420_10BIT_RFBC ||
                   dst_buf_format == HAL_PIXEL_FORMAT_YUV422_10BIT_RFBC ||
                   dst_buf_format == HAL_PIXEL_FORMAT_YUV420_10BIT_I) {
            dst_buf_format = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
        }

        uint64_t dst_buf_usage = RK_GRALLOC_USAGE_STRIDE_ALIGN_64 | MALI_GRALLOC_USAGE_NO_AFBC;
        //检查当前平台RGA是否支持4G以上内存，如果不支持，则申请4G以内内存
        if (!ResourceManager::getInstance()->GetRgaSupportAbove4GB()) {
            dst_buf_usage |= RK_GRALLOC_USAGE_WITHIN_4G;
        }

        std::shared_ptr<DrmBuffer> dst_buffer =
                bufferQueue->DequeueDrmBuffer(dst_width, dst_height, dst_buf_format, dst_buf_usage, "DVP_TF-target");
        if (!dst_buffer) {
            HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64
                       " Dequeue DstBuffer fail! wxh=(%dx%d),format=%d,usage = 0x%" PRIx64,
                       ctx->GetTunnelId(), src_buffer->GetExternalId(), dst_width, dst_height, dst_buf_format,
                       dst_buf_usage);
            continue;
        }
        // Set dst buffer info
        // RGA 的特殊修改，需要通过 wstride
        int dst_stride;
        if (dst_buffer->GetFourccFormat() == DRM_FORMAT_NV15)
            dst_stride = dst_buffer->GetByteStride();
        else
            dst_stride = dst_buffer->GetStride();

        int dst_format;
        if (gIsRK3588()) {
            if (!hwc_rga_utils::isRK3588RGA3SupportFormat(dst_buf_format)) {
                HWC2_ALOGE("RK3588 RGA3 do not support this format, transform failed!");
                return -1;
            } else {
                dst_format = hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3588(dst_buf_format));
            }
        } else if (gIsRK3576()) {
            if (!hwc_rga_utils::isRK3576RGA2SupportFormat(dst_buf_format)) {
                HWC2_ALOGE("RK3576 RGA2.5 do not support this format, transform failed!");
                return -1;
            } else {
                dst_format = hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3576(dst_buf_format));
            }
        } else {
            //目前RGA格式匹配，3576格式最全，默认使用此转换函数
            dst_format = hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3576(dst_buf_format));
        }
        if (dst_format == -1) dst_format = dst_buf_format;

        dst = wrapbuffer_handle(dst_buffer->GetRgaHandle(), dst_buffer->GetWidth(), dst_buffer->GetHeight(), dst_format,
                                dst_stride, dst_buffer->GetHeightStride());

        dst_rect.x = 0;
        dst_rect.y = 0;
        dst_rect.width = ALIGN_DOWN((int)(dst_width), 2);
        dst_rect.height = ALIGN_DOWN((int)(dst_height), 2);

        dst_buffer->SetCrop(0, 0, dst_rect.width, dst_rect.height);
        dst_buffer->SetExternalId(src_buffer->GetExternalId());
        int usage = 0;
        // 处理旋转
        switch (transform) {
            case DRM_MODE_ROTATE_0:
                usage = 0;
                break;
            case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X:
                usage = IM_HAL_TRANSFORM_FLIP_H;
                break;
            case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y:
                usage = IM_HAL_TRANSFORM_FLIP_V;
                break;
            case DRM_MODE_ROTATE_90:
                usage = IM_HAL_TRANSFORM_ROT_90;
                break;
            case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y:
                usage = IM_HAL_TRANSFORM_ROT_180;
                break;
            case DRM_MODE_ROTATE_270:
                usage = IM_HAL_TRANSFORM_ROT_270;
                break;
            // RGA2/RGA3的 flip + rotate 场景，硬件内部处理是先 rotate 再 flip
            // 而 Android 请求的是先 flip 再 rotate，故此请求需要做转换
            // Android请求 flip-v + rotate-90  等价于 rotate-90 + flip-h
            case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90:
                usage = IM_HAL_TRANSFORM_ROT_90 | IM_HAL_TRANSFORM_FLIP_H;
                break;
            // Android请求 flip-h + rotate-90  等价于 rotate-90 + flip-v
            case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90:
                usage = IM_HAL_TRANSFORM_ROT_90 | IM_HAL_TRANSFORM_FLIP_V;
                break;
            default:
                usage = 0;
                ALOGE_IF(LogLevel(DBG_DEBUG), "Unknow sf transform 0x%x", transform);
        }

        IM_STATUS im_state;
        // Call Im2d 格式转换
        im_state = imcheck_composite(src, dst, pat, src_rect, dst_rect, pat_rect, usage | IM_ASYNC);
        if (im_state != IM_STATUS_NOERROR) {
            HWC2_ALOGE("call im2d scale fail, %s", imStrError(im_state));
            bufferQueue->QueueBuffer(dst_buffer);
            continue;
        }

        int i = 0;
        int acquire_fence = ctx->DupAcquireFence(src_buffer->GetExternalId());

        // GetEnableRgaAcquireFence 查询是否使用RGA的AcquireFence支持，
        // 由属性 vendor.hwc.enable_rga_acquire_fence控制，默认为0，即不使用RGA AcquireFence支持
        // 不使用RGA AcquireFence支持时，即使多屏中存在无旋转屏幕，也不再支持低延迟送显功能
        //
        // RGA驱动版本为1.3.5及之前，在使用HDMI-in传递的AcquireFence输入时，可能会出现内核崩溃问题。
        // 若确认内核驱动已支持，可设置vendor.hwc.enable_rga_acquire_fence=1启用RGA的AcquireFence.

        if (acquire_fence > 0) {
            if (!ResourceManager::getInstance()->GetEnableRgaAcquireFence()) {
                int ret = sync_wait(acquire_fence, 500);
                if (ret) {
                    HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64
                               " Wait AcqurieFence 500ms Failed! fence_fd = %d, ret = %d",
                               ctx->GetTunnelId(), src_buffer->GetExternalId(), acquire_fence, ret);
                }
                close(acquire_fence);
                acquire_fence = -1;
            } else {
                HWC2_ALOGD_IF_VERBOSE("RGA acquire fence is enabled, fence_fd = %d", acquire_fence);
            }
        } else {
            HWC2_ALOGD_IF_VERBOSE("RGA acquire fence dup failed or signaled, fence_fd = %d", acquire_fence);
        }

        int releaseFence = -1;
        im_opt_t imOpt;
        memset(&imOpt, 0x00, sizeof(im_opt_t));
        if (gIsRK3588()) {
            imOpt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
        }
        im_state = improcess(src, dst, pat, src_rect, dst_rect, pat_rect, acquire_fence, &releaseFence, &imOpt,
                             usage | IM_ASYNC);
        if (im_state != IM_STATUS_SUCCESS) {
            HWC2_ALOGE("call im2d scale fail, %s", imStrError(im_state));
            bufferQueue->QueueBuffer(dst_buffer);
            if (releaseFence > 0) {
                close(releaseFence);
            }
            continue;
        }

        if (releaseFence > 0) {
            dst_buffer->SetFinishFence(releaseFence);
            //由于可能存在多种旋转角度，进行多次旋转，需要将多次旋转ReleaseFence Merge到一起，
            //以确保在释放源buffer前等待所有旋转操作处理完成
            if (mergedReleaseFence > 0) {
                int fd = sync_merge("DVP_TF_Fence", mergedReleaseFence, releaseFence);
                if (fd > 0) {
                    close(mergedReleaseFence);
                    mergedReleaseFence = fd;
                }
            } else {
                mergedReleaseFence = dup(releaseFence);
            }
        }

        char value[PROPERTY_VALUE_MAX];
        property_get("vendor.dump", value, "false");
        if (!strcmp(value, "true")) {
            dst_buffer->DumpData();
        }

        bufferQueue->QueueBuffer(dst_buffer);
    }

    if (mergedReleaseFence > 0) {
        src_buffer->SetFinishFence(mergedReleaseFence);
    }
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

    std::vector<int> tunnelShouldRelease;
    std::vector<int> tunnelShouldAcquire;

    std::unique_lock<std::mutex> lock(mtx_);

    //打印释放失败的tunnel，以警告可能存在的内存泄露
    printPendingReleaseTunnel();

    //检查当前tunnel状态
    for (auto& map_ctx : mMapCtx_) {
        //如果引用计数为0，则释放tunnel
        if (map_ctx.second->ConnectionCnt() == 0) {
            tunnelShouldRelease.push_back(map_ctx.first);
        } else {
            tunnelShouldAcquire.push_back(map_ctx.first);
        }
    }

    //释放需要释放的tunnel
    for (auto tunnel_id : tunnelShouldRelease) {
        HWC2_ALOGD_IF_DEBUG("Tunnel %d is not connected, release tunnel", tunnel_id);
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
        while (ctx->lBuffer_.size() > 0) {
            std::shared_ptr<DrmBuffer> frontBuffer = ctx->lBuffer_.front();
            uint64_t buffer_id = frontBuffer->GetExternalId();
            //Wait for DoTransform RGA finish
            int ret = frontBuffer->WaitFinishFence();
            if (ret) {
                HWC2_ALOGE("tunnel_id=%d wait transform fence failed!", tunnel_id);
            }

            if (ctx->WaitPqAcquireFence(buffer_id, 1500)) {
                HWC2_ALOGE("tunnel_id=%d Wait Pq AcquireFence 1500ms Failed!", ctx->GetTunnelId());
            }

            ctx->SignalReleaseFence(-1, buffer_id);

            ctx->lBuffer_.pop_front();
        }
        int ret = g_rkvt_ops.rk_vt_disconnect(iTunnelFd_, ctx->GetTunnelId(), RKVT_ROLE_CONSUMER);
        if (ret < 0) {
            HWC2_ALOGE("rk_vt_disconnect fail TunnelId=%d, ret=%d.", ctx->GetTunnelId(), ret);
            mPendingReleaseTunnel_.push_back(tunnel_id);
            mMapCtx_.erase(tunnel_id);
            continue;
        }
        mMapCtx_.erase(tunnel_id);
        HWC2_ALOGD_IF_DEBUG("tunnel_id=%d disconnect success! ", tunnel_id);
    }

    if (tunnelShouldAcquire.size() > 0) {
        //如果上次获取失败，等待10ms防止死循环
        if (!bLastAcquireSucceed) {
            usleep(10000);
        }
        //reset bLastAcquireSucceed flag
        bLastAcquireSucceed = false;
    }

    //当前活动的tunnel，进行acquire buffer操作
    for (auto tunnel_id : tunnelShouldAcquire) {
        std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];

        // 请求最新帧
        vt_buffer_t* acquire_buffer = NULL;
        int64_t queue_timestamp = 0;
        uint64_t buffer_id = 0;
        int acquire_fence_fd = -1;

        //固定等待时间为50ms，如果存在fps<20的情况再另做修改
        int waitTime_ms = 50;

        //解开锁等待vtunnl送buffer
        lock.unlock();
        ret = g_rkvt_ops.rk_vt_acquire_buffer(iTunnelFd_, ctx->GetTunnelId(), waitTime_ms, &acquire_buffer,
                                              &queue_timestamp);
        //不管是否获取成功均需要重新锁定不然有重复unlock风险
        lock.lock();
        if (ret != 0) {
            HWC2_ALOGE("tunnel_id=%d rk_vt_acquire_buffer failed! ret:%d", ctx->GetTunnelId(), ret);
            continue;
        }
        bLastAcquireSucceed = true;
        buffer_id = acquire_buffer->buffer_id;
        acquire_fence_fd = acquire_buffer->rdy_render_fence_fd;

        // 获取 buffer cache信息
        std::shared_ptr<DrmBuffer> buffer = ctx->GetBufferCache(acquire_buffer);
        if (buffer != NULL && !buffer->initCheck()) {
            HWC2_ALOGE("tunnel_id=%d buffer_id:0x%" PRIx64
                       " DrmBuffer import fail, "
                       "acquire_buffer=%p present_time=%" PRIi64,
                       ctx->GetTunnelId(), acquire_buffer->buffer_id, acquire_buffer, queue_timestamp);

            //release之后buffer_id不一定还能访问到，提前存一份buffer_id
            uint64_t buffer_id = acquire_buffer->buffer_id;
            ret = g_rkvt_ops.rk_vt_release_buffer(iTunnelFd_, ctx->GetTunnelId(), acquire_buffer);
            if (ret) {
                HWC2_ALOGE("tunnel_id=%d BufferId=0x%" PRIx64 " release buffer failed, ret=%d.", ctx->GetTunnelId(),
                           buffer_id, ret);
                ctx->mReleaseFailedBuffer_.push_back(buffer_id);
            }
            continue;
        }

        //Setup Acquire Fence
        if (acquire_buffer->rdy_render_fence_fd > 0) {
            ctx->SetAcquireFence(acquire_buffer->buffer_id, acquire_buffer->rdy_render_fence_fd);
            acquire_buffer->rdy_render_fence_fd = -1;
        }

        //将当前buffer的生产者queue buffer时间戳和hwc acquire buffer时间戳记录下来
        ctx->SetTimeStamp(acquire_buffer->buffer_id, queue_timestamp);

        // 创建 ReleaseFence
        ret = ctx->AddReleaseFence(acquire_buffer->buffer_id);
        if (ret) {
            HWC2_ALOGE("tunnel_id=%d BufferId=0x%" PRIx64 " AddReleaseFence fail, ret=%d.", ctx->GetTunnelId(),
                       acquire_buffer->buffer_id, ret);

            //release之后buffer_id不一定还能访问到，提前存一份buffer_id
            uint64_t buffer_id = acquire_buffer->buffer_id;

            ret = g_rkvt_ops.rk_vt_release_buffer(iTunnelFd_, ctx->GetTunnelId(), acquire_buffer);
            if (ret) {
                HWC2_ALOGE("tunnel_id=%d BufferId=0x%" PRIx64 " release buffer failed, ret=%d.", ctx->GetTunnelId(),
                           buffer_id, ret);
                ctx->mReleaseFailedBuffer_.push_back(buffer_id);
            }
            continue;
        }

        ret = ctx->AddReleaseFenceRefCnt(-1, acquire_buffer->buffer_id);
        if (ret) {
            HWC2_ALOGE("tunnel_id=%d BufferId=0x%" PRIx64 " AddReleaseFenceRefCnt fail, ret=%d.", ctx->GetTunnelId(),
                       acquire_buffer->buffer_id, ret);
        }

        int disableReleaseFence = hwc_get_int_property("vendor.hwc.disable_releaseFence", "0");
        if (disableReleaseFence)
            iFenceMode_ = DisableReleaseFence;
        else
            iFenceMode_ = EnableReleaseFence;

        //If Release Fence is Enabled,
        if (iFenceMode_ == EnableReleaseFence) {
            sp<ReleaseFence> release_fence = ctx->GetReleaseFence(acquire_buffer->buffer_id);
            if (release_fence != NULL) {
                acquire_buffer->fence_fd = dup(release_fence->getFd());
                HWC2_ALOGD_IF_DEBUG("tunnel_id=%d buffer_id=0x%" PRIx64 " release fence:%d acquire_buffer->fence_fd=%d",
                                    ctx->GetTunnelId(), acquire_buffer->buffer_id, release_fence->getFd(),
                                    acquire_buffer->fence_fd);
            }
        } else {
            acquire_buffer->fence_fd = -1;
        }

        //Now we can release Buffer
        ret = g_rkvt_ops.rk_vt_release_buffer(iTunnelFd_, ctx->GetTunnelId(), acquire_buffer);
        if (ret) {
            HWC2_ALOGE("tunnel_id=%d, buffer_id=0x%" PRIx64 " Buffer release failed, ret=%d.", ctx->GetTunnelId(),
                       buffer_id, ret);
            ctx->mReleaseFailedBuffer_.push_back(buffer_id);
        }
        ctx->ReleaseBufferInfo(buffer_id);
        ctx->PrintReleaseFailedBuffer();

#ifdef USE_LIBPQ_HWPQ
        std::shared_ptr<DrmBuffer> originalBuffer = buffer;
        if (gIsRK3576()) {
            int hwpq_mode = hwc_get_int_property("persist.vendor.tvinput.rkpq.mode", "0");
            if (hwpq_mode == 2) {
                HWC2_ALOGD_IF_DEBUG("persist.vendor.tvinput.rkpq.mode =%d Do pq", hwpq_mode);
                auto hwpq_buffer = DoHwPq(ctx, buffer);

                if (hwpq_buffer != NULL) {
                    hwpq_buffer->SetExternalId(buffer->GetExternalId());
                    buffer = hwpq_buffer;
                }
            } else {
                buffer->RemoveHwPqRegs();
            }
        }
#endif

        // 收集所需要的旋转类型
        std::set<uint32_t> transforms;
        for (auto tfpair : ctx->mTransform_) {
            if (tfpair.second != DRM_MODE_ROTATE_0 && tfpair.second != 0) transforms.emplace(tfpair.second);
        }

        //DoTransform可能耗时，解锁进行，同时函数内确保线程安全。
        lock.unlock();
        auto getTimeStamp = []() {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return (int64_t)ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
        };
        int64_t begin_tf_timestamp = getTimeStamp();
#ifdef USE_LIBPQ_HWPQ
        ret = DoTransform(ctx, originalBuffer, transforms);
#else
        ret = DoTransform(ctx, buffer, transforms);
#endif
        int64_t end_tf_timestamp = getTimeStamp();
        if (ret) {
            HWC2_ALOGE("DoTransform failed, ret = %d", ret);
        }
        if (LogLevel(DBG_DEBUG) && transforms.size() > 0) {
            std::ostringstream stream;
            stream << "Sideband-Transform:Collect transform: ";
            for (auto tf : transforms) {
                stream << "0x" << std::hex << tf << ", ";
            }
            stream << "transform time: " << std::dec << (end_tf_timestamp - begin_tf_timestamp) << "us";
            HWC2_ALOGD_IF_DEBUG("%s", stream.str().c_str());
        }

        lock.lock();
        //将DoTransform旋转的buffer存放到队列中，队列长度为2，旧的一帧理论上已做完转换，保证acquirebuffer过程尽快返回。
        for (uint32_t transform : transforms) {
            if (ctx->mapTransformBufferQueue_.count(transform) && ctx->mapTransformBufferQueue_[transform]) {
                std::shared_ptr<DrmBuffer> tf_buffer = ctx->mapTransformBufferQueue_[transform]->BackDrmBuffer();

                if (ctx->mTransfromBuffers_.count(transform)) {
                    ctx->mTransfromBuffers_[transform].push_back(tf_buffer);
                    //只保留最新两帧，如果队列长度>2则释放最旧一帧
                    if (ctx->mTransfromBuffers_[transform].size() > 2) ctx->mTransfromBuffers_[transform].pop_front();
                } else {
                    //第一帧，新建list
                    ctx->mTransfromBuffers_[transform] = std::list<std::shared_ptr<DrmBuffer>>({tf_buffer});
                }
            }

            if (ctx->mTransfromBuffers_.count(transform) && ctx->mTransfromBuffers_[transform].size() > 0) {
                if (LogLevel(DBG_DEBUG)) {
                    std::ostringstream stream;
                    stream << "Sideband-Transform:Transform 0x" << std::hex << transform;
                    stream << " have buffer ";
                    for (auto buffer : ctx->mTransfromBuffers_[transform]) {
                        stream << "0x" << std::hex << buffer->GetExternalId() << ",";
                    }
                    HWC2_ALOGD_IF_DEBUG("%s", stream.str().c_str());
                }
            } else {
                HWC2_ALOGE("Sideband-Transform:Transform 0x%" PRIx32 " buffer list is empty, transform may failed",
                           transform);
            }
        }

        std::set<uint32_t> unused_tf;
        for (auto& tfbuffer : ctx->mTransfromBuffers_) {
            if (transforms.count(tfbuffer.first) == 0) {
                unused_tf.emplace(tfbuffer.first);
            }
        }
        for (auto tf : unused_tf) ctx->mTransfromBuffers_.erase(tf);

        //Add buff to list
        ctx->lBuffer_.push_back(buffer);
        //if more than 2 buffer release old one
        while (ctx->lBuffer_.size() > 2) {
            std::shared_ptr<DrmBuffer> frontBuffer = ctx->lBuffer_.front();
            uint64_t buffer_id = frontBuffer->GetExternalId();
            //Wait for DoTransform RGA process done
            int ret = frontBuffer->WaitFinishFence();
            if (ret) {
                HWC2_ALOGE("tunnel_id=%d wait transform fence failed!", tunnel_id);
            }
            //Signal Release Fence
            if (ctx->WaitPqAcquireFence(buffer_id, 1500)) {
                HWC2_ALOGE("tunnel_id=%d Wait Pq AcquireFence 1500ms Failed!", ctx->GetTunnelId());
            }
            ctx->SignalReleaseFence(-1, buffer_id);

            ctx->lBuffer_.pop_front();
        }

        HWC2_ALOGD_IF_DEBUG("Buffer acquire success. tunnel_id=%d buffer_id:0x%" PRIx64 " fence_fd:%d",
                            ctx->GetTunnelId(), buffer_id, acquire_fence_fd);
    }
}

int DrmVideoProducer::SetProducerFps(int tunnel_id, float fps) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock{mtx_};

    //Update tunnel fps for
    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_ALOGE("mMapCtx_ can't find tunnel_id=%d", tunnel_id);
        return -1;
    }
    mMapCtx_[tunnel_id]->SetProducerFps(fps);

    return 0;
}

float DrmVideoProducer::GetProducerFps(int tunnel_id) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock{mtx_};

    if (!bInit_) {
        HWC2_ALOGE(" fail, bInit_=%d tunnel_id=%d", bInit_, tunnel_id);
        return 60;
    }
    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_ALOGE("mMapCtx_ can't find tunnel_id=%d", tunnel_id);
        return 60;
    }

    // 获取 VideoProducer 上下文
    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    return ctx->GetProducerFps();
}

void DrmVideoProducer::PrintTimeStamp(int display_id, int tunnel_id, uint64_t buffer_id) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> lock{mtx_};

    if (!bInit_) {
        HWC2_ALOGE(" fail, bInit_=%d tunnel_id=%d", bInit_, tunnel_id);
        return;
    }

    if (!mMapCtx_.count(tunnel_id)) {
        HWC2_ALOGE("mMapCtx_ can't find tunnel_id=%d", tunnel_id);
        return;
    }

    std::shared_ptr<VpContext> ctx = mMapCtx_[tunnel_id];
    ctx->VpPrintTimestamp(display_id, buffer_id);

    return;
}

void DrmVideoProducer::printPendingReleaseTunnel() {
    if (mPendingReleaseTunnel_.size() != 0) {
        char buf[200] = {0};
        int printCount = mPendingReleaseTunnel_.size();

        if (printCount > 5) printCount = 5;

        for (int i = 0; i < printCount; i++) sprintf(buf, "%s,%d", buf, mPendingReleaseTunnel_[i]);

        HWC2_ALOGW("Tunnel_id=%s...(total %zu tunnel(s)) pervious disconnect failed!", buf,
                   mPendingReleaseTunnel_.size());
    }
}

};  // namespace android