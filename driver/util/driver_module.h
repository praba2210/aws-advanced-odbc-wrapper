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

#ifndef DRIVER_MODULE_H_
#define DRIVER_MODULE_H_

// The wrapper's own library. Pin() keeps it mapped for the process's life, once a base driver call can outlive the
// app's last free: the driver manager may unload the driver then (the Windows DM at the last SQLFreeHandle(DBC)
// without pooling, unixODBC with DontDLClose=0, iODBC with UnloadSafe=1), while a thread still has to return into it
// and run the teardowns handed to it.
namespace DriverModule {
    void Pin();
    bool IsPinned();  // for the tests
}  // namespace DriverModule

#endif  // DRIVER_MODULE_H_
