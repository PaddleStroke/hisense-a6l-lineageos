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

#include <libminradio/SlotContext.h>

#include <android-base/logging.h>
#include <libminradio/RadioSlotBase.h>

namespace android::hardware::radio::minimal {

SlotContext::SlotContext(unsigned slotIndex) : mSlotIndex(slotIndex) {}

void SlotContext::setConnected() {
    CHECK(!mIsConnected) << "Can't setConnected twice";
    mIsConnected = true;
    for (auto weakHal : mHals) {
        auto hal = weakHal.lock();
        if (!hal) continue;
        hal->onConnected();
    }
}

bool SlotContext::isConnected() const {
    return mIsConnected;
}

unsigned SlotContext::getSlotIndex() const {
    return mSlotIndex;
}

void SlotContext::addHal(std::weak_ptr<RadioSlotBase> hal) {
    mHals.push_back(hal);
}

}  // namespace android::hardware::radio::minimal
