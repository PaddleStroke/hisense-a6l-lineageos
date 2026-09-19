#include <binder/IServiceManager.h>
#include <android/os/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/IBinder.h>
#include <utils/String16.h>
#include <android/log.h>
#include <android-base/properties.h>
#include <cstdio>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    setbuf(stdout, nullptr);
    __android_log_set_logger(__android_log_stderr_logger);
    alarm(10);
    std::printf("A6L_BINDER_CLIENT begin ready=%s\n", android::base::GetProperty("servicemanager.ready", "unset").c_str());
    auto process = android::ProcessState::initWithDriver(argv[1]);
    std::puts("A6L_BINDER_CLIENT driver_open");
    // This test deliberately uses a private device. The default manager's
    // transport selector checks /dev/binder and would choose RPC in V38.
    auto sm = android::interface_cast<android::os::IServiceManager>(process->getContextObject(nullptr));
    std::puts("A6L_BINDER_CLIENT service_manager_proxy");
    if (sm == nullptr) return 3;
    android::sp<android::IBinder> manager;
    auto status = sm->checkService("manager", &manager);
    if (!status.isOk()) return 6;
    if (manager == nullptr) return 4;
    auto result = manager->pingBinder();
    std::printf("A6L_BINDER_CLIENT manager_found=1 ping_status=%d\n", result);
    return result == android::OK ? 0 : 5;
}
