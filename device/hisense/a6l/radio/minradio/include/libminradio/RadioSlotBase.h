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

#include <libminradio/SlotContext.h>

#include <memory>

namespace android::hardware::radio::minimal {

class RadioSlotBase {
  private:
    bool mHasResponseFunctions = false;

  protected:
    std::shared_ptr<SlotContext> mContext;

    void setResponseFunctionsBase();

    /**
     * Called when new response functions are set. This is the place to send initial indications,
     * such as rilConnected or radioStateChanged.
     *
     * This callback is deferred if the RIL is not connected. In such case, it will be called after
     * getting rilConnected indication.
     */
    virtual void onUpdatedResponseFunctions();

  public:
    RadioSlotBase(std::shared_ptr<SlotContext> context);
    virtual ~RadioSlotBase() = default;

    void onConnected();
};

}  // namespace android::hardware::radio::minimal
