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
    bool bValid;
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
    std::string sWin0FailReason;
    android_dataspace_t eDataSpace;
    void Reset(std::string name = "Unknow") {
        bValid = true;
        bUsed = iOffset = bYuv = uModifier = bInput4K = bOutput4K = 0;
        iZpos = -1;
        sWinName = name;
        sWin0FailReason = "";
    }
    bool CheckWin1(DrmHwcLayer* layer, int zpos) {
        std::string sWin1FailReason = "";
        bool bDebugLog = LogLevel(DBG_DEBUG);
        bool check_ok = true;
        do {
            if (!bUsed) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "Win0 not used.";
                break;
            }
            if (!bValid) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "Two-win not Valid.";
                break;
            }
            // 要求zpos连续
            if ((zpos - iZpos) != 1 && !(zpos == iZpos)) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "Discontinuous zpos.";
                break;
            }
            // 要求RGB/YUV匹配+色域匹配
            if (bYuv != layer->bYuv_) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "YUV/RGB not match.";
                break;
            }
            if ((eDataSpace & HAL_DATASPACE_STANDARD_MASK) != (layer->eDataSpace_ & HAL_DATASPACE_STANDARD_MASK)) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "ColorSpace not match.";
                break;
            }
            if ((eDataSpace & HAL_DATASPACE_TRANSFER_MASK) != (layer->eDataSpace_ & HAL_DATASPACE_TRANSFER_MASK)) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "Transfer not match.";
                break;
            }
            // 要求宽度小于2048
            if (layer->GetWidthWithOffsetBlockAligned() > 2048) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "4K Win1 input.";
                break;
            }
            // 不支持旋转
            if ((layer->transform & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) != 0) {
                check_ok = false;
                if (bDebugLog) sWin1FailReason += "win1 ROTATE!=0.";
                break;
            }
            if (bFbTarget) {
                // FbTarget的Modifier不准确，使用FBC类型进行判断，要求FBC模式一致
                if ((bAFBC != layer->bAfbcd_) || (bRFBC != layer->bRfbcd_)) {
                    check_ok = false;
                    if (bDebugLog) sWin1FailReason += "FBC Mode not match.";
                    break;
                }
            } else {
                // 要求FBC/raster/tile模式一致
                if (uModifier != layer->uModifier_) {
                    check_ok = false;
                    if (bDebugLog) sWin1FailReason += "Modifier not match.";
                    break;
                }
            }
            if (!(gIsRK3576())) {
                // 要求offset奇偶一致
                int dst_x_offset = layer->display_frame.left;
                if ((iOffset % 2) != (dst_x_offset % 2)) {
                    check_ok = false;
                    if (bDebugLog) sWin1FailReason += "Offset not match.";
                    break;
                }
            }
        } while (0);
        if (!check_ok) {
            ALOGD_IF(LogLevel(DBG_DEBUG), "\t %s Disable TwoWinMode, Reason=%s", sWinName.c_str(),
                     (sWin0FailReason + sWin1FailReason).c_str());
        }
        bValid = check_ok;
        return check_ok;
    }
    void CheckWin0(DrmHwcLayer* layer, int zpos, bool mode_8k = false) {
        bUsed = true;
        bValid = true;
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

        bool bDebugLog = LogLevel(DBG_DEBUG);
        do {
            if ((layer->transform & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) != 0) {
                bValid = false;
                if (bDebugLog) sWin0FailReason += "win0 ROTATE!=0, ";
                break;
            }
            if (mode_8k) {
                bValid = false;
                if (bDebugLog) sWin0FailReason += "8K mode, ";
                break;
            }
            if (bInput4K) {
                bValid = false;
                if (bDebugLog) sWin0FailReason += "4K win0 input,";
                break;
            }
        } while (0);

        if (bValid == false) {
            if (bDebugLog) sWin0FailReason += "not support TwoWinMode.";
        }
    }
};

}  // namespace android

#endif  // RK_DRM_VOP_UTILS_H
