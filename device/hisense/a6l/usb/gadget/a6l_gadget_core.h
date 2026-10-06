// SPDX-License-Identifier: Apache-2.0
// Hisense A6L USB gadget core (android-usb, 29 Sep 2026): configfs + FunctionFS composition logic of the A6L gadget
// HAL, free of Android dependencies so it runs in host tests against a fake configfs/functionfs/sysfs tree.
//
// Kernel side (mainline dwc3 "a800000.usb", V67/r5 config): libcomposite + configfs functions ffs (adb/mtp/ptp),
// rndis, ncm (built in), midi (=m, usb_f_midi.ko NOT staged -> reported unsupported until it is). There is no
// f_accessory / f_audio_source / android_usb in mainline: ACCESSORY, AUDIO_SOURCE, UVC and CTRL are unsupported.
// The gadget skeleton (g1, strings, functions, FunctionFS mounts) is created by init (rom/init/init.a6l.usb.rc).
//
// A6L kernel gate a6l_manual_usb=1 (kernel/a6l-manual-usb-connect.patch, on the ROM cmdline): binding the UDC does
// NOT pull D+ up; every bind is followed by soft_connect "connect".
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace a6l {
namespace usb {

// = android.hardware.usb.gadget.GadgetFunction (AIDL V2)
constexpr uint64_t kNone = 0;
constexpr uint64_t kAdb = 1;
constexpr uint64_t kAccessory = 1 << 1;
constexpr uint64_t kMtp = 1 << 2;
constexpr uint64_t kMidi = 1 << 3;
constexpr uint64_t kPtp = 1 << 4;
constexpr uint64_t kRndis = 1 << 5;
constexpr uint64_t kAudioSource = 1 << 6;
constexpr uint64_t kUvc = 1 << 7;
constexpr uint64_t kCtrl = 1 << 8;
constexpr uint64_t kNcm = 1 << 10;

constexpr uint16_t kVendorId = 0x18d1;  // AOSP/Pixel convention (system/core/rootdir/init.usb.rc ids)

struct FfsFunction {
    std::string instance;                // "adb" -> functions/ffs.adb, mount /dev/usb-ffs/adb
    std::vector<std::string> endpoints;  // files that exist once the daemon wrote its descriptors
};

struct Composition {
    bool supported = false;
    std::string error;                   // why not supported
    uint16_t pid = 0;
    std::string configName;              // configs/b.1/strings/0x409/configuration
    std::vector<std::string> functions;  // function dir names under functions/, in link order (f1, f2, ...)
    std::vector<FfsFunction> ffs;        // FunctionFS functions that must be ready before the UDC is bound
    bool iad = false;                    // RNDIS/NCM: device class Misc/IAD (0xEF/0x02/0x01) for Windows hosts
};

// Pure mapping GadgetFunction bitmap -> composition. NONE -> supported, no functions.
Composition compose(uint64_t functions);

struct Paths {
    std::string gadget = "/config/usb_gadget/g1";
    std::string ffsRoot = "/dev/usb-ffs";
    std::string udcClass = "/sys/class/udc";
};

using Logger = std::function<void(int prio /*0 info, 1 warn, 2 error*/, const std::string&)>;

// File operations on the configfs gadget. Every method returns false (and logs) on a failed required step.
class Gadget {
  public:
    Gadget(Paths paths, std::function<std::string()> udcName, Logger log);
    const Paths& paths() const { return mPaths; }
    std::string udc() const;                     // sys.usb.controller (if present in /sys/class/udc), else the first entry
    bool bound() const;                          // g1/UDC names a controller
    bool unbind();                               // UDC "none" (ignored when already unbound)
    bool unlinkAll();                            // remove configs/b.1/f* links
    bool apply(const Composition& c);            // ids, class, configuration string, links
    bool functionAvailable(const std::string& fn) const;  // functions/<fn> exists
    bool ffsReady(const Composition& c) const;   // all endpoint files present
    bool bind();                                 // write UDC + soft_connect connect
    // r6c: bound AND carries exactly this composition (idProduct + the ordered configs/b.1/f* link targets), e.g. the
    // pre-framework adb binding made by init.a6l.usb.rc; such a gadget is adopted instead of unbound and re-bound.
    bool matches(const Composition& c) const;
    std::string currentSpeed() const;            // /sys/class/udc/<udc>/current_speed (trimmed), "" if unknown

    unsigned bindFailures() const { return mBindErrs; }  // consecutive failed bind() calls (0 after a success)

  private:
    Paths mPaths;
    std::function<std::string()> mUdcName;
    Logger mLog;
    unsigned mBindErrs = 0;  // r6c: rate-limits the bind error log (QEMU r6c: 562 identical lines, no UDC there)
};

// Serialises requests from the HAL and keeps the gadget bound while FunctionFS daemons (adbd, MtpServer via
// system_server) come and go: a monitor thread (inotify on the FunctionFS mounts, polling fallback) binds the UDC
// once all endpoints of the requested composition exist and re-binds after a daemon restart unbound it.
class Controller {
  public:
    // kApplyFailed (r6c): the gadget could not be written (unbind/unlink/ids/links, e.g. EACCES on configfs) - the caller
    // hands the gadget back to init's early-adb rules; kError: applied but not bound in time (the monitor keeps trying).
    enum class Result { kSuccess, kError, kNotSupported, kApplyFailed };
    Controller(Gadget* gadget, Logger log, unsigned disconnectWaitMs = 100);
    ~Controller();
    void start();
    void stop();
    // Tears the gadget down, applies the new composition and waits up to timeoutMs for the UDC bind.
    Result setFunctions(uint64_t functions, int64_t timeoutMs);
    // Pull down, wait, pull up again (IUsbGadget.reset). Fails when nothing is applied.
    bool reset();
    uint64_t currentFunctions() const;
    bool applied() const;
    unsigned bindCount() const;  // tests / dumps
    unsigned adoptCount() const; // r6c: requests satisfied by adopting an existing identical binding

  private:
    void monitorLoop();
    bool tryBindLocked();
    void wakeMonitor();
    void rewatchLocked();

    Gadget* mGadget;
    Logger mLog;
    unsigned mDisconnectWaitMs;
    mutable std::mutex mLock;
    std::condition_variable mCv;
    std::thread mThread;
    bool mRunning = false;
    bool mStop = false;
    uint64_t mFunctions = kNone;
    Composition mComp;
    bool mArmed = false;    // a composition with functions is applied and should be bound
    bool mApplied = false;  // mArmed && bound
    unsigned mGeneration = 0;
    unsigned mBinds = 0;
    unsigned mAdopts = 0;
    int mInotifyFd = -1;
    int mEventFd = -1;
    std::vector<int> mWatches;
};

}  // namespace usb
}  // namespace a6l
