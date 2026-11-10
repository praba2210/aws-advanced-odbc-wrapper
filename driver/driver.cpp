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

#include "driver.h"

#include "error.h"
#include "plugin/base_plugin.h"
#include "util/logger_wrapper.h"
#include "util/plugin_service.h"
#include "util/rds_lib_loader.h"

BaseEnv::~BaseEnv() {
    if (handle != SQL_NULL_HENV) {
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader,
            RdsFuncNames::FreeHandle, SQL_HANDLE_ENV, handle);
        if (!res.fn_load_success || !SQL_SUCCEEDED(res.fn_result)) {
            LOG(WARNING) << "Unable to free the underlying environment handle";
        }
    }
}  // `logger` goes last, after the line above

ENV::~ENV() {
    logger_wrapper = nullptr;
}

DBC::~DBC() {
    plugin_service = nullptr;
}

STMT::~STMT() = default;

DESC::~DESC() = default;
