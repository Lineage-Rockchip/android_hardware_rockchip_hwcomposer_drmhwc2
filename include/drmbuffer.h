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

#ifndef _DRM_BUFFER_H_
#define _DRM_BUFFER_H_

#include "rockchip/drmgralloc.h"
#include "utils/autofd.h"
// Use im2d api
#include <im2d.hpp>

#include <ui/GraphicBuffer.h>

#ifdef USE_LIBPQ_HWPQ
#include "Pq.h"
#endif

namespace android {

class DrmBuffer {
  public:
    DrmBuffer(int w, int h, int format, uint64_t usage = 0, std::string sName_ = "unset", int parent_id = 0);
    DrmBuffer(native_handle_t* in_handle);
    DrmBuffer(int fd, int width, int height, int stride, int heightStride, int byteStride, int fourccFormat, int size,
              uint64_t bufferId, uint64_t modifier, std::string name = "DrmBuffer");
    ~DrmBuffer();
    int Init();
    bool initCheck();
    buffer_handle_t GetHandle();
    native_handle_t* GetInHandle();
    uint64_t GetId();
    uint64_t GetExternalId();
    int SetExternalId(uint64_t externel_id);
    int GetParentId();
    int SetParentId(int parent_id);
    int GetFd();
    std::string GetName();
    int GetWidth();
    int GetHeight();
    int GetFormat();
    int GetStride();
    int GetHeightStride();
    int GetByteStride();
    int GetSize();
    uint64_t GetUsage();
    std::vector<uint32_t> GetByteStridePlanes();
    int SetCrop(int left, int top, int right, int bottom);
    int GetCrop(int* left, int* top, int* right, int* bottom);
    uint32_t GetFourccFormat();
    uint64_t GetModifier();
    uint64_t GetBufferId();
    uint32_t GetGemHandle();
    uint32_t DrmFormatToPlaneNum(uint32_t drm_format);
    bool IsAfbc();
    bool IsRfbc();
    uint32_t GetFbId();
    rga_buffer_handle_t GetRgaHandle();
    void* Lock();
    int Unlock();
    int GetFinishFence();
    int SetFinishFence(int fence);
    int MergeFinishFence(int fence);
    int WaitFinishFence(int timeout_ms = 3000);
    int GetReleaseFence();
    int SetReleaseFence(int fence);
    int WaitReleaseFence(int timeout_ms = 1500);
    int DumpData(uint32_t frame_no, int zpos, const char* tag);

    void SetDataspace(android_dataspace_t dataspace);
    android_dataspace_t GetDataspace();

#ifdef USE_LIBPQ_HWPQ
    std::shared_ptr<rk_hwpq_reg> GetHwPqRegs() {
        if (spHwPqReg_ == NULL) spHwPqReg_ = std::make_shared<rk_hwpq_reg>();
        return spHwPqReg_;
    }
    bool HasHwPqRegs() { return spHwPqReg_ != NULL; }
    void RemoveHwPqRegs() { spHwPqReg_ = NULL; }
#endif

#ifdef RK3528
    // RK3528 解码器支持预缩小功能
    bool IsPreScaleBuffer();
    int SwitchToPreScaleBuffer();
    uint32_t GetPreScaleFbId();
#endif

  private:
    uint64_t uId = 0;
    int iParentId_ = -1;
    uint64_t iExternelId_ = 0;
    // BufferInfo
    int iFd_ = -1;
    int iWidth_ = -1;
    int iHeight_ = -1;
    int iFormat_ = -1;
    int iStride_ = -1;
    int iHeightStride_ = -1;
    int iByteStride_ = -1;
    std::vector<uint32_t> uByteStridePlanes_;
    int iSize_ = -1;
    uint64_t iUsage_ = 0;
    uint32_t uFourccFormat_ = 0;
    uint64_t uModifier_ = 0;
    uint64_t uBufferId_ = 0;
    uint32_t uGemHandle_ = 0;
    uint32_t uFbId_ = 0;
    rga_buffer_handle_t uRgaHandle_ = 0;
    android_dataspace_t uDataspace_ = HAL_DATASPACE_UNKNOWN;

    // rect crop info
    int iLeft_ = -1;
    int iTop_ = -1;
    int iRight_ = -1;
    int iBottom_ = -1;

    // Fence info
    UniqueFd iFinishFence_;
    UniqueFd iReleaseFence_;

#ifdef RK3528
    bool bIsPreScale_ = false;
    metadata_for_rkvdec_scaling_t mMetadata_;
    uint32_t uPreScaleFbId_ = 0;
    int ResetPreScaleBuffer();
#endif
    // Init flags
    bool bInit_ = false;
    std::string sName_;
    native_handle_t* inBuffer_ = NULL;
    buffer_handle_t buffer_ = NULL;
    sp<GraphicBuffer> ptrBuffer_ = NULL;
    DrmGralloc* ptrDrmGralloc_ = NULL;
    mutable std::mutex mtx_;
#ifdef USE_LIBPQ_HWPQ
    std::shared_ptr<rk_hwpq_reg> spHwPqReg_ = NULL;
#endif
};

}  // namespace android
#endif  // #ifndef _DRM_BUFFER_QUEUE_H_
