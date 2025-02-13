/*
 * Copyright (C) 2018 Fuzhou Rockchip Electronics Co.Ltd.
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

#ifndef _DRM_XML_H_
#define _DRM_XML_H_

#define DRM_XML_PATH_NAME "vendor.hwc.env_xml_path"
#define DRM_XML_PATH_SYS_NAME "persist.sys.hwc.env_xml_path"
#define DRM_XML_PATH_VENDOR_NAME "persist.vendor.hwc.env_xml_path"
#define DRM_XML_SYS_UPDATE "sys.hwc.display_pipeline_timeline"
#define DRM_XML_VENDOR_UPDATE "vendor.hwc.display_pipeline_timeline"

enum DrmDisplayMode {
    DRM_DISPLAY_MODE_NORMAL = 0,
    DRM_DISPLAY_MODE_SPLICE,            // 拼接模式
    DRM_DISPLAY_MODE_HORIZONTAL_SPLIT,  // 异显模式
};

struct DrmXmlVersion {
    int Major = -1;
    int Minor = -1;
    int PatchLevel = -1;
};

// ConnectorInfoXml
struct ConnectorInfoXml {
    char Type[20];
    int32_t TypeId = -1;
    int32_t SrcX = -1;
    int32_t SrcY = -1;
    int32_t SrcW = -1;
    int32_t SrcH = -1;
    int32_t DstX = -1;
    int32_t DstY = -1;
    int32_t DstW = -1;
    int32_t DstH = -1;
    int32_t Transform = -1;
    int32_t Primary = -1;
    int32_t Extend = -1;
};

// DisplayMode
struct DisplayModeXml {
    DrmXmlVersion Version;
    bool Enable = false;
    bool Valid = false;
    int32_t Mode = -1;
    int32_t FbWidth = -1;
    int32_t FbHeight = -1;
    int32_t ConnectorCnt = 0;
    ConnectorInfoXml ConnectorInfo[10];
};

#endif  // _DRM_XML_H_