// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL service (agent ril, 24 Sep 2026): android.hardware.radio.* AIDL V4 over
// QMI/QRTR for the SDM660 modem on a mainline kernel.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

namespace android::hardware::radio::a6l {

using namespace std::string_literals;

static std::vector<std::shared_ptr<ndk::ICInterface>> gPublished;

template <typename T>
static std::shared_ptr<T> publish(const std::string& slot, std::shared_ptr<minimal::SlotContext> ctx) {
    const auto instance = T::descriptor + "/"s + slot;
    if (!AServiceManager_isDeclared(instance.c_str())) {
        LOG(INFO) << instance << " is not declared in VINTF, skipped";
        return nullptr;
    }
    auto hal = ndk::SharedRefBase::make<T>(ctx);
    gPublished.push_back(hal);
    ctx->addHal(hal);
    ModemCore::get(ctx->getSlotIndex()).addListener(hal.get());
    CHECK_EQ(AServiceManager_addService(hal->asBinder().get(), instance.c_str()), STATUS_OK) << instance;
    LOG(INFO) << "published " << instance;
    return hal;
}

static void main() {
    base::InitLogging(nullptr, base::LogdLogger(base::RADIO));
    base::SetDefaultTag("a6l-radio");
    base::SetMinimumLogSeverity(base::DEBUG);
    LOG(INFO) << "A6L QMI radio HAL starting (AIDL V4, QRTR)";
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();

    {
        const auto instance = A6lRadioConfig::descriptor + "/default"s;
        auto cfg = ndk::SharedRefBase::make<A6lRadioConfig>();
        gPublished.push_back(cfg);
        CHECK_EQ(AServiceManager_addService(cfg->asBinder().get(), instance.c_str()), STATUS_OK);
    }
    // ril3 (DSDS): slot2 is published when the product runs dual SIM (ro.vendor.a6l.ril.slots=2 or
    // persist.radio.multisim.config=dsds) AND the VINTF fragment declares it; otherwise single SIM
    // exactly as v1.
    const int slots = ModemCore::slotCount();
    LOG(INFO) << "slots: " << slots;
    for (int i = 1; i <= slots; i++) {
        const std::string name = "slot" + std::to_string(i);
        auto ctx = std::make_shared<minimal::SlotContext>(i);
        publish<A6lRadioData>(name, ctx);
        publish<A6lRadioModem>(name, ctx);
        publish<A6lRadioNetwork>(name, ctx);
        publish<A6lRadioSim>(name, ctx);
        publish<A6lRadioMessaging>(name, ctx);
        publish<A6lRadioVoice>(name, ctx);
        auto& c = ModemCore::get(i);
        // rilConnected is sent only once the modem answers; until then every request gets
        // RADIO_NOT_AVAILABLE (the framework shows "no service", like a phone with the modem off).
        c.onFirstReady([ctx, name] {
            LOG(INFO) << "modem ready: signalling rilConnected to the framework (" << name << ")";
            ctx->setConnected();
        });
        c.start();
    }
    ABinderProcess_joinThreadPool();
    LOG(FATAL) << "binder thread pool exited";
}

}  // namespace android::hardware::radio::a6l

int main() {
    android::hardware::radio::a6l::main();
    return 1;
}
