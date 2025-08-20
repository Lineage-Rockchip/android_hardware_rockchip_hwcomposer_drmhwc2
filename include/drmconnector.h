/*
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

#ifndef ANDROID_DRM_CONNECTOR_H_
#define ANDROID_DRM_CONNECTOR_H_

#include "DrmUnique.h"
#include "drmencoder.h"
#include "drmmode.h"
#include "drmproperty.h"
#include "rockchip/drmbaseparameter.h"
#include "rockchip/drmtype.h"
#include "rockchip/utils/drmdebug.h"

#include <stdint.h>
#include <xf86drmMode.h>
#include <map>
#include <mutex>
#include <vector>

enum rk_if_color_format {
    RK_IF_FORMAT_RGB,      /* default RGB */
    RK_IF_FORMAT_YCBCR444, /* YCBCR 444 */
    RK_IF_FORMAT_YCBCR422, /* YCBCR 422 */
    RK_IF_FORMAT_YCBCR420, /* YCBCR 420 */
    RK_IF_FORMAT_YCBCR_HQ, /* Highest subsampled YUV */
    RK_IF_FORMAT_YCBCR_LQ, /* Lowest subsampled YUV */
    RK_IF_FORMAT_MAX,
};

namespace android {

#define DRM_CONNECTOR_SPLIT_MODE_MASK 0xf0
#define DRM_CONNECTOR_SPLIT_RATIO 2

class DrmDevice;

class DrmConnector {
  public:
    DrmConnector(DrmDevice* drm, drmModeConnectorPtr c, DrmEncoder* current_encoder,
                 std::vector<DrmEncoder*>& possible_encoders);
    DrmConnector(const DrmProperty&) = delete;
    DrmConnector& operator=(const DrmProperty&) = delete;

    int Init();

    int UpdateEdidProperty();
    auto GetEdidBlob() -> DrmModePropertyBlobUnique;

    uint32_t id() const;
    uint32_t type() { return type_; }
    uint32_t type_id() const { return type_id_; };
    uint32_t unique_id() const { return unique_id_; };
    const char* unique_name() const { return cUniqueName_; }
    int display() const;
    void set_display(int display);
    int display_next() const;
    void set_display_next(int display_next);
    int priority() const;
    void set_priority(uint32_t priority);
    uint32_t possible_displays() const;
    void set_possible_displays(uint32_t possible_displays);

    bool internal() const;
    bool external() const;
    bool hotplug() const;
    bool writeback() const;
    bool valid_type() const;

    int GetFramebufferInfo(int display_id, uint32_t* w, uint32_t* h, uint32_t* fps);
    int UpdateModes();
    int UpdateVrrModes();
    int UpdatePropertys();
    int GetBestDisplayMode(int display_id, int update_base_timeline);
    int GetSuitableMode(int display_id, uint64_t max_width, uint64_t dlck);
    int UpdateBCSH(int display_id, int update_base_timeline);
    int UpdateOutputFormat(int display_id, int update_base_timeline);
    int UpdateOverscan(int display_id, int* left, int* top, int* right, int* bottom);
    int SetDisplayModeInfo(int display_id);
    bool ParseHdmiOutputFormat(char* strprop, int* format, int* depth);
    void ResetModesReady() { bModeReady_ = false; };
    bool ModesReady() { return bModeReady_; };

    const std::vector<DrmMode>& modes() const { return modes_; }
    const std::vector<DrmMode>& raw_modes() const { return raw_modes_; }
    const DrmMode& best_mode() const;
    const DrmMode& active_mode() const;
    const DrmMode& current_mode() const;
    void set_best_mode(const DrmMode& mode);
    void set_active_mode(const DrmMode& mode);
    void set_current_mode(const DrmMode& mode);
    void SetDpmsMode(uint32_t dpms_mode);

    bool isExistMode(const DrmMode& in_mode);

    const DrmProperty& dpms_property() const;
    const DrmProperty& crtc_id_property() const;
    const DrmProperty& writeback_pixel_formats() const;
    const DrmProperty& writeback_fb_id() const;
    const DrmProperty& writeback_out_fence() const;

    const std::vector<DrmEncoder*>& possible_encoders() const { return possible_encoders_; }
    DrmEncoder* encoder() const;
    void set_encoder(DrmEncoder* encoder);
    drmModeConnection state();
    bool hwc_state_change_and_plug();

    void update_hotplug_state();
    drmModeConnection hotplug_state();

    uint32_t mm_width() const;
    uint32_t mm_height() const;

    uint32_t get_preferred_mode_id() const { return preferred_mode_id_; }

    // RK Support
    bool isSupportSt2084() { return bSupportSt2084_; }
    bool isSupportHLG() { return bSupportHLG_; }
    bool is_hdmi_support_hdr() const;
    int switch_hdmi_hdr_mode(drmModeAtomicReqPtr pset, android_dataspace_t colorspace, bool is_10bit);
    int switch_hdmi_hdr_mode_by_medadata(drmModeAtomicReqPtr pset, uint32_t color_prim,
                                         hdr_output_metadata* hdr_metadata, bool is_10bit);

    int GetSplitModeId() const;
    void ResetSplitMode();
    bool isHorizontalSplit() const;
    int setHorizontalSplit();

    int setCropSplitPrimary();
    bool IsSplitPrimary();
    bool isCropSplit() const;
    int setCropSplit(int32_t fbWidth, int32_t fbHeight, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                     int32_t transform);
    int getCropSplitFb(int32_t* fbWidth, int32_t* fbHeight);
    int getCropInfo(int32_t* srcX, int32_t* srcY, int32_t* srcW, int32_t* srcH);
    int getCropSplitTransform();

    const DrmProperty& brightness_id_property() const;
    const DrmProperty& contrast_id_property() const;
    const DrmProperty& saturation_id_property() const;
    const DrmProperty& hue_id_property() const;
    const DrmProperty& hdr_metadata_property() const;
    const DrmProperty& hdr_panel_property() const;
    const DrmProperty& colorspace_property() const;
    const DrmProperty& color_format_property() const;
    const DrmProperty& color_depth_property() const;

    auto& GetEdidProperty() const { return edid_property_; }
    const std::vector<DrmHdr>& get_hdr_support_list() const { return drmHdr_; }
    struct drm_hdr_static_metadata_infoframe* get_hdr_metadata_ptr() { return &hdr_metadata_; };
    const struct disp_info* baseparameter_info() { return baseparameter_ready_ ? &baseparameter_ : NULL; }
    int FilterColorFormatWithCaps(int inFormat);

    // VRR
    std::vector<int> vrr_modes() {
        std::unique_lock<std::recursive_mutex> lock(mRecursiveMutex);
        return vrr_modes_;
    }

    uint8_t* MakeFakeEDID();

    int32_t get_kernel_crtc_id() const { return mKernelCrtcId_; }
    void reset_kernel_crtc_id() { mKernelCrtcId_ = 0; }

    // Connector Mirror 功能
    void enable_connector_mirror_mode(int primary_display_id, int mirror_display_id) {
        mirror_mode = true;
        mirror_primary = false;
        mirror_primary_id = primary_display_id;
        // MirrorPrimary ID 等于当前Connector DisplayId信息，则为MirrorPrimary
        if (mirror_primary_id == display_) {
            mirror_primary = true;
            if (connected_mirror_display_id_set.count(mirror_display_id) == 0) {
                connected_mirror_display_id_set.insert(mirror_display_id);
                HWC2_LOGI("MirrorDisplay: MirrorPrimary display-id=%d, insert display-id=%d, mirror display size=%zu",
                          display_, mirror_display_id, connected_mirror_display_id_set.size());
            }
        } else {
            mirror_primary = false;
            connected_mirror_display_id_set.clear();
            HWC2_LOGI("MirrorDisplay: MirrorExternal display-id=%d, MirrorPrimary display-id=%d", mirror_display_id,
                      primary_display_id);
        }
    }

    void disable_connector_mirror_mode(int display_id) {
        if (mirror_primary) {
            if (connected_mirror_display_id_set.count(display_id) > 0) {
                connected_mirror_display_id_set.erase(display_id);
                HWC2_LOGI("MirrorDisplay: MirrorPrimary display-id=%d, Mirror display-id=%d connected_mirror_cnt=%zu",
                          display_, display_id, connected_mirror_display_id_set.size());
            }

            if (connected_mirror_display_id_set.size() == 0) {
                mirror_mode = false;
                mirror_primary = false;
            }
        } else {
            mirror_mode = false;
        }
    }

    bool is_connector_mirror_mode() { return mirror_mode; }
    bool is_connector_mirror_primary() { return mirror_mode && mirror_primary; }
    int get_connector_mirror_primary_id() { return mirror_mode ? mirror_primary_id : -1; }
    std::set<int> get_connector_mirror_display_id() { return connected_mirror_display_id_set; }

    bool is_last_mirror_display_id(int display_id) {
        if (connected_mirror_display_id_set.size() != 1) {
            return false;
        }

        return connected_mirror_display_id_set.count(display_id) > 0;
    }
    // Connector Mirror 功能
    int UpdateHotplugState(bool fast_mode);

    // PowerState
    uint64_t power_state() const { return mPowerState_; }
    void set_power_state(uint64_t state) { mPowerState_ = state; }

  private:
    DrmDevice* drm_;

    uint32_t id_;
    DrmEncoder* encoder_;
    int display_;
    int display_next_;

    uint32_t type_;
    uint32_t type_id_;
    uint32_t unique_id_;
    uint32_t priority_;
    drmModeConnection state_;
    bool plug_;
    drmModeConnection hotplug_state_;

    uint32_t mm_width_;
    uint32_t mm_height_;

    DrmMode active_mode_;
    DrmMode current_mode_;
    DrmMode best_mode_;
    std::vector<DrmMode> modes_;
    std::vector<DrmMode> raw_modes_;
    std::vector<int> vrr_modes_;
    std::vector<DrmHdr> drmHdr_;

    DrmProperty dpms_property_;
    DrmProperty crtc_id_property_;
    DrmProperty edid_property_;
    DrmProperty writeback_pixel_formats_;
    DrmProperty writeback_fb_id_;
    DrmProperty writeback_out_fence_;

    //RK support
    DrmProperty brightness_id_property_;
    DrmProperty contrast_id_property_;
    DrmProperty saturation_id_property_;
    DrmProperty hue_id_property_;
    DrmProperty hdr_metadata_property_;
    DrmProperty hdr_panel_property_;
    DrmProperty colorspace_property_;

    DrmProperty color_format_property_;
    DrmProperty color_depth_property_;
    DrmProperty color_format_caps_property_;
    DrmProperty color_depth_caps_property_;

    DrmProperty connector_id_property_;
    DrmProperty split_mode_property_;
    std::vector<DrmEncoder*> possible_encoders_;
    drmModeConnectorPtr connector_;

    uint32_t preferred_mode_id_;
    uint32_t possible_displays_;

    // Update mode list
    bool bModeReady_;
    // HDR Support
    bool bSupportSt2084_;
    bool bSupportHLG_;
    struct drm_hdr_static_metadata_infoframe hdr_metadata_;
    DrmColorspaceType colorspace_ = DrmColorspaceType::DEFAULT;
    struct hdr_output_metadata last_hdr_metadata_;
    // Baseparameter Support
    bool baseparameter_ready_;
    int iTimeline_ = 0;
    struct disp_info baseparameter_;
    char cUniqueName_[30] = {0};
    // BCSH
    uint32_t uBrightness_ = 50;
    uint32_t uContrast_ = 50;
    uint32_t uSaturation_ = 50;
    uint32_t uHue_ = 50;
    // output format
    int uColorFormat_ = output_ycbcr_high_subsampling;
    ;
    int uColorDepth_ = Automatic;
    // Split mode
    bool bSplitMode_ = false;
    // Horizontal mode
    bool bHorizontalSplit_ = false;
    // Crop mode
    bool bSplitPrimary_ = false;
    bool bCropSplit_ = false;
    int32_t FbWidth_ = 0;
    int32_t FbHeight_ = 0;
    int32_t SrcX_ = 0;
    int32_t SrcY_ = 0;
    int32_t SrcW_ = 0;
    int32_t SrcH_ = 0;
    int32_t iCropSplitTransform = 0;

    // Connector mirror
    bool mirror_mode = false;
    bool mirror_primary = false;
    int mirror_primary_id = -1;
    std::set<int> connected_mirror_display_id_set;

    uint32_t blob_id_ = 0;
    struct dummyEdid {
        uint8_t data[128];
        const uint32_t len = sizeof(data);
    };
    std::shared_ptr<dummyEdid> mDummyEDID_ = NULL;

    int32_t mKernelCrtcId_;

    uint64_t mPowerState_ = DRM_MODE_DPMS_OFF;

    char overscan_property_[PROPERTY_VALUE_MAX] = {0};
    char output_format_pro_[PROPERTY_VALUE_MAX] = {0};
    char resolution_property_[PROPERTY_VALUE_MAX] = {0};
    char framebuffer_property_[PROPERTY_VALUE_MAX] = {0};

    mutable std::recursive_mutex mRecursiveMutex;
    int UpdateOutputFormat(drmModeAtomicReqPtr pset);
};
}  // namespace android

#endif  // ANDROID_DRM_PLANE_H_
