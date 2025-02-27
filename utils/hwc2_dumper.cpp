
#include "utils/hwc2_dumper.h"
#include <regex>
#include <string>
#include "3rd/drm/drmhwc2_fourcc.h"

namespace android {
bool HwcDumper::directoryExists(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        return false;  // 无法访问路径
    } else if (info.st_mode & S_IFDIR) {
        return true;  // 是目录
    } else {
        return false;  // 不是目录
    }
}

std::string HwcDumper::GetDumpName(const char* tag, int frame_no, int z, HWC2::Composition compositon, int stride,
                                   int height_stride, int byte_stride, uint32_t fourcc_format, uint64_t modifier,
                                   std::string buffer_name, uint64_t buffer_id) {
    // 替换正斜杠变为.
    std::string buffer_name_tmp = std::regex_replace(buffer_name, std::regex("\\/"), "_");
    // 将空格替换为下划线
    std::string buffer_name_result = std::regex_replace(buffer_name_tmp, std::regex("\\s"), "_");

    char data_name[200] = {0};
    snprintf(data_name, sizeof(data_name), "%s%05d_Z%d_%s_%dx%d_%d_%c%c%c%c_%s_@%.35s_id_0x%" PRIx64 ".bin", tag,
             frame_no, z, HwcDumper::GetInstance()->CompositionTypeToStr(compositon), stride, height_stride,
             byte_stride, fourcc_format & 0xff, (fourcc_format >> 8) & 0xff, (fourcc_format >> 16) & 0xff,
             (fourcc_format >> 24) & 0xff, ModifierToStr(modifier),
             buffer_name_result.size() < 5 ? "unset" : buffer_name_result.c_str(), buffer_id);
    return data_name;
}

std::string HwcDumper::GetDumpName(const char* tag, int frame_no, int z, int stride, int height_stride, int byte_stride,
                                   uint32_t fourcc_format, uint64_t modifier, std::string buffer_name,
                                   uint64_t buffer_id) {
    // 替换正斜杠变为.
    std::string buffer_name_tmp = std::regex_replace(buffer_name, std::regex("\\/"), "_");
    // 将空格替换为下划线
    std::string buffer_name_result = std::regex_replace(buffer_name_tmp, std::regex("\\s"), "_");

    char data_name[200] = {0};
    snprintf(data_name, sizeof(data_name), "%s%05d_Z%d_NONE_%dx%d_%d_%c%c%c%c_%s_@%.35s_id_0x%" PRIx64 ".bin", tag,
             frame_no, z, stride, height_stride, byte_stride, fourcc_format & 0xff, (fourcc_format >> 8) & 0xff,
             (fourcc_format >> 16) & 0xff, (fourcc_format >> 24) & 0xff, ModifierToStr(modifier),
             buffer_name_result.size() < 5 ? "unset" : buffer_name_result.c_str(), buffer_id);
    return data_name;
}

int HwcDumper::DumpData(void* data, size_t size, const char* filename) {
    if (data == NULL) {
        HWC2_LOGE("Dumper: data=%p, DumpData error!", data);
        return -1;
    }

    if (!directoryExists(HWC_DUMP_DIR)) {
        int ret = mkdir(HWC_DUMP_DIR, 0755);
        if (ret) {
            HWC2_LOGE("Dumper: Can not create dir %s, ret=%d", HWC_DUMP_DIR, ret);
            return -1;
        } else {
            HWC2_LOGI("Dumper: create dir = %s success!", HWC_DUMP_DIR);
        }
    }

    char path[256] = {0};
    size_t writen = 0;
    snprintf(path, 256, HWC_DUMP_DIR "/%s", filename);
    FILE* fp = fopen(path, "wb");
    if (fp) {
        writen = fwrite(data, 1, size, fp);
        fflush(fp);
        fclose(fp);
    } else {
        HWC2_LOGE("Dumper: Can not open path %s", path);
        return -1;
    }
    return (writen == size ? 0 : -1);
}

const char* HwcDumper::CompositionTypeToStr(HWC2::Composition composition) {
    switch (composition) {
        case HWC2::Composition::Invalid:
            return "INVALID";
        case HWC2::Composition::Client:
            return "CLIENT";
        case HWC2::Composition::Device:
            return "DEVICE";
        case HWC2::Composition::SolidColor:
            return "SOLID_COLOR";
        case HWC2::Composition::Cursor:
            return "CURSOR";
        case HWC2::Composition::Sideband:
            return "SIDEBAND";
        default:
            return "Unknown";
    }
}

const char* HwcDumper::ModifierToStr(uint64_t modifier) {
    // 如果是RFBC压缩格式
    if (IS_ROCKCHIP_RFBC_MOD(modifier)) {
        return "RFBC";
    }
    // 如果是AFBC压缩格式
    if (fourcc_mod_is_vendor(modifier, ARM)) {
        return "AFBC";
    }

    return "Raster";
}

}  // namespace android