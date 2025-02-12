#pragma once

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "drmbuffer.h"
#include "rockchip/utils/drmdebug.h"
#include "im2d.hpp"

#ifndef RK_GRALLOC_USAGE_WITHIN_4G
#define RK_GRALLOC_USAGE_WITHIN_4G (1ULL << 56)
#endif

// 颜色缓冲区的包装类
class SolidColorBuffer {
 public:
  SolidColorBuffer(uint32_t rgba, size_t width, size_t height)
      : rgba_(rgba),
        width_(width),
        height_(height),
        initCompleted_(false),
        fillCompleted_(false) {
    auto start = std::chrono::steady_clock::now();

    // 异步初始化 DrmBuffer
    initFuture_ = std::async(std::launch::async, [this]() {
      auto initStart = std::chrono::steady_clock::now();

      drmBuffer_ = std::make_unique<
          android::DrmBuffer>(width_, height_, HAL_PIXEL_FORMAT_RGBA_8888,
                              GRALLOC_USAGE_HW_COMPOSER |
                                  GRALLOC_USAGE_SW_WRITE_OFTEN |
                                  RK_GRALLOC_USAGE_WITHIN_4G,
                              "SolidColor");

      if (drmBuffer_->Init() != 0) {
        HWC2_ALOGE("Failed to init DrmBuffer");
        return;
      }

      auto initEnd = std::chrono::steady_clock::now();
      auto initDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                              initEnd - initStart)
                              .count();
      HWC2_ALOGD_IF_DEBUG("SolidColorBuffer DrmBuffer init took %lld us", initDuration);

      initCompleted_.store(true, std::memory_order_release);

      // 在初始化完成后启动异步填充
      startAsyncFill();
    });

    auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    HWC2_ALOGD_IF_DEBUG("SolidColorBuffer creation took %lld us", totalDuration);
  }

  ~SolidColorBuffer() {
    // 确保所有异步操作完成
    if (initFuture_.valid()) {
      initFuture_.wait();
    }
    if (fillFuture_.valid()) {
      fillFuture_.wait();
    }
  }

  android::DrmBuffer* getDrmBuffer() {
    waitForInit();  // 确保初始化完成
    return drmBuffer_.get();
  }

  uint32_t getRgba() const {
    return rgba_;
  }
  size_t getWidth() const {
    return width_;
  }
  size_t getHeight() const {
    return height_;
  }

  // 检查初始化是否完成
  bool isInitCompleted() const {
    return initCompleted_.load(std::memory_order_acquire);
  }

  // 检查填充是否完成
  bool isFillCompleted() const {
    return fillCompleted_.load(std::memory_order_acquire);
  }

  // 等待初始化完成
  void waitForInit() {
    if (initFuture_.valid()) {
      initFuture_.wait();
    }
  }

  // 等待填充完成
  void waitForFillComplete() {
    waitForInit();  // 必须先等待初始化完成
    if (fillFuture_.valid()) {
      fillFuture_.wait();
    }
  }

 private:
  // 启动异步填充
  void startAsyncFill() {
    fillFuture_ = std::async(std::launch::async, [this]() {

      auto fillStart = std::chrono::steady_clock::now();
      bool fillSuccess = false;
      char value[PROPERTY_VALUE_MAX];
      property_get("vendor.hwc.disable_solid_color_fill_by_rga", value, "0");
      if(atoi(value) == 0){
        rga_buffer_t src;
        im_rect src_rect;
        memset(&src, 0, sizeof(rga_buffer_t));
        memset(&src_rect, 0, sizeof(im_rect));

        // Set src buffer info
        src.fd      = drmBuffer_->GetFd();
        src.width   = drmBuffer_->GetWidth();
        src.height  = drmBuffer_->GetHeight();
        src.wstride = drmBuffer_->GetStride();
        src.hstride = drmBuffer_->GetHeightStride();
        src.format  = drmBuffer_->GetFormat();

        // Set src rect info
        src_rect.x           = 0;
        src_rect.y           = 0;
        src_rect.width       = src.width;
        src_rect.height      = src.height;

        IM_STATUS im_state;

        // Call Im2d 格式转换
        im_state = imfill(src, src_rect, rgba_);

        if (im_state == IM_STATUS_SUCCESS)
        {
            HWC2_ALOGD_IF_DEBUG("SolidColorBuffer: call im2d convert to rgb888 Success");
            fillSuccess = true;
        }else{
            HWC2_ALOGE("SolidColorBuffer: call im2d convert to rgb888 failed");
        }
        auto fillEnd = std::chrono::steady_clock::now();
        auto fillDuration = std::chrono::duration_cast<
                                std::chrono::microseconds>(fillEnd - fillStart)
                                .count();
        HWC2_ALOGD_IF_DEBUG("SolidColorBuffer: RGA Color fill took %lld us", fillDuration);
        fillCompleted_.store(true, std::memory_order_release);
      }

      // 如果RGA填充失败，则使用CPU填充
      if(!fillSuccess){
        if (uint32_t* data = static_cast<uint32_t*>(drmBuffer_->Lock())) {

          std::fill_n(data, width_ * height_, rgba_);

          auto fillEnd = std::chrono::steady_clock::now();
          auto fillDuration = std::chrono::duration_cast<
                                  std::chrono::microseconds>(fillEnd - fillStart)
                                  .count();
          HWC2_ALOGD_IF_DEBUG("SolidColorBuffer: Color fill took %lld us", fillDuration);

          drmBuffer_->Unlock();
          fillCompleted_.store(true, std::memory_order_release);
        }
      }

    });
  }

  std::unique_ptr<android::DrmBuffer> drmBuffer_;
  uint32_t rgba_;
  size_t width_;
  size_t height_;
  std::future<void> initFuture_;
  std::future<void> fillFuture_;
  std::atomic<bool> initCompleted_;
  std::atomic<bool> fillCompleted_;
};

class SolidColorManager {
 public:
  SolidColorManager() = default;

  // 获取或创建指定 RGBA 值的颜色缓冲区
  std::shared_ptr<SolidColorBuffer> getOrCreate(uint32_t rgba, size_t width,
                                                size_t height) {
    auto start = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = makeKey(rgba, width, height);

    auto it = bufferMap_.find(key);
    if (it != bufferMap_.end()) {
      if (auto buffer = it->second.lock()) {
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        HWC2_ALOGD_IF_DEBUG("Buffer cache hit took %lld us", duration);
        return buffer;
      }
      bufferMap_.erase(it);
    }

    auto createStart = std::chrono::steady_clock::now();
    auto buffer = std::make_shared<SolidColorBuffer>(rgba, width, height);
    bufferMap_[key] = buffer;

    auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    auto createDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - createStart)
                              .count();

    HWC2_ALOGD_IF_DEBUG("Buffer creation took %lld us, total getOrCreate took %lld us",
               createDuration, totalDuration);

    return buffer;
  }

 private:
  // 生成唯一键值
  static uint64_t makeKey(uint32_t rgba, size_t width, size_t height) {
    return (static_cast<uint64_t>(rgba) << 32) |
           (static_cast<uint64_t>(width) << 16) | static_cast<uint64_t>(height);
  }

  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::weak_ptr<SolidColorBuffer>> bufferMap_;
};