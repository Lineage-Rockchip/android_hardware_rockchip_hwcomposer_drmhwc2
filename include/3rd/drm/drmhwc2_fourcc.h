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

#ifndef DRMHWC2_FOURCC_H
#define DRMHWC2_FOURCC_H
#include "drm_fourcc.h"

#ifndef DRM_FORMAT_NV15
/*
 * 2 plane YCbCr
 * index 0 = Y plane, [39:0] Y3:Y2:Y1:Y0 little endian
 * index 1 = Cr:Cb plane, [39:0] Cr1:Cb1:Cr0:Cb0 little endian
 */
#define DRM_FORMAT_NV15 fourcc_code('N', 'V', '1', '5') /* 2x2 subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_NV12_10
#define DRM_FORMAT_NV12_10 fourcc_code('N', 'A', '1', '2') /* 2x2 subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_NV20
#define DRM_FORMAT_NV20 fourcc_code('N', 'V', '2', '0') /* 2x1 subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_NV30
#define DRM_FORMAT_NV30 fourcc_code('N', 'V', '3', '0') /* non-subsampled Cr:Cb plane */
#endif

#ifndef DRM_FORMAT_YUV420_8BIT
#define DRM_FORMAT_YUV420_8BIT fourcc_code('Y', 'U', '0', '8')
#endif

#ifndef DRM_FORMAT_YUV420_10BIT
#define DRM_FORMAT_YUV420_10BIT fourcc_code('Y', 'U', '1', '0')
#endif

#ifndef DRM_FORMAT_YUYV
#define DRM_FORMAT_YUYV fourcc_code('Y', 'U', 'Y', 'V') /* [31:0] Cr0:Y1:Cb0:Y0 8:8:8:8 little endian */
#endif

#ifndef DRM_FORMAT_Y210
#define DRM_FORMAT_Y210 \
    fourcc_code('Y', '2', '1', '0') /* [63:0] Cr0:0:Y1:0:Cb0:0:Y0:0 10:6:10:6:10:6:10:6 little endian per 2 Y pixels */
#endif

#ifndef DRM_FORMAT_VUY888
#define DRM_FORMAT_VUY888 fourcc_code('V', 'U', '2', '4') /* [23:0] Cr:Cb:Y 8:8:8 little endian */
#endif

#ifndef DRM_FORMAT_VUY101010
#define DRM_FORMAT_VUY101010 \
    fourcc_code('V', 'U', '3', '0') /* Y followed by U then V, 10:10:10. Non-linear modifier only */
#endif

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 fourcc_code('P', '0', '1', '0') /* 2x2 subsampled Cr:Cb plane 10 bits per channel */
#endif

#ifndef DRM_FORMAT_YUV420_8BIT
#define DRM_FORMAT_YUV420_8BIT fourcc_code('Y', 'U', '0', '8')
#endif

#ifndef DRM_FORMAT_YUV420_10BIT
#define DRM_FORMAT_YUV420_10BIT fourcc_code('Y', 'U', '1', '0')
#endif

#ifndef DRM_FORMAT_Y210
#define DRM_FORMAT_Y210 \
    fourcc_code('Y', '2', '1', '0') /* [63:0] Cr0:0:Y1:0:Cb0:0:Y0:0 10:6:10:6:10:6:10:6 little endian per 2 Y pixels */
#endif

#ifndef DRM_FORMAT_VUY888
#define DRM_FORMAT_VUY888 fourcc_code('V', 'U', '2', '4') /* [23:0] Cr:Cb:Y 8:8:8 little endian */
#endif

#ifndef DRM_FORMAT_VUY101010
#define DRM_FORMAT_VUY101010 \
    fourcc_code('V', 'U', '3', '0') /* Y followed by U then V, 10:10:10. Non-linear modifier only */
#endif

#ifndef DRM_FORMAT_XRGB16161616F
#define DRM_FORMAT_XRGB16161616F fourcc_code('X', 'R', '4', 'H') /* [63:0] x:R:G:B 16:16:16:16 little endian */
#endif

#ifndef DRM_FORMAT_XBGR16161616F
#define DRM_FORMAT_XBGR16161616F fourcc_code('X', 'B', '4', 'H') /* [63:0] x:B:G:R 16:16:16:16 little endian */
#endif

#ifndef DRM_FORMAT_ARGB16161616F
#define DRM_FORMAT_ARGB16161616F fourcc_code('A', 'R', '4', 'H') /* [63:0] A:R:G:B 16:16:16:16 little endian */
#endif

#ifndef DRM_FORMAT_ABGR16161616F
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H') /* [63:0] A:B:G:R 16:16:16:16 little endian */
#endif

#ifndef DRM_FORMAT_MOD_VENDOR_ROCKCHIP
#define DRM_FORMAT_MOD_VENDOR_ROCKCHIP 0x0b
#endif

#ifndef fourcc_mod_get_vendor
#define fourcc_mod_get_vendor(modifier) (((modifier) >> 56) & 0xff)
#endif

#ifndef fourcc_mod_is_vendor
#define fourcc_mod_is_vendor(modifier, vendor) (fourcc_mod_get_vendor(modifier) == DRM_FORMAT_MOD_VENDOR_##vendor)
#endif

#ifndef DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT 52
#endif

#ifndef DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK 0xf
#endif

#ifndef DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC
#define DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC 0x1
#endif

#ifndef ROCKCHIP_RFBC_BLOCK_SIZE_64x4
#define ROCKCHIP_RFBC_BLOCK_SIZE_64x4 (1ULL)
#endif

#ifndef DRM_FORMAT_MOD_ROCKCHIP_CODE
#define DRM_FORMAT_MOD_ROCKCHIP_CODE(__type, __val) \
    fourcc_mod_code(ROCKCHIP,                       \
                    ((__u64)(__type) << DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT) | ((__val) & 0x000fffffffffffffULL))
#endif

#ifndef DRM_FORMAT_MOD_ROCKCHIP_RFBC
/* Rockchip rfbc modifier format */
#define DRM_FORMAT_MOD_ROCKCHIP_RFBC(mode) DRM_FORMAT_MOD_ROCKCHIP_CODE(DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC, mode)
#endif

#ifndef IS_ROCKCHIP_RFBC_MOD
#define IS_ROCKCHIP_RFBC_MOD(val)                                                           \
    (fourcc_mod_is_vendor((val), ROCKCHIP) &&                                               \
     (((val) >> DRM_FORMAT_MOD_ROCKCHIP_TYPE_SHIFT) & DRM_FORMAT_MOD_ROCKCHIP_TYPE_MASK) == \
             DRM_FORMAT_MOD_ROCKCHIP_TYPE_RFBC)
#endif

#if ((defined(RK3326)) && (PLATFORM_SDK_VERSION <= 28))

#ifndef DRM_MODE_ROTATE_0
#define DRM_MODE_ROTATE_0 (1 << 0)
#define DRM_MODE_ROTATE_90 (1 << 1)
#define DRM_MODE_ROTATE_180 (1 << 2)
#define DRM_MODE_ROTATE_270 (1 << 3)
#endif

#ifndef DRM_MODE_REFLECT_X
#define DRM_MODE_REFLECT_X (1 << 4)
#define DRM_MODE_REFLECT_Y (1 << 5)
#endif

#ifndef DRM_MODE_ROTATE_MASK
#define DRM_MODE_ROTATE_MASK (DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270)
#endif

#ifndef DRM_MODE_REFLECT_MASK
#define DRM_MODE_REFLECT_MASK (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)
#endif

#ifndef AFBC_FORMAT_MOD_BLOCK_SIZE_16x16
#define AFBC_FORMAT_MOD_BLOCK_SIZE_MASK 0xf
#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 (1ULL)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 (2ULL)
#endif

#ifndef DRM_FORMAT_MOD_ARM_TYPE_AFBC
#define DRM_FORMAT_MOD_ARM_TYPE_AFBC 0x00
#endif

#ifndef DRM_FORMAT_MOD_ARM_TYPE_MISC
#define DRM_FORMAT_MOD_ARM_TYPE_MISC 0x01
#endif

#define DRM_FORMAT_MOD_ARM_CODE(__type, __val) \
    fourcc_mod_code(ARM, ((__u64)(__type) << 52) | ((__val) & 0x000fffffffffffffULL))

#ifdef DRM_FORMAT_MOD_ARM_AFBC
#undef DRM_FORMAT_MOD_ARM_AFBC
#endif

#define DRM_FORMAT_MOD_ARM_AFBC(__afbc_mode) DRM_FORMAT_MOD_ARM_CODE(DRM_FORMAT_MOD_ARM_TYPE_AFBC, __afbc_mode)

#endif

#endif
