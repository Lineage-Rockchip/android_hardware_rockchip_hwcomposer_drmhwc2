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
#define LOG_TAG "drm-buffer"

#include <drm_fourcc.h>
#include <drmbuffer.h>
#include <rockchip/utils/drmdebug.h>
#include "utils/hwc2_dumper.h"

#include <cutils/atomic.h>
#include <inttypes.h>
#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <regex>
namespace android {

#define ALIGN_DOWN(value, base) (value & (~(base - 1)))

static uint64_t getUniqueId() {
    static volatile int32_t nextId = 0;
    uint64_t id = static_cast<uint32_t>(android_atomic_inc(&nextId));
    return id;
}
// MALI_GRALLOC_USAGE_NO_AFBC 是 arm_gralloc 扩展的 私有的 usage_bit_flag,
// MALI_GRALLOC_USAGE_NO_AFBC = GRALLOC_USAGE_PRIVATE_1 : 1U << 29
// RK_GRALLOC_USAGE_WITHIN_4G = GRALLOC_USAGE_PRIVATE_11: 1ULL << 56
// RK_GRALLOC_USAGE_STRIDE_ALIGN_64 = GRALLOC_USAGE_PRIVATE_7 : 1ULL << 60
// 定义在 hardware/rockchip/libgralloc/bifrost/src/mali_gralloc_usages.h 中

#ifndef RK_GRALLOC_USAGE_WITHIN_4G
#define RK_GRALLOC_USAGE_WITHIN_4G (1ULL << 56)
#endif

#ifndef RK_GRALLOC_USAGE_STRIDE_ALIGN_16
#define RK_GRALLOC_USAGE_STRIDE_ALIGN_16 (1ULL << 57)
#endif

DrmBuffer::DrmBuffer(int w, int h, int format, uint64_t usage, std::string name, int parent_id)
    : uId(getUniqueId()),
      iParentId_(parent_id),
      iFd_(-1),
      iWidth_(w),
      iHeight_(h),
      iFormat_(format),
      iStride_(-1),
      iByteStride_(-1),
      iUsage_(GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_PRIVATE_1 | usage),
      uFourccFormat_(0),
      uModifier_(0),
      uBufferId_(0),
      uGemHandle_(0),
      uFbId_(0),
      uRgaHandle_(0),
      uDataspace_(HAL_DATASPACE_UNKNOWN),
      iLeft_(-1),
      iTop_(-1),
      iRight_(-1),
      iBottom_(-1),
      iFinishFence_(-1),
      iReleaseFence_(-1),
      bInit_(false),
      sName_(name),
      buffer_(NULL),
      ptrBuffer_(NULL),
      ptrDrmGralloc_(DrmGralloc::getInstance()),
      mType_(DRM_BUFFER_TYPE_NONE) {}

DrmBuffer::DrmBuffer(native_handle_t* in_handle)
    : uId(getUniqueId()),
      iFd_(-1),
      iWidth_(-1),
      iHeight_(-1),
      iFormat_(-1),
      iStride_(-1),
      iByteStride_(-1),
      iUsage_(0),
      uFourccFormat_(0),
      uModifier_(0),
      uBufferId_(0),
      uGemHandle_(0),
      uFbId_(0),
      uRgaHandle_(0),
      uDataspace_(HAL_DATASPACE_UNKNOWN),
      iLeft_(-1),
      iTop_(-1),
      iRight_(-1),
      iBottom_(-1),
      iFinishFence_(-1),
      iReleaseFence_(-1),
      bInit_(false),
      sName_(""),
      inBuffer_(in_handle),
      ptrBuffer_(NULL),
      ptrDrmGralloc_(DrmGralloc::getInstance()),
      mType_(DRM_BUFFER_TYPE_NONE) {
    int ret = ptrDrmGralloc_->importBuffer(inBuffer_, &buffer_);
    if (ret) {
        ALOGE("importBuffer in_handle=%p, local_handle=%p fail, ret=%d", inBuffer_, buffer_, ret);
    }
    ptrDrmGralloc_->hwc_get_handle_buffer_id(buffer_, &uBufferId_);

    iFd_ = ptrDrmGralloc_->hwc_get_handle_primefd(buffer_);
    iWidth_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_WIDTH);
    iHeight_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT);
    iStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_STRIDE);
    // 由于RK3528 版本无法获取 Stride ，故使用 width 代替 stride
    if (iStride_ == 0) {
        iStride_ = iWidth_;
    }
    iHeightStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT_STRIDE);
    iByteStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_BYTE_STRIDE_WORKROUND);
    iSize_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_SIZE);
    iFormat_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_FORMAT);
    uFourccFormat_ = ptrDrmGralloc_->hwc_get_handle_fourcc_format(buffer_);
    uModifier_ = ptrDrmGralloc_->hwc_get_handle_format_modifier(buffer_);
    ptrDrmGralloc_->hwc_get_handle_plane_bytes_stride(buffer_, uByteStridePlanes_);
    ptrDrmGralloc_->hwc_get_handle_buffer_id(buffer_, &uBufferId_);
    ptrDrmGralloc_->hwc_get_handle_name(buffer_, sName_);
    ret = ptrDrmGralloc_->hwc_get_gemhandle_from_fd(iFd_, uBufferId_, &uGemHandle_);
    if (ret) {
        HWC2_LOGE("%s hwc_get_gemhandle_from_fd fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
        return;
    }

    // hdr metadata
    bHasHdrMetadata_ = false;
    iHdrMetadataOffset_ = -1;
    int64_t hdr_meta_offset = ptrDrmGralloc_->hwc_get_offset_of_dynamic_hdr_metadata(buffer_);
    if (hdr_meta_offset > 0) {
        // 存在 hdr metadata
        bHasHdrMetadata_ = true;
        iHdrMetadataOffset_ = hdr_meta_offset;
        int size = static_cast<int>(iSize_ - iHdrMetadataOffset_);
        int offset = static_cast<int>(iHdrMetadataOffset_);
        void* addr = ptrDrmGralloc_->hwc_buf_mmap_readonly(iFd_, size, offset);
        if (addr != NULL) {
            pHdrMetadataVir_ = addr;
            iHdrMetadataMapSize_ = size;
        }
        HWC2_LOGI("iHdrMetadataOffset_=%" PRIi64, iHdrMetadataOffset_);
    }

    HWC2_LOGV_IF("Import buffer fd=%d w=%d h=%d s=%d hs=%d bs=%d f=%d fcc=%c%c%c%c mdf=0x%" PRIx64
                 " BufferId=0x%" PRIx64 " bHdrHasMetadata=%d offset=%" PRIi64 " name=%s ",
                 iFd_, iWidth_, iHeight_, iStride_, iHeightStride_, iByteStride_, iFormat_, uFourccFormat_,
                 uFourccFormat_ >> 8, uFourccFormat_ >> 16, uFourccFormat_ >> 24, uModifier_, uBufferId_,
                 bHasHdrMetadata_, iHdrMetadataOffset_, sName_.c_str());

    uFbId_ = 0;
#ifdef RK3528
    uPreScaleFbId_ = 0;
#endif
    bInit_ = true;
    return;
}

DrmBuffer::DrmBuffer(int fd, int width, int height, int stride, int heightStride, int byteStride, int fourccFormat,
                     int size, uint64_t bufferId, uint64_t modifier, std::string name)
    : uId(getUniqueId()),
      iFd_(fd),
      iWidth_(width),
      iHeight_(height),
      iFormat_(-1),
      iStride_(stride),
      iHeightStride_(heightStride),
      iByteStride_(byteStride),
      iSize_(size),
      iUsage_(0),
      uFourccFormat_(fourccFormat),
      uModifier_(modifier),
      uBufferId_(bufferId),
      iFinishFence_(-1),
      iReleaseFence_(-1),
      bInit_(false),
      sName_(name),
      inBuffer_(NULL),
      ptrBuffer_(NULL),
      ptrDrmGralloc_(DrmGralloc::getInstance()),
      mType_(DRM_BUFFER_TYPE_NONE) {
    uByteStridePlanes_ = {0, 0, 0, 0};
    int ret = ptrDrmGralloc_->hwc_get_gemhandle_from_fd(iFd_, uBufferId_, &uGemHandle_);
    if (ret) {
        HWC2_LOGE("%s hwc_get_gemhandle_from_fd fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
        return;
    }

    HWC2_LOGD_IF("Import buffer fd=%d w=%d h=%d s=%d hs=%d bs=%d f=%d fcc=%c%c%c%c mdf=0x%" PRIx64
                 " BufferId=0x%" PRIx64 " name=%s ",
                 iFd_, iWidth_, iHeight_, iStride_, iHeightStride_, iByteStride_, iFormat_, uFourccFormat_,
                 uFourccFormat_ >> 8, uFourccFormat_ >> 16, uFourccFormat_ >> 24, uModifier_, uBufferId_,
                 sName_.c_str());

    uFbId_ = 0;
#ifdef RK3528
    uPreScaleFbId_ = 0;
#endif
    bInit_ = true;
    return;
}

DrmBuffer::~DrmBuffer() {
    WaitFinishFence();
    WaitReleaseFence();

    ptrBuffer_ = NULL;

    if (uFbId_ > 0) {
        if (ptrDrmGralloc_->hwc_fbid_rm_cache(ptrDrmGralloc_->get_drm_device(), uFbId_)) {
            HWC2_LOGE("BufferId=0x%" PRIx64 " Failed to rm uFbId_ %d", uBufferId_, uFbId_);
        }
        uFbId_ = 0;
    }

#ifdef RK3528
    if (uPreScaleFbId_ > 0) {
        if (ptrDrmGralloc_->hwc_fbid_rm_cache(ptrDrmGralloc_->get_drm_device(), uPreScaleFbId_)) {
            HWC2_LOGE("BufferId=0x%" PRIx64 " Failed to rm uPreScaleFbId_ %d", uBufferId_, uPreScaleFbId_);
        }

        uPreScaleFbId_ = 0;
    }
#endif

    if (pHdrMetadataVir_ != NULL) {
        DrmGralloc::getInstance()->hwc_buf_unmap(pHdrMetadataVir_, iHdrMetadataMapSize_);
        pHdrMetadataVir_ = NULL;
    }

    if (uRgaHandle_ > 0) {
        int ret = releasebuffer_handle(uRgaHandle_);
        if (ret < 0) {
            HWC2_LOGE("RgaHandle %s releasebuffer fail, buffer_id=0x%" PRIx64, sName_.c_str(), uBufferId_);
        }
        uRgaHandle_ = 0;
    }

    int ret = ptrDrmGralloc_->hwc_free_gemhandle(uBufferId_);
    if (ret) {
        HWC2_LOGE("%s hwc_free_gemhandle fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
    }

    if (inBuffer_ != NULL) {
        ret = ptrDrmGralloc_->freeBuffer(buffer_);
        if (ret) {
            ALOGE("freeBuffer in_handle=%p, local_handle=%p fail, ret=%d", inBuffer_, buffer_, ret);
        }
        HWC2_LOGI("freeBuffer in_handle=%p, local_handle=%p uBufferId_=0x%" PRIx64 " Success, ret=%d", inBuffer_,
                  buffer_, uBufferId_, ret);
    }
}

int DrmBuffer::Init() {
    if (bInit_) {
        HWC2_LOGI("DrmBuffer has init, w=%d h=%d format=%d", iWidth_, iHeight_, iFormat_);
        return 0;
    }

    if (iWidth_ <= 0 || iHeight_ <= 0 || iFormat_ <= 0) {
        HWC2_LOGE("DrmBuffer init fail, w=%d h=%d format=%d", iWidth_, iHeight_, iFormat_);
        return -1;
    }

    // Gralloc 0.3 不支持 uint64_t usage
#ifdef USE_GRALLOC_0
    HWC2_LOGW_IF("Gralloc0.3 unsupport uint64_t usage, force cvt 0x%" PRIx64 " to uint32_t 0x%" PRIx64, iUsage_,
                 iUsage_ & UINT32_MAX);
    iUsage_ = iUsage_ & UINT32_MAX;
#endif

    ptrBuffer_ = new GraphicBuffer(iWidth_, iHeight_, iFormat_, 0, iUsage_, sName_);
    if (ptrBuffer_->initCheck()) {
        HWC2_LOGE("new GraphicBuffer fail, w=%d h=%d format=%d", iWidth_, iHeight_, iFormat_);
        return -1;
    }
    buffer_ = ptrBuffer_->handle;
    iFd_ = ptrDrmGralloc_->hwc_get_handle_primefd(buffer_);
    iWidth_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_WIDTH);
    iHeight_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT);
    iStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_STRIDE);

    // 由于RK3528 版本无法获取 Stride ，故使用 width 代替 stride
    if (iStride_ == 0) {
        iStride_ = iWidth_;
    }

    iHeightStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT_STRIDE);
    iByteStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_BYTE_STRIDE_WORKROUND);
    iSize_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_SIZE);
    iFormat_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_FORMAT);
    uFourccFormat_ = ptrDrmGralloc_->hwc_get_handle_fourcc_format(buffer_);
    uModifier_ = ptrDrmGralloc_->hwc_get_handle_format_modifier(buffer_);
    ptrDrmGralloc_->hwc_get_handle_buffer_id(buffer_, &uBufferId_);
    ptrDrmGralloc_->hwc_get_handle_plane_bytes_stride(buffer_, uByteStridePlanes_);
    int ret = ptrDrmGralloc_->hwc_get_gemhandle_from_fd(iFd_, uBufferId_, &uGemHandle_);
    if (ret) {
        HWC2_LOGE("%s hwc_get_gemhandle_from_fd fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
        return -1;
    }
    uFbId_ = 0;
    bInit_ = true;

    return 0;
}

bool DrmBuffer::initCheck() {
    return bInit_;
}
buffer_handle_t DrmBuffer::GetHandle() {
    return buffer_;
}
native_handle_t* DrmBuffer::GetInHandle() {
    return inBuffer_;
}
std::string DrmBuffer::GetName() {
    return sName_;
}
uint64_t DrmBuffer::GetId() {
    return uId;
}
uint64_t DrmBuffer::GetExternalId() {
    return iExternelId_;
}
int DrmBuffer::SetExternalId(uint64_t externel_id) {
    iExternelId_ = externel_id;
    return 0;
}
int DrmBuffer::GetParentId() {
    return iParentId_;
}
int DrmBuffer::SetParentId(int parent_id) {
    iParentId_ = parent_id;
    return 0;
}
int DrmBuffer::GetFd() {
    return iFd_;
}
int DrmBuffer::GetWidth() {
    return iWidth_;
}
int DrmBuffer::GetHeight() {
    return iHeight_;
}
int DrmBuffer::GetHeightStride() {
    return iHeightStride_;
}
int DrmBuffer::GetFormat() {
    return iFormat_;
}
int DrmBuffer::GetStride() {
    return iStride_;
}
int DrmBuffer::GetByteStride() {
    return iByteStride_;
}
int DrmBuffer::GetSize() {
    return iSize_;
}
uint64_t DrmBuffer::GetUsage() {
    return iUsage_;
}

std::vector<uint32_t> DrmBuffer::GetByteStridePlanes() {
    return uByteStridePlanes_;
}

int DrmBuffer::SetCrop(int left, int top, int right, int bottom) {
    iLeft_ = left;
    iTop_ = top;
    iRight_ = right;
    iBottom_ = bottom;
    return 0;
}

int DrmBuffer::GetCrop(int* left, int* top, int* right, int* bottom) {
    *left = iLeft_;
    *top = iTop_;
    *right = iRight_;
    *bottom = iBottom_;
    return 0;
}
uint32_t DrmBuffer::GetFourccFormat() {
    return uFourccFormat_;
}
uint64_t DrmBuffer::GetModifier() {
    return uModifier_;
}
uint64_t DrmBuffer::GetBufferId() {
    return uBufferId_;
}
uint32_t DrmBuffer::GetGemHandle() {
    return uGemHandle_;
}

uint32_t DrmBuffer::DrmFormatToPlaneNum(uint32_t drm_format) {
    switch (drm_format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
        case DRM_FORMAT_NV24:
        case DRM_FORMAT_NV42:
        case DRM_FORMAT_NV16:
        case DRM_FORMAT_NV61:
        case DRM_FORMAT_NV12_10:
        case DRM_FORMAT_NV15:
        case DRM_FORMAT_NV30:
        case DRM_FORMAT_NV20:
            return 2;
        default:
            return 1;
    }
}

bool DrmBuffer::IsAfbc() {
    return fourcc_mod_is_vendor(uModifier_, ARM);
}

bool DrmBuffer::IsRfbc() {
    return IS_ROCKCHIP_RFBC_MOD(uModifier_);
}

void DrmBuffer::SetDataspace(android_dataspace_t dataspace) {
    uDataspace_ = dataspace;
}

android_dataspace_t DrmBuffer::GetDataspace() {
    return uDataspace_;
}

uint32_t DrmBuffer::GetFbId() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (uFbId_ > 0) return uFbId_;

#ifdef RK3528
    if (uPreScaleFbId_ > 0) {
        if (ptrDrmGralloc_->hwc_fbid_rm_cache(ptrDrmGralloc_->get_drm_device(), uPreScaleFbId_)) {
            HWC2_LOGE("BufferId=0x%" PRIx64 " Failed to rm uPreScaleFbId_ %d", uBufferId_, uPreScaleFbId_);
        }

        uPreScaleFbId_ = 0;
    }
    if (bIsPreScale_) {
        ResetPreScaleBuffer();
    }
#endif

    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t gem_handles[4] = {0};
    uint64_t modifier[4] = {0};

    gem_handles[0] = uGemHandle_;
    offsets[0] = 0;

    // set bo patches
    // special for nv24 / nv42
    if (uFourccFormat_ == DRM_FORMAT_NV24 || uFourccFormat_ == DRM_FORMAT_NV42) {
        // 获取 plane_info byte stride 信息
        if (uByteStridePlanes_.size() > 0) {
            for (int i = 0; i < uByteStridePlanes_.size(); i++) {
                pitches[i] = uByteStridePlanes_[i];
            }
        } else {
            pitches[0] = iByteStride_;
            pitches[1] = pitches[0] * 2;
        }
    } else {
        pitches[0] = iByteStride_;
        if (DrmFormatToPlaneNum(uFourccFormat_) == 2) {
            pitches[1] = pitches[0];
        }
    }

    if (DrmFormatToPlaneNum(uFourccFormat_) == 2) {
        if (uFourccFormat_ == DRM_FORMAT_NV24 || uFourccFormat_ == DRM_FORMAT_NV42) {
            gem_handles[1] = uGemHandle_;
            offsets[1] = pitches[0] * iHeightStride_;
        } else {
            gem_handles[1] = uGemHandle_;
            offsets[1] = pitches[1] * iHeightStride_;
        }
    }

    modifier[0] = uModifier_;
    if (DrmFormatToPlaneNum(uFourccFormat_) == 2) modifier[1] = uModifier_;

    int ret = ptrDrmGralloc_->hwc_fbid_get_and_cached(uBufferId_, ptrDrmGralloc_->get_drm_device(), iWidth_,
                                                      iHeightStride_, uFourccFormat_, gem_handles, pitches, offsets,
                                                      modifier, &uFbId_, DRM_MODE_FB_MODIFIERS);

    HWC2_LOGD_IF(
            "ImportBuffer fd=%d,w=%d,h=%d,format=%c%c%c%c,"
            "gem_handle=%d,pitches[0]=%d,fb_id=%d,modifier = %" PRIx64,
            ptrDrmGralloc_->get_drm_device(), iWidth_, iHeightStride_, uFourccFormat_, uFourccFormat_ >> 8,
            uFourccFormat_ >> 16, uFourccFormat_ >> 24, gem_handles[0], pitches[0], uFbId_, modifier[0]);

    if (ret) {
        ALOGE("could not create drm fb %d", ret);
        HWC2_LOGE(
                "ImportBuffer fd=%d,w=%d,h=%d,format=%c%c%c%c,"
                "gem_handle=%d,pitches[0]=%d,fb_id=%d,modifier = %" PRIx64,
                ptrDrmGralloc_->get_drm_device(), iWidth_, iHeightStride_, uFourccFormat_, uFourccFormat_ >> 8,
                uFourccFormat_ >> 16, uFourccFormat_ >> 24, gem_handles[0], pitches[0], uFbId_, modifier[0]);
        return ret;
    }

    return uFbId_;
}

void* DrmBuffer::Lock() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!buffer_) {
        HWC2_LOGI("LayerId=%" PRIu64 " Buffer is null.", uId);
        return NULL;
    }

    void* cpu_addr = NULL;
    static int frame_cnt = 0;

    cpu_addr = ptrDrmGralloc_->hwc_get_handle_lock(buffer_, iWidth_, iHeight_);
    if (cpu_addr == NULL) {
        HWC2_LOGE("buffer-id=%" PRIu64 " lock fail", uId);
        return NULL;
    }

    return cpu_addr;
}

int DrmBuffer::Unlock() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!buffer_) {
        HWC2_LOGI("LayerId=%" PRIu64 " Buffer is null.", uId);
        return -1;
    }

    int ret = ptrDrmGralloc_->hwc_get_handle_unlock(buffer_);
    if (ret) {
        HWC2_LOGE("buffer-id=%" PRIu64 " unlock fail ret = %d ", uId, ret);
        return ret;
    }
    return ret;
}

int DrmBuffer::GetFinishFence() {
    return iFinishFence_.Dup();
}

int DrmBuffer::SetFinishFence(int fence) {
    std::lock_guard<std::mutex> lk(mtx_);

    //if iFinishFence_ present, wait and close it.
    if (iFinishFence_.get() > 0) {
        int ret = sync_wait(iFinishFence_.get(), 3000);
        iFinishFence_.Close();
        if (ret) {
            HWC2_LOGE("Failed to wait for finish fence %d/%d 3000ms", iFinishFence_.get(), ret);
            return -1;
        }
    }

    iFinishFence_.Set(fence);
    return 0;
}

int DrmBuffer::MergeFinishFence(int fence) {
    std::lock_guard<std::mutex> lk(mtx_);
    int merge_fd = fence;
    if (iFinishFence_.get() > 0) {
        int fd = sync_merge("DrmbufferMergeFence", fence, iFinishFence_.get());
        if (fd > 0) {
            close(fence);
            fence = -1;
            iFinishFence_.Close();
            merge_fd = fd;
        }
    }
    iFinishFence_.Set(merge_fd);
    return 0;
}

int DrmBuffer::WaitFinishFence(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    int ret = 0;
    if (iFinishFence_.get() > 0) {
        int fence_fd = dup(iFinishFence_.get());
        int old_fd = iFinishFence_.get();
        if (fence_fd < 0) {
            HWC2_LOGE("Failed to dup fence fd BufferId=0x%" PRIx64 " for Finish fence %d/%d ret=%d/ %dms", uBufferId_,
                      fence_fd, old_fd, ret, timeout_ms);
            return -1;
        }
        lk.unlock();
        ret = sync_wait(fence_fd, timeout_ms);
        if (ret) {
            if (timeout_ms > 0) {
                HWC2_LOGE("Failed to wait for finish fence %d/%d/%d ret=%d %dms", fence_fd, old_fd, iFinishFence_.get(),
                          ret, timeout_ms);
            }
        }
        close(fence_fd);
        fence_fd = -1;
    }
    return ret;
}

int DrmBuffer::GetReleaseFence() {
    std::lock_guard<std::mutex> lk(mtx_);
    return iReleaseFence_.Dup();
}

int DrmBuffer::SetReleaseFence(int fence) {
    std::lock_guard<std::mutex> lk(mtx_);
    iReleaseFence_.Set(fence);
    return 0;
}

int DrmBuffer::WaitReleaseFence(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    int ret = 0;
    if (iReleaseFence_.get() > 0) {
        int fence_fd = dup(iReleaseFence_.get());
        if (fence_fd < 0) {
            HWC2_LOGE("Failed to dup fence fd BufferId=0x%" PRIx64 " for release fence %d/%d %dms", uBufferId_,
                      fence_fd, ret, timeout_ms);
            return -1;
        }
        lk.unlock();
        ret = sync_wait(fence_fd, timeout_ms);
        if (ret) {
            if (timeout_ms > 0) {
                HWC2_LOGE("Failed to wait BufferId=0x%" PRIx64 " for release fence %d/%d %dms", uBufferId_, fence_fd,
                          ret, timeout_ms);
            }
        }
        close(fence_fd);
        fence_fd = -1;
    }
    return ret;
}

int DrmBuffer::DumpData(uint32_t frame_no, int zpos, const char* tag) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!buffer_) HWC2_LOGI("LayerId=%" PRIu64 " Buffer is null.", uId);

    void* cpu_addr = NULL;
    static int frame_cnt = 0;
    int ret = 0;

    //if iFinishFence_ present, wait and close it.
    if (iFinishFence_.get() > 0) {
        ret = sync_wait(iFinishFence_.get(), 3000);
        if (ret) {
            HWC2_LOGE("Failed to wait for finish fence %d/%d 3000ms", iFinishFence_.get(), ret);
        }
        iFinishFence_.Close();
    }

    cpu_addr = ptrDrmGralloc_->hwc_get_handle_lock(buffer_, iWidth_, iHeight_);
    if (cpu_addr == NULL) {
        HWC2_LOGE("buffer-id=%" PRIu64 " lock fail", uId);
        return -1;
    }

    std::string dump_file_name = HwcDumper::GetInstance()->GetDumpName(
            tag, frame_no, zpos, iStride_, iHeightStride_ > 0 ? iHeightStride_ : iHeight_, iByteStride_, uFourccFormat_,
            uModifier_, sName_, uBufferId_);

    HwcDumper::GetInstance()->DumpData(cpu_addr, iSize_, dump_file_name.c_str());

    ret = ptrDrmGralloc_->hwc_get_handle_unlock(buffer_);
    if (ret) {
        HWC2_LOGE("buffer-id=%" PRIu64 " unlock fail ret = %d ", uId, ret);
        return ret;
    }

    return ret;
}

#ifdef RK3528
bool DrmBuffer::IsPreScaleBuffer() {
    return bIsPreScale_;
}

int DrmBuffer::SwitchToPreScaleBuffer() {
    std::lock_guard<std::mutex> lk(mtx_);
    metadata_for_rkvdec_scaling_t* metadata = NULL;

    ptrDrmGralloc_->lock_rkvdec_scaling_metadata(buffer_, &metadata);
    HWC2_LOGI_IF("lock_rkvdec_scaling_metadata buffer_=%p metadata=%p", buffer_, metadata);
    if (metadata != NULL) {
        metadata->requestMask = 1;
        memcpy(&mMetadata_, metadata, sizeof(metadata_for_rkvdec_scaling_t));
        if (metadata->replyMask > 0) {
            bIsPreScale_ = true;
            SetCrop(metadata->srcTop, metadata->srcLeft, metadata->srcRight, metadata->srcBottom);

            iWidth_ = metadata->width;
            iHeight_ = metadata->height;
            iHeightStride_ = metadata->height;
            iStride_ = metadata->pixel_stride;
            iFormat_ = metadata->format;
            iUsage_ = metadata->usage;
            iByteStride_ = metadata->byteStride[0];
            uModifier_ = metadata->modifier;
            uFourccFormat_ = ptrDrmGralloc_->hwc_get_fourcc_from_hal_format(metadata->format);
        }

        // 打印参数
        HWC2_LOGI_IF("Name=%s metadata = %p", sName_.c_str(), metadata);
        HWC2_LOGI_IF("version=0x%" PRIx64 " requestMask=0x%" PRIx64
                     " "
                     "replyMask=0x%" PRIx64 " BufferId=0x%" PRIx64,
                     metadata->version, metadata->requestMask, metadata->replyMask, uBufferId_);
        HWC2_LOGI_IF("w=%d h=%d s=%d f=%d m=0x%" PRIx64 " usage=0x%x ", metadata->width, metadata->height,
                     metadata->pixel_stride, metadata->format, metadata->modifier, metadata->usage);
        HWC2_LOGI_IF("crop=(%d,%d,%d,%d) ", metadata->srcLeft, metadata->srcTop, metadata->srcRight,
                     metadata->srcBottom);
        HWC2_LOGI_IF("layer_cnt=%d offset=%d,%d,%d,%d byteStride=%d,%d,%d,%d) ", metadata->layer_cnt,
                     metadata->offset[0], metadata->offset[1], metadata->offset[2], metadata->offset[3],
                     metadata->byteStride[0], metadata->byteStride[1], metadata->byteStride[2],
                     metadata->byteStride[3]);
        ptrDrmGralloc_->unlock_rkvdec_scaling_metadata(buffer_);
    }

    if (bIsPreScale_)
        return 0;
    else
        return -1;
}

//注意：如果此方法外部调用，需要修改加锁并注意死锁问题
int DrmBuffer::ResetPreScaleBuffer() {
    bIsPreScale_ = false;
    iFd_ = ptrDrmGralloc_->hwc_get_handle_primefd(buffer_);
    iWidth_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_WIDTH);
    iHeight_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT);
    iStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_STRIDE);
    // 由于RK3528 版本无法获取 Stride ，故使用 width 代替 stride
    if (iStride_ == 0) {
        iStride_ = iWidth_;
    }

    iHeightStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_HEIGHT_STRIDE);
    iByteStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_BYTE_STRIDE_WORKROUND);
    iSize_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_SIZE);
    iFormat_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_, ATT_FORMAT);
    uFourccFormat_ = ptrDrmGralloc_->hwc_get_handle_fourcc_format(buffer_);
    uModifier_ = ptrDrmGralloc_->hwc_get_handle_format_modifier(buffer_);
    ptrDrmGralloc_->hwc_get_handle_buffer_id(buffer_, &uBufferId_);
    ptrDrmGralloc_->hwc_get_handle_name(buffer_, sName_);

    HWC2_LOGI("ResetPreScale Buffer fd=%d w=%d h=%d s=%d hs=%d bs=%d f=%d fcc=%c%c%c%c mdf=0x%" PRIx64
              " BufferId=0x%" PRIx64 "name=%s ",
              iFd_, iWidth_, iHeight_, iStride_, iHeightStride_, iByteStride_, iFormat_, uFourccFormat_,
              uFourccFormat_ >> 8, uFourccFormat_ >> 16, uFourccFormat_ >> 24, uModifier_, uBufferId_, sName_.c_str());
    bIsPreScale_ = false;
    return 0;
}

uint32_t DrmBuffer::GetPreScaleFbId() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (uPreScaleFbId_ > 0) {
        return uPreScaleFbId_;
    }

    if (uFbId_ > 0) {
        if (ptrDrmGralloc_->hwc_fbid_rm_cache(ptrDrmGralloc_->get_drm_device(), uFbId_)) {
            HWC2_LOGE("BufferId=0x%" PRIx64 " Failed to rm uFbId_ %d", uBufferId_, uFbId_);
        }
        uFbId_ = 0;
    }

    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t gem_handles[4] = {0};
    uint64_t modifier[4] = {0};

    pitches[0] = iByteStride_;
    gem_handles[0] = uGemHandle_;

    if (bIsPreScale_) {
        offsets[0] = mMetadata_.offset[0];
        offsets[1] = mMetadata_.offset[1];
        offsets[2] = mMetadata_.offset[2];
        offsets[3] = mMetadata_.offset[3];
    }

    if (DrmFormatToPlaneNum(uFourccFormat_) == 2) {
        if (uFourccFormat_ == DRM_FORMAT_NV24 || uFourccFormat_ == DRM_FORMAT_NV42 ||
            uFourccFormat_ == DRM_FORMAT_NV30) {
            pitches[1] = pitches[0] * 2;
            gem_handles[1] = uGemHandle_;
            if (offsets[1] == 0) {
                offsets[1] = offsets[0] + pitches[1] * iHeightStride_;
            }
        } else {
            pitches[1] = pitches[0];
            gem_handles[1] = uGemHandle_;
            if (offsets[1] == 0) {
                offsets[1] = offsets[0] + pitches[1] * iHeightStride_;
            }
        }
    }

    modifier[0] = uModifier_;
    if (DrmFormatToPlaneNum(uFourccFormat_) == 2) modifier[1] = uModifier_;

    if (uFourccFormat_ == DRM_FORMAT_NV12_10 && uModifier_ == 0) {
        iWidth_ = iWidth_ / 1.25;
        iWidth_ = ALIGN_DOWN(iWidth_, 2);
    }

    if (uFourccFormat_ == DRM_FORMAT_NV15 && uModifier_ == 0) {
        iWidth_ = iWidth_ / 1.25;
        iWidth_ = ALIGN_DOWN(iWidth_, 2);
    }

    int ret = ptrDrmGralloc_->hwc_fbid_get_and_cached(uBufferId_, ptrDrmGralloc_->get_drm_device(), iWidth_,
                                                      iHeightStride_, uFourccFormat_, gem_handles, pitches, offsets,
                                                      modifier, &uPreScaleFbId_, DRM_MODE_FB_MODIFIERS);

    HWC2_LOGD_IF(
            "ImportBuffer fd=%d,w=%d,h=%d,format=%c%c%c%c,"
            "gem_handle=%d %d %d %d, pitches[0]=%d %d %d %d, offsets[0]=%d %d %d %d fb_id=%d, modifier = %" PRIx64,
            ptrDrmGralloc_->get_drm_device(), iWidth_, iHeightStride_, uFourccFormat_, uFourccFormat_ >> 8,
            uFourccFormat_ >> 16, uFourccFormat_ >> 24, gem_handles[0], gem_handles[1], gem_handles[2], gem_handles[3],
            pitches[0], pitches[1], pitches[2], pitches[3], offsets[0], offsets[1], offsets[2], offsets[3],
            uPreScaleFbId_, modifier[0]);
    if (ret) {
        ALOGE("could not create drm fb %d", ret);
        HWC2_LOGE(
                "ImportBuffer fd=%d,w=%d,h=%d,format=%c%c%c%c,"
                "gem_handle=%d %d %d %d, pitches[0]=%d %d %d %d, offsets[0]=%d %d %d %d fb_id=%d, modifier = %" PRIx64,
                ptrDrmGralloc_->get_drm_device(), iWidth_, iHeightStride_, uFourccFormat_, uFourccFormat_ >> 8,
                uFourccFormat_ >> 16, uFourccFormat_ >> 24, gem_handles[0], gem_handles[1], gem_handles[2],
                gem_handles[3], pitches[0], pitches[1], pitches[2], pitches[3], offsets[0], offsets[1], offsets[2],
                offsets[3], uPreScaleFbId_, modifier[0]);
        return ret;
    }

    return uPreScaleFbId_;
}
#endif

rga_buffer_handle_t DrmBuffer::GetRgaHandle() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (uRgaHandle_ > 0) return uRgaHandle_;

    uRgaHandle_ = importbuffer_fd(iFd_, iSize_);
    if (uRgaHandle_ == 0) {
        HWC2_LOGE("RgaHandle %s import fail, fd=%d, size=%d buffer_id=0x%" PRIx64, sName_.c_str(), iFd_, iSize_,
                  uBufferId_);
        return 0;
    }
    return uRgaHandle_;
}

}  // namespace android
