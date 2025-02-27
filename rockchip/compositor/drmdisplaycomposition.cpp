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

#define LOG_TAG "hwc-drm-display-composition"

#include "drmdisplaycomposition.h"
#include <stdlib.h>
#include "drmcrtc.h"
#include "drmdevice.h"
#include "drmdisplaycompositor.h"
#include "drmplane.h"
#include "platform.h"
#include "rockchip/drmtype.h"
#include "utils/autolock.h"
#include "utils/drmfence.h"

#include <algorithm>
#include <unordered_set>

#include <libsync/sw_sync.h>
#include <log/log.h>
#include <utils/Trace.h>
#include <xf86drmMode.h>

namespace android {

DrmDisplayComposition::~DrmDisplayComposition() {
    SignalCompositionDone();
    pthread_mutex_destroy(&lock_);
}

int DrmDisplayComposition::Init(DrmDevice* drm, DrmCrtc* crtc, Importer* importer, Planner* planner, uint64_t frame_no,
                                uint64_t display_id) {
    drm_ = drm;
    crtc_ = crtc;  // Can be NULL if we haven't modeset yet
    importer_ = importer;
    planner_ = planner;
    frame_no_ = frame_no;
    display_id_ = display_id;

    int ret = pthread_mutex_init(&lock_, NULL);
    if (ret) {
        ALOGE("Failed to initialize drm compositor lock %d\n", ret);
        return ret;
    }

    hdr_mode_ = DRM_HWC_SDR;
    bYuv10bit_ = false;
    has_sideband2_layer_ = false;
    has_svep_memc_layer_ = false;
    return 0;
}

bool DrmDisplayComposition::validate_composition_type(DrmCompositionType des) {
    return type_ == DRM_COMPOSITION_TYPE_EMPTY || type_ == des;
}

int DrmDisplayComposition::SetLayers(DrmHwcLayer* layers, size_t num_layers, bool geometry_changed) {
    if (!validate_composition_type(DRM_COMPOSITION_TYPE_FRAME)) return -EINVAL;

    layers_.clear();

    geometry_changed_ = geometry_changed;

    sideband_tunnel_id_ = 0;
    has_sideband2_layer_ = false;
    has_hwpq_layer_ = false;
    has_svep_memc_layer_ = false;

    for (size_t layer_index = 0; layer_index < num_layers; layer_index++) {
        if (layers[layer_index].bUseSr_ || layers[layer_index].bUseMemc_) {
            has_svep_layer_ = true;
        }
#ifdef USE_LIBSVEP_MEMC
        if (layers[layer_index].bUseMemc_) {
            has_svep_memc_layer_ = true;
            svep_memc_ = layers[layer_index].svep_memc_;
        }
#endif
        if (layers[layer_index].bSidebandStreamLayer_ && layers[layer_index].iTunnelId_ > 0) {
            has_sideband2_layer_ = layers[layer_index].bSideband2_;
            sideband_tunnel_id_ = layers[layer_index].iTunnelId_;
        }

#ifdef USE_LIBPQ_HWPQ
        if (layers[layer_index].hwPqReg_ != NULL) {
            has_hwpq_layer_ = true;
        }
#endif

        layers_.emplace_back(std::move(layers[layer_index]));
    }

    //sort
    if (layers_.size() > 1) {
        for (auto i = layers_.begin(); i != layers_.end() - 1; i++) {
            for (auto j = i + 1; j != layers_.end(); j++) {
                if ((*i).iDrmZpos_ > (*j).iDrmZpos_) {
                    std::swap(*i, *j);
                }
            }
        }
    }
    type_ = DRM_COMPOSITION_TYPE_FRAME;
    return 0;
}

int DrmDisplayComposition::SetDpmsMode(uint32_t dpms_mode) {
    if (!validate_composition_type(DRM_COMPOSITION_TYPE_DPMS)) return -EINVAL;
    dpms_mode_ = dpms_mode;
    type_ = DRM_COMPOSITION_TYPE_DPMS;
    return 0;
}

int DrmDisplayComposition::SetDisplayMode(const DrmMode& display_mode) {
    if (!validate_composition_type(DRM_COMPOSITION_TYPE_MODESET)) return -EINVAL;
    display_mode_ = display_mode;
    dpms_mode_ = DRM_MODE_DPMS_ON;
    type_ = DRM_COMPOSITION_TYPE_MODESET;
    return 0;
}

int DrmDisplayComposition::SetDisplayHdrMode(DrmHdrType hdr_mode, android_dataspace_t dataspace) {
    hdr_mode_ = hdr_mode;
    dataspace_ = dataspace;

    // 存在 10bit YUV图层
    for (auto& layer : layers_) {
        if (layer.bYuv10bit_) bYuv10bit_ = true;
    }

    return 0;
}

int DrmDisplayComposition::SetDropMode(bool mode) {
    drop_mode_ = mode;
    return 0;
};

bool DrmDisplayComposition::IsDropMode() {
    return drop_mode_;
};

int DrmDisplayComposition::AddPlaneDisable(DrmPlane* plane) {
    composition_planes_.emplace_back(DrmCompositionPlane::Type::kDisable, plane, crtc_);
    return 0;
}

int DrmDisplayComposition::AddPlaneComposition(DrmCompositionPlane&& plane) {
    composition_planes_.emplace_back(std::move(plane));
    return 0;
}

int DrmDisplayComposition::DisableUnusedPlanes() {
    if (type_ != DRM_COMPOSITION_TYPE_FRAME) return 0;

    std::vector<PlaneGroup*> plane_groups = drm_->GetPlaneGroups();

    int soc_id = crtc()->get_soc_id();

    //loop plane groups.
    for (std::vector<PlaneGroup*>::const_iterator iter = plane_groups.begin(); iter != plane_groups.end(); ++iter) {
        // Reserved DrmPlane feature.
        if ((*iter)->bReserved) continue;

        bool can_disable_plane = false;
        //loop plane
        uint32_t crtc_mask = 1 << crtc()->pipe();
        if ((*iter)->acquire(crtc_mask, display_id_)) {
            can_disable_plane = true;
        }

        if (isRK3566(soc_id)) can_disable_plane = true;

        if (can_disable_plane) {
            bool win0_has_disable = false;
            for (std::vector<DrmPlane*>::const_iterator iter_plane = (*iter)->planes.begin();
                 !(*iter)->planes.empty() && iter_plane != (*iter)->planes.end(); ++iter_plane) {
                if (!(*iter_plane)->is_use()) {
                    // 如果 win0 已经被disable,则不打印 win1/2/3 的disable日志
                    if (strstr((*iter_plane)->name(), "win0") != NULL) {
                        win0_has_disable = true;
                        HWC2_LOGD_IF("DisableUnusedPlanes plane_groups plane id=%d (%s)(all)", (*iter_plane)->id(),
                                     (*iter_plane)->name());
                    } else if (win0_has_disable == false) {
                        HWC2_LOGD_IF("DisableUnusedPlanes plane_groups plane id=%d (%s)", (*iter_plane)->id(),
                                     (*iter_plane)->name());
                    }

                    AddPlaneDisable(*iter_plane);
                    // break;
                    // 延迟使用
                    if ((*iter)->delay_use_cnt > 0) {
                        (*iter)->delay_use_cnt--;
                    }
                }

                if ((*iter)->is_will_disable()) {
                    HWC2_LOGD_IF("DisableUnusedPlanes will disable plane_groups plane id=%d (%s)", (*iter_plane)->id(),
                                 (*iter_plane)->name());
                    AddPlaneDisable(*iter_plane);
                    update_drmplane_assign_ = true;
                }
            }
        }
    }
    return 0;
}

int DrmDisplayComposition::CreateAndAssignReleaseFences(SyncTimeline& sync_timeline) {
    ATRACE_CALL();
    AutoLock lock(&lock_, __func__);
    if (lock.Lock()) return -1;
    signal_ = false;
    std::unordered_set<DrmHwcLayer*> comp_layers;

    for (const DrmCompositionPlane& plane : composition_planes_) {
        if (plane.type() == DrmCompositionPlane::Type::kLayer) {
            for (auto i : plane.source_layers()) {
                DrmHwcLayer* source_layer = &layers_[i];
                comp_layers.emplace(source_layer);
            }
        }
    }

    if (comp_layers.empty()) return 0;

    char acBuf[32];
    for (DrmHwcLayer* layer : comp_layers) {
        if (!layer) {
            continue;
        }

        // bSideband2_ 不需要创建 ReleaseFence
        if (layer->bSideband2_) {
            continue;
        }

        int sync_timeline_cnt = sync_timeline.IncTimeline();
        sprintf(acBuf, "RFD%" PRIu64 "-FN%" PRIu64 "-TC%d", display_id_, frame_no_, sync_timeline_cnt);
        layer->release_fence = sp<ReleaseFence>(new ReleaseFence(sync_timeline, sync_timeline_cnt, acBuf));
        if (layer->release_fence->isValid()) {
            HWC2_LOGD_IF(" Create ReleaseFence(%s) Sucess: frame = %" PRIu64 " LayerName=%s", acBuf, frame_no_,
                         layer->sLayerName_.c_str());
#if (defined USE_LIBSR) || (defined USE_LIBSVEP_MEMC)
            if (layer->bUseSr_ && layer->pSrBuffer_ != NULL) {
                layer->pSrBuffer_->SetReleaseFence(dup(layer->release_fence->getFd()));
                HWC2_LOGD_IF(" Create SrReleaseFence(%s) Sucess: frame = %" PRIu64 " LayerName=%s", acBuf, frame_no_,
                             layer->sLayerName_.c_str());
            }
#endif
            if (layer->bUseRga_ && layer->pRgaBuffer_ != NULL) {
                layer->pRgaBuffer_->SetReleaseFence(dup(layer->release_fence->getFd()));
                HWC2_LOGD_IF(" Create RgaReleaseFence(%s) Sucess: frame = %" PRIu64 " LayerName=%s", acBuf, frame_no_,
                             layer->sLayerName_.c_str());
            }
#ifdef USE_LIBPQ_HWPQ
            if (layer->bUseHwpqScale_ && layer->pPqBuffer_ != NULL) {
                layer->pPqBuffer_->SetReleaseFence(dup(layer->release_fence->getFd()));
                HWC2_LOGD_IF(" Create HwPqReleaseFence(%s) Sucess: frame = %" PRIu64 " LayerName=%s", acBuf, frame_no_,
                             layer->sLayerName_.c_str());
            }
#endif
        } else {
            HWC2_LOGE(" Create ReleaseFence(%s) Fail!: frame = %" PRIu64 " LayerName=%s", acBuf, frame_no_,
                      layer->sLayerName_.c_str());
            return -1;
        }
    }
    return 0;
}

sp<ReleaseFence> DrmDisplayComposition::GetReleaseFence(hwc2_layer_t layer_id) {
    ATRACE_CALL();
    AutoLock lock(&lock_, __func__);
    if (lock.Lock()) return ReleaseFence::NO_FENCE;
    std::unordered_set<DrmHwcLayer*> comp_layers;

    for (const DrmCompositionPlane& plane : composition_planes_) {
        if (plane.type() == DrmCompositionPlane::Type::kLayer) {
            for (auto i : plane.source_layers()) {
                DrmHwcLayer* source_layer = &layers_[i];
                comp_layers.emplace(source_layer);
            }
        }
    }

    if (comp_layers.empty()) return ReleaseFence::NO_FENCE;

    for (DrmHwcLayer* layer : comp_layers) {
        if (!layer) {
            continue;
        }
        if (layer->uId_ == layer_id) {
            return layer->release_fence;
        }
    }
    return ReleaseFence::NO_FENCE;
}

int DrmDisplayComposition::SignalCompositionDone() {
    ATRACE_CALL();
    AutoLock lock(&lock_, __func__);
    if (lock.Lock()) return -1;

    if (signal_) {
        HWC2_LOGD_IF("Have been signal frame = %" PRIu64 ", not to signal.", frame_no_);
        return 0;
    }

    std::unordered_set<DrmHwcLayer*> comp_layers;
    for (const DrmCompositionPlane& plane : composition_planes_) {
        if (plane.type() == DrmCompositionPlane::Type::kLayer) {
            for (auto i : plane.source_layers()) {
                DrmHwcLayer* source_layer = &layers_[i];
                comp_layers.emplace(source_layer);
            }
        }
    }

    if (comp_layers.empty()) return 0;

    for (DrmHwcLayer* layer : comp_layers) {
        if (!layer || !layer->release_fence->isValid()) {
            if (layer)
                HWC2_LOGD_IF("Fd(%d)isValid %d frame = %" PRIu64
                             " %s Info: size=%d act=%d signal=%d err=%d LayerName=%s ",
                             layer->release_fence->getFd(), layer->release_fence->isValid(), frame_no_,
                             layer->release_fence->getName().c_str(), layer->release_fence->getSize(),
                             layer->release_fence->getActiveCount(), layer->release_fence->getSignaledCount(),
                             layer->release_fence->getErrorCount(), layer->sLayerName_.c_str());
            continue;
        }
        int act, sig;
        if (LogLevel(DBG_DEBUG)) {
            act = layer->release_fence->getActiveCount();
            sig = layer->release_fence->getSignaledCount();
        }

        int ret = layer->release_fence->signal();
        if (LogLevel(DBG_DEBUG)) {
            HWC2_LOGD_IF("Signal %s frame = %" PRIu64 " %s Info: size=%d act=%d signal=%d err=%d BufferId=0x%" PRIx64
                         " SignalTime=%s LayerName=%s ",
                         act == 1 && sig == 0 && layer->release_fence->getActiveCount() == 0 &&
                                         layer->release_fence->getSignaledCount() == 1
                                 ? "Sucess"
                                 : "Fail",
                         frame_no_, layer->release_fence->getName().c_str(), layer->release_fence->getSize(),
                         layer->release_fence->getActiveCount(), layer->release_fence->getSignaledCount(),
                         layer->release_fence->getErrorCount(), layer->uBufferId_, layer->release_fence->dump().c_str(),
                         layer->sLayerName_.c_str());
        }
    }
    signal_ = true;
    return 0;
}

static const char* DrmCompositionTypeToString(DrmCompositionType type) {
    switch (type) {
        case DRM_COMPOSITION_TYPE_EMPTY:
            return "EMPTY";
        case DRM_COMPOSITION_TYPE_FRAME:
            return "FRAME";
        case DRM_COMPOSITION_TYPE_DPMS:
            return "DPMS";
        case DRM_COMPOSITION_TYPE_MODESET:
            return "MODESET";
        default:
            return "<invalid>";
    }
}

static const char* DPMSModeToString(int dpms_mode) {
    switch (dpms_mode) {
        case DRM_MODE_DPMS_ON:
            return "ON";
        case DRM_MODE_DPMS_OFF:
            return "OFF";
        default:
            return "<invalid>";
    }
}

static void DumpBuffer(const DrmHwcBuffer& buffer, std::ostringstream* out) {
    if (!buffer) {
        *out << "buffer=<invalid>";
        return;
    }

    *out << "buffer[w/h/format]=";
    *out << buffer->width << "/" << buffer->height << "/" << buffer->format;
}

static void DumpTransform(uint32_t transform, std::ostringstream* out) {
    *out << "[";

    if (transform == 0) *out << "IDENTITY";

    bool separator = false;
    if (transform & DrmHwcTransform::kFlipH) {
        *out << "FLIPH";
        separator = true;
    }
    if (transform & DrmHwcTransform::kFlipV) {
        if (separator) *out << "|";
        *out << "FLIPV";
        separator = true;
    }
    if (transform & DrmHwcTransform::kRotate90) {
        if (separator) *out << "|";
        *out << "ROTATE90";
        separator = true;
    }
    if (transform & DrmHwcTransform::kRotate180) {
        if (separator) *out << "|";
        *out << "ROTATE180";
        separator = true;
    }
    if (transform & DrmHwcTransform::kRotate270) {
        if (separator) *out << "|";
        *out << "ROTATE270";
        separator = true;
    }

    uint32_t valid_bits = DrmHwcTransform::kFlipH | DrmHwcTransform::kFlipV | DrmHwcTransform::kRotate90 |
                          DrmHwcTransform::kRotate180 | DrmHwcTransform::kRotate270;
    if (transform & ~valid_bits) {
        if (separator) *out << "|";
        *out << "INVALID";
    }
    *out << "]";
}

static const char* BlendingToString(DrmHwcBlending blending) {
    switch (blending) {
        case DrmHwcBlending::kNone:
            return "NONE";
        case DrmHwcBlending::kPreMult:
            return "PREMULT";
        case DrmHwcBlending::kCoverage:
            return "COVERAGE";
        default:
            return "<invalid>";
    }
}

void DrmDisplayComposition::Dump(String8& output) {
    output.appendFormat("Composition Dump:\n");
    output.appendFormat("FrameNo:%" PRIu64 ", ", frame_no_);
    output.appendFormat("Type: %s, ", DrmCompositionTypeToString(type_));
    output.appendFormat("Crtc=%d, ", crtc_ ? crtc_->id() : -1);
    // output.appendFormat(" display_mode=%dx%d", display_mode_.h_display(), display_mode_.v_display());
    // output.appendFormat(" dpms_mode=%s", DPMSModeToString(dpms_mode_));
    output.appendFormat("HdrMode=%s, ", DrmHdrTypeToString(hdr_mode_));
    output.appendFormat("LayerSize=%zu, ", layers_.size());
    output.appendFormat("PlaneSize=%zu", composition_planes_.size());
    output.append("\n");

    output.appendFormat("DrmPlane Dump:\n");
    // clang-format off
    output.append(
              "----+---+---------------+-----+------+--------+------------------------+------------------------+-----------------+---------+---------+----------+-----------+--------+--------+-------------\n"
              " EN | z |   PlaneName   | id  | fcc  |   fbc  | source crop (l,t,r,b)  |         frame          | stride x height |  trans  |  blnd   | g_alpha  | encoding  | range  |   hdr  |   name      \n"
              "----+---+---------------+-----+------+--------+------------------------+------------------------+-----------------+---------+---------+----------+-----------+--------+--------+-------------\n");
    // clang-format on

    for (auto& comp_plane : composition_planes_) {
        DrmPlane* plane = comp_plane.plane();
        const std::vector<size_t>& source_layers = comp_plane.source_layers();
        if (comp_plane.type() != DrmCompositionPlane::Type::kDisable) {
            output.appendFormat("%3s |", "Y");
            // z坐标
            output.appendFormat("%2d |", comp_plane.get_zpos());
            output.appendFormat("%14s |", plane->name());
            if (source_layers.empty()) {
                ALOGE("Can't handle empty source layer CompositionPlane.");
                continue;
            }

            if (source_layers.size() > 1) {
                ALOGE("Can't handle more than one source layer sz=%zu type=%d", source_layers.size(),
                      comp_plane.type());
                continue;
            }

            if (source_layers.front() >= layers_.size()) {
                ALOGE("Source layer index %zu out of bounds %zu type=%d", source_layers.front(), layers_.size(),
                      comp_plane.type());
                break;
            }

            DrmHwcLayer& layer = layers_[source_layers.front()];
            output.appendFormat("%4d |", layer.uId_);
            output.appendFormat(" %c%c%c%c |", layer.uFourccFormat_, layer.uFourccFormat_ >> 8,
                                layer.uFourccFormat_ >> 16, layer.uFourccFormat_ >> 24);
            if (layer.bAfbcd_) {
                output.appendFormat("%7s |", "AFBC");
            } else if (layer.bRfbcd_) {
                output.appendFormat("%7s |", "RFBC");
            } else {
                output.appendFormat("%7s |", "Raster");
            }

            output.appendFormat("%5d,%5d,%5d,%5d |", (int)layer.source_crop.left, (int)layer.source_crop.top,
                                (int)layer.source_crop.right, (int)layer.source_crop.bottom);
            output.appendFormat("%5d,%5d,%5d,%5d |", (int)layer.display_frame.left, (int)layer.display_frame.top,
                                (int)layer.display_frame.right, (int)layer.display_frame.bottom);
            output.appendFormat(" %6d x%7d |", layer.iStride_, layer.iHeightStride_);
            output.appendFormat("%8s |", layer.TransformToString(layer.transform).c_str());
            output.appendFormat("%8s |", layer.BlendingToString(layer.blending).c_str());
            output.appendFormat("%9d |", layer.alpha);

            if (gIsDrmVerison6_1()) {
                output.appendFormat("%10s |",
                                    DrmColorEncodingToString(layer.uColorSpace.colorspace_kernel_6_1_.color_encoding_));
                output.appendFormat("%7s |",
                                    DrmColorRangeToString(layer.uColorSpace.colorspace_kernel_6_1_.color_range_));
            } else {
                output.appendFormat("%10s |", V4L2ColorspaceToString(layer.uColorSpace.colorspace_kernel_510_));
                output.appendFormat("%7s |", V4L2ColorspaceToString(layer.uColorSpace.colorspace_kernel_510_));
            }
            if (layer.IsMetadataHdr_) {
                output.appendFormat("%7s |", "MetaHdr");
            } else if (layer.bHdr_) {
                output.appendFormat("%7s |", "Hdr");
            } else {
                output.appendFormat("%7s |", "None");
            }
            output.appendFormat(" %s", layer.sLayerName_.c_str());
            output.appendFormat("\n");
        } else {
            if (strstr(plane->name(), "win0") != NULL) {
                output.appendFormat("%3s |", "N");             // EN
                output.appendFormat("%2s |", "");              // z
                output.appendFormat("%14s |", plane->name());  // PlaneName
                output.appendFormat("%4s |", "");              // id
                output.appendFormat("%5s |", "");              // fcc
                output.appendFormat("%7s |", "");              // fbc
                output.appendFormat("%23s |", "");             //   source crop (l,t,r,b)
                output.appendFormat("%23s |", "");             // frame
                output.appendFormat("%16s |", "");             // stride x height
                output.appendFormat("%8s |", "");              // trans
                output.appendFormat("%8s |", "");              // blnd
                output.appendFormat("%9s |", "");              // g_alpha
                output.appendFormat("%10s |", "");             // encoding
                output.appendFormat("%7s |", "");              // range
                output.appendFormat("%7s |", "");              // hdr
                output.appendFormat("%s", "");                 // name
                output.appendFormat("\n");
            }
        }
    }
    // clang-format off
    output.append(
            "----+---+---------------+-----+------+--------+------------------------+------------------------+-----------------+---------+---------+----------+-----------+--------+--------+-------------\n");
    // clang-format on
}
}  // namespace android
