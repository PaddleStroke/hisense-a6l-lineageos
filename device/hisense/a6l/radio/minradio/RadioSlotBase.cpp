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

#include <libminradio/RadioSlotBase.h>

namespace android::hardware::radio::minimal {

RadioSlotBase::RadioSlotBase(std::shared_ptr<SlotContext> context) : mContext(context) {}

void RadioSlotBase::setResponseFunctionsBase() {
    mHasResponseFunctions = true;
    if (mContext->isConnected()) onUpdatedResponseFunctions();
}

void RadioSlotBase::onUpdatedResponseFunctions() {}

void RadioSlotBase::onConnected() {
    if (mHasResponseFunctions) setResponseFunctionsBase();
}

}  // namespace android::hardware::radio::minimal
