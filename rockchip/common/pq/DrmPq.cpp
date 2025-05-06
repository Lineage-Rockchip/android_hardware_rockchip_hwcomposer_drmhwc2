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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "hwc-drm-pq"

#include "rockchip/pq/DrmPq.h"
#include "rockchip/utils/drmdebug.h"
#include "utils/drmfence.h"

DrmPq::DrmPq()
    : bInit_(false),
      mPq_(std::make_shared<Pq>()),
      mBufferQueue_(std::make_shared<DrmBufferQueue>(4)),
      mLastAcquireFence_(AcquireFence::NO_FENCE),
      mLastHwPqUseNewBuffer_(false),
      mLastHwPqReg_(nullptr) {}

DrmPq::~DrmPq() {};
int DrmPq::Init() {
    if (bInit_) return 0;

    int ret = mPq_->Init(PQ_VERSION);
    if (ret != 0) {
        HWC2_LOGE("DrmPq Init Failed, ret = %d", ret);
        return ret;
    }
    bInit_ = true;
    HWC2_LOGI("DrmPq Init Version=%s Successfully.", PQ_VERSION);
    return 0;
}

int DrmPq::RunHwPqUiMode(std::vector<DrmHwcLayer>& layers) {
    std::unique_lock<std::recursive_mutex> lock(mRecursiveMutex);
    if (bInit_ == false) {
        HWC2_LOGE("DrmPq Init fail, disable DrmPQ.");
        return -1;
    }
    int ret = 0;
    std::shared_ptr<DrmBuffer> dst_buffer;

    //如果存在视频图层有HWPQ metadata 先取出来
    int hwpq_meta_fd = -1;
    int64_t hwpq_meta_offset = 0;
    int64_t hwpq_meta_size = 0;

    HwPqImageInfo pq_dst_info;

    DrmGralloc* gralloc = DrmGralloc::getInstance();

    if (gralloc == NULL) {
        HWC2_LOGI_IF("DrmGralloc is null, Can not get PQ Metadata");
    } else {
        for (auto& layer : layers) {
            if (layer.bYuv_ && layer.bHasMetadata_) {
                int64_t iMetaDataOffset_ = gralloc->hwc_get_offset_of_pq_metadata(layer.sf_handle);
                if (iMetaDataOffset_ > 0) {
                    hwpq_meta_fd = layer.iFd_;
                    hwpq_meta_offset = iMetaDataOffset_;
                    hwpq_meta_size = layer.iSize_;
                    HWC2_LOGD_IF("Pq metadata:fd=%d offset=%" PRIi64 " name:%s", hwpq_meta_fd, hwpq_meta_offset,
                                 layer.sLayerName_.c_str());
                    break;
                }
            }
        }
    }

    for (auto& drmLayer : layers) {
        if (drmLayer.bFbTarget_) {
            if (!mLastHwPqBufInfo_.match(drmLayer)) {
                // 1. Fill buffer Info
                HwPqImageInfo src;
                src.mBufferInfo_.iFd_ = drmLayer.iFd_;
                src.mBufferInfo_.iWidth_ = drmLayer.iWidth_;
                src.mBufferInfo_.iHeight_ = drmLayer.iHeight_;
                src.mBufferInfo_.iFormat_ = drmLayer.iFormat_;
                src.mBufferInfo_.iStride_ = drmLayer.iStride_;
                src.mBufferInfo_.iHeightStride_ = drmLayer.iHeightStride_;
                src.mBufferInfo_.uBufferId_ = drmLayer.uBufferId_;
                src.mBufferInfo_.uDataSpace_ = (uint64_t)drmLayer.eDataSpace_;

                src.iRotation_ = drmLayer.transform;
                src.mIsFbcdFormat_ = drmLayer.bAfbcd_ || drmLayer.bRfbcd_;

                src.mCrop_.iLeft_ = (int)drmLayer.source_crop.left;
                src.mCrop_.iTop_ = (int)drmLayer.source_crop.top;
                src.mCrop_.iRight_ = (int)drmLayer.source_crop.right;
                src.mCrop_.iBottom_ = (int)drmLayer.source_crop.bottom;
                //Get and set MetaData
                src.iMetaDataFd_ = hwpq_meta_fd;
                src.iMetaDataSize_ = hwpq_meta_size;
                src.iMetaDataOffset_ = hwpq_meta_offset;

                src.mDisplayStatus_ = drmLayer.hwPqDisplayStatus;

                // 2. Alloc Dst buffer
                if (drmLayer.hwPqReg_ == NULL) {
                    drmLayer.hwPqReg_ = std::shared_ptr<rk_hwpq_reg>(new rk_hwpq_reg);
                    if (drmLayer.hwPqReg_ == NULL) {
                        HWC2_LOGE("rk_hwpq_reg alloc failed");
                        return -1;
                    }
                }

                // 3. Set buffer Info
                dst_buffer = NULL;
                pq_dst_info.mBufferInfo_.iFd_ = -1;
                pq_dst_info.mRkHwpqReg_ = drmLayer.hwPqReg_.get();

                bool needAllocNewBuffer = false;

                mPq_->Query(src, pq_dst_info, &needAllocNewBuffer);

                // 4. Alloc buffer if needed
                if (needAllocNewBuffer) {
                    HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
                    dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                            pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                            pq_dst_info.mBufferInfo_.iFormat_,
                            RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC, "PQ-Video-target");

                    if (dst_buffer == NULL) {
                        HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
                        return -1;
                    }

                    // 4.1 Set dst buffer Info
                    pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
                    pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
                    pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
                    pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
                    pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
                    pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
                    pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
                    //Update info for new buffer
                    src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).VopSrcRect_ =
                            pq_dst_info.mCrop_;
                    src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uFourccFormat_ =
                            dst_buffer->GetFourccFormat();
                    src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).iHalFormat_ =
                            dst_buffer->GetFormat();
                    src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uModifier_ =
                            dst_buffer->GetModifier();
                    src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uDataSpace_ =
                            pq_dst_info.mBufferInfo_.uDataSpace_;
                } else {
                    pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
                }
                HwPqImageInfo dst_buffer_Info_ = pq_dst_info;
                HWC2_LOGD_IF("src: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, src.mBufferInfo_.iFd_,
                             src.mBufferInfo_.iWidth_, src.mBufferInfo_.iHeight_, src.mBufferInfo_.iFormat_,
                             src.mBufferInfo_.iStride_, src.mBufferInfo_.iHeightStride_, src.mBufferInfo_.uBufferId_);
                ret = mPq_->SetHwPqSrcImage(src, pq_dst_info, &needAllocNewBuffer);
                if (ret) {
                    if (ret == PqUnInit)
                        HWC2_LOGW("Pq SetHwPqSrcImage fail, Pq may still initializing, ret = %d", ret);
                    else if (PqUnSupported)
                        HWC2_LOGE("Pq SetHwPqSrcImage fail, Src is UnSupported, ret = %d", ret);
                    else
                        HWC2_LOGE("Pq SetHwPqSrcImage fail, ret = %d", ret);
                    return ret;
                }

                // 4. ReAlloc buffer if needed
                if (needAllocNewBuffer) {
                    //if New buffer is needed, and it's resolution changed, realloc buffer
                    if (!dst_buffer || pq_dst_info.mBufferInfo_.iWidth_ != dst_buffer_Info_.mBufferInfo_.iWidth_ ||
                        pq_dst_info.mBufferInfo_.iHeight_ != dst_buffer_Info_.mBufferInfo_.iHeight_ ||
                        pq_dst_info.mBufferInfo_.iFormat_ != dst_buffer_Info_.mBufferInfo_.iFormat_) {
                        HWC2_LOGW(
                                "HWPQ: Dst buffer changed between querry and SetHwPqSrcImage!![%dx%d f=%d]->[%dx%d "
                                "f=%d]",
                                dst_buffer_Info_.mBufferInfo_.iWidth_, dst_buffer_Info_.mBufferInfo_.iHeight_,
                                dst_buffer_Info_.mBufferInfo_.iFormat_, pq_dst_info.mBufferInfo_.iWidth_,
                                pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_);
                        if (dst_buffer) {
                            mBufferQueue_->QueueBuffer(dst_buffer);
                            dst_buffer = NULL;
                        }
                        HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
                        dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                                pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                                pq_dst_info.mBufferInfo_.iFormat_,
                                RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC, "PQ-Video-target");

                        if (dst_buffer == NULL) {
                            HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
                            return -1;
                        }
                    }

                    // 4.1 Set dst buffer Info
                    pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
                    pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
                    pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
                    pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
                    pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
                    pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
                    pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
                } else {
                    //If New buffer is not need anymore
                    if (dst_buffer) {
                        mBufferQueue_->QueueBuffer(dst_buffer);
                        dst_buffer = NULL;
                    }
                    pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
                }

                HWC2_LOGD_IF("dst: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64,
                             pq_dst_info.mBufferInfo_.iFd_, pq_dst_info.mBufferInfo_.iWidth_,
                             pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_,
                             pq_dst_info.mBufferInfo_.iStride_, pq_dst_info.mBufferInfo_.iHeightStride_,
                             pq_dst_info.mBufferInfo_.uBufferId_);
                ret = mPq_->SetHwPqDstImage(pq_dst_info);
                if (ret) {
                    HWC2_LOGE("Pq SetHwPqDstImage fail ret = %d", ret);
                    if (dst_buffer != NULL) mBufferQueue_->QueueBuffer(dst_buffer);
                    return ret;
                }
                // 5. Run HWPQ
                int output_fence = -1;
                int acquire_fence = -1;

                if (drmLayer.acquire_fence->isValid()) {
                    acquire_fence = dup(drmLayer.acquire_fence->getFd());
                }

                int ret = mPq_->RunHwPqAsync(&output_fence, acquire_fence);
                if (ret) {
                    HWC2_LOGE("RunHwPqAsync fail! ret = %d", ret);
                    if (dst_buffer != NULL) mBufferQueue_->QueueBuffer(dst_buffer);
                    return ret;
                }

                // 6. Update Layer info
                drmLayer.hwPqRegAcquireFence_ = sp<AcquireFence>(new AcquireFence(output_fence));

                mLastHwPqBufInfo_.set(drmLayer);
                mLastHwPqReg_ = drmLayer.hwPqReg_;
                mLastAcquireFence_ = drmLayer.hwPqRegAcquireFence_;

                if (dst_buffer != NULL) {
                    mLastHwPqUseNewBuffer_ = true;

                    dst_buffer->SetFinishFence(dup(output_fence));
                    mBufferQueue_->QueueBuffer(dst_buffer);

                    drmLayer.bUseHwpqScale_ = true;
                    drmLayer.bUsePq_ = true;
                    drmLayer.pPqBuffer_ = dst_buffer;
                    drmLayer.acquire_fence = sp<AcquireFence>(new AcquireFence(dup(output_fence)));
                    hwc_frect_t source_crop;
                    source_crop.left = pq_dst_info.mCrop_.iLeft_;
                    source_crop.top = pq_dst_info.mCrop_.iTop_;
                    source_crop.right = pq_dst_info.mCrop_.iRight_;
                    source_crop.bottom = pq_dst_info.mCrop_.iBottom_;

                    dst_buffer->SetCrop(source_crop.left, source_crop.top, source_crop.right, source_crop.bottom);

                    drmLayer.UpdateAndStoreInfoFromDrmBuffer(
                            dst_buffer->GetHandle(), dst_buffer->GetFd(), dst_buffer->GetFormat(),
                            dst_buffer->GetWidth(), dst_buffer->GetHeight(), dst_buffer->GetStride(),
                            dst_buffer->GetHeightStride(), dst_buffer->GetByteStride(), dst_buffer->GetSize(),
                            dst_buffer->GetUsage(), dst_buffer->GetFourccFormat(), dst_buffer->GetModifier(),
                            dst_buffer->GetByteStridePlanes(), dst_buffer->GetName(), source_crop,
                            dst_buffer->GetBufferId(), dst_buffer->GetGemHandle(), drmLayer.transform);
                    //dump HwPq layer if needed
                    char value[PROPERTY_VALUE_MAX];
                    property_get("vendor.dump", value, "false");
                    if (!strcmp(value, "true")) {
                        dst_buffer->WaitFinishFence();
                        dst_buffer->DumpData(drmLayer.uFrameNo_, drmLayer.iZpos_, "M");
                    }
                } else {
                    mLastHwPqUseNewBuffer_ = false;
                }

                drmLayer.Init();
                if (gIsDrmVerison6_1()) {
                    drmLayer.uColorSpace.colorspace_kernel_6_1_.color_encoding_ = DRM_COLOR_YCBCR_BT601;
                    drmLayer.uColorSpace.colorspace_kernel_6_1_.color_range_ = DRM_COLOR_YCBCR_FULL_RANGE;
                } else {
                    drmLayer.uColorSpace.colorspace_kernel_510_ = V4L2_COLORSPACE_JPEG;
                }
                return 0;

            } else {  //Use last buffer

                if (mLastHwPqUseNewBuffer_)
                    dst_buffer = mBufferQueue_->BackDrmBuffer();
                else {
                    dst_buffer = NULL;
                }

                if (dst_buffer != NULL) {
                    int left = 0, top = 0, right = 0, bottom = 0;
                    dst_buffer->GetCrop(&left, &top, &right, &bottom);
                    hwc_frect_t source_crop;
                    source_crop.left = left;
                    source_crop.top = top;
                    source_crop.right = right;
                    source_crop.bottom = bottom;
                    drmLayer.UpdateAndStoreInfoFromDrmBuffer(
                            dst_buffer->GetHandle(), dst_buffer->GetFd(), dst_buffer->GetFormat(),
                            dst_buffer->GetWidth(), dst_buffer->GetHeight(), dst_buffer->GetStride(),
                            dst_buffer->GetHeightStride(), dst_buffer->GetByteStride(), dst_buffer->GetSize(),
                            dst_buffer->GetUsage(), dst_buffer->GetFourccFormat(), dst_buffer->GetModifier(),
                            dst_buffer->GetByteStridePlanes(), dst_buffer->GetName(), source_crop,
                            dst_buffer->GetBufferId(), dst_buffer->GetGemHandle(), drmLayer.transform);
                    // Update Layer info
                    drmLayer.acquire_fence = mLastAcquireFence_;
                    drmLayer.bUseHwpqScale_ = true;
                    drmLayer.bUsePq_ = true;
                    drmLayer.pPqBuffer_ = dst_buffer;
                } else {
                    HWC2_LOGD_IF("dst_buffer = null, hwpq use original buffer");
                }

                drmLayer.hwPqReg_ = mLastHwPqReg_;
                drmLayer.Init();
                if (gIsDrmVerison6_1()) {
                    drmLayer.uColorSpace.colorspace_kernel_6_1_.color_encoding_ = DRM_COLOR_YCBCR_BT601;
                    drmLayer.uColorSpace.colorspace_kernel_6_1_.color_range_ = DRM_COLOR_YCBCR_FULL_RANGE;
                } else {
                    drmLayer.uColorSpace.colorspace_kernel_510_ = V4L2_COLORSPACE_JPEG;
                }
                return 0;
            }
        }
    }
    return -1;
}

int DrmPq::RunHwPqVideoMode(DrmHwcLayer* drmLayer) {
    std::unique_lock<std::recursive_mutex> lock(mRecursiveMutex);
    if (bInit_ == false) {
        HWC2_LOGE("DrmPq Init fail, disable DrmPQ.");
        return -1;
    }
    int ret = 0;
    HwPqImageInfo pq_dst_info;
    std::shared_ptr<DrmBuffer> dst_buffer;
    bool isHwPqAsPostProcess = false;

    if (drmLayer->bYuv_) {
        if (!mLastHwPqBufInfo_.match(drmLayer)) {
            if (drmLayer->bUseRga_) {
                isHwPqAsPostProcess = true;
            }
            // 1. Fill buffer Info
            HwPqImageInfo src;

            if (isHwPqAsPostProcess) {
                src.mBufferInfo_.iFd_ = drmLayer->storeLayerInfo_.iFd_;
                src.mBufferInfo_.iWidth_ = drmLayer->storeLayerInfo_.iWidth_;
                src.mBufferInfo_.iHeight_ = drmLayer->storeLayerInfo_.iHeight_;
                src.mBufferInfo_.iFormat_ = drmLayer->storeLayerInfo_.iFormat_;
                src.mBufferInfo_.iStride_ = drmLayer->storeLayerInfo_.iStride_;
                src.mBufferInfo_.iHeightStride_ = drmLayer->storeLayerInfo_.iHeightStride_;
                src.mBufferInfo_.uBufferId_ = drmLayer->storeLayerInfo_.uBufferId_;
                src.mBufferInfo_.uDataSpace_ = (uint64_t)drmLayer->storeLayerInfo_.eDataSpace_;

                src.iRotation_ = drmLayer->storeLayerInfo_.transform;
                src.mIsFbcdFormat_ = drmLayer->storeLayerInfo_.bAfbcd_ || drmLayer->storeLayerInfo_.bRfbcd_;

                src.mCrop_.iLeft_ = (int)drmLayer->storeLayerInfo_.source_crop.left;
                src.mCrop_.iTop_ = (int)drmLayer->storeLayerInfo_.source_crop.top;
                src.mCrop_.iRight_ = (int)drmLayer->storeLayerInfo_.source_crop.right;
                src.mCrop_.iBottom_ = (int)drmLayer->storeLayerInfo_.source_crop.bottom;
            } else {
                src.mBufferInfo_.iFd_ = drmLayer->iFd_;
                src.mBufferInfo_.iWidth_ = drmLayer->iWidth_;
                src.mBufferInfo_.iHeight_ = drmLayer->iHeight_;
                src.mBufferInfo_.iFormat_ = drmLayer->iFormat_;
                src.mBufferInfo_.iStride_ = drmLayer->iStride_;
                src.mBufferInfo_.iHeightStride_ = drmLayer->iHeightStride_;
                src.mBufferInfo_.uBufferId_ = drmLayer->uBufferId_;
                src.mBufferInfo_.uDataSpace_ = (uint64_t)drmLayer->eDataSpace_;

                src.iRotation_ = drmLayer->transform;
                src.mIsFbcdFormat_ = drmLayer->bAfbcd_ || drmLayer->bRfbcd_;

                src.mCrop_.iLeft_ = (int)drmLayer->source_crop.left;
                src.mCrop_.iTop_ = (int)drmLayer->source_crop.top;
                src.mCrop_.iRight_ = (int)drmLayer->source_crop.right;
                src.mCrop_.iBottom_ = (int)drmLayer->source_crop.bottom;
            }
            //Get and set MetaData
            src.iMetaDataFd_ = -1;
            src.iMetaDataSize_ = 0;
            src.iMetaDataOffset_ = 0;

            DrmGralloc* gralloc = DrmGralloc::getInstance();
            if (gralloc == NULL) {
                HWC2_LOGI_IF("DrmGralloc is null, Can not get PQ Metadata");
            } else if (drmLayer->bHasMetadata_) {
                src.iMetaDataOffset_ = gralloc->hwc_get_offset_of_pq_metadata(drmLayer->sf_handle);
                if (src.iMetaDataOffset_ > 0) {
                    src.iMetaDataFd_ = drmLayer->iFd_;
                    src.iMetaDataSize_ = drmLayer->iSize_;
                    HWC2_LOGD_IF("Pq metadata:fd=%d offset=%" PRIi64 " size=%" PRIi64, src.iMetaDataFd_,
                                 src.iMetaDataOffset_, src.iMetaDataSize_);
                }
            }

            src.mDisplayStatus_ = drmLayer->hwPqDisplayStatus;

            // 2. Alloc Dst buffer
            if (drmLayer->hwPqReg_ == NULL) {
                drmLayer->hwPqReg_ = std::shared_ptr<rk_hwpq_reg>(new rk_hwpq_reg);
                if (drmLayer->hwPqReg_ == NULL) {
                    HWC2_LOGE("rk_hwpq_reg alloc failed");
                    return -1;
                }
            }

            // 3. Set buffer Info
            dst_buffer = NULL;
            pq_dst_info.mBufferInfo_.iFd_ = -1;
            pq_dst_info.mRkHwpqReg_ = drmLayer->hwPqReg_.get();

            bool needAllocNewBuffer = false;

            mPq_->Query(src, pq_dst_info, &needAllocNewBuffer);

            // 4. Alloc buffer if needed
            if (needAllocNewBuffer) {
                HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
                dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                        pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                        pq_dst_info.mBufferInfo_.iFormat_,
                        RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC, "PQ-Video-target");

                if (dst_buffer == NULL) {
                    HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
                    return -1;
                }

                // 4.1 Set dst buffer Info
                pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
                pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
                pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
                pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
                pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
                pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
                pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
                //Update info for new buffer
                src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).VopSrcRect_ = pq_dst_info.mCrop_;
                src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uFourccFormat_ =
                        dst_buffer->GetFourccFormat();
                src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).iHalFormat_ =
                        dst_buffer->GetFormat();
                src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uModifier_ =
                        dst_buffer->GetModifier();
                src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uDataSpace_ =
                        pq_dst_info.mBufferInfo_.uDataSpace_;
            } else {
                pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
            }
            HwPqImageInfo dst_buffer_Info_ = pq_dst_info;
            HWC2_LOGD_IF("src: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, src.mBufferInfo_.iFd_,
                         src.mBufferInfo_.iWidth_, src.mBufferInfo_.iHeight_, src.mBufferInfo_.iFormat_,
                         src.mBufferInfo_.iStride_, src.mBufferInfo_.iHeightStride_, src.mBufferInfo_.uBufferId_);
            ret = mPq_->SetHwPqSrcImage(src, pq_dst_info, &needAllocNewBuffer);
            if (ret) {
                if (ret == PqUnInit)
                    HWC2_LOGW("Pq SetHwPqSrcImage fail, Pq may still initializing, ret = %d", ret);
                else if (PqUnSupported)
                    HWC2_LOGE("Pq SetHwPqSrcImage fail, Src is UnSupported, ret = %d", ret);
                else
                    HWC2_LOGE("Pq SetHwPqSrcImage fail, ret = %d", ret);
                return ret;
            }

            // 4. ReAlloc buffer if needed
            if (needAllocNewBuffer) {
                //if New buffer is needed, and it's resolution changed, realloc buffer
                if (!dst_buffer || pq_dst_info.mBufferInfo_.iWidth_ != dst_buffer_Info_.mBufferInfo_.iWidth_ ||
                    pq_dst_info.mBufferInfo_.iHeight_ != dst_buffer_Info_.mBufferInfo_.iHeight_ ||
                    pq_dst_info.mBufferInfo_.iFormat_ != dst_buffer_Info_.mBufferInfo_.iFormat_) {
                    HWC2_LOGW("HWPQ: Dst buffer changed between querry and SetHwPqSrcImage!![%dx%d f=%d]->[%dx%d f=%d]",
                              dst_buffer_Info_.mBufferInfo_.iWidth_, dst_buffer_Info_.mBufferInfo_.iHeight_,
                              dst_buffer_Info_.mBufferInfo_.iFormat_, pq_dst_info.mBufferInfo_.iWidth_,
                              pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_);
                    if (dst_buffer) {
                        mBufferQueue_->QueueBuffer(dst_buffer);
                        dst_buffer = NULL;
                    }
                    HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
                    dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                            pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                            pq_dst_info.mBufferInfo_.iFormat_,
                            RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC, "PQ-Video-target");

                    if (dst_buffer == NULL) {
                        HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
                        return -1;
                    }
                }

                // 4.1 Set dst buffer Info
                pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
                pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
                pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
                pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
                pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
                pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
                pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
            } else {
                //If New buffer is not need anymore
                if (dst_buffer) {
                    mBufferQueue_->QueueBuffer(dst_buffer);
                    dst_buffer = NULL;
                }
                pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
            }

            HWC2_LOGD_IF("dst: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, pq_dst_info.mBufferInfo_.iFd_,
                         pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                         pq_dst_info.mBufferInfo_.iFormat_, pq_dst_info.mBufferInfo_.iStride_,
                         pq_dst_info.mBufferInfo_.iHeightStride_, pq_dst_info.mBufferInfo_.uBufferId_);
            ret = mPq_->SetHwPqDstImage(pq_dst_info);
            if (ret) {
                HWC2_LOGE("Pq SetHwPqDstImage fail ret = %d", ret);
                if (dst_buffer != NULL) mBufferQueue_->QueueBuffer(dst_buffer);
                return ret;
            }
            // 5. Run HWPQ
            int output_fence = -1;
            int acquire_fence = -1;

            if (drmLayer->acquire_fence->isValid()) {
                acquire_fence = dup(drmLayer->acquire_fence->getFd());
            }

            int ret = mPq_->RunHwPqAsync(&output_fence, acquire_fence);
            if (ret) {
                HWC2_LOGE("RunHwPqAsync fail! ret = %d", ret);
                if (dst_buffer != NULL) mBufferQueue_->QueueBuffer(dst_buffer);
                return ret;
            }

            // 6. Update Layer info
            drmLayer->hwPqRegAcquireFence_ = sp<AcquireFence>(new AcquireFence(output_fence));
            if (isHwPqAsPostProcess) {
                //in Hwpq As PostProcess mode, only pq register is needed, release buffer now
                if (dst_buffer != NULL) {
                    dst_buffer->SetFinishFence(dup(output_fence));
                    mBufferQueue_->QueueBuffer(dst_buffer);
                    dst_buffer = NULL;
                }
            }

            mLastHwPqBufInfo_.set(drmLayer);
            mLastHwPqReg_ = drmLayer->hwPqReg_;
            mLastAcquireFence_ = drmLayer->hwPqRegAcquireFence_;

            if (dst_buffer != NULL) {
                mLastHwPqUseNewBuffer_ = true;

                dst_buffer->SetFinishFence(dup(output_fence));
                mBufferQueue_->QueueBuffer(dst_buffer);

                drmLayer->bUseHwpqScale_ = true;
                drmLayer->pPqBuffer_ = dst_buffer;
                drmLayer->acquire_fence = sp<AcquireFence>(new AcquireFence(dup(output_fence)));
                hwc_frect_t source_crop;
                source_crop.left = pq_dst_info.mCrop_.iLeft_;
                source_crop.top = pq_dst_info.mCrop_.iTop_;
                source_crop.right = pq_dst_info.mCrop_.iRight_;
                source_crop.bottom = pq_dst_info.mCrop_.iBottom_;

                dst_buffer->SetCrop(source_crop.left, source_crop.top, source_crop.right, source_crop.bottom);

                drmLayer->UpdateAndStoreInfoFromDrmBuffer(
                        dst_buffer->GetHandle(), dst_buffer->GetFd(), dst_buffer->GetFormat(), dst_buffer->GetWidth(),
                        dst_buffer->GetHeight(), dst_buffer->GetStride(), dst_buffer->GetHeightStride(),
                        dst_buffer->GetByteStride(), dst_buffer->GetSize(), dst_buffer->GetUsage(),
                        dst_buffer->GetFourccFormat(), dst_buffer->GetModifier(), dst_buffer->GetByteStridePlanes(),
                        dst_buffer->GetName(), source_crop, dst_buffer->GetBufferId(), dst_buffer->GetGemHandle(),
                        drmLayer->transform);
                //dump HwPq layer if needed
                char value[PROPERTY_VALUE_MAX];
                property_get("vendor.dump", value, "false");
                if (!strcmp(value, "true")) {
                    dst_buffer->WaitFinishFence();
                    dst_buffer->DumpData(0, drmLayer->iZpos_, "M");
                }
            } else {
                mLastHwPqUseNewBuffer_ = false;
            }

            return 0;

        } else {  //Use last buffer

            if (mLastHwPqUseNewBuffer_)
                dst_buffer = mBufferQueue_->BackDrmBuffer();
            else {
                dst_buffer = NULL;
            }

            if (dst_buffer != NULL) {
                int left = 0, top = 0, right = 0, bottom = 0;
                dst_buffer->GetCrop(&left, &top, &right, &bottom);
                hwc_frect_t source_crop;
                source_crop.left = left;
                source_crop.top = top;
                source_crop.right = right;
                source_crop.bottom = bottom;
                drmLayer->UpdateAndStoreInfoFromDrmBuffer(
                        dst_buffer->GetHandle(), dst_buffer->GetFd(), dst_buffer->GetFormat(), dst_buffer->GetWidth(),
                        dst_buffer->GetHeight(), dst_buffer->GetStride(), dst_buffer->GetHeightStride(),
                        dst_buffer->GetByteStride(), dst_buffer->GetSize(), dst_buffer->GetUsage(),
                        dst_buffer->GetFourccFormat(), dst_buffer->GetModifier(), dst_buffer->GetByteStridePlanes(),
                        dst_buffer->GetName(), source_crop, dst_buffer->GetBufferId(), dst_buffer->GetGemHandle(),
                        drmLayer->transform);
                // Update Layer info
                drmLayer->acquire_fence = mLastAcquireFence_;
                drmLayer->bUseHwpqScale_ = true;
                drmLayer->pPqBuffer_ = dst_buffer;
            } else {
                HWC2_LOGD_IF("dst_buffer = null, hwpq use original buffer");
            }

            drmLayer->hwPqReg_ = mLastHwPqReg_;
            return 0;
        }
    } else {
        HWC2_LOGV_IF("Found cluster0-win0 vp0 layer is not video, skip hwpq policy");
        return -1;
    }
}

std::shared_ptr<DrmBuffer> DrmPq::RunHwPqSidebandMode(std::shared_ptr<DrmBuffer> src_buffer,
                                                      std::shared_ptr<android::HwpqDisplayStatus> display_state) {
    std::unique_lock<std::recursive_mutex> lock(mRecursiveMutex);
    if (bInit_ == false) {
        HWC2_LOGE("DrmPq Init fail, disable DrmPQ.");
        return NULL;
    }
    int left, top, right, bottom;
    src_buffer->GetCrop(&left, &top, &right, &bottom);

    int act_w = right - left;
    int act_h = bottom - top;

    int ret = 0;
    // 1. 初始化Ctx
    // 2. Fill buffer Info
    HwPqImageInfo src;
    src.mBufferInfo_.iFd_ = src_buffer->GetFd();
    src.mBufferInfo_.iWidth_ = src_buffer->GetWidth();
    src.mBufferInfo_.iHeight_ = src_buffer->GetHeight();
    src.mBufferInfo_.iFormat_ = src_buffer->GetFormat();
    src.mBufferInfo_.iStride_ = src_buffer->GetStride();
    src.mBufferInfo_.iHeightStride_ = src_buffer->GetHeightStride();
    src.mBufferInfo_.uBufferId_ = src_buffer->GetBufferId();
    src.mBufferInfo_.uDataSpace_ = (uint64_t)src_buffer->GetDataspace();

    src.mIsFbcdFormat_ = (src_buffer->IsAfbc() || src_buffer->IsRfbc());

    src.mCrop_.iLeft_ = (int)left;
    src.mCrop_.iTop_ = (int)top;
    src.mCrop_.iRight_ = (int)right;
    src.mCrop_.iBottom_ = (int)bottom;

    src.iMetaDataFd_ = -1;
    src.iMetaDataSize_ = 0;
    src.iMetaDataOffset_ = 0;

    src.mDisplayStatus_ = display_state;

    // src.mVirtualAddress_ = src_buffer->Lock();
    DrmGralloc* gralloc = DrmGralloc::getInstance();
    if (gralloc == NULL) {
        HWC2_LOGI_IF("DrmGralloc is null, Can not get PQ Metadata");
    } else {
        src.iMetaDataOffset_ = gralloc->hwc_get_offset_of_pq_metadata(src_buffer->GetHandle());
        if (src.iMetaDataOffset_ > 0) {
            src.iMetaDataFd_ = src_buffer->GetFd();
            src.iMetaDataSize_ = src_buffer->GetSize();
        }
    }

    // 3. Set buffer Info
    std::shared_ptr<DrmBuffer> dst_buffer = NULL;
    HwPqImageInfo pq_dst_info;
    pq_dst_info.mBufferInfo_.iFd_ = -1;
    pq_dst_info.mRkHwpqReg_ = src_buffer->GetHwPqRegs().get();

    bool needAllocNewBuffer = false;

    mPq_->Query(src, pq_dst_info, &needAllocNewBuffer);

    // 4. Alloc buffer if needed
    if (needAllocNewBuffer) {
        HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
        dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_,
                RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC, "PQ-Sideband-target");

        if (dst_buffer == NULL) {
            HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
            return NULL;
        }

        // 4.1 Set dst buffer Info
        pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
        pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
        pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
        pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
        pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
        pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
        pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
        //Update info for new buffer
        src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).VopSrcRect_ = pq_dst_info.mCrop_;
        src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uFourccFormat_ =
                dst_buffer->GetFourccFormat();
        src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).iHalFormat_ = dst_buffer->GetFormat();
        src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uModifier_ = dst_buffer->GetModifier();
        src.mDisplayStatus_->mapPlaneInfo_.at(src.mDisplayStatus_->iPqZpos).uDataSpace_ =
                pq_dst_info.mBufferInfo_.uDataSpace_;
    } else {
        pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
    }
    HwPqImageInfo dst_buffer_Info_ = pq_dst_info;
    HWC2_LOGD_IF("src: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, src.mBufferInfo_.iFd_,
                 src.mBufferInfo_.iWidth_, src.mBufferInfo_.iHeight_, src.mBufferInfo_.iFormat_,
                 src.mBufferInfo_.iStride_, src.mBufferInfo_.iHeightStride_, src.mBufferInfo_.uBufferId_);
    ret = mPq_->SetHwPqSrcImage(src, pq_dst_info, &needAllocNewBuffer);
    if (ret) {
        if (ret == PqUnInit)
            HWC2_LOGW("Pq SetHwPqSrcImage fail, Pq may still initializing, ret = %d", ret);
        else if (PqUnSupported)
            HWC2_LOGE("Pq SetHwPqSrcImage fail, Src is UnSupported, ret = %d", ret);
        else
            HWC2_LOGE("Pq SetHwPqSrcImage fail, ret = %d", ret);
        return NULL;
    }

    // 4. ReAlloc buffer if needed
    if (needAllocNewBuffer) {
        //if New buffer is needed, and it's resolution changed, realloc buffer
        if (!dst_buffer || pq_dst_info.mBufferInfo_.iWidth_ != dst_buffer_Info_.mBufferInfo_.iWidth_ ||
            pq_dst_info.mBufferInfo_.iHeight_ != dst_buffer_Info_.mBufferInfo_.iHeight_ ||
            pq_dst_info.mBufferInfo_.iFormat_ != dst_buffer_Info_.mBufferInfo_.iFormat_) {
            HWC2_LOGW("HWPQ: Dst buffer changed between querry and SetHwPqSrcImage!![%dx%d f=%d]->[%dx%d f=%d]",
                      dst_buffer_Info_.mBufferInfo_.iWidth_, dst_buffer_Info_.mBufferInfo_.iHeight_,
                      dst_buffer_Info_.mBufferInfo_.iFormat_, pq_dst_info.mBufferInfo_.iWidth_,
                      pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_);
            if (dst_buffer) {
                mBufferQueue_->QueueBuffer(dst_buffer);
                dst_buffer = NULL;
            }
            HWC2_LOGD_IF("hwpq use new vdpp buffer mode, alloc Buffer!");
            dst_buffer = mBufferQueue_->DequeueDrmBuffer(
                    pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_,
                    pq_dst_info.mBufferInfo_.iFormat_, RK_GRALLOC_USAGE_STRIDE_ALIGN_16 | MALI_GRALLOC_USAGE_NO_AFBC,
                    "PQ-Video-target");

            if (dst_buffer == NULL) {
                HWC2_LOGD_IF("DequeueDrmBuffer fail!, skip this policy.");
                return NULL;
            }
        }

        // 4.1 Set dst buffer Info
        pq_dst_info.mBufferInfo_.iFd_ = dst_buffer->GetFd();
        pq_dst_info.mBufferInfo_.iWidth_ = dst_buffer->GetWidth();
        pq_dst_info.mBufferInfo_.iHeight_ = dst_buffer->GetHeight();
        pq_dst_info.mBufferInfo_.iFormat_ = dst_buffer->GetFormat();
        pq_dst_info.mBufferInfo_.iStride_ = dst_buffer->GetStride();
        pq_dst_info.mBufferInfo_.iHeightStride_ = dst_buffer->GetHeightStride();
        pq_dst_info.mBufferInfo_.uBufferId_ = dst_buffer->GetBufferId();
    } else {
        //If New buffer is not need anymore
        if (dst_buffer) {
            mBufferQueue_->QueueBuffer(dst_buffer);
            dst_buffer = NULL;
        }
        pq_dst_info.mBufferInfo_ = src.mBufferInfo_;
    }

    HWC2_LOGD_IF("dst: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, pq_dst_info.mBufferInfo_.iFd_,
                 pq_dst_info.mBufferInfo_.iWidth_, pq_dst_info.mBufferInfo_.iHeight_, pq_dst_info.mBufferInfo_.iFormat_,
                 pq_dst_info.mBufferInfo_.iStride_, pq_dst_info.mBufferInfo_.iHeightStride_,
                 pq_dst_info.mBufferInfo_.uBufferId_);

    //Alloc rk_hwpq_reg变量
    if (dst_buffer == NULL)
        pq_dst_info.mRkHwpqReg_ = src_buffer->GetHwPqRegs().get();
    else
        pq_dst_info.mRkHwpqReg_ = dst_buffer->GetHwPqRegs().get();

    // SR 使能需要关闭HWPQ 锐化
    if (src_buffer->GetBufferType() == DRM_BUFFER_TYPE_SR) {
        mPq_->setSharpEnable(false);
    } else {
        mPq_->setSharpEnable(true);
    }

    //设置目标属性
    ret = mPq_->SetHwPqDstImage(pq_dst_info);
    if (ret) {
        HWC2_LOGE("buffer_id=0x%" PRIx64 " Pq SetHwPqDstImage fail, ret = %d", src_buffer->GetExternalId(), ret);
        return NULL;
    }

    src_buffer->WaitFinishFence();

    //执行PQ
    ret = mPq_->RunHwPq();
    if (ret) {
        HWC2_LOGE("buffer_id=0x%" PRIx64 " .hwPq Run fail ret = %d", src_buffer->GetExternalId(), ret);
        return NULL;
    }

    if (dst_buffer) {
        char value[PROPERTY_VALUE_MAX];
        property_get("vendor.dump", value, "false");
        if (!strcmp(value, "true")) {
            dst_buffer->DumpData(0, 0, "S");
        }
        //Update Crop form Query Result
        dst_buffer->SetCrop(pq_dst_info.mCrop_.iLeft_, pq_dst_info.mCrop_.iTop_, pq_dst_info.mCrop_.iRight_,
                            pq_dst_info.mCrop_.iBottom_);
        mBufferQueue_->QueueBuffer(dst_buffer);
        return dst_buffer;
    }
    return src_buffer;
}

int DrmPq::CheckHwPqDstScale(DrmHwcLayer* drmLayer) {
    std::unique_lock<std::recursive_mutex> lock(mRecursiveMutex);
    if (bInit_ == false) {
        HWC2_LOGE("DrmPq Init fail, disable DrmPQ.");
        return -1;
    }

    if (drmLayer->bCheckPqScale) {
        return 0;
    }
    int ret = 0;
    //目前只考虑对视频图层做HWPQ
    if (!drmLayer->bYuv_) {
        HWC2_LOGV_IF("layer is not video, skip hwpq");
        return -1;
    }
    if (drmLayer->bAfbcd_ || drmLayer->bRfbcd_) {
        HWC2_LOGD_IF("HwPq do not support Fbc layer:%s", drmLayer->sLayerName_.c_str());
        return -1;
    }
    // 1. Fill buffer Info
    HwPqImageInfo src;
    src.mBufferInfo_.iFd_ = drmLayer->iFd_;
    src.mBufferInfo_.iWidth_ = drmLayer->iWidth_;
    src.mBufferInfo_.iHeight_ = drmLayer->iHeight_;
    src.mBufferInfo_.iFormat_ = drmLayer->iFormat_;
    src.mBufferInfo_.iStride_ = drmLayer->iStride_;
    src.mBufferInfo_.iHeightStride_ = drmLayer->iHeightStride_;
    src.mBufferInfo_.uBufferId_ = drmLayer->uBufferId_;
    src.mBufferInfo_.uDataSpace_ = (uint64_t)drmLayer->eDataSpace_;

    src.iMetaDataFd_ = -1;
    src.iMetaDataSize_ = 0;
    src.iMetaDataOffset_ = 0;

    DrmGralloc* gralloc = DrmGralloc::getInstance();
    if (gralloc == NULL) {
        HWC2_LOGI_IF("DrmGralloc is null, Can not get PQ Metadata");
    } else if (drmLayer->bHasMetadata_) {
        src.iMetaDataOffset_ = gralloc->hwc_get_offset_of_pq_metadata(drmLayer->sf_handle);
        if (src.iMetaDataOffset_ > 0) {
            src.iMetaDataFd_ = drmLayer->iFd_;
            src.iMetaDataSize_ = drmLayer->iSize_;
            HWC2_LOGD_IF("Pq metadata:fd=%d offset=%" PRIi64 " size=%" PRIi64, src.iMetaDataFd_, src.iMetaDataOffset_,
                         src.iMetaDataSize_);
        }
    }

    src.mCrop_.iLeft_ = (int)drmLayer->source_crop.left;
    src.mCrop_.iTop_ = (int)drmLayer->source_crop.top;
    src.mCrop_.iRight_ = (int)drmLayer->source_crop.right;
    src.mCrop_.iBottom_ = (int)drmLayer->source_crop.bottom;

    // 2. Alloc Dst buffer
    if (drmLayer->hwPqReg_ == NULL) {
        drmLayer->hwPqReg_ = std::shared_ptr<rk_hwpq_reg>(new rk_hwpq_reg);
        if (drmLayer->hwPqReg_ == NULL) {
            HWC2_LOGE("rk_hwpq_reg alloc failed");
            return -1;
        }
    }

    // 3. Set buffer Info
    HwPqImageInfo pq_dst_info;
    pq_dst_info.mBufferInfo_.iFd_ = -1;
    pq_dst_info.mRkHwpqReg_ = drmLayer->hwPqReg_.get();

    bool needAllocNewBuffer = false;
    HWC2_LOGD_IF("src: fd:%d,wh:%d,%d, format:%d, ws/hs=%d,%d buffid:0x%" PRIx64, src.mBufferInfo_.iFd_,
                 src.mBufferInfo_.iWidth_, src.mBufferInfo_.iHeight_, src.mBufferInfo_.iFormat_,
                 src.mBufferInfo_.iStride_, src.mBufferInfo_.iHeightStride_, src.mBufferInfo_.uBufferId_);

    ret = mPq_->Query(src, pq_dst_info, &needAllocNewBuffer);
    if (ret) {
        if (ret == PqUnInit)
            HWC2_LOGW("Pq SetHwPqSrcImage fail, Pq may still initializing, ret = %d", ret);
        else if (PqUnSupported)
            HWC2_LOGE("Pq SetHwPqSrcImage fail, Src is UnSupported, ret = %d", ret);
        else
            HWC2_LOGE("Pq SetHwPqSrcImage fail, ret = %d", ret);
        return ret;
    }
    int dst_w = static_cast<int>(drmLayer->display_frame.right - drmLayer->display_frame.left);
    int dst_h = static_cast<int>(drmLayer->display_frame.bottom - drmLayer->display_frame.top);
    int src_w = pq_dst_info.mCrop_.Width();
    int src_h = pq_dst_info.mCrop_.Height();
    drmLayer->bCheckPqScale = true;
    drmLayer->fPqHScale = (float)src_w / (float)dst_w;
    drmLayer->fPqVScale = (float)src_h / (float)dst_h;
    HWC2_LOGD_IF("Get After HWPQ scale:(%f,%f), name=%s", drmLayer->fPqHScale, drmLayer->fPqVScale,
                 drmLayer->sLayerName_.c_str());
    return 0;
}


int DrmPq::RunHwPqGetReg(DrmHwcLayer* drmLayer) {
    int ret = 0;

    // 1. Alloc dst HWPQ regs
    if (drmLayer->hwPqReg_ == NULL) {
        drmLayer->hwPqReg_ = std::shared_ptr<rk_hwpq_reg>(new rk_hwpq_reg);
        if (drmLayer->hwPqReg_ == NULL) {
            HWC2_LOGE("HWPQ: rk_hwpq_reg alloc failed");
            return -1;
        }
    }

    // 2. Get HWPQ regs
    ret = mPq_->getHwPqRegConfig(drmLayer->hwPqReg_.get());

    if (ret) {
        HWC2_LOGD_IF("HWPQ: Failed to get reg config! ret = %d", ret);
        drmLayer->hwPqReg_ = NULL;
    }

    return ret;
}