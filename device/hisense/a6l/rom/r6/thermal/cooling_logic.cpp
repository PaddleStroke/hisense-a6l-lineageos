// SPDX-License-Identifier: Apache-2.0
#include "cooling_logic.h"

#include <dirent.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace a6l::thermal {

static bool line(const std::string& path, std::string* v) {
    std::ifstream f(path);
    if (!f || !std::getline(f, *v)) return false;
    while (!v->empty() && (v->back() == '\n' || v->back() == '\r' || v->back() == ' ')) v->pop_back();
    return true;
}

static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

int coolingKindFor(const std::string& t) {
    // mainline names: "cpufreq-cpu0" (cpufreq_cooling, per policy), older "thermal-cpufreq-0"
    if (t.rfind("cpufreq-", 0) == 0 || t.rfind("thermal-cpufreq", 0) == 0 || t == "Processor") return C_CPU;
    // devfreq_cooling: "devfreq-<dev_name>", the A6L GPU is 5000000.gpu (sdm630.dtsi adreno_gpu)
    if (has(t, "gpu") || has(t, "kgsl") || t == "devfreq") return C_GPU;  // bare "devfreq" = pre-5.x naming, GPU only
    if (t.rfind("devfreq", 0) == 0) return C_COMPONENT;                      // bus/other devfreq
    // power_supply cooling registers under the psy name (qcom-battery, pmi8998_charger, qcom-smbx-charger, battery, ...)
    if (has(t, "battery") || has(t, "charger") || has(t, "smb") || has(t, "bms") || has(t, "usb")) return C_BATTERY;
    // qcom QMI cooling (modem TMD, not built for the A6L today) and display backlight cooling
    if (has(t, "modem")) return C_MODEM;
    if (t == "pa" || t.rfind("pa_", 0) == 0) return C_POWER_AMPLIFIER;
    if (has(t, "backlight") || has(t, "panel") || has(t, "display")) return C_DISPLAY;
    if (has(t, "wlan") || has(t, "wifi")) return C_WIFI;
    if (has(t, "flash") || has(t, "torch")) return C_FLASHLIGHT;
    if (has(t, "fan")) return C_FAN;
    return C_COMPONENT;
}

bool readState(const std::string& path, long* out) {
    std::string v;
    if (!line(path, &v) || v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long r = std::strtol(v.c_str(), &end, 10);
    if (errno || end == v.c_str() || *end) return false;
    *out = r;
    return true;
}

std::vector<Cooling> scanCooling(const std::string& sysRoot) {
    std::vector<std::pair<int, Cooling>> found;
    const std::string base = sysRoot + "/class/thermal";
    DIR* d = opendir(base.c_str());
    if (!d) return {};
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        const std::string pre = "cooling_device";
        if (n.rfind(pre, 0) != 0 || n.size() == pre.size() ||
            n.find_first_not_of("0123456789", pre.size()) != std::string::npos)
            continue;
        Cooling c;
        if (!line(base + "/" + n + "/type", &c.name) || c.name.empty()) continue;
        c.type = coolingKindFor(c.name);
        c.curPath = base + "/" + n + "/cur_state";
        if (!readState(base + "/" + n + "/max_state", &c.maxState)) c.maxState = -1;
        found.emplace_back(std::atoi(n.c_str() + pre.size()), c);
    }
    closedir(d);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Cooling> out;
    for (auto& f : found) {
        // two devices with one type (e.g. two "devfreq" on old kernels): make the reported names unique
        std::string nm = f.second.name;
        int k = 1;
        while (std::any_of(out.begin(), out.end(), [&](const Cooling& o) { return o.name == nm; }))
            nm = f.second.name + "-" + std::to_string(k++);
        f.second.name = nm;
        out.push_back(f.second);
    }
    return out;
}

}  // namespace a6l::thermal
