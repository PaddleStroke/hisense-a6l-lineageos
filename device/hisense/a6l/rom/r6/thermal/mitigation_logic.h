// SPDX-License-Identifier: Apache-2.0
// A6L thermal HAL: stock-like skin mitigation (thermal-20261012, firmware/extracted/thermal-20261012/README.md).
// Pure C++ (no Android deps), host-tested by tests/test_mitigation.cpp, shared by the HAL (Thermal.cpp poll loop) and
// the live-test daemon a6l-thermal-mitigd (firmware/extracted/thermal-20261012/mitigd).
//
// Stock (firmware/extracted/vendor/etc/thermal-engine.conf) throttles on the PM660 quiet_therm thermistor only, with
// Qualcomm "ss" (single step) sections sampled every 2 s:
//   SS-QUIET-THERM-GOLD    set 45 C / clear 40 C -> gold cluster,   first cap 1747200 kHz
//   SS-QUIET-THERM-SILVER  set 47 C / clear 40 C -> silver cluster, first cap 1536000 kHz
//   SS-QUIET-THERM-GPU     set 47 C / clear 40 C -> GPU,            first cap 588 MHz
//   SS-GPU                 tsens gpu set 95 C / clear 65 C -> GPU,  first cap 588 MHz
// The mainline kernel has no equivalent (its DT cooling-maps act on the die sensors at 70 C passive). This module
// reproduces the ss behaviour on the userspace max-frequency knobs: cpufreq scaling_max_freq (A6L a6l_cpufreq_osm) and
// devfreq max_freq (Adreno 512, msm). Both are QoS max requests: the kernel's own thermal caps still apply on top.
//
// Controller per rule (one sample per poll):
//   idle    -> active  when T >= set: cap = the highest table step <= device_max_limit (stock first step)
//   active, T >= set   step one table step DOWN when the temperature is still rising since the last change
//                      (T >= Tref + kRise) or stays clearly above the set point (T >= set + kAbove), at most once per
//                      kDownDwell samples (thermal lag of the skin: no run-away to the floor on one slow ramp)
//   active, clr<T<set  step one step UP (never above the first cap) when the temperature is below the set point by
//                      kBelow and not rising, at most once per kUpDwell samples
//   active, T <= clr   release (cap = hardware maximum), as stock set_point_clr
//   T unreadable       hold
// Several rules on one device: the lowest cap wins (as thermal-engine).
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace a6l::thermal {

struct MitigationRule {
    std::string name;     // stock section name, e.g. SS-QUIET-THERM-GOLD
    std::string sensor;   // thermal-a6l.conf sensor name (e.g. "skin", "gpu")
    std::string device;   // cpu-big | cpu-little | cpu:policyN | gpu | devfreq:<name>
    float set = 0;        // degC
    float clear = 0;      // degC, < set
    long maxLimit = 0;    // first cap, device units (kHz cpufreq, Hz devfreq)
    long floor = 0;       // lowest cap this rule imposes (device units); 0 = the lowest table step
};

// Config: one rule per line, '#' comments.
//   <name> <sensor> <device> <set degC> <clear degC> <device_max_limit> <floor|->
bool parseMitigation(const std::string& text, std::vector<MitigationRule>* out, std::string* err);

struct FreqDevice {
    std::string name;          // policyN | <devfreq name>
    std::string maxPath;       // scaling_max_freq | max_freq
    std::vector<long> freqs;   // ascending, unique
    long hwMax = 0;            // cpuinfo_max_freq (cpufreq) | highest available (devfreq)
    bool devfreq = false;
};

// <sysRoot>/devices/system/cpu/cpufreq/policyN (needs cpuinfo_max_freq + scaling_available_frequencies + scaling_max_freq)
std::vector<FreqDevice> scanCpufreq(const std::string& sysRoot);
// <sysRoot>/class/devfreq/<name> (needs available_frequencies + max_freq)
std::vector<FreqDevice> scanDevfreq(const std::string& sysRoot);
// device spec -> device; false when absent on this boot. cpu-big/cpu-little need >= 2 policies (distinct cpuinfo_max_freq).
bool resolveDevice(const std::string& spec, const std::vector<FreqDevice>& cpus, const std::vector<FreqDevice>& devfreqs,
                   FreqDevice* out);

class SsController {
  public:
    static constexpr float kRise = 0.2f;    // degC rise since the last change that counts as "still heating"
    static constexpr float kAbove = 1.0f;   // degC above the set point that steps down even without a rise
    static constexpr float kBelow = 1.0f;   // degC below the set point before stepping back up
    static constexpr int kDownDwell = 3;    // samples between two steps down (6 s at 2 s)
    static constexpr int kUpDwell = 5;      // samples between two steps up (10 s at 2 s)

    explicit SsController(MitigationRule r) : r_(std::move(r)) {}
    // t in degC (NAN = unreadable). Returns this rule's cap for the device (dev.hwMax = no mitigation).
    long update(float t, const FreqDevice& dev);
    bool active() const { return active_; }
    const MitigationRule& rule() const { return r_; }
    void reset() { active_ = false; idx_ = -1; dwell_ = 0; }

  private:
    MitigationRule r_;
    bool active_ = false;
    int idx_ = -1;     // index of the current cap in dev.freqs
    float ref_ = 0;    // temperature at the last change
    int dwell_ = 0;    // samples since the last change
};

// Rules + devices + sysfs writes. Not thread-safe (one caller: the HAL poll loop or the daemon loop).
class Mitigator {
  public:
    struct Change {
        std::string device;  // FreqDevice::name
        std::string spec;    // device spec of the first rule on it (cpu-big ...)
        long from, to, hwMax;
        int level;           // table steps below hwMax (0 = not mitigated)
        bool ok;             // write succeeded
        std::string why;     // rule(s) and temperature(s)
    };
    // claim = owner mode (HAL): the first time a device is found, a value below hwMax left in its knob (e.g. by a HAL
    // instance that crashed while capping) is reset to hwMax. Off for the live-test daemon (it never resets a knob it did
    // not write; it releases its own caps on SIGTERM/SIGINT).
    Mitigator(std::vector<MitigationRule> rules, std::string sysRoot, bool dryRun = false, bool claim = false);
    // re-resolve devices (cpufreq / msm.ko may load after the caller starts); keeps controller state of known devices
    void rescan();
    // one sample: temp(sensorName) -> degC or NAN. Returns the cap changes written (or attempted) this sample.
    std::vector<Change> step(const std::function<float(const std::string&)>& temp);
    // write hwMax to every device this instance capped (exit / stop)
    std::vector<Change> releaseAll();
    // per device spec: current level (steps below hwMax), for reporting (CoolingDevice); absent devices are omitted
    std::map<std::string, int> levels() const;
    const std::vector<MitigationRule>& rules() const { return rules_; }

  private:
    struct Dev {
        FreqDevice dev;
        std::string spec;
        long applied = -1;     // last value written successfully (-1 = never written)
        long wanted = -1;      // last computed cap
        int failures = 0;
        int sinceWrite = 0;    // samples since the last (re)write while capped
    };
    bool write(Dev& d, long v);
    std::vector<MitigationRule> rules_;
    std::vector<SsController> ctl_;
    std::vector<int> devOf_;   // rule index -> index in devs_ (-1 absent)
    std::vector<Dev> devs_;
    std::string sysRoot_;
    bool dryRun_;
    bool claim_;
};

}  // namespace a6l::thermal
