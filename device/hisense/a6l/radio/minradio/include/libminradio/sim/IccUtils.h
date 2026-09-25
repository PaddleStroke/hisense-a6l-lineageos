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

#pragma once

#include <aidl/android/hardware/radio/sim/IccIoResult.h>

#include <span>
#include <string>

namespace android::hardware::radio::minimal::sim {

::aidl::android::hardware::radio::sim::IccIoResult toIccIoResult(std::span<uint8_t const> bytes);
::aidl::android::hardware::radio::sim::IccIoResult toIccIoResult(std::vector<uint8_t>&& bytes);
::aidl::android::hardware::radio::sim::IccIoResult toIccIoResult(std::string_view simResponse);
::aidl::android::hardware::radio::sim::IccIoResult toIccIoResult(uint16_t errorCode);

std::vector<uint8_t> hexStringToBytes(std::string_view str);
std::vector<uint8_t> hexStringToBch(std::string_view str);
std::string bytesToHexString(std::span<uint8_t const> bytes);
std::string bytesToHexString(std::vector<uint8_t>&& bytes);
std::string bchToHexString(std::span<uint8_t const> bytes);

std::vector<uint8_t> uint8ToBytes(uint8_t val);
std::vector<uint8_t> uint16ToBytes(uint16_t val);

std::vector<uint8_t> encodeFplmns(std::span<std::string_view> fplmns);
std::vector<uint8_t> encodeMsisdn(std::string_view phoneNumber);
std::vector<uint8_t> encodeAd(uint8_t mncLength);

}  // namespace android::hardware::radio::minimal::sim
