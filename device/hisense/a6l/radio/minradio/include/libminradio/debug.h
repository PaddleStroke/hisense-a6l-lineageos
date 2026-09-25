/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "binder_printing.h"

#include <android-base/logging.h>

namespace android::hardware::radio::minimal::debug {

static constexpr bool kSuperVerbose = true;
static constexpr bool kSuperCrazyVerbose = false;

// clang-format off
#define LOG_CALL_ALWAYS \
    LOG(VERBOSE) << '[' << serial << ("] " RADIO_MODULE ".") << __func__ << ' '

#define LOG_CALL                                                             \
    if constexpr (::android::hardware::radio::minimal::debug::kSuperVerbose) \
        LOG_CALL_ALWAYS

#define LOG_CALL_RESPONSE                                                    \
    if constexpr (::android::hardware::radio::minimal::debug::kSuperCrazyVerbose) \
        LOG(VERBOSE) << '[' << info.serial << ("] " RADIO_MODULE ".") << __func__ << ' '

#define LOG_CALL_NOSERIAL                                                    \
    if constexpr (::android::hardware::radio::minimal::debug::kSuperVerbose) \
        LOG(VERBOSE) << (RADIO_MODULE ".") << __func__ << ' '
// clang-format on

/**
 * Logs calls implemented to pretend doing the right thing, but doing nothing instead.
 */
#define LOG_CALL_IGNORED LOG_CALL_ALWAYS << "(ignored) "

/**
 * Logs calls always responding with REQUEST_NOT_SUPPORTED error.
 */
#define LOG_NOT_SUPPORTED LOG_CALL_ALWAYS << "(not supported) "

/**
 * Logs calls to deprecated methods. They should be never called by the framework nor xTS.
 */
#define LOG_AND_RETURN_DEPRECATED()                                                          \
    LOG(ERROR) << '[' << serial << ("] " RADIO_MODULE ".") << __func__ << " (deprecated!) "; \
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION)

}  // namespace android::hardware::radio::minimal::debug
