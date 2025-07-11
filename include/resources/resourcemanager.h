/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef RESOURCEMANAGER_H
#define RESOURCEMANAGER_H

#include "drmdevice.h"
#include "drmdisplaycompositor.h"
#include "drmhwctwo.h"
#include "platform.h"
#include "rockchip/drmbaseparameter.h"
#include "rockchip/drmgralloc.h"
#include "solid_color.h"

#include "drmbufferqueue.h"

#include <im2d.hpp>

#include <string.h>
#include <map>
#include <mutex>
#include <set>

#ifdef USE_HWC_PROXY_SERVICE
#include "rockchip/platform/RkHwcProxyClient.h"
#endif

#ifdef USE_LIBSR
#include "SvepSr.h"
#endif

#ifdef USE_LIBSVEP_MEMC
#include "SvepMemc.h"
#endif

#ifdef USE_LIBPQ_HWPQ
#include "rockchip/pq/DrmPq.h"
#endif
typedef struct MirrorDisplayInfo {
    uint64_t buffer_id = 0;
    std::string name = std::string("UnSet");
} MirrorDisplayInfo_t;

namespace android {
class DrmDisplayCompositor;
class DrmHwcTwo;

typedef enum HwVirtualDisplayMode {
    HWC2_DISABLE_HW_VIRTUAL_DISPLAY = 0,
    HWC2_HW_VIRTUAL_DISPLAY_USE_VOP = 1,
    HWC2_HW_VIRTUAL_DISPLAY_USE_RGA = 2,
} HwVirtualDisplayMode_t;

class ResourceManager {
  public:
    static ResourceManager* getInstance() {
        static ResourceManager drmResourceManager_;
        return &drmResourceManager_;
    }

    int Init(DrmHwcTwo* hwc2);
    DrmDevice* GetDrmDevice(int display);
    std::shared_ptr<Importer> GetImporter(int display);
    DrmConnector* AvailableWritebackConnector(int display);
    int InitProperty();
    const std::vector<std::unique_ptr<DrmDevice>>& GetDrmDevices() const { return drms_; }

    DrmHwcTwo* GetHwc2() const { return hwc2_; }

    int getDisplayCount() const { return num_displays_; }

    std::map<int, int> getDisplays() const { return displays_; }

    uint32_t getActiveDisplayCnt() { return active_display_.size(); }

    int getFb0Fd() { return fb0_fd; }
    int getSocId() { return soc_id_; }
    std::shared_ptr<DrmDisplayCompositor> GetDrmDisplayCompositor(DrmCrtc* crtc);

    // WriteBack interface.
    int GetWBDisplay() const;
    bool isWBMode() const;
    const DrmMode& GetWBMode() const;
    int EnableWriteBackMode(int display);
    HwVirtualDisplayMode_t ChooseWriteBackMode(int display);
    // Virtual Display Composition by gles.
    bool IsDisableHwVirtualDisplay();
    // WriteBack by vop
    bool IsWriteBackByVop();
    int WriteBackUseVop(int display);
    int UpdateWriteBackResolutionUseVop(int display);

    // WriteBack by rga
    bool IsWriteBackByRga();
    int WriteBackUseRga(int display);
    int DisableWriteBackMode(int display);
    int UpdateWriteBackResolution(int display);
    std::shared_ptr<DrmBuffer> GetResetWBBuffer();
    std::shared_ptr<DrmBuffer> GetNextWBBuffer();
    std::shared_ptr<DrmBuffer> GetDrawingWBBuffer();
    int GetFinishWBBufferSize();
    int OutputWBBuffer(int display_id, rga_buffer_t& dst, im_rect& src_rect, int32_t* retire_fence,
                       uint64_t* last_frame_no);
    int SwapWBBuffer(uint64_t frame_no);
    // 更新WriteBack Display的图层信息，以便判断是否可以开启WriteBack
    void AddWBDisplayLayerInfo(std::map<uint32_t, MirrorDisplayInfo_t>& Info);
    void SetWriteBackRunningState(bool enable, const char* tag);
    bool GetWriteBackRunningState();

    // 检查是否是WriteBack Display 的 Mirror Dispaly
    bool IsWBMirrorDisplay(uint64_t display_id, uint32_t zorder, MirrorDisplayInfo_t info);
    // WriteBack interface.
    int RegisterWBCallback(uint64_t display_id, void (*callback)(std::shared_ptr<DrmBuffer>, void* arg),
                           void* argument);
    int UnregisterWBCallback(uint64_t display_id, int callback_id);

    // 系统属性开关
    bool IsCompositionDropMode() const;
    bool IsDynamicDisplayMode() const;
    bool IsSidebandStream2Mode() const;
    int GetCacheBufferLimitSize() const;
    bool GetEnableEdidReport() const;
    bool GetEnableRgaAcquireFence() const;
    bool GetRgaSupportAbove4GB() const;
    bool IsSingleDisplayMode() const;
    bool IsSupportSolidColor() const;
    bool IsGsiSDK() const;

    std::shared_ptr<SolidColorBuffer> GetSolidColorBuffer(uint32_t rgba, uint32_t w, uint32_t h);
    bool GetBlockHotplugInPowerOff() const;

#ifdef USE_LIBSR
    std::shared_ptr<SvepSr> GetSvepSr();
    std::shared_ptr<SvepSr> ResetAndGetSvepSr();
#endif

#ifdef USE_LIBSVEP_MEMC
    std::shared_ptr<SvepMemc> GetSvepMemc();
    std::shared_ptr<SvepMemc> ResetAndGetSvepMemc();
#endif

#ifdef USE_LIBPQ_HWPQ
    std::shared_ptr<DrmPq> GetDrmPq();
    std::shared_ptr<DrmPq> ResetAndGetDrmPq();
#endif

#ifdef USE_LIBPQ
    enum SwpqMode {
        SWPQ_MODE_NOUSE = 0,
        SWPQ_MODE_PQ = 1,
        SWPQ_MODE_WBLD = 2,
    };

    std::recursive_mutex& GetSwpqMutex() { return mSwpqMutex_; }

    bool NeedReinitSwpqForMode(SwpqMode mode) {
        int last_mode = mSwpqMode_.exchange(mode);
        if (last_mode != SWPQ_MODE_NOUSE && last_mode != mode) {
            return true;
        }
        return false;
    }
#endif

  private:
    ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    int AddDrmDevice();

    int num_displays_;
    std::set<int> active_display_;
    std::vector<std::unique_ptr<DrmDevice>> drms_;
    std::vector<std::shared_ptr<Importer>> importers_;
    std::map<int, std::shared_ptr<DrmDisplayCompositor>> mapDrmDisplayCompositor_;
    std::map<int, int> displays_;
    DrmGralloc* drmGralloc_;
    DrmHwcTwo* hwc2_;

#ifdef USE_HWC_PROXY_SERVICE
    // As the client of rkdisplay aidl service, HWC executes hw_output related functions
    RkHwcProxyClientWorker hwc_proxy_client_worker_;
#endif

    int fb0_fd;
    int soc_id_;
    int drmVersion_;
    bool dynamic_assigin_enable_;

    // <-- WriteBack Mode info
    // WriteBack 引用计数，若每一个virtual display 请求 WriteBack，则引用计数+1
    int bEnableWriteBackRef_ = 0;
    // WriteBack 使能 display 的 id
    int iWriteBackDisplayId_ = -1;
    // Hw Virtual Display 实现模式, 存在 gles / vop / rga 三种模式
    HwVirtualDisplayMode_t mVDMode_;
    // WriteBack 格式信息
    uint32_t iWBWidth_;
    uint32_t iWBHeight_;
    int iWBFormat_;
    // WriteBack分辨率
    DrmMode mWBMode_;
    // 需要临时关闭 WriteBack 模式，比如 屏端没有图层使能
    bool bWriteBackRuningEnable_;

    // WriteBack BufferQueue 信息，用于实现环形缓冲区结构
    std::shared_ptr<DrmBufferQueue> mWriteBackBQ_ = NULL;
    std::shared_ptr<DrmBuffer> mResetBackBuffer_ = NULL;
    std::shared_ptr<DrmBuffer> mNextWriteBackBuffer_ = NULL;
    std::shared_ptr<DrmBuffer> mDrawingWriteBackBuffer_ = NULL;
    std::vector<std::shared_ptr<DrmBuffer>> mFinishBufferQueue_;
    std::map</*display_id*/ uint64_t,
             std::map</*Callback id*/ int,
                      std::pair</*function*/ void (*)(std::shared_ptr<DrmBuffer>, void* arg), /*argument*/ void*>>>
            mWriteBackCb_;
    int iWriteBackCallbackCount_ = 0;

    // 主要用于保存WriteBack 图层信息，用于评估虚拟屏幕是否支持从WriteBack获取图像
    std::map<uint32_t /*z*/, MirrorDisplayInfo_t> mWBDisplayLayerInfo_;
    std::map<int, std::set<uint64_t>> mMapDisplayBufferSet_;
    // -->

    // SolidColor manager
    SolidColorManager mSolidColorManager_;
    // Composition丢帧模式
    bool mCompositionDropMode_;
    // 使能动态分辨率切换模式
    bool mDynamicDisplayMode_;
    // sideband 2.0
    bool mSidebandStream2Mode_;
    // cache buffer max size limit enable
    int mCacheBufferLimitSize_ = 0;
    // Enable Edid report by GetDisplayIdentificationData
    bool mEnableEdidReport_ = false;
    // Enable RGA acquire fence, RGA kernel driver before 20240724 may crash with acquire fence
    bool mEnableRgaAcquireFence = false;
    // Enable signal display
    bool mEnableSingleDisplayMode_ = false;
    // Enable signal display
    bool mEnableSolidColorSupport_ = false;
    // Block hotplug if primary display is powered off
    bool mBlockHotplugInPowerOff_ = false;
    // 检查是否是 GSI SDK
    bool mIsGsiSDK_ = false;
    // 使用pq平均亮度接口的DisplayID
    int mPqLumaAvgDisplayId_ = 0;
    // 使用pq平均亮度接口的间隔
    int mPqLumaAvgInterval_ = -1;

#ifdef USE_LIBSR
    // SVEP SR
    std::shared_ptr<SvepSr> mSvepSr_ = nullptr;
#endif

#ifdef USE_LIBSVEP_MEMC
    std::shared_ptr<SvepMemc> mSvepMemc_ = nullptr;
#endif

#ifdef USE_LIBPQ_HWPQ
    // DRM PQ
    std::shared_ptr<DrmPq> mDrmPq_ = nullptr;
#endif

#ifdef USE_LIBPQ
    mutable std::recursive_mutex mSwpqMutex_;
    std::atomic<int> mSwpqMode_ = 0;
#endif

    mutable std::recursive_mutex mRecursiveMutex;
};
}  // namespace android

#endif  // RESOURCEMANAGER_H
