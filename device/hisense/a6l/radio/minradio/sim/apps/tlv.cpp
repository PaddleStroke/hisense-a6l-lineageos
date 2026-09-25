/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "tlv.h"

#include <android-base/logging.h>

namespace android::hardware::radio::minimal::sim {

std::vector<uint8_t> makeTlv(uint32_t tag, std::span<uint8_t const> value) {
    // If needed, implement ISO 7816 5.2.2.1
    CHECK(tag <= 0xFFFF) << "3-byte tag numbers (" << tag << ") are not implemented";

    // If we end up needing more, implement ISO 7816 5.2.2.2
    CHECK(value.size() <= 0x7F) << "Large tag lengths are not implemented: " << value.size()
                                << " for " << tag;

    std::vector<uint8_t> serialized;
    if (tag <= 0xFF) {
        serialized = {static_cast<uint8_t>(tag), static_cast<uint8_t>(value.size())};
    } else {
        serialized = {static_cast<uint8_t>(tag >> 8), static_cast<uint8_t>(tag & 0xFF),
                      static_cast<uint8_t>(value.size())};
    }

    serialized.insert(serialized.end(), value.begin(), value.end());
    return serialized;
}

namespace tlv_operators {

std::vector<uint8_t> operator+(std::span<uint8_t const> a, std::span<uint8_t const> b) {
    std::vector<uint8_t> concatenated;
    concatenated.insert(concatenated.end(), a.begin(), a.end());
    concatenated.insert(concatenated.end(), b.begin(), b.end());
    return concatenated;
}

}  // namespace tlv_operators

}  // namespace android::hardware::radio::minimal::sim
