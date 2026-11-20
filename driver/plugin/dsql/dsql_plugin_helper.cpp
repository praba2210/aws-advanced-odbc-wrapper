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

#include "dsql_plugin_helper.h"

#include "../../util/rds_lib_loader.h"

std::optional<int> DsqlPluginHelper::BaseLibpqVersion([[maybe_unused]] const std::shared_ptr<RdsLibLoader>& loader)
{
    using PqLibVersionFn = int (*)();
#ifdef _WIN32
    HMODULE libpq = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"libpq.dll", &libpq)) {
        return std::nullopt;
    }
    const auto function = reinterpret_cast<PqLibVersionFn>(GetProcAddress(libpq, "PQlibVersion"));
#else
    if (!loader) {
        return std::nullopt;
    }
    const auto function = reinterpret_cast<PqLibVersionFn>(loader->GetFunction("PQlibVersion"));
#endif
    if (!function) {
        return std::nullopt;
    }
    return function();
}
