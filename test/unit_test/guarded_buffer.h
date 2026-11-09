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

#ifndef TEST_GUARDED_BUFFER_H
#define TEST_GUARDED_BUFFER_H

// POSIX only: the tests that use it skip their guarded part on Windows.
#ifndef _WIN32
#include "../../driver/util/windows_headers.h"

#include <sql.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>

// A copy of an app's buffer whose last byte is the last one before an unreadable (PROT_NONE) page: reading past it
// faults, without ASan. Run the call in a death test's child (EXPECT_EXIT), so that a fault fails the test, not the
// run.
class GuardedBuffer {
public:
    GuardedBuffer(const void* data, size_t bytes) : page_(static_cast<size_t>(sysconf(_SC_PAGESIZE))) {
        const size_t data_pages = (bytes + page_ - 1) / page_;
        mapped_bytes_ = (data_pages + 1) * page_;
        void* mapped = mmap(nullptr, mapped_bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) {
            return;
        }
        mapped_ = static_cast<char*>(mapped);
        char* guard = mapped_ + (data_pages * page_);
        if (mprotect(guard, page_, PROT_NONE) != 0) {
            return;
        }
        begin_ = guard - bytes;
        if (bytes > 0) {
            std::memcpy(begin_, data, bytes);
        }
    }
    ~GuardedBuffer() {
        if (mapped_ != nullptr) {
            munmap(mapped_, mapped_bytes_);
        }
    }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;
    GuardedBuffer(GuardedBuffer&&) = delete;
    GuardedBuffer& operator=(GuardedBuffer&&) = delete;

    [[nodiscard]] SQLTCHAR* Get() const {
        return reinterpret_cast<SQLTCHAR*>(begin_);
    }

private:
    size_t page_;
    size_t mapped_bytes_ = 0;
    char* mapped_ = nullptr;
    char* begin_ = nullptr;
};

// In a death test's child, before a call that may fault: no core file for it.
inline void NoCoreFile() {
    const rlimit none{.rlim_cur = 0, .rlim_max = 0};
    setrlimit(RLIMIT_CORE, &none);
}
#endif

#endif // TEST_GUARDED_BUFFER_H
