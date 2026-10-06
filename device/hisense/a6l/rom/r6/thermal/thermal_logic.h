// SPDX-License-Identifier: Apache-2.0
// A6L thermal HAL core (completeness audit r6 prep, 29 Sep 2026). Pure C++ (no Android deps): config parsing, sysfs zone
// resolution, temperature reads and throttling-severity computation. Host-tested by tests/test_thermal_logic.cpp.
#pragma once
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace a6l::thermal {

// Values match android.hardware.thermal TemperatureType / ThrottlingSeverity (AIDL V3).
enum Type { CPU = 0, GPU = 1, BATTERY = 2, SKIN = 3, USB_PORT = 4, POWER_AMPLIFIER = 5, MODEM = 12, SOC = 13,
            AMBIENT = 18 };
constexpr int kSeverities = 7;  // NONE, LIGHT, MODERATE, SEVERE, CRITICAL, EMERGENCY, SHUTDOWN

// One way to read a sensor. thermal-r5prep (29 Sep 2026): a sensor line may list alternatives separated by '|', the
// first one present on this boot wins (e.g. skin = PM660 quiet_therm over IIO, else the fuel-gauge battery temperature).
struct Source {
    enum Kind { ZONE, FILE, IIO } kind = ZONE;
    std::vector<std::string> names;   // ZONE: thermal zone types; IIO: channel labels; FILE: one absolute path
    float scale = NAN;                // NAN = the line's scale column
};

struct Sensor {
    std::string name;                 // reported Temperature.name
    int type = -1;                    // Type
    std::vector<Source> sources;      // alternatives, in order
    std::vector<std::string> zones;   // = the first ZONE source's names (kept for callers/tests of the r6 original)
    std::string file;                 // = the first FILE source's path
    float scale = 0.001f;             // raw value * scale = degC (the line's scale column)
    float resolvedScale = 0.001f;     // set by resolve(): scale of the source that matched (its @scale, else `scale`)
    float hot[kSeverities];           // hotThrottlingThresholds, index = severity; NAN = unset (index 0 always NAN)
    std::string path;                 // resolved temperature file ("" = sensor absent on this boot)
    Sensor() { for (float& h : hot) h = NAN; }
};

// Config: one sensor per line, '#' comments.
//   <name> <CPU|GPU|BATTERY|SKIN|USB_PORT|POWER_AMPLIFIER|MODEM|SOC|AMBIENT> <source>[|<source>...] <scale> <t1> .. <t6>
//   source = zone:a,b,... | file:/abs/path | iio:label,label2,...   each optionally suffixed @<scale> (overrides the column)
// t1..t6 = LIGHT..SHUTDOWN in degC, '-' = unset. Thresholds that are set must be strictly increasing.
bool parseConfig(const std::string& text, std::vector<Sensor>* out, std::string* err);

// thermal zone type -> ".../thermal_zoneN/temp" under <sysRoot>/class/thermal
std::map<std::string, std::string> scanZones(const std::string& sysRoot);

// IIO channel label -> processed temperature file, from <sysRoot>/bus/iio/devices/iio:device*/: both the non-indexed
// "in_temp_<label>_input" (qcom-spmi-adc5 sets extend_name = DT label) and indexed "in_tempN_input" + "in_tempN_label".
std::map<std::string, std::string> scanIio(const std::string& sysRoot);

// fills Sensor::path/resolvedScale from the first source present: zone map (exact type, or type + "-thermal" / minus "-thermal"),
// iio map, or a file (a FILE source that is not the last alternative must exist; the last one is taken unchecked, as in
// the r6 original, so a power_supply that registers after the HAL starts is still read)
void resolve(std::vector<Sensor>* sensors, const std::map<std::string, std::string>& zones,
             const std::map<std::string, std::string>& iio = {});

// reads an integer sysfs value and applies scale; false on error / non-numeric content
bool readTemp(const std::string& path, float scale, float* out);

// throttling severity with hysteresis: rising uses the thresholds as is, a severity is only left when the temperature
// has fallen `hyst` degC below its threshold. prev = previous severity (0 on first read).
int severityFor(float t, const float hot[kSeverities], int prev, float hyst);

}  // namespace a6l::thermal
