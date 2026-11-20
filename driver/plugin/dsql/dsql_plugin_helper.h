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

#ifndef DSQL_PLUGIN_HELPER_H_
#define DSQL_PLUGIN_HELPER_H_

#include "../../util/windows_headers.h"

#include <memory>
#include <optional>

class RdsLibLoader;

namespace DsqlPluginHelper {
    // The base driver's libpq version (PQlibVersion, e.g. 160004), or nullopt when the function can't be found. On
    // POSIX it is a dlsym through the loaded driver, which finds the libpq the driver depends on; on Windows it is
    // GetProcAddress on the libpq.dll already loaded in the process, as GetProcAddress doesn't search imports.
    std::optional<int> BaseLibpqVersion(const std::shared_ptr<RdsLibLoader>& loader);
}  // namespace DsqlPluginHelper

#endif // DSQL_PLUGIN_HELPER_H_
