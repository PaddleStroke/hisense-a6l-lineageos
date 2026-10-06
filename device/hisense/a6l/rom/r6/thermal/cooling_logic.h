// SPDX-License-Identifier: Apache-2.0
// A6L thermal HAL: kernel cooling devices (thermal-r5prep, 29 Sep 2026). Pure C++, host-tested (tests/test_cooling_logic.cpp).
// The HAL only REPORTS cooling devices (IThermal.getCoolingDevices + ICoolingDeviceChangedCallback): the kernel thermal
// governors (step_wise, DT cooling-maps) own the actual throttling. Which devices exist depends on the kernel:
//   cpufreq-cpuN   CPU_FREQ_THERMAL, one per cpufreq policy: only once cpufreq is enabled on the A6L (H52; r5 has none)
//   devfreq-*gpu*  DEVFREQ_THERMAL for the Adreno 512 (msm.ko GPU; the DT gpu-thermal cooling-map points at it)
//   <psy name>     power_supply cooling (psy with CHARGE_CONTROL_LIMIT[_MAX]); qcom_smbx/pmi8998_fg have none in r5
#pragma once
#include <string>
#include <vector>

namespace a6l::thermal {

// Values match android.hardware.thermal CoolingType (AIDL V3).
enum CoolingKind { C_FAN = 0, C_BATTERY = 1, C_CPU = 2, C_GPU = 3, C_MODEM = 4, C_COMPONENT = 6, C_POWER_AMPLIFIER = 8,
                   C_DISPLAY = 9, C_WIFI = 11, C_CAMERA = 12, C_FLASHLIGHT = 13, C_USB_PORT = 14 };

struct Cooling {
    std::string name;       // reported CoolingDevice.name (kernel cooling_device type, unchanged)
    int type = C_COMPONENT; // CoolingKind
    std::string curPath;    // .../cooling_deviceN/cur_state
    long maxState = -1;     // max_state at scan time (-1 unreadable)
};

// kernel cooling_device "type" string -> CoolingKind
int coolingKindFor(const std::string& kernelType);

// all <sysRoot>/class/thermal/cooling_deviceN, sorted by N; entries without a readable type are skipped
std::vector<Cooling> scanCooling(const std::string& sysRoot);

// integer sysfs read (cur_state); false on error
bool readState(const std::string& path, long* out);

}  // namespace a6l::thermal
