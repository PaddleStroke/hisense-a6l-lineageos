// SPDX-License-Identifier: Apache-2.0
// Hisense A6L USB gadget core (android-usb, 29 Sep 2026). See a6l_gadget_core.h.
#include "a6l_gadget_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace a6l {
namespace usb {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\0'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

bool readFile(const std::string& path, std::string* out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[256];
    std::string s;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) break;
        s.append(buf, n);
        if (s.size() > 4096) break;
    }
    close(fd);
    *out = s;
    return true;
}

// sysfs/configfs attributes: never create, one write() of the whole value.
bool writeFile(const std::string& path, const std::string& value, int* err) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_TRUNC);
    if (fd < 0) {
        *err = errno;
        return false;
    }
    ssize_t n;
    do {
        n = write(fd, value.data(), value.size());
    } while (n < 0 && errno == EINTR);
    int e = errno;
    if (close(fd) != 0 && n >= 0) {
        *err = errno;
        return false;
    }
    if (n != static_cast<ssize_t>(value.size())) {
        *err = n < 0 ? e : EIO;
        return false;
    }
    return true;
}

bool exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string hex4(unsigned v) {
    char b[8];
    snprintf(b, sizeof(b), "0x%04x", v & 0xffff);
    return b;
}

}  // namespace

Composition compose(uint64_t functions) {
    Composition c;
    const uint64_t unsupported = kAccessory | kAudioSource | kUvc | kCtrl;
    const uint64_t known = kAdb | kMtp | kMidi | kPtp | kRndis | kNcm | unsupported;
    if (functions & ~known) {
        c.error = "unknown function bits";
        return c;
    }
    if (functions & unsupported) {
        // mainline has no f_accessory/f_audio_source (AOA kernel functions), UVC needs usb_f_uvc.ko + a camera
        // source, CTRL is the userspace-AOA control function (not enabled on this device)
        c.error = "accessory/audio_source/uvc/ctrl not supported by the A6L mainline kernel";
        return c;
    }
    const uint64_t main = functions & (kMtp | kMidi | kPtp | kRndis | kNcm);
    if (main & (main - 1)) {
        c.error = "only one of mtp/ptp/rndis/midi/ncm (+adb) per configuration";
        return c;
    }
    const bool adb = functions & kAdb;
    c.supported = true;
    if (functions == kNone) return c;  // charging only: nothing bound
    const std::vector<std::string> ffsMtpEps = {"ep1", "ep2", "ep3"};
    switch (main) {
        case 0:
            c.pid = 0x4ee7;
            c.configName = "adb";
            break;
        case kMtp:
            c.pid = adb ? 0x4ee2 : 0x4ee1;
            c.configName = "mtp";
            c.functions.push_back("ffs.mtp");
            c.ffs.push_back({"mtp", ffsMtpEps});
            break;
        case kPtp:
            c.pid = adb ? 0x4ee6 : 0x4ee5;
            c.configName = "ptp";
            c.functions.push_back("ffs.ptp");
            c.ffs.push_back({"ptp", ffsMtpEps});
            break;
        case kRndis:
            c.pid = adb ? 0x4ee4 : 0x4ee3;
            c.configName = "rndis";
            c.functions.push_back("rndis.gs4");
            c.iad = true;
            break;
        case kMidi:
            c.pid = adb ? 0x4ee9 : 0x4ee8;
            c.configName = "midi";
            c.functions.push_back("midi.gs5");
            break;
        case kNcm:
            c.pid = adb ? 0x4eec : 0x4eeb;
            c.configName = "ncm";
            c.functions.push_back("ncm.gs9");
            c.iad = true;
            break;
    }
    if (adb) {
        if (main) c.configName += "_adb";
        c.functions.push_back("ffs.adb");
        c.ffs.push_back({"adb", {"ep1", "ep2"}});
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ Gadget

Gadget::Gadget(Paths paths, std::function<std::string()> udcName, Logger log)
    : mPaths(std::move(paths)), mUdcName(std::move(udcName)), mLog(std::move(log)) {}

std::string Gadget::udc() const {
    std::string n = mUdcName ? trim(mUdcName()) : "";
    if (!n.empty() && exists(mPaths.udcClass + "/" + n)) return n;
    DIR* d = opendir(mPaths.udcClass.c_str());
    if (!d) return "";
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names.empty() ? "" : names.front();
}

bool Gadget::bound() const {
    std::string s;
    if (!readFile(mPaths.gadget + "/UDC", &s)) return false;
    s = trim(s);
    return !s.empty() && s != "none";
}

bool Gadget::unbind() {
    if (!bound()) return true;
    int err = 0;
    if (!writeFile(mPaths.gadget + "/UDC", "none", &err) && bound()) {
        mLog(2, "unbind: UDC none failed: " + std::string(strerror(err)));
        return false;
    }
    return true;
}

bool Gadget::unlinkAll() {
    const std::string cfg = mPaths.gadget + "/configs/b.1";
    DIR* d = opendir(cfg.c_str());
    if (!d) {
        mLog(2, "unlinkAll: cannot open " + cfg);
        return false;
    }
    std::vector<std::string> links;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = cfg + "/" + e->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) links.push_back(p);
    }
    closedir(d);
    bool ok = true;
    for (const auto& l : links) {
        if (unlink(l.c_str()) != 0) {
            mLog(2, "unlinkAll: unlink " + l + ": " + strerror(errno));
            ok = false;
        }
    }
    return ok;
}

bool Gadget::functionAvailable(const std::string& fn) const {
    struct stat st;
    return stat((mPaths.gadget + "/functions/" + fn).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Gadget::apply(const Composition& c) {
    int err = 0;
    const std::string g = mPaths.gadget;
    struct {
        const char* attr;
        std::string value;
    } attrs[] = {
            {"idVendor", hex4(kVendorId)},
            {"idProduct", hex4(c.pid)},
            {"bDeviceClass", c.iad ? "0xef" : "0x0"},
            {"bDeviceSubClass", c.iad ? "0x2" : "0x0"},
            {"bDeviceProtocol", c.iad ? "0x1" : "0x0"},
    };
    for (const auto& a : attrs) {
        if (!writeFile(g + "/" + a.attr, a.value, &err)) {
            mLog(2, std::string("apply: ") + a.attr + ": " + strerror(err));
            return false;
        }
    }
    if (!writeFile(g + "/configs/b.1/strings/0x409/configuration", c.configName, &err))
        mLog(1, std::string("apply: configuration string: ") + strerror(err));  // cosmetic
    int i = 1;
    for (const auto& fn : c.functions) {
        const std::string target = g + "/functions/" + fn;
        const std::string link = g + "/configs/b.1/f" + std::to_string(i++);
        if (!functionAvailable(fn)) {
            mLog(2, "apply: function " + fn + " missing (kernel function module not loaded?)");
            return false;
        }
        if (symlink(target.c_str(), link.c_str()) != 0) {
            mLog(2, "apply: link " + fn + ": " + strerror(errno));
            return false;
        }
    }
    return true;
}

bool Gadget::ffsReady(const Composition& c) const {
    for (const auto& f : c.ffs)
        for (const auto& ep : f.endpoints)
            if (!exists(mPaths.ffsRoot + "/" + f.instance + "/" + ep)) return false;
    return true;
}

bool Gadget::bind() {
    const std::string name = udc();
    // r6c: a bind that keeps failing (no UDC, EBUSY) is retried by the monitor; log the 1st, then every 100th
    const bool say = (mBindErrs % 100) == 0;
    const std::string rep = mBindErrs ? " (failed " + std::to_string(mBindErrs) + "x before)" : "";
    if (name.empty()) {
        if (say) mLog(2, "bind: no UDC (sys.usb.controller not in " + mPaths.udcClass + " and none listed)" + rep);
        mBindErrs++;
        return false;
    }
    int err = 0;
    if (!writeFile(mPaths.gadget + "/UDC", name, &err)) {
        if (say) mLog(2, "bind: UDC " + name + ": " + strerror(err) + rep);
        mBindErrs++;
        return false;
    }
    mBindErrs = 0;
    // a6l_manual_usb=1: the UDC core holds the D+ pull-up after every bind until soft_connect "connect"
    const std::string sc = mPaths.udcClass + "/" + name + "/soft_connect";
    if (exists(sc) && !writeFile(sc, "connect", &err))
        mLog(1, "bind: soft_connect connect: " + std::string(strerror(err)) + " (host may not see the device)");
    mLog(0, "bound to " + name);
    return true;
}

bool Gadget::matches(const Composition& c) const {
    if (!bound()) return false;
    std::string pid;
    if (!readFile(mPaths.gadget + "/idProduct", &pid) || strtoul(trim(pid).c_str(), nullptr, 0) != c.pid) return false;
    const std::string cfg = mPaths.gadget + "/configs/b.1";
    DIR* d = opendir(cfg.c_str());
    if (!d) return false;
    std::vector<std::pair<int, std::string>> links;  // (index of fN, basename of the target)
    bool ok = true;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string p = cfg + "/" + e->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) continue;
        char b[512];
        ssize_t n = readlink(p.c_str(), b, sizeof(b) - 1);
        if (n <= 0 || e->d_name[0] != 'f') {
            ok = false;
            continue;
        }
        b[n] = 0;
        std::string t(b);
        size_t slash = t.find_last_of('/');
        links.emplace_back(atoi(e->d_name + 1), slash == std::string::npos ? t : t.substr(slash + 1));
    }
    closedir(d);
    if (!ok || links.size() != c.functions.size()) return false;
    std::sort(links.begin(), links.end());
    for (size_t i = 0; i < links.size(); i++)
        if (links[i].first != static_cast<int>(i + 1) || links[i].second != c.functions[i]) return false;
    return true;
}

std::string Gadget::currentSpeed() const {
    const std::string name = udc();
    std::string s;
    if (name.empty() || !readFile(mPaths.udcClass + "/" + name + "/current_speed", &s)) return "";
    return trim(s);
}

// ------------------------------------------------------------------------------------------------ Controller

Controller::Controller(Gadget* gadget, Logger log, unsigned disconnectWaitMs)
    : mGadget(gadget), mLog(std::move(log)), mDisconnectWaitMs(disconnectWaitMs) {}

Controller::~Controller() {
    stop();
}

void Controller::start() {
    std::lock_guard<std::mutex> lk(mLock);
    if (mRunning) return;
    mInotifyFd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (mInotifyFd < 0) mLog(1, std::string("inotify unavailable, polling: ") + strerror(errno));
    mEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    mStop = false;
    mRunning = true;
    mThread = std::thread(&Controller::monitorLoop, this);
}

void Controller::stop() {
    {
        std::lock_guard<std::mutex> lk(mLock);
        if (!mRunning) return;
        mStop = true;
    }
    wakeMonitor();
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
    std::lock_guard<std::mutex> lk(mLock);
    mRunning = false;
    if (mInotifyFd >= 0) close(mInotifyFd);
    if (mEventFd >= 0) close(mEventFd);
    mInotifyFd = mEventFd = -1;
    mWatches.clear();
}

void Controller::wakeMonitor() {
    if (mEventFd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(mEventFd, &one, sizeof(one));
        (void)r;
    }
}

void Controller::rewatchLocked() {
    if (mInotifyFd < 0) return;
    for (int w : mWatches) inotify_rm_watch(mInotifyFd, w);
    mWatches.clear();
    for (const auto& f : mComp.ffs) {
        const std::string dir = mGadget->paths().ffsRoot + "/" + f.instance;
        int w = inotify_add_watch(mInotifyFd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_ATTRIB);
        if (w < 0)
            mLog(1, "inotify " + dir + ": " + strerror(errno) + " (polling)");
        else
            mWatches.push_back(w);
    }
}

bool Controller::tryBindLocked() {
    if (!mArmed) return false;
    if (mGadget->bound()) {
        if (!mApplied) {
            mApplied = true;
            mCv.notify_all();
        }
        return true;
    }
    if (mApplied) mLog(1, "gadget unbound (FunctionFS daemon restarted?), waiting to re-bind");
    mApplied = false;
    if (!mGadget->ffsReady(mComp)) return false;
    if (!mGadget->bind()) return false;
    mApplied = true;
    mBinds++;
    mCv.notify_all();
    return true;
}

void Controller::monitorLoop() {
    for (;;) {
        int timeoutMs;
        int ifd, efd;
        {
            std::lock_guard<std::mutex> lk(mLock);
            if (mStop) return;
            ifd = mInotifyFd;
            efd = mEventFd;
            const bool watching = ifd >= 0 && !mWatches.empty() && mWatches.size() == mComp.ffs.size();
            if (!mArmed)
                timeoutMs = efd >= 0 ? -1 : 1000;
            else if (mApplied)
                timeoutMs = watching ? 5000 : 1000;  // re-check for an unexpected unbind
            else if (mGadget->bindFailures() >= 10)
                timeoutMs = 2000;                    // r6c: a bind that keeps failing: back off
            else
                timeoutMs = watching ? 500 : 100;    // waiting for descriptors / retrying a failed bind
        }
        struct pollfd p[2];
        int n = 0;
        if (efd >= 0) p[n++] = {efd, POLLIN, 0};
        if (ifd >= 0) p[n++] = {ifd, POLLIN, 0};
        int r = n ? poll(p, n, timeoutMs) : (usleep(timeoutMs * 1000), 0);
        if (r < 0 && errno != EINTR) usleep(100 * 1000);
        char buf[4096];
        if (efd >= 0) while (read(efd, buf, sizeof(uint64_t)) > 0) {}
        if (ifd >= 0) while (read(ifd, buf, sizeof(buf)) > 0) {}
        std::lock_guard<std::mutex> lk(mLock);
        if (mStop) return;
        tryBindLocked();
    }
}

Controller::Result Controller::setFunctions(uint64_t functions, int64_t timeoutMs) {
    std::unique_lock<std::mutex> lk(mLock);
    const unsigned gen = ++mGeneration;
    const Composition comp = compose(functions);
    const bool wasBound = mGadget->bound();
    // r6c: usbd asks for adb at boot while init's early-adb rule may already have bound exactly that (and adbd is
    // talking to the host): adopt it. Unbinding here would also make adbd close/re-open FunctionFS for nothing.
    if (comp.supported && functions != kNone && wasBound && mGadget->matches(comp)) {
        mFunctions = functions;
        mComp = comp;
        mArmed = true;
        mApplied = true;
        mAdopts++;
        rewatchLocked();
        wakeMonitor();
        mCv.notify_all();
        mLog(0, "setFunctions: gadget already bound with this composition (" + comp.configName + "), adopted");
        return Result::kSuccess;
    }
    mArmed = false;
    mApplied = false;
    mFunctions = functions;
    mComp = Composition();
    bool ok = mGadget->unbind();
    ok = mGadget->unlinkAll() && ok;
    if (!comp.supported) {
        mLog(2, "setFunctions " + std::to_string(functions) + ": " + comp.error);
        return Result::kNotSupported;
    }
    for (const auto& fn : comp.functions) {
        if (!mGadget->functionAvailable(fn)) {
            mLog(2, "setFunctions: function " + fn + " not available in configfs (kernel module missing)");
            return Result::kNotSupported;
        }
    }
    if (!ok) return Result::kApplyFailed;
    if (wasBound && mDisconnectWaitMs) {
        // leave D+ low long enough for the host to see a disconnect before the new composition appears
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(mDisconnectWaitMs));
        lk.lock();
        if (gen != mGeneration) return Result::kError;  // superseded
    }
    if (functions == kNone) {
        mApplied = true;  // charging only = nothing bound
        return Result::kSuccess;
    }
    if (!mGadget->apply(comp)) {
        mGadget->unlinkAll();
        return Result::kApplyFailed;
    }
    mComp = comp;
    mArmed = true;
    rewatchLocked();
    tryBindLocked();
    wakeMonitor();
    if (timeoutMs < 0) timeoutMs = 0;
    mCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [&] { return mApplied || gen != mGeneration || mStop; });
    if (gen != mGeneration) return Result::kError;
    if (!mApplied) {
        mLog(1, "setFunctions: not bound within " + std::to_string(timeoutMs) +
                        " ms (FunctionFS descriptors missing); the monitor keeps waiting");
        return Result::kError;
    }
    return Result::kSuccess;
}

bool Controller::reset() {
    std::unique_lock<std::mutex> lk(mLock);
    if (!mArmed || !mApplied) return false;
    const unsigned gen = mGeneration;
    if (!mGadget->unbind()) return false;
    mApplied = false;
    lk.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(mDisconnectWaitMs ? mDisconnectWaitMs : 1));
    lk.lock();
    if (gen != mGeneration) return true;  // a newer request owns the gadget now
    tryBindLocked();
    wakeMonitor();
    return true;
}

uint64_t Controller::currentFunctions() const {
    std::lock_guard<std::mutex> lk(mLock);
    return mFunctions;
}

bool Controller::applied() const {
    std::lock_guard<std::mutex> lk(mLock);
    return mApplied;
}

unsigned Controller::bindCount() const {
    std::lock_guard<std::mutex> lk(mLock);
    return mBinds;
}

unsigned Controller::adoptCount() const {
    std::lock_guard<std::mutex> lk(mLock);
    return mAdopts;
}

}  // namespace usb
}  // namespace a6l
