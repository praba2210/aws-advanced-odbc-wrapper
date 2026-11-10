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

#ifndef SECURE_WIPE_H_
#define SECURE_WIPE_H_

#include "windows_headers.h"

#include <cstddef>
#include <cstring>
#include <string>

// Zeroes size bytes at data with writes the compiler can't drop as dead stores, for memory that held a password or a
// token and is about to be freed or go out of scope.
inline void SecureWipe(void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
#ifdef _WIN32
    SecureZeroMemory(data, size);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(data, size);
#else
    // memset_s needs __STDC_WANT_LIB_EXT1__ before the first <string.h>, which a header can't make sure of.
    volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
#endif
}

// Wipes everything a string's buffer holds, up to its capacity (an earlier, longer value too), and empties it.
inline void SecureWipeString(std::string& text) {
    text.resize(text.capacity());  // within the capacity: no reallocation
    SecureWipe(text.data(), text.size());
    text.clear();
}

// Wipes a string's or a vector's elements when it goes out of scope, on every return.
template <typename Container>
class ScopedWipe {
public:
    explicit ScopedWipe(Container& container) : container_(container) {}
    ~ScopedWipe() { SecureWipe(container_.data(), container_.size() * sizeof(*container_.data())); }

    ScopedWipe(const ScopedWipe&) = delete;
    ScopedWipe& operator=(const ScopedWipe&) = delete;
    ScopedWipe(ScopedWipe&&) = delete;
    ScopedWipe& operator=(ScopedWipe&&) = delete;

private:
    Container& container_;
};

#endif // SECURE_WIPE_H_
