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

#ifndef RESOURCE_CACHE_H
#define RESOURCE_CACHE_H

#include <android-base/unique_fd.h>

#include "rockchip/drmgralloc.h"

// Use im2d api
#include <im2d.hpp>
#include "rockchip/utils/rgautils.h"

#include <mutex>

namespace android {

class GemHandle {
  public:
    GemHandle();
    ~GemHandle();
    GemHandle(const GemHandle&) = delete;
    GemHandle& operator=(const GemHandle&) = delete;
    int InitGemHandle(const char* name, uint64_t buffer_fd, uint64_t buffer_id);
    uint32_t GetGemHandle();
    bool isValid();

  private:
    DrmGralloc* drmGralloc_;
    uint64_t uBufferId_ = 0;
    uint32_t uGemHandle_ = 0;
    const char* name_;
};

class RgaHandle {
  public:
    RgaHandle();
    ~RgaHandle();
    RgaHandle(const RgaHandle&) = delete;
    RgaHandle& operator=(const RgaHandle&) = delete;
    uint32_t GetRgaHandle(const char* name, int fd, int size, uint64_t buffer_id);
    bool isValid();

  private:
    rga_buffer_handle_t uRgaHandle_ = 0;
    uint64_t uBufferId_ = 0;
    int iSize_ = 0;
    int iFd_ = -1;
    const char* name_ = NULL;
    mutable std::recursive_mutex mRecursiveMutex;
};

class LayerInfoCache {
  public:
    LayerInfoCache();
    ~LayerInfoCache();
    LayerInfoCache(const LayerInfoCache&) = delete;
    LayerInfoCache& operator=(const LayerInfoCache&) = delete;

    bool NeedCacheBufferHandle(buffer_handle_t handle);
    int CacheBufferHandle(buffer_handle_t handle);

    base::unique_fd uniqueFd_;
    buffer_handle_t buffer_ = NULL;
    int iFormat_ = 0;
    int iWidth_ = 0;
    int iHeight_ = 0;
    int iStride_ = 0;
    int iHeightStride_ = 0;
    int iSize_ = 0;
    int iByteStride_ = 0;
    std::vector<uint32_t> uByteStridePlanes_;
    uint64_t iUsage_ = 0;
    uint32_t uFourccFormat_ = 0;
    uint32_t uGemHandle_ = 0;
    uint64_t uModifier_ = 0;
    uint64_t uBufferId_ = 0;
    float fVideoFps_ = 0;
    GemHandle gemHandle_;
    std::string sLayerName_;
    bool bFbIdCached_ = false;
    // PQ metadata 信息
    bool bHasPqMetadata_ = false;
    int64_t iPqMetadataOffset_ = -1;
    // HDR metadata 信息
    bool bHasHdrMetadata_ = false;
    int64_t iHdrMetadataOffset_ = -1;
    void* pHdrMetadataVir_ = nullptr;
    int iHdrMetadataMapSize_ = -1;
    // RGA handle cache
    RgaHandle rgaHandle_;
};

};  // namespace android
#endif