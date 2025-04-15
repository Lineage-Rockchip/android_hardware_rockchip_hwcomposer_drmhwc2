/*
 * Copyright (C) 2024 Rockchip Electronics Co.Ltd.
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
#ifndef PQ_UTILS_H
#define PQ_UTILS_H

#ifndef ATRACE_TAG
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#endif

#ifndef LOG_TAG
#define LOG_TAG "drm-vop-hwpq"
#endif

#include <drmlayer.h>
#include <drmplane.h>
#include <iomanip>
#include <sstream>
#ifdef USE_LIBPQ_HWPQ
#include "Pq.h"
#endif
namespace android {
class PqUtils {
  public:
    struct PqDisplayCtx {
        int iDisplayWidth;
        int iDisplayHeight;
        int uSocId;
    };

    enum RkHwPqMode {
        HWPQ_DISABLE = 0,
        HWPQ_UI_MODE = 1,
        HWPQ_HDMIRX_MODE = 2,
        HWPQ_VIDEO_MODE = 3,
        HWPQ_MODE_MAX,
    };

#ifdef USE_LIBPQ_HWPQ
    static void dumpPqBufInfo(const HwpqDisplayStatus::LayerBufferInfo& bufinfo, std::stringstream& ss) {
        ss << "|    |" << std::dec << std::setfill(' ') << std::setw(6) << bufinfo.intRect_.iLeft_ << std::setfill(' ')
           << std::setw(6) << bufinfo.intRect_.iTop_ << std::setfill(' ') << std::setw(6) << bufinfo.intRect_.iRight_
           << std::setfill(' ') << std::setw(6) << bufinfo.intRect_.iBottom_;

        ss << "|" << std::setfill(' ') << std::setw(6) << bufinfo.srcRect_.iLeft_ << std::setfill(' ') << std::setw(6)
           << bufinfo.srcRect_.iTop_ << std::setfill(' ') << std::setw(6) << bufinfo.srcRect_.iRight_
           << std::setfill(' ') << std::setw(6) << bufinfo.srcRect_.iBottom_;

        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << std::hex << bufinfo.uFourccFormat_;
        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << bufinfo.uTransform_;
        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << bufinfo.uDataSpace_;
        ss << "|" << bufinfo.sLayerName_;
        ss << std::endl;
    }
    static void dumpPqPlaneInfo(const HwpqDisplayStatus::PlaneInfo& planeinfo, std::stringstream& ss) {
        ss << "|" << std::dec << std::setfill(' ') << std::setw(4) << planeinfo.iZpos_;
        ss << "|" << std::setfill(' ') << std::setw(6) << planeinfo.VopDstRect_.iLeft_ << std::setfill(' ')
           << std::setw(6) << planeinfo.VopDstRect_.iTop_ << std::setfill(' ') << std::setw(6)
           << planeinfo.VopDstRect_.iRight_ << std::setfill(' ') << std::setw(6) << planeinfo.VopDstRect_.iBottom_;

        ss << "|" << std::setfill(' ') << std::setw(6) << planeinfo.VopSrcRect_.iLeft_ << std::setfill(' ')
           << std::setw(6) << planeinfo.VopSrcRect_.iTop_ << std::setfill(' ') << std::setw(6)
           << planeinfo.VopSrcRect_.iRight_ << std::setfill(' ') << std::setw(6) << planeinfo.VopSrcRect_.iBottom_;

        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << std::hex << planeinfo.uFourccFormat_;
        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << planeinfo.uTransform_;
        ss << "|" << "0x" << std::setfill('0') << std::setw(8) << planeinfo.uDataSpace_;
        ss << "|" << std::setw(8) << planeinfo.iLayerType_;
        ss << "|" << planeinfo.sLayerName_;
        ss << std::endl;
    }

    static std::shared_ptr<HwpqDisplayStatus> CollectPqDisplayStatus(std::vector<DrmHwcLayer*>& drm_hwc_layers,
                                                                     PqDisplayCtx ctx) {
        std::shared_ptr<HwpqDisplayStatus> status = std::make_shared<HwpqDisplayStatus>();
        //初始化平台信息
        status->iPqZpos = -1;
        status->uSocId_ = ctx.uSocId;
        status->iResolutionWidth_ = ctx.iDisplayWidth;
        status->iResolutionHeight_ = ctx.iDisplayHeight;

        int client_layer_zpos = -1;
        std::vector<android::HwpqDisplayStatus::LayerBufferInfo> clientComposeInfo;
        bool use_client_composite = false;
        bool use_video_transform = false;
        bool use_video_client_composite = false;

        for (auto& drmlayer : drm_hwc_layers) {
            //查找fb_target图层
            if (drmlayer->bFbTarget_) {
                client_layer_zpos = drmlayer->iDrmZpos_;
                break;
            }
        }
        for (auto& drmlayer : drm_hwc_layers) {
            if (!drmlayer->bMatch_) {
                if (drmlayer->bFbTarget_) {
                    continue;
                }
                //GPU合成图层，若使用(HWC/RKCV/RGA/Skip)合成，drmlayer->bMatch_为true
                //只要存在非match的图层，必然存在GPU合成
                use_client_composite = true;
                if (drmlayer->bYuv_) {
                    use_video_client_composite = true;
                }
                //GPU合成的图层信息，存到clientComposeInfo内
                clientComposeInfo.emplace_back();
                PopulatePqLayerBufferInfo(drmlayer, clientComposeInfo.back());
            } else {
                //HWC合成图层，TODO：后续增加(RKCV/RGA/Skip)合成判断
                //获取win_type和zpos

                bool is_hwpq_layer = IsHwpqLayer(drmlayer->uWinType_);
                int zpos = drmlayer->iDrmZpos_;
                HwpqDisplayStatus::PlaneInfo layer_info;

                PopulatePqLayerInfo(drmlayer, layer_info);
                status->mapPlaneInfo_.emplace(zpos, layer_info);

                if (is_hwpq_layer) {
                    status->iPqZpos = zpos;
                    //如果hwpq图层有变换
                    if (drmlayer->bUseRga_) {
                        use_video_transform = true;
                    }
                }
            }
        }

        //将GPU合成的图层信息存储到对应的plane info内
        if (clientComposeInfo.size() > 0) {
            if (!status->mapPlaneInfo_.count(client_layer_zpos)) {
                HWC2_LOGE("Error, got client composite layer but cannot found fb target!");
            } else {
                status->mapPlaneInfo_[client_layer_zpos].vecComposeInfo_ = clientComposeInfo;
            }
        }
        //检查是否有pq图层
        if (status->iPqZpos < 0 || status->mapPlaneInfo_.count(status->iPqZpos) == 0) {
            HWC2_LOGE("Error Could not found PQ layer!");
            return NULL;
        }

        auto setPqLayerType = [&status](HwpqDisplayStatus::PlaneInfo::PQ_LayerType type) {
            status->mapPlaneInfo_.at(status->iPqZpos).iLayerType_ = type;
        };
        //标注pq layer类型
        if (use_client_composite && client_layer_zpos == status->iPqZpos) {
            if (use_video_client_composite) {
                setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_VIDEO_WITH_UI);
            } else {
                setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_UI);
            }
        } else {
            if (use_video_transform) {
                setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_VIDEO_TRANSFORM);
            } else {
                setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_VIDEO);
            }
        }
        //dump收集到的结构
        if (LogLevel(DBG_VERBOSE)) {
            std::stringstream ss;
            for (auto& plane_info_map : status->mapPlaneInfo_) {
                auto& plane_info = plane_info_map.second;
                ss << "|Zpos|  VopDstRect(l,t,r,b)   |  VopSrcRect(l,t,r,b)   |  Fourcc  | Transform| Dataspace|  Type "
                      " | Name"
                   << std::endl;
                dumpPqPlaneInfo(plane_info, ss);
                if (plane_info.vecComposeInfo_.size() > 0) {
                    ss << "|    |    intRect(l,t,r,b)    |    srcRect(l,t,r,b)    |  Fourcc  | Transform| Dataspace| "
                          "Name"
                       << std::endl;
                    for (auto& buf_info : plane_info.vecComposeInfo_) {
                        dumpPqBufInfo(buf_info, ss);
                    }
                }
            }
            HWC2_LOGD_IF("dump HWPQ_DisplayStatus: iPqZpos=%d, type=%d \n%s", status->iPqZpos,
                         status->mapPlaneInfo_.at(status->iPqZpos).iLayerType_, ss.str().c_str());
        }

        return status;
    }

    static std::shared_ptr<HwpqDisplayStatus> CollectPqSidebandDisplayStatus(std::vector<DrmHwcLayer>& drm_hwc_layers,
                                                                             PqDisplayCtx ctx) {
        std::shared_ptr<HwpqDisplayStatus> status = std::make_shared<HwpqDisplayStatus>();
        //初始化平台信息
        status->iPqZpos = -1;
        status->uSocId_ = ctx.uSocId;
        status->iResolutionWidth_ = ctx.iDisplayWidth;
        status->iResolutionHeight_ = ctx.iDisplayHeight;

        std::vector<android::HwpqDisplayStatus::LayerBufferInfo> clientComposeInfo;

        for (auto& drmlayer : drm_hwc_layers) {
            //HWC合成图层，TODO：后续增加(RKCV/RGA/Skip)合成判断
            //获取win_type和zpos

            bool is_hwpq_layer = IsHwpqLayer(drmlayer.uWinType_);
            int zpos = drmlayer.iDrmZpos_;
            HwpqDisplayStatus::PlaneInfo layer_info;

            PopulatePqLayerInfo(&drmlayer, layer_info);
            status->mapPlaneInfo_.emplace(zpos, layer_info);

            if (is_hwpq_layer) {
                status->iPqZpos = zpos;
            }
        }

        //检查是否有pq图层
        if (status->iPqZpos < 0 || status->mapPlaneInfo_.count(status->iPqZpos) == 0) {
            HWC2_LOGE("Error Could not found PQ layer!");
            return NULL;
        }

        auto setPqLayerType = [&status](HwpqDisplayStatus::PlaneInfo::PQ_LayerType type) {
            status->mapPlaneInfo_.at(status->iPqZpos).iLayerType_ = type;
        };
        //标注pq layer类型
        if (drm_hwc_layers.size() > 1) {
            setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_VIDEO_WITH_UI);

        } else {
            setPqLayerType(HwpqDisplayStatus::PlaneInfo::PQ_LayerType::PQ_LAYER_TYPE_VIDEO);
        }

        //dump收集到的结构
        if (LogLevel(DBG_DEBUG)) {
            std::stringstream ss;
            for (auto& plane_info_map : status->mapPlaneInfo_) {
                auto& plane_info = plane_info_map.second;
                ss << "|Zpos|  VopDstRect(l,t,r,b)   |  VopSrcRect(l,t,r,b)   |  Fourcc  | Transform| Dataspace|  Type "
                      " | Name"
                   << std::endl;
                dumpPqPlaneInfo(plane_info, ss);
                if (plane_info.vecComposeInfo_.size() > 0) {
                    ss << "|    |    intRect(l,t,r,b)    |    srcRect(l,t,r,b)    |  Fourcc  | Transform| Dataspace| "
                          "Name"
                       << std::endl;
                    for (auto& buf_info : plane_info.vecComposeInfo_) {
                        dumpPqBufInfo(buf_info, ss);
                    }
                }
            }
            HWC2_LOGD_IF("dump HWPQ_DisplayStatus: iPqZpos=%d, type=%d \n%s", status->iPqZpos,
                         status->mapPlaneInfo_.at(status->iPqZpos).iLayerType_, ss.str().c_str());
        }

        return status;
    }

    static inline void PopulatePqLayerBufferInfo(DrmHwcLayer* layer,
                                                 HwpqDisplayStatus::LayerBufferInfo& layerBufferInfo) {
        layerBufferInfo.srcRect_.iLeft_ = layer->source_crop.left;
        layerBufferInfo.srcRect_.iRight_ = layer->source_crop.right;
        layerBufferInfo.srcRect_.iTop_ = layer->source_crop.top;
        layerBufferInfo.srcRect_.iBottom_ = layer->source_crop.bottom;

        layerBufferInfo.intRect_.iLeft_ = layer->display_frame_sf.left;
        layerBufferInfo.intRect_.iRight_ = layer->display_frame_sf.right;
        layerBufferInfo.intRect_.iTop_ = layer->display_frame_sf.top;
        layerBufferInfo.intRect_.iBottom_ = layer->display_frame_sf.bottom;

        layerBufferInfo.uFourccFormat_ = layer->uFourccFormat_;
        layerBufferInfo.iHalFormat_ = layer->iFormat_;

        layerBufferInfo.uTransform_ = layer->transform;
        layerBufferInfo.uDataSpace_ = layer->eDataSpace_;
        layerBufferInfo.sLayerName_ = layer->sLayerName_;
    }

    static inline void PopulatePqLayerInfo(DrmHwcLayer* layer, HwpqDisplayStatus::PlaneInfo& layerInfo) {
        layerInfo.VopSrcRect_.iLeft_ = layer->source_crop.left;
        layerInfo.VopSrcRect_.iRight_ = layer->source_crop.right;
        layerInfo.VopSrcRect_.iTop_ = layer->source_crop.top;
        layerInfo.VopSrcRect_.iBottom_ = layer->source_crop.bottom;

        layerInfo.VopDstRect_.iLeft_ = layer->display_frame.left;
        layerInfo.VopDstRect_.iRight_ = layer->display_frame.right;
        layerInfo.VopDstRect_.iTop_ = layer->display_frame.top;
        layerInfo.VopDstRect_.iBottom_ = layer->display_frame.bottom;

        layerInfo.uFourccFormat_ = layer->uFourccFormat_;
        layerInfo.iHalFormat_ = layer->iFormat_;
        layerInfo.uModifier_ = layer->uModifier_;

        layerInfo.uTransform_ = layer->transform;
        layerInfo.uDataSpace_ = layer->eDataSpace_;
        layerInfo.sLayerName_ = layer->sLayerName_;

        HwpqDisplayStatus::PlaneInfo::PlaneType plane_type;
        if (gIsRK3576()) {
            if (layer->uWinType_ & PLANE_RK3576_ALL_CLUSTER_MASK) {
                plane_type.type = HwpqDisplayStatus::PlaneInfo::PLANE_TYPE_CLUSTER;
            } else if (layer->uWinType_ & PLANE_RK3576_ALL_ESMART_MASK) {
                plane_type.type = HwpqDisplayStatus::PlaneInfo::PLANE_TYPE_ESMART;
            }
        }
        layerInfo.PlaneType_ = plane_type;
        layerInfo.iZpos_ = layer->iDrmZpos_;
    }

    static bool IsHwpqLayer(uint64_t win_type) {
        if (gIsRK3576()) {
            return (win_type == PLANE_RK3576_CLUSTER0_WIN0);
        }
        return false;
    }
#endif
};
}  // namespace android
#endif