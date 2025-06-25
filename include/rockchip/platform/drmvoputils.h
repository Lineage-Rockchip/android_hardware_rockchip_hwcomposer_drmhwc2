/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef RK_DRM_VOP_UTILS_H
#define RK_DRM_VOP_UTILS_H

#include <string>
#include "drmlayer.h"
#include "rockchip/utils/drmdebug.h"

namespace android {
struct TwoWinCheckCtx {
    bool bValidWin0;
    bool bValidWin1;
    bool bUsed;
    int iZpos;
    int iOffset;
    int bYuv;
    uint64_t uModifier;
    bool bFbTarget;
    bool bAFBC;
    bool bRFBC;
    bool bInput4K;
    bool bOutput4K;
    std::string sWinName;
    const char* sWin0FailReason;
    android_dataspace_t eDataSpace;
    void Reset(std::string name = "Unknow") {
        bValidWin0 = false;
        bValidWin1 = false;
        bUsed = iOffset = bYuv = uModifier = bInput4K = bOutput4K = 0;
        iZpos = -1;
        sWinName = name;
        sWin0FailReason = "";
    }
    bool CheckWin1(DrmHwcLayer* layer, int zpos) {
        bValidWin1 = true;
        if (!bUsed) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=Win0 not used.", sWinName.c_str());
            return bValidWin1;
        }
        if (!bValidWin0) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=%s Two-win not Valid.", sWinName.c_str(), sWin0FailReason);
            return bValidWin1;
        }
        // 要求zpos连续
        if ((zpos - iZpos) != 1 && !(zpos == iZpos)) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=Discontinuous zpos, (%d : %d).", sWinName.c_str(), iZpos, zpos);
            return bValidWin1;
        }
        // 要求RGB/YUV匹配+色域匹配
        if (bYuv != layer->bYuv_) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=YUV/RGB not match, (%s : %s).", sWinName.c_str(),
                         bYuv ? "YUV" : "RGB", layer->bYuv_ ? "YUV" : "RGB");
            return bValidWin1;
        }
        if ((eDataSpace & HAL_DATASPACE_STANDARD_MASK) != (layer->eDataSpace_ & HAL_DATASPACE_STANDARD_MASK)) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=ColorSpace not match, dataspace=(0x%x : 0x%x)",
                         sWinName.c_str(), eDataSpace, layer->eDataSpace_);
            return bValidWin1;
        }
        if ((eDataSpace & HAL_DATASPACE_TRANSFER_MASK) != (layer->eDataSpace_ & HAL_DATASPACE_TRANSFER_MASK)) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=Transfer not match, dataspace=(0x%x : 0x%x)", sWinName.c_str(),
                         eDataSpace, layer->eDataSpace_);
            return bValidWin1;
        }
        // 要求宽度小于2048
        int aligned_w = layer->GetWidthWithOffsetBlockAligned();
        if (aligned_w > 2048) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=4K Win1 input, Align_w=%d", sWinName.c_str(), aligned_w);
            return bValidWin1;
        }
        // 不支持旋转
        if ((layer->transform & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) != 0) {
            bValidWin1 = false;
            HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=win1 ROTATE!=0, transform=0x%x", sWinName.c_str(),
                         layer->transform);
            return bValidWin1;
        }
        if (bFbTarget || layer->bFbTarget_) {
            // FbTarget的Modifier不准确，使用FBC类型进行判断，要求FBC模式一致
            if ((bAFBC != layer->bAfbcd_) || (bRFBC != layer->bRfbcd_)) {
                bValidWin1 = false;
                HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=FBC Mode not match: AFBC=(%d : %d) RFBC=(%d : %d)",
                             sWinName.c_str(), bAFBC, layer->bAfbcd_, bRFBC, layer->bRfbcd_);
                return bValidWin1;
            }
        } else {
            // 要求FBC/raster/tile模式一致
            if (uModifier != layer->uModifier_) {
                bValidWin1 = false;
                HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=Modifier not match: (0x%" PRIx64 " : 0x%" PRIx64 ")",
                             sWinName.c_str(), uModifier, layer->uModifier_);
                return bValidWin1;
            }
        }
        if (!(gIsRK3576())) {
            // 要求offset奇偶一致
            int dst_x_offset = layer->display_frame.left;
            if ((iOffset % 2) != (dst_x_offset % 2)) {
                bValidWin1 = false;
                HWC2_LOGD_IF("%s Disable TwoWinMode, Reason=Offset not match: (%s : %s)", sWinName.c_str(),
                             (iOffset % 2) ? "odd" : "even", (dst_x_offset % 2) ? "odd" : "even");
                return bValidWin1;
            }
        }

        return bValidWin1;
    }
    void CheckWin0(DrmHwcLayer* layer, int zpos, bool mode_8k = false) {
        bUsed = true;
        bValidWin0 = true;
        iZpos = zpos;
        iOffset = layer->display_frame.left;
        bYuv = layer->bYuv_;
        uModifier = layer->uModifier_;
        bFbTarget = layer->bFbTarget_;
        bAFBC = layer->bAfbcd_;
        bRFBC = layer->bRfbcd_;
        bInput4K = layer->GetWidthWithOffsetBlockAligned() > 2048;
        eDataSpace = layer->eDataSpace_;
        sWin0FailReason = "";

        if ((layer->transform & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) != 0) {
            bValidWin0 = false;
            sWin0FailReason = "win0 ROTATE!=0, not support TwoWinMode.";
            return;
        }
        if (mode_8k) {
            bValidWin0 = false;
            sWin0FailReason = "8K mode, not support TwoWinMode.";
            return;
        }
        if (bInput4K) {
            bValidWin0 = false;
            sWin0FailReason = "4K win0 input, not support TwoWinMode.";
            return;
        }
    }
};

}  // namespace android

#endif  // RK_DRM_VOP_UTILS_H
