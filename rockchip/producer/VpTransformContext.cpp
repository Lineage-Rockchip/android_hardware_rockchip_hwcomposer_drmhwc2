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

#include "rockchip/producer/VpTransformContext.h"
#include "resources/resourcemanager.h"
#include "rockchip/utils/drmdebug.h"
#include "rockchip/utils/rgautils.h"
#include "utils/drmfence.h"

#include <inttypes.h>
#include <utils/Trace.h>

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

namespace android {

static int64_t SidebandGetTimestampUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t current_time_us = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
    return current_time_us;
}

VpComposition::VpComposition(int tunnel_id, uint32_t transform, std::shared_ptr<DrmBuffer> buffer,
                             sp<ReleaseFence> release_fence, int64_t queue_timestamp_us)
    : mTunnelId_(tunnel_id),
      mTransform_(transform),
      mFrameNo_(0),
      mBuffer_(buffer),
      mPqBuffer_(nullptr),
      mAcquireFence_(AcquireFence::NO_FENCE),
      mReleaseFence_(release_fence),
      mQueueTimestampUs_(queue_timestamp_us),
      mCreateTimestampUs_(SidebandGetTimestampUs()),
      mCommitTimestampUs_(0) {};

VpComposition::~VpComposition() {
    if (mReleaseFence_ != NULL) {
        int act = mReleaseFence_->getActiveCount();
        int sig = mReleaseFence_->getSignaledCount();
        mReleaseFence_->signal();
        ALOGD_IF(LogLevel(DBG_DEBUG),
                 "~VpComposition Signal %s Name=%s Info: size=%d act=%d signal=%d "
                 "err=%d SignalTime=%s",
                 (act == 1 && sig == 0 && mReleaseFence_->getActiveCount() == 0 &&
                  mReleaseFence_->getSignaledCount() == 1)
                         ? "Sucess"
                         : "Fail",
                 mReleaseFence_->getName().c_str(), mReleaseFence_->getSize(), mReleaseFence_->getActiveCount(),
                 mReleaseFence_->getSignaledCount(), mReleaseFence_->getErrorCount(), mReleaseFence_->dump().c_str());
        mReleaseFence_ = NULL;
    }
}

void VpComposition::MarkCommitTimestamp() {
    if (mCommitTimestampUs_ == 0) {
        mCommitTimestampUs_ = SidebandGetTimestampUs();
    }
    return;
}

void VpComposition::Dump(String8& output) {
    output.appendFormat("\t");
    output.appendFormat(" %8" PRIu64 " |", mFrameNo_);
    output.appendFormat("  B%-5" PRIx64 " |", mBuffer_->GetExternalId());
    output.appendFormat("  0x%-12" PRIx64 " |", mBuffer_->GetBufferId());
    output.appendFormat(" %5" PRId32 " |", mBuffer_->GetFbId());
    output.appendFormat(" %c%c%c%c |", mBuffer_->GetFourccFormat(), mBuffer_->GetFourccFormat() >> 8,
                        mBuffer_->GetFourccFormat() >> 16, mBuffer_->GetFourccFormat() >> 24);
    output.appendFormat(" %6d x%7d |", mBuffer_->GetStride(), mBuffer_->GetHeightStride());
    output.appendFormat(" %5s |", mBuffer_->WaitFinishFence(0) == 0 ? "Yes" : "No");
    output.appendFormat(" %7s |", mCommitTimestampUs_ > 0 ? "Yes" : "No");
    output.appendFormat(" %8.2f |", (mCreateTimestampUs_ - mQueueTimestampUs_) / 1000.0);
    if (mCommitTimestampUs_ > 0) {
        output.appendFormat(" %8.2f |", (mCommitTimestampUs_ - mQueueTimestampUs_) / 1000.0);
    } else {
        output.appendFormat(" %8s |", "NoP");
    }
    output.appendFormat(" %s", mName_.c_str());
    output.appendFormat("\n");
}

VpTransformContext::VpTransformContext(int tunnel_id, uint32_t transform, android_dataspace_t dataspace)
    : mTunnelId_(tunnel_id),
      mTransform_(transform),
      mFenceTag_("VP-" + TransformToString(mTransform_)),
      uFrameNo_(0),
      mBufferQueue_(std::make_shared<DrmBufferQueue>(4)),
      mSrEnableOnelineMode_(false),
      mSrAuthTimeline_(0),
      mLastFpsFrameNo_(0),
      mLastFpsTimestampUs_(0),
      mFps_(0.0),
      bNeedRelease_(false),
      mCommitLatencyCnt_(0),
      mCommitLatencyTotalMs_(0.0),
      mCommitAvgLatencyMs_(0.0),
      mDataspace_(dataspace) {
    mListVpCompositions_.clear();
    mMapDisplayFrame_.clear();
}

std::string VpTransformContext::TransformToString(uint32_t transform) const {
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

    if (transform & DRM_MODE_RK_POST_PROCESS_SR) {
        result += "_SR";
    } else if (transform & DRM_MODE_RK_POST_PROCESS_MEMC) {
        result += "_MM";
    }

    if (transform & DRM_MODE_RK_POST_SCALE_DOWN_2) {
        result += "_2";
    } else if (transform & DRM_MODE_RK_POST_SCALE_DOWN_4) {
        result += "_4";
    } else if (transform & DRM_MODE_RK_POST_SCALE_DOWN_8) {
        result += "_8";
    }

    return result;
}

void VpTransformContext::Dump(String8& output) {
    std::unique_lock<std::mutex> lock(mMutex_);
    output.appendFormat("\t[%s] TunnelId=%d VpCompositionSize=%zu Fps=%.2f Latency=%.3f ms FrameNo=%" PRIu64 "\n",
                        TransformToString(mTransform_).c_str(), mTunnelId_, mListVpCompositions_.size(), mFps_,
                        mCommitAvgLatencyMs_, uFrameNo_);
    // clang-format off
    output.append(
            "\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n"
            "\t    FN    |  HndId  |    BufferId     | FbId  | Fcc  | stride x height | Ready | Present |  Q->A ms |  Q->P ms |    name              \n"
            "\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n");
    // clang-format on

    if (mListVpCompositions_.size() > 0) {
        for (auto& composition : mListVpCompositions_) {
            composition->Dump(output);
        }
    }
    // clang-format off
    output.append("\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n");
    // clang-format on

    return;
}

void VpTransformContext::Dump() {
    std::unique_lock<std::mutex> lock(mMutex_);
    String8 output;
    output.appendFormat("\t[%s] TunnelId=%d VpCompositionSize=%zu Fps=%.2f Latency=%.3f ms FrameNo=%" PRIu64 "\n",
                        TransformToString(mTransform_).c_str(), mTunnelId_, mListVpCompositions_.size(), mFps_,
                        mCommitAvgLatencyMs_, uFrameNo_);
    // clang-format off
    output.append(
            "\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n"
            "\t    FN    |  HndId  |    BufferId     | FbId  | Fcc  | stride x height | Ready | Present |  Q->A ms |  Q->P ms |    name              \n"
            "\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n");
    // clang-format on
    ALOGD("%s", output.c_str());
    output.clear();

    if (mListVpCompositions_.size() > 0) {
        for (auto& composition : mListVpCompositions_) {
            composition->Dump(output);
            ALOGD("%s", output.c_str());
            output.clear();
        }
    }
    // clang-format off
    output.append("\t----------+---------+-----------------+-------+------+-----------------+-------+---------+----------+----------+----------------------\n");
    // clang-format on
    ALOGD("%s", output.c_str());
    output.clear();

    return;
}

sp<ReleaseFence> VpTransformContext::AddReleaseFence(std::shared_ptr<VpComposition> input) {
    std::unique_lock<std::mutex> lock(mMutex_);

    int ret = mTimeLine_.createReleaseFence("%s-T%d-B%" PRIi64 "-FN%" PRIu64, mFenceTag_.c_str(), input->TunnelId(),
                                            input->GetBuffer()->GetExternalId(), uFrameNo_);
    if (ret) {
        HWC2_LOGE("VpTransformContext FenceTag=%s tunnel-id=%d FrameNo=%" PRIu64, mFenceTag_.c_str(), input->TunnelId(),
                  uFrameNo_);
        return ReleaseFence::NO_FENCE;
    }

    return mTimeLine_.GetNextReleaseFence();
}

int VpTransformContext::ReleaseUnuseComposition() {
    std::unique_lock<std::mutex> lock(mMutex_);
    while (mListVpCompositions_.size() > 2) {
        // 最旧 VpCompositon
        std::shared_ptr<VpComposition> first_old_composition = mListVpCompositions_.front();
        mListVpCompositions_.pop_front();
        // 次旧 VpCompositon
        std::shared_ptr<VpComposition> second_old_composition = mListVpCompositions_.front();
        mListVpCompositions_.pop_front();

        // HWC2_LOGI("Name=%s WaitFinishFence(0)=%d WaitReleaseFence(0)=%d oldest_composition.use_count()=%ld",
        //           first_old_composition->Name().c_str(), first_old_composition->GetBuffer()->WaitFinishFence(0),
        //           first_old_composition->GetBuffer()->WaitReleaseFence(0), first_old_composition.use_count());
        // 丢弃逻辑为次旧完成,且最旧没有其他显示引用，则释放最旧
        if (second_old_composition->GetBuffer()->WaitFinishFence(0) == 0 && first_old_composition.use_count() == 1) {
            // 统计延迟数据
            if (first_old_composition->GetCommitLatencyMs() > 0) {
                mCommitLatencyCnt_++;
                mCommitLatencyTotalMs_ += first_old_composition->GetCommitLatencyMs();
                // 统计60帧的平均延迟数据
                if (mCommitLatencyCnt_ > 60) {
                    mCommitAvgLatencyMs_ = mCommitLatencyTotalMs_ / mCommitLatencyCnt_;
                    mCommitLatencyCnt_ = 0;
                    mCommitLatencyTotalMs_ = 0;
                }
            }
            first_old_composition = nullptr;
            mListVpCompositions_.push_front(second_old_composition);
            continue;
        } else {
            mListVpCompositions_.push_front(second_old_composition);
            mListVpCompositions_.push_front(first_old_composition);
            return 0;
        }
    }
    return 0;
}

int VpTransformContext::TryReleaseAllResource() {
    std::unique_lock<std::mutex> lock(mMutex_);
    bNeedRelease_ = true;
    while (mListVpCompositions_.size() > 0) {
        std::shared_ptr<VpComposition> oldest_composition = mListVpCompositions_.front();
        mListVpCompositions_.pop_front();
        // HWC2_LOGI("Name=%s WaitFinishFence(0)=%d WaitReleaseFence(0)=%d oldest_composition.use_count()=%ld",
        //           oldest_composition->Name().c_str(), oldest_composition->GetBuffer()->WaitFinishFence(0),
        //           oldest_composition->GetBuffer()->WaitReleaseFence(0), oldest_composition.use_count());
        if (oldest_composition->GetBuffer()->WaitFinishFence(0) == 0 && oldest_composition.use_count() == 1) {
            oldest_composition = nullptr;
            continue;
        } else {
            mListVpCompositions_.push_front(oldest_composition);
            return -1;
        }
    }
    return 0;
}

int VpTransformContext::Handle(std::shared_ptr<VpComposition> input) {
    ATRACE_CALL();
    // 如果使能SvepSr则直接处理
    int ret = 0;
    if (mTransform_ == DRM_MODE_ROTATE_0) {
        ret = DoStore(input);
        if (ret) {
            HWC2_LOGE("DoStore Name=%s fail, ret=%d", input->Name().c_str(), ret);
            return ret;
        }

#ifdef USE_LIBSR
    } else if (mTransform_ & DRM_MODE_RK_POST_PROCESS_SR) {
        int ret = DoTransformBySvepSr(input);
        if (ret) {
            HWC2_LOGE("DoTransformBySvepSr Name=%s fail, ret=%d", input->Name().c_str(), ret);
            return ret;
        }
#endif
#ifdef USE_LIBSVEP_MEMC
    } else if (mTransform_ & DRM_MODE_RK_POST_PROCESS_MEMC) {
        int ret = DoTransformBySvepMemc(input);
        if (ret) {
            HWC2_LOGE("DoTransformBySvepMemc Name=%s fail, ret=%d", input->Name().c_str(), ret);
            return ret;
        }
#endif
    } else {
        int ret = DoTransformByRga(input);
        if (ret) {
            HWC2_LOGE("DoTransformByRga Name=%s fail, ret=%d", input->Name().c_str(), ret);
            return ret;
        }
    }

    // 释放不使用的 Composition
    ReleaseUnuseComposition();
    // 计算帧率
    CalculateFPS();
    return 0;
}

void VpTransformContext::CalculateFPS() {
    uFrameNo_++;
    //每10帧计算一次FPS
    if (mLastFpsTimestampUs_ == 0) {
        mLastFpsTimestampUs_ = SidebandGetTimestampUs();
    }
    if ((uFrameNo_ - mLastFpsFrameNo_) > 10) {
        int64_t current_time_us = SidebandGetTimestampUs();
        mFps_ = (uFrameNo_ - mLastFpsFrameNo_) / (((current_time_us - mLastFpsTimestampUs_) / (1000.0 * 1000)));
        mLastFpsFrameNo_ = uFrameNo_;
        mLastFpsTimestampUs_ = current_time_us;
    }
    return;
}
int VpTransformContext::DoTransformByRga(std::shared_ptr<VpComposition> input) {
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

    std::shared_ptr<DrmBuffer> src_buffer = input->GetBuffer();
    // 3.2 Set src buffer info
    int src_format = -1;
    if (gIsRK3588()) {
        if (!hwc_rga_utils::isRK3588RGA3SupportFormat(src_buffer->GetFormat())) {
            HWC2_LOGE("RK3588 RGA3 do not support this format, transform failed!");
            return -1;
        } else {
            src_format =
                    hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3588(src_buffer->GetFormat()));
        }
    } else if (gIsRK3576()) {
        if (!hwc_rga_utils::isRK3576RGA2SupportFormat(src_buffer->GetFormat())) {
            HWC2_LOGE("RK3576 RGA2.5 do not support this format, transform failed!");
            return -1;
        } else {
            src_format =
                    hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3576(src_buffer->GetFormat()));
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
    int dst_width = src_rect.width;
    int dst_height = src_rect.height;
    // 根据缩小的mask执行缩小
    if ((mTransform_ & DRM_MODE_RK_POST_SCALE_DOWN_2) > 0) {
        dst_width = ALIGN_DOWN(src_rect.width / 2, 4);
        dst_height = ALIGN_DOWN(src_rect.height / 2, 4);
    } else if ((mTransform_ & DRM_MODE_RK_POST_SCALE_DOWN_4) > 0) {
        dst_width = ALIGN_DOWN(src_rect.width / 4, 4);
        dst_height = ALIGN_DOWN(src_rect.height / 4, 4);
    } else if ((mTransform_ & DRM_MODE_RK_POST_SCALE_DOWN_8) > 0) {
        dst_width = ALIGN_DOWN(src_rect.width / 8, 4);
        dst_height = ALIGN_DOWN(src_rect.height / 8, 4);
    }

    if ((mTransform_ == DRM_MODE_ROTATE_90) || (mTransform_ & DRM_MODE_ROTATE_270)) {
        std::swap(dst_height, dst_width);
    }

    int dst_buf_format = src_buffer->GetFormat();
    if (dst_buf_format == HAL_PIXEL_FORMAT_YUV420_8BIT_RFBC || dst_buf_format == HAL_PIXEL_FORMAT_YUV422_8BIT_RFBC ||
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

    // RK_GRALLOC_USAGE_STRIDE_ALIGN_64 with BGR buffer in byte-stride do not guarantee 16 pixel alignment in pixel
    // Use RGBX format to ensure that
    if (dst_buf_format == HAL_PIXEL_FORMAT_BGR_888 || dst_buf_format == HAL_PIXEL_FORMAT_RGB_888) {
        dst_buf_format = HAL_PIXEL_FORMAT_RGBA_8888;
    }

    std::shared_ptr<DrmBuffer> dst_buffer = mBufferQueue_->DequeueDrmBuffer(
            dst_width, dst_height, dst_buf_format, dst_buf_usage, "SidebandTransformRgaBuffer");
    if (!dst_buffer) {
        HWC2_LOGE("tunnel_id=%d, buffer_id=0x%" PRIx64
                  " Dequeue DstBuffer fail! wxh=(%dx%d),format=%d,usage = 0x%" PRIx64,
                  input->TunnelId(), src_buffer->GetExternalId(), dst_width, dst_height, dst_buf_format, dst_buf_usage);
        return -1;
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
            HWC2_LOGE("RK3588 RGA3 do not support this format, transform failed!");
            return -1;
        } else {
            dst_format = hwc_rga_utils::HwcGetRgaFormat(hwc_rga_utils::UnifyAndroidFormatForRK3588(dst_buf_format));
        }
    } else if (gIsRK3576()) {
        if (!hwc_rga_utils::isRK3576RGA2SupportFormat(dst_buf_format)) {
            HWC2_LOGE("RK3576 RGA2.5 do not support this format, transform failed!");
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
    switch (mTransform_ & (DRM_MODE_REFLECT_MASK | DRM_MODE_ROTATE_MASK)) {
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
            HWC2_LOGW("Unknow sf transform %s", TransformToString(mTransform_).c_str());
    }

    IM_STATUS im_state;
    // Call Im2d 格式转换
    im_state = imcheck_composite(src, dst, pat, src_rect, dst_rect, pat_rect, usage | IM_ASYNC);
    if (im_state != IM_STATUS_NOERROR) {
        HWC2_LOGE("call im2d scale fail, %s", imStrError(im_state));
        mBufferQueue_->QueueBuffer(dst_buffer);
        return -1;
    }

    // GetEnableRgaAcquireFence 查询是否使用RGA的AcquireFence支持，
    // 由属性 vendor.hwc.enable_rga_acquire_fence控制，默认为0，即不使用RGA AcquireFence支持
    // 不使用RGA AcquireFence支持时，即使多屏中存在无旋转屏幕，也不再支持低延迟送显功能
    //
    // RGA驱动版本为1.3.5及之前，在使用HDMI-in传递的AcquireFence输入时，可能会出现内核崩溃问题。
    // 若确认内核驱动已支持，可设置vendor.hwc.enable_rga_acquire_fence=1启用RGA的AcquireFence.

    int rga_acquire_fence = -1;
    if (input->GetBuffer() != NULL) {
        if (!ResourceManager::getInstance()->GetEnableRgaAcquireFence()) {
            if (input->GetBuffer()->WaitFinishFence(500)) {
                HWC2_LOGE("%s Wait AcqurieFence 500ms Failed!", input->Name().c_str());
            }
        } else {
            rga_acquire_fence = input->GetBuffer()->GetFinishFence();
        }
    }

    int rga_release_fence = -1;
    im_opt_t imOpt;
    memset(&imOpt, 0x00, sizeof(im_opt_t));
    if (gIsRK3588()) {
        imOpt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
    }
    im_state = improcess(src, dst, pat, src_rect, dst_rect, pat_rect, rga_acquire_fence, &rga_release_fence, &imOpt,
                         usage | IM_ASYNC);
    if (im_state != IM_STATUS_SUCCESS) {
        HWC2_LOGE("call im2d scale fail, %s", imStrError(im_state));
        mBufferQueue_->QueueBuffer(dst_buffer);
        if (rga_release_fence > 0) {
            close(rga_release_fence);
        }
        return -1;
    }

    // HWC2_LOGI("%s RGA transform success!", input->Name().c_str());

    if (rga_release_fence > 0) {
        dst_buffer->SetFinishFence(dup(rga_release_fence));
        //由于可能存在多种旋转角度，进行多次旋转，需要将多次旋转ReleaseFence Merge到一起，
        //以确保在释放源buffer前等待所有旋转操作处理完成
        src_buffer->MergeFinishFence(rga_release_fence);
    }

    sp<ReleaseFence> release_fence = AddReleaseFence(input);
    dst_buffer->SetReleaseFence(dup(release_fence->getFd()));
    mBufferQueue_->QueueBuffer(dst_buffer);

    if (HwcAProperty::getInstance().getBoolWithInterval("vendor.dump", 500, false)) {
        dst_buffer->DumpData(GetFrameNo(), 0, "S");
    }

    std::unique_lock<std::mutex> lock(mMutex_);

    std::shared_ptr<VpComposition> compositon = std::make_shared<VpComposition>(
            input->TunnelId(), mTransform_, dst_buffer, release_fence, input->GetQueueTimestamp());
    compositon->SetFrameNo(uFrameNo_);
    compositon->SetName(release_fence->getName());

    mListVpCompositions_.push_back(compositon);

    return 0;
}

int VpTransformContext::DoStore(std::shared_ptr<VpComposition> input) {
    ATRACE_CALL();
    sp<ReleaseFence> release_fence = AddReleaseFence(input);
    if (release_fence != NULL && release_fence->isValid()) {
        input->SetReleaseFence(release_fence);
        input->SetFrameNo(uFrameNo_);
        input->SetName(release_fence->getName());
    }
    if (HwcAProperty::getInstance().getBoolWithInterval("vendor.dump", 500, false)) {
        input->GetBuffer()->DumpData(GetFrameNo(), 0, "S");
    }
    std::unique_lock<std::mutex> lock(mMutex_);
    mListVpCompositions_.push_back(input);
    return 0;
}

#ifdef USE_LIBSR

SrDatapace VpTransformContext::ConvertToSrDataspace(android_dataspace_t dataspace) {
    SrDatapace output = SR_DATASPACE_UNKNOWN;

    bool is_601 = false;
    switch (dataspace & HAL_DATASPACE_STANDARD_MASK) {
        case HAL_DATASPACE_STANDARD_BT601_625:
        case HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED:
        case HAL_DATASPACE_STANDARD_BT601_525:
        case HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED:
            is_601 = true;
            break;
        case HAL_DATASPACE_STANDARD_BT709:
            is_601 = false;
            break;
        default:
            is_601 = false;
            break;
    }

    bool is_full = false;
    switch (dataspace & HAL_DATASPACE_RANGE_MASK) {
        case HAL_DATASPACE_RANGE_FULL:
            is_full = true;
            break;
        case HAL_DATASPACE_RANGE_LIMITED:
            is_full = false;
            break;
        default:
            is_full = false;
            break;
    }

    if (is_601) {
        if (is_full) {
            output = SR_DATAPACE_BT601_FULL_RANGE;
        } else {
            output = SR_DATAPACE_BT601_LIMIT_RANGE;
        }
    } else {
        if (is_full) {
            output = SR_DATAPACE_BT709_FULL_RANGE;
        } else {
            output = SR_DATAPACE_BT709_LIMIT_RANGE;
        }
    }
    return output;
}

int VpTransformContext::DoTransformBySvepSr(std::shared_ptr<VpComposition> input) {
    ATRACE_CALL();
    // 0. SR模块初始化
    std::shared_ptr<SvepSr> svep_sr = ResourceManager::getInstance()->GetSvepSr();
    if (svep_sr.get() != NULL) {
        SrError error = svep_sr->Init(SR_VERSION, true);
        if (error != SrError::None) {
            int auth_timeline = HwcAProperty::getInstance().getIntWithInterval("sys.svep.auth_timeline", 500, 0);
            if (mSrAuthTimeline_ != auth_timeline) {
                mSrAuthTimeline_ = auth_timeline;
                svep_sr = ResourceManager::getInstance()->ResetAndGetSvepSr();
                error = svep_sr->Init(SR_VERSION, true);
                if (error != SrError::None) {
                    HWC2_LOGD_IF("Sr Init fail, plase check License.\n");
                    return -1;
                }
            } else {
                HWC2_LOGD_IF("Sr Init fail, plase check License.\n");
                return -1;
            }
        }
    } else {
        return -1;
    }

    std::shared_ptr<DrmBuffer> src_buffer = input->GetBuffer();
    std::shared_ptr<DrmBuffer> dst_buffer;

    SrImageInfo sr_src;
    SrImageInfo sr_dst;

    // 以下参数更新后需要强制触发svep处理更新图像数据
    int enhancement_rate = HwcAProperty::getInstance().getIntWithInterval(SR_ENHANCEMENT_RATE_NAME, 500, 0);
    int contrast_mode = HwcAProperty::getInstance().getIntWithInterval(SR_CONTRAST_MODE_NAME, 500, 0);
    int contrast_offset = HwcAProperty::getInstance().getIntWithInterval(SR_CONTRAST_MODE_OFFSET, 500, 0);
    int diable_osd_mode = HwcAProperty::getInstance().getIntWithInterval(SR_OSD_DISABLE_MODE, 500, 0);
    int osd_oneline_mode = HwcAProperty::getInstance().getIntWithInterval(SR_OSD_VIDEO_ONELINE_MODE, 500, 0);
    int osd_oneline_wait_second =
            HwcAProperty::getInstance().getIntWithInterval(SR_OSD_VIDEO_ONELINE_WATI_SEC, 500, 12);

    // 2. 设置SR强度
    SrError error = svep_sr->SetEnhancementRate(enhancement_rate);
    if (error != SrError::None) {
        HWC2_LOGE("Sr SetEnhancementRate fail.\n");
        return -1;
    }

    // 3. 设置SR对比模式为扫描模式
    error = svep_sr->SetContrastMode(contrast_mode, contrast_offset);
    if (error != SrError::None) {
        HWC2_LOGE("Sr SetContrastMode fail.\n");
        return -1;
    }

    // 4. 设置SR OSD模式与字符串
    error = svep_sr->SetOsdMode(SR_OSD_ENABLE_VIDEO, SR_OSD_VIDEO_STR);
    if (error != SrError::None) {
        HWC2_LOGE("Sr SetOsdMode fail.\n");
        return -1;
    }

    // 处理旋转
    SrRotateMode rotate = SR_ROTATE_0;
    switch (mTransform_ & (DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK)) {
        case DRM_MODE_ROTATE_0:
            rotate = SR_ROTATE_0;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X:
            rotate = SR_REFLECT_X;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y:
            rotate = SR_REFLECT_Y;
            break;
        case DRM_MODE_ROTATE_90:
            rotate = SR_ROTATE_90;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y:
            rotate = SR_ROTATE_180;
            break;
        case DRM_MODE_ROTATE_270:
            rotate = SR_ROTATE_270;
            break;
        // case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90 :
        //   usage = IM_HAL_TRANSFORM_FLIP_H | IM_HAL_TRANSFORM_ROT_90;
        //   break;
        // case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90:
        //   usage = IM_HAL_TRANSFORM_FLIP_V | IM_HAL_TRANSFORM_ROT_90;
        //   break;
        default:
            rotate = SR_ROTATE_0;
            ALOGE_IF(LogLevel(DBG_DEBUG), "Unknow sf transform %s", TransformToString(mTransform_).c_str());
    }

    // 4. 设置SR rotate 模式
    error = svep_sr->SetRotateMode(rotate);
    if (error != SrError::None) {
        HWC2_LOGE("Sr SetOsdMode fail.\n");
        return -1;
    }

    // 2. Set buffer Info
    sr_src.mBufferInfo_.iFd_ = src_buffer->GetFd();
    sr_src.mBufferInfo_.iWidth_ = src_buffer->GetWidth();
    sr_src.mBufferInfo_.iHeight_ = src_buffer->GetHeight();
    sr_src.mBufferInfo_.iFormat_ = src_buffer->GetFourccFormat();
    sr_src.mBufferInfo_.iStride_ = src_buffer->GetStride();
    sr_src.mBufferInfo_.iHeightStride_ = src_buffer->GetHeightStride();
    sr_src.mBufferInfo_.iSize_ = src_buffer->GetSize();
    sr_src.mBufferInfo_.uBufferId_ = src_buffer->GetBufferId();
    sr_src.mBufferInfo_.uColorSpace_ = ConvertToSrDataspace(mDataspace_);
    int left = 0, top = 0, right = 0, bottom = 0;
    src_buffer->GetCrop(&left, &top, &right, &bottom);

    sr_src.mCrop_.iLeft_ = left;
    sr_src.mCrop_.iTop_ = top;
    sr_src.mCrop_.iRight_ = right;
    sr_src.mCrop_.iBottom_ = bottom;

    SrMode sr_mde = SrMode::UN_SUPPORT;
    SrError ret = svep_sr->MatchSrMode(&sr_src, SR_MODE_NONE, &sr_mde);
    if (ret) {
        HWC2_LOGE("Sr SetSrcImage fail\n");
        return -1;
    }

    // 3. Get dst info
    SrImageInfo target_image_info;
    error = svep_sr->GetDetImageInfo(&target_image_info);
    if (error) {
        printf("Sr GetDstRequireInfo fail\n");
        return -1;
    }

    // SR 目标图像参数
    int sr_dst_w = 0;
    int sr_dst_h = 0;
    // 获取VOP端的送显参数，屏幕最大的一组值
    for (auto pair_display_frame : mMapDisplayFrame_) {
        int display_frame_w = pair_display_frame.second.right - pair_display_frame.second.left;
        int display_frame_h = pair_display_frame.second.bottom - pair_display_frame.second.top;
        if (display_frame_w > sr_dst_w) {
            sr_dst_w = ALIGN_DOWN(display_frame_w, 2);
        }
        if (display_frame_h > sr_dst_h) {
            sr_dst_h = ALIGN_DOWN(display_frame_h, 2);
        }
    }

    // 若屏幕大于HDMI-IN原始图像数据，则使用原始图像的参数，主要目的是VOP避免VOP做大倍率缩小
    if (sr_dst_w == 0 || sr_dst_h == 0 || sr_dst_w > target_image_info.mBufferInfo_.iWidth_ ||
        sr_dst_h > target_image_info.mBufferInfo_.iHeight_) {
        sr_dst_w = target_image_info.mBufferInfo_.iWidth_;
        sr_dst_h = target_image_info.mBufferInfo_.iHeight_;
    }

    // 4. Alloc dst_buffer
    dst_buffer = mBufferQueue_->DequeueDrmBuffer(
            sr_dst_w, sr_dst_h, HAL_PIXEL_FORMAT_YCrCb_NV12,
            RK_GRALLOC_USAGE_STRIDE_ALIGN_64 | RK_GRALLOC_USAGE_WITHIN_4G | MALI_GRALLOC_USAGE_NO_AFBC,
            "SR-SidebandStream");

    if (dst_buffer == NULL) {
        HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
        return -1;
    }

    SrOsdMode osd_mode = SR_OSD_ENABLE_VIDEO;
    const wchar_t* osd_str = SR_OSD_VIDEO_STR;
    error = svep_sr->SetOsdMode(osd_mode, osd_str);
    if (error != SrError::None) {
        HWC2_LOGE("Sr SetOsdMode fail.\n");
        return -1;
    }
    // 5. Set buffer Info
    sr_dst.mBufferInfo_.iFd_ = dst_buffer->GetFd();
    sr_dst.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
    sr_dst.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
    sr_dst.mBufferInfo_.iFormat_ = dst_buffer->GetFourccFormat();
    sr_dst.mBufferInfo_.iStride_ = dst_buffer->GetStride();
    sr_dst.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
    sr_dst.mBufferInfo_.iSize_ = dst_buffer->GetSize();
    sr_dst.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();

    hwc_frect_t source_crop;
    sr_dst.mCrop_.iLeft_ = 0;
    sr_dst.mCrop_.iTop_ = 0;
    sr_dst.mCrop_.iRight_ = sr_dst_w;
    sr_dst.mCrop_.iBottom_ = sr_dst_h;

    dst_buffer->SetCrop(sr_dst.mCrop_.iLeft_, sr_dst.mCrop_.iTop_, sr_dst.mCrop_.iRight_, sr_dst.mCrop_.iBottom_);
    dst_buffer->SetExternalId(src_buffer->GetExternalId());

    int rga_acquire_fence = -1;
    if (input->GetBuffer() != NULL) {
        if (input->GetBuffer()->WaitFinishFence(500)) {
            HWC2_LOGE("%s Wait AcqurieFence 500ms Failed!", input->Name().c_str());
        }
    }

    int output_fence = -1;
    // 13. RunAsync
    error = svep_sr->RunAsync(&sr_src, &sr_dst, &output_fence);
    if (error != SrError::None) {
        HWC2_LOGD_IF("RunAsync fail!");
        mBufferQueue_->QueueBuffer(dst_buffer);
        return -1;
    }

    // HWC2_LOGI("DoTransformBySvepSr %s SR transform success! dst-buffer id=0x%" PRIx64, input->Name().c_str(),
    //           dst_buffer->GetBufferId());

    dst_buffer->SetFinishFence(output_fence);
    dst_buffer->SetBufferType(DrmBufferType::DRM_BUFFER_TYPE_SR);
    dst_buffer->SetDataspace(HAL_DATASPACE_V0_BT709);

    sp<ReleaseFence> release_fence = AddReleaseFence(input);
    dst_buffer->SetReleaseFence(dup(release_fence->getFd()));
    mBufferQueue_->QueueBuffer(dst_buffer);

    if (HwcAProperty::getInstance().getBoolWithInterval("vendor.dump", 500, false)) {
        src_buffer->DumpData(GetFrameNo(), 0, "S");
        dst_buffer->DumpData(GetFrameNo(), 0, "S");
    }

    std::unique_lock<std::mutex> lock(mMutex_);

    std::shared_ptr<VpComposition> compositon = std::make_shared<VpComposition>(
            input->TunnelId(), mTransform_, dst_buffer, release_fence, input->GetQueueTimestamp());
    compositon->SetFrameNo(uFrameNo_);
    compositon->SetName(release_fence->getName());

    mListVpCompositions_.push_back(compositon);
    return 0;
}
#endif

#ifdef USE_LIBSVEP_MEMC
int VpTransformContext::DoTransformBySvepMemc(std::shared_ptr<VpComposition> input) {
    ALOGD_IF(LogLevel(DBG_DEBUG), "%s:line=%d", __FUNCTION__, __LINE__);
    // memc初始化
    std::shared_ptr<SvepMemc> svep_memc = ResourceManager::getInstance()->GetSvepMemc();
    if (svep_memc.get() != NULL) {
        if (svep_memc->Init(MEMC_VERSION, true)) {
            HWC2_LOGE("Memc init failed.");
            return -1;
        }
    }

    bool enableSr = (HwcAProperty::getInstance().getIntWithInterval(MEMC_ENABLR_SR_NAME, 500, 0) > 0);
    bool enableMemcComparation = (HwcAProperty::getInstance().getIntWithInterval(MEMC_CONTRAST_MODE_NAME, 500, 0) > 0);
    bool enableMemcOsd = (HwcAProperty::getInstance().getIntWithInterval(MEMC_OSD_DISABLE_MODE, 500, 0) == 0);
    bool enableMemcOsdOneline =
            (HwcAProperty::getInstance().getIntWithInterval(MEMC_OSD_VIDEO_ONELINE_MODE, 500, 0) > 0);
    int osd_oneline_wait_second =
            HwcAProperty::getInstance().getIntWithInterval(MEMC_OSD_VIDEO_ONELINE_WATI_SEC, 500, 12);

    // 处理旋转
    MemcRotateMode rotate = MEMC_ROTATE_0;
    switch (mTransform_ & (DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK)) {
        case DRM_MODE_ROTATE_0:
            rotate = MEMC_ROTATE_0;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X:
            rotate = MEMC_REFLECT_X;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y:
            rotate = MEMC_REFLECT_Y;
            break;
        case DRM_MODE_ROTATE_90:
            rotate = MEMC_ROTATE_90;
            break;
        case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y:
            rotate = MEMC_ROTATE_180;
            break;
        case DRM_MODE_ROTATE_270:
            rotate = MEMC_ROTATE_270;
            break;
        // case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90 :
        //   usage = IM_HAL_TRANSFORM_FLIP_H | IM_HAL_TRANSFORM_ROT_90;
        //   break;
        // case DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90:
        //   usage = IM_HAL_TRANSFORM_FLIP_V | IM_HAL_TRANSFORM_ROT_90;
        //   break;
        default:
            rotate = MEMC_ROTATE_0;
            ALOGE_IF(LogLevel(DBG_DEBUG), "Unknow sf transform %s", TransformToString(mTransform_).c_str());
    }

    // 设置 rotate 模式
    MemcError error = svep_memc->SetRotateMode(rotate);
    if (error != MemcError::MEMC_NO_ERROR) {
        HWC2_LOGE("Memc SetRotateMode fail.\n");
        return -1;
    }

    svep_memc->SetContrastMode(enableMemcComparation);
    if (error != MemcError::MEMC_NO_ERROR) {
        HWC2_LOGE("Memc SetContrastMode fail.\n");
        return -1;
    }

    MemcOsdMode osd_mode = MEMC_OSD_ENABLE_VIDEO;
    const wchar_t* osd_str = MEMC_OSD_VIDEO_STR;
    if (enableMemcOsd) {
        if (enableMemcOsdOneline) {
            osd_mode = MEMC_OSD_ENABLE_VIDEO_ONELINE;
            osd_str = MEMC_OSD_VIDEO_ONELINE_STR;
        }
    } else {
        osd_mode = MEMC_OSD_DISABLE;
        osd_str = NULL;
    }

    svep_memc->SetOsdMode(osd_mode, osd_str);
    if (error != MemcError::MEMC_NO_ERROR) {
        HWC2_LOGE("Memc SetRotateMode fail.\n");
        return -1;
    }

    error = svep_memc->EnableSvepSr(enableSr);
    if (error != MemcError::MEMC_NO_ERROR) {
        HWC2_LOGE("Memc EnableSvepSr fail.\n");
        return -1;
    }

    // Set buffer Info
    std::shared_ptr<DrmBuffer> src_buffer = input->GetBuffer();
    MemcImageInfo memc_src;
    memc_src.mBufferInfo_.iFd_ = src_buffer->GetFd();
    memc_src.mBufferInfo_.iWidth_ = src_buffer->GetWidth();
    memc_src.mBufferInfo_.iHeight_ = src_buffer->GetHeight();
    memc_src.mBufferInfo_.iFormat_ = src_buffer->GetFourccFormat();
    memc_src.mBufferInfo_.iStride_ = src_buffer->GetStride();
    memc_src.mBufferInfo_.iHeightStride_ = src_buffer->GetHeightStride();
    memc_src.mBufferInfo_.iSize_ = src_buffer->GetSize();
    memc_src.mBufferInfo_.uBufferId_ = src_buffer->GetBufferId();
    memc_src.mBufferInfo_.uColorSpace_ = MEMC_DATASPACE_UNKNOWN;
    int left = 0, top = 0, right = 0, bottom = 0;
    src_buffer->GetCrop(&left, &top, &right, &bottom);

    memc_src.mCrop_.iLeft_ = left;
    memc_src.mCrop_.iTop_ = top;
    memc_src.mCrop_.iRight_ = right;
    memc_src.mCrop_.iBottom_ = bottom;

    MemcMode memc_mode = MemcMode::MEMC_UNSUPPORT;
    error = svep_memc->MatchMemcMode(&memc_src, &memc_mode);
    if (error) {
        HWC2_LOGE("MatchMemcMode fail\n");
        return -1;
    }

    if (input->GetBuffer() != NULL) {
        if (input->GetBuffer()->WaitFinishFence(500)) {
            HWC2_LOGE("%s Wait AcqurieFence 500ms Failed!", input->Name().c_str());
        }
    }

    error = svep_memc->SetSrcImage(&memc_src);
    if (error != MemcError::MEMC_NO_ERROR) {
        HWC2_LOGE("SetSrcImage fail!, skip this policy. ret=%d", error);
        return -1;
    }

    return 0;
}
#endif

std::shared_ptr<VpComposition> VpTransformContext::GetVpComposition(bool wait_finish) {
    std::unique_lock<std::mutex> lock(mMutex_);
    std::shared_ptr<VpComposition> output_composition = nullptr;
    if (mListVpCompositions_.size() > 0) {
        if (wait_finish) {
            for (auto& composition : mListVpCompositions_) {
                if (composition->GetBuffer()->WaitFinishFence(0) == 0) {
                    output_composition = composition;
                }
            }
        } else {
            output_composition = mListVpCompositions_.back();
        }
    }

    // if (output_composition != NULL) {
    //     HWC2_LOGD_IF("Name=%s BufferId=0x%" PRIx64 " Finish=%s size=%zu", output_composition->Name().c_str(),
    //                  output_composition->GetBuffer()->GetBufferId(),
    //                  output_composition->GetBuffer()->WaitFinishFence(0) == 0 ? "Yes" : "No",
    //                  mListVpCompositions_.size());
    // }
    return output_composition;
}

void VpTransformContext::SetDisplayFrameInfo(int display_id, vt_rect_t* dis_rect) {
    std::unique_lock<std::mutex> lock(mMutex_);
    mMapDisplayFrame_[display_id] = *dis_rect;
}

};  // namespace android