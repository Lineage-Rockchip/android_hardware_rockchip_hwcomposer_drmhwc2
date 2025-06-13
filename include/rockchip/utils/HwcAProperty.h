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
#ifndef HWC_A_PROPERTY_H
#define HWC_A_PROPERTY_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

class HwcAProperty {
  public:
    static HwcAProperty& getInstance();

    std::string getString(const std::string& key, const std::string& defaultValue = "");
    bool getBool(const std::string& key, bool defaultValue = false);
    int getInt(const std::string& key, int defaultValue = 0);

    std::string getStringWithInterval(const std::string& key, int intervalMs, const std::string& defaultValue = "");
    bool getBoolWithInterval(const std::string& key, int intervalMs, bool defaultValue = false);
    int getIntWithInterval(const std::string& key, int intervalMs, int defaultValue = 0);

    void setProperty(const std::string& key, const std::string& value);
    void setPropertyAsync(const std::string& key, const std::string& value);

    // Dump 当前缓存内容
    void dump();

  private:
    HwcAProperty();
    ~HwcAProperty();

    HwcAProperty(const HwcAProperty&) = delete;
    HwcAProperty& operator=(const HwcAProperty&) = delete;

    std::string fetchProperty(const std::string& key, const std::string& defaultValue);
    std::string getRawWithInterval(const std::string& key, int intervalMs, const std::string& defaultValue);

    struct PropertyEntry {
        std::string value;
        std::chrono::steady_clock::time_point lastAccess;
        std::string comment;
    };

    struct WriteTask {
        std::string key;
        std::string value;
    };

    std::unordered_map<std::string, PropertyEntry> mIntervalCache;
    std::mutex mMutex;

    std::thread mWorkerThread;
    std::queue<WriteTask> mWriteQueue;
    std::mutex mQueueMutex;
    std::condition_variable mQueueCond;
    std::atomic<bool> mRunning;

    void writeThreadFunc();

    // 初始化注册属性
    void initializePropertyTable();
};

#endif  // HWC_A_PROPERTY_H
