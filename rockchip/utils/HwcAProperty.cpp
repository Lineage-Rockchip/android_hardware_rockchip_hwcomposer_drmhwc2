/*
 * Copyright (C) 2025 Rockchip Electronics Co.Ltd.
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
#include "rockchip/utils/HwcAProperty.h"

#include <cutils/properties.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>

HwcAProperty::HwcAProperty() : mRunning(true) {
    initializePropertyTable();
    mWorkerThread = std::thread(&HwcAProperty::writeThreadFunc, this);
}

HwcAProperty::~HwcAProperty() {
    mRunning = false;
    mQueueCond.notify_all();
    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }
}

HwcAProperty& HwcAProperty::getInstance() {
    static HwcAProperty instance;
    return instance;
}

// ========== 初始化表格 ==========
void HwcAProperty::initializePropertyTable() {
    std::lock_guard<std::mutex> lock(mMutex);

    // 注册支持的属性及其注释
    mIntervalCache = {
            {"persist.sys.debug_mode", {"", std::chrono::steady_clock::now(), "是否启用调试模式"}},
            {"persist.sys.retry", {"", std::chrono::steady_clock::now(), "重试次数"}},
            {"persist.sys.name", {"", std::chrono::steady_clock::now(), "设备名"}},
            {"persist.sys.feature_flag", {"", std::chrono::steady_clock::now(), "功能开关标志位"}},
    };
}

// ========== 实时读取 ==========
std::string HwcAProperty::getString(const std::string& key, const std::string& defaultValue) {
    return fetchProperty(key, defaultValue);
}

bool HwcAProperty::getBool(const std::string& key, bool defaultValue) {
    std::string val = fetchProperty(key, defaultValue ? "1" : "0");
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "1" || val == "true" || val == "y" || val == "yes" || val == "on");
}

int HwcAProperty::getInt(const std::string& key, int defaultValue) {
    std::string val = fetchProperty(key, std::to_string(defaultValue));
    char* endPtr = nullptr;
    long intResult = std::strtol(val.c_str(), &endPtr, 10);
    if (endPtr == val.c_str() || *endPtr != '\0') {
        return defaultValue;
    }
    return static_cast<int>(intResult);
}

// ========== 间隔读取 ==========
std::string HwcAProperty::getRawWithInterval(const std::string& key, int intervalMs, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto now = std::chrono::steady_clock::now();

    auto it = mIntervalCache.find(key);
    if (it == mIntervalCache.end()) {
        // 动态添加，comment为空
        std::string val = fetchProperty(key, defaultValue);
        mIntervalCache[key] = {val, now, ""};
        return val;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.lastAccess).count();
    if (elapsed < intervalMs) {
        return it->second.value;
    }

    std::string val = fetchProperty(key, defaultValue);
    it->second.value = val;
    it->second.lastAccess = now;
    return val;
}

std::string HwcAProperty::getStringWithInterval(const std::string& key, int intervalMs,
                                                const std::string& defaultValue) {
    return getRawWithInterval(key, intervalMs, defaultValue);
}

bool HwcAProperty::getBoolWithInterval(const std::string& key, int intervalMs, bool defaultValue) {
    std::string val = getRawWithInterval(key, intervalMs, defaultValue ? "1" : "0");
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "1" || val == "true" || val == "y" || val == "yes" || val == "on");
}

int HwcAProperty::getIntWithInterval(const std::string& key, int intervalMs, int defaultValue) {
    std::string val = getRawWithInterval(key, intervalMs, std::to_string(defaultValue));
    char* endPtr = nullptr;
    long intResult = std::strtol(val.c_str(), &endPtr, 10);
    if (endPtr == val.c_str() || *endPtr != '\0') {
        return defaultValue;
    }
    return static_cast<int>(intResult);
}

// ========== 属性读取 ==========
std::string HwcAProperty::fetchProperty(const std::string& key, const std::string& defaultValue) {
    char value[PROPERTY_VALUE_MAX] = {0};
    int len = property_get(key.c_str(), value, defaultValue.c_str());
    if (len <= 0) {
        return defaultValue;
    }
    return std::string(value);
}
// ========== 同步写入 ==========
void HwcAProperty::setProperty(const std::string& key, const std::string& value) {
    property_set(key.c_str(), value.c_str());
}

// ========== 异步写入 ==========
void HwcAProperty::setPropertyAsync(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mWriteQueue.push({key, value});
    }
    mQueueCond.notify_one();
}

void HwcAProperty::writeThreadFunc() {
    while (mRunning) {
        WriteTask task;
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mQueueCond.wait(lock, [this]() { return !mWriteQueue.empty() || !mRunning; });

            if (!mRunning && mWriteQueue.empty()) break;

            task = mWriteQueue.front();
            mWriteQueue.pop();
        }

        property_set(task.key.c_str(), task.value.c_str());
    }
}

// ========== Dump 接口 ==========
void HwcAProperty::dump() {
    std::lock_guard<std::mutex> lock(mMutex);

    std::cout << std::left << std::setw(30) << "Property Key" << std::setw(20) << "Value"
              << "Comment" << std::endl;
    std::cout << std::string(80, '-') << std::endl;
    for (const auto& pair : mIntervalCache) {
        const auto& key = pair.first;
        const auto& entry = pair.second;
        std::cout << std::left << std::setw(30) << key << std::setw(20) << entry.value << entry.comment << std::endl;
    }
}
