// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "driver_module.h"

#include "windows_headers.h"

#include <atomic>
#include <mutex>

#ifndef _WIN32
    #include <dlfcn.h>
#endif

#include "logger_wrapper.h"

namespace {
std::atomic<bool> g_pinned{false};  // its address is in this library
}  // namespace

void DriverModule::Pin() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&g_pinned), &self)) {
            LOG(WARNING) << "Could not pin the driver library: error " << GetLastError();
            return;
        }
#else
        // The loaded copy (RTLD_NOLOAD), made undeletable (RTLD_NODELETE: glibc and macOS dyld). The handle is kept.
        Dl_info info{};
        if (dladdr(&g_pinned, &info) == 0 || info.dli_fname == nullptr
            || dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) == nullptr) {
            LOG(WARNING) << "Could not pin the driver library";
            return;
        }
#endif
        g_pinned = true;
    });
}

bool DriverModule::IsPinned() { return g_pinned; }
