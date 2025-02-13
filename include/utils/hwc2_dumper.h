
#ifndef HWC2_DUMPER_H
#define HWC2_DUMPER_H

#include <hardware/hwcomposer2.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Trace.h>
#include <iostream>
#include <list>
#include "rockchip/utils/drmdebug.h"

#define HWC_DUMP_DIR "/data/dump"

#ifndef ATRACE_TAG
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#endif

#ifndef LOG_TAG
#define LOG_TAG "hwc-dumper"
#endif

namespace android {

class HwcDumper {
  public:
    static HwcDumper* GetInstance() {
        static HwcDumper Dumper;
        return &Dumper;
    }

    std::string GetDumpName(const char* tag, int frame_no, int z, HWC2::Composition compositon, int stride,
                            int height_stride, int byte_stride, uint32_t fourcc_format, uint64_t modifier,
                            std::string buffer_name, uint64_t buffer_id);
    std::string GetDumpName(const char* tag, int frame_no, int z, int stride, int height_stride, int byte_stride,
                            uint32_t fourcc_format, uint64_t modifier, std::string buffer_name, uint64_t buffer_id);
    int DumpData(void* data, size_t size, const char* filename);
    const char* CompositionTypeToStr(HWC2::Composition val);
    const char* ModifierToStr(uint64_t modifier);

  private:
    HwcDumper() {};
    bool directoryExists(const char* path);
};

}  // namespace android

#endif  // HWC2_DUMPER_H