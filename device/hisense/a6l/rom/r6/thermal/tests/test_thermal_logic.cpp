// SPDX-License-Identifier: Apache-2.0
// Host tests for rom/r6/thermal/thermal_logic.cpp: g++ -std=c++17 (tests/run-tests.sh). Prints A6L_THERMAL_TEST PASS|FAIL.
#include "../thermal_logic.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>

using namespace a6l::thermal;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)

static void put(const std::string& p, const std::string& v) { std::ofstream(p) << v; }

static std::string slurp(const char* p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); }

int main(int argc, char** argv) {
    // ---- the shipped config parses and has what the framework needs (SKIN + BATTERY with a SHUTDOWN trip)
    std::vector<Sensor> s;
    std::string err;
    std::string conf = slurp(argc > 1 ? argv[1] : "../thermal-a6l.conf");
    CHECK(parseConfig(conf, &s, &err));
    if (!err.empty()) std::cerr << err << "\n";
    int skin = -1, batt = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].type == SKIN) skin = i;
        if (s[i].type == BATTERY) batt = i;
    }
    CHECK(skin >= 0 && batt >= 0);
    CHECK(batt >= 0 && s[batt].hot[6] == 60.0f && s[batt].scale == 0.1f);
    CHECK(skin >= 0 && std::isnan(s[skin].hot[0]) && std::isfinite(s[skin].hot[1]));
    CHECK(s.size() >= 11);  // config-agnostic: the sensor set grows (pm660 zones, IIO thermistors)

    // ---- parser rejects
    CHECK(!parseConfig("a CPU zone:x 0.001 1 2 3\n", &s, &err));                      // field count
    CHECK(!parseConfig("a FOO zone:x 0.001 - - - - - -\n", &s, &err));                  // type
    CHECK(!parseConfig("a CPU zone:x 0.001 50 40 - - - -\n", &s, &err));                // not increasing
    CHECK(!parseConfig("a CPU zone:x 0.001 50 50 - - - -\n", &s, &err));                // equal
    CHECK(!parseConfig("a CPU path:x 0.001 - - - - - -\n", &s, &err));                  // source
    CHECK(!parseConfig("a CPU file:rel 0.001 - - - - - -\n", &s, &err));                // relative file
    CHECK(!parseConfig("a CPU zone:x 0 - - - - - -\n", &s, &err));                      // scale
    CHECK(!parseConfig("a CPU zone:x 0.001 4x - - - - -\n", &s, &err));                 // number
    CHECK(!parseConfig("a CPU zone:x 0.001 - - - - - -\na GPU zone:y 0.001 - - - - - -\n", &s, &err));  // duplicate
    CHECK(!parseConfig("# only comments\n\n", &s, &err));
    CHECK(err.find("no sensors") != std::string::npos);
    CHECK(parseConfig("a CPU zone:x,y 0.001 - 60 - - - 90 # tail comment\n", &s, &err));
    CHECK(s.size() == 1 && s[0].zones.size() == 2 && std::isnan(s[0].hot[1]) && s[0].hot[2] == 60 && s[0].hot[6] == 90);

    // ---- fake sysfs: zone scan, suffix tolerance, duplicate types, file fallback, absent sensor
    char tmpl[] = "/tmp/a6l-thermal-XXXXXX";
    std::string root = mkdtemp(tmpl);
    mkdir((root + "/class").c_str(), 0755);
    mkdir((root + "/class/thermal").c_str(), 0755);
    auto zone = [&](int n, const std::string& type, const std::string& temp) {
        std::string z = root + "/class/thermal/thermal_zone" + std::to_string(n);
        mkdir(z.c_str(), 0755);
        put(z + "/type", type + "\n");
        put(z + "/temp", temp + "\n");
    };
    zone(3, "cpu0-thermal", "45000");
    zone(12, "gpu", "51234");
    zone(10, "dup", "1000");
    zone(2, "dup", "2000");
    put(root + "/batt", "305\n");
    auto zones = scanZones(root);
    CHECK(zones.size() == 3);
    CHECK(zones["dup"] == root + "/class/thermal/thermal_zone2/temp");
    CHECK(parseConfig("c CPU zone:cpu0 0.001 - - - - - -\n"
                      "g GPU zone:gpu-thermal 0.001 - - - - - -\n"
                      "b BATTERY file:" + root + "/batt 0.1 - - - - - -\n"
                      "z BATTERY zone:nothere file:/x 0.1 - - - - - -\n", &s, &err) == false);  // 11 fields
    CHECK(parseConfig("c CPU zone:cpu0 0.001 - - - - - -\n"
                      "g GPU zone:gpu-thermal 0.001 - - - - - -\n"
                      "b BATTERY file:" + root + "/batt 0.1 - - - - - -\n"
                      "n SOC zone:nothere 0.001 - - - - - -\n"
                      "p CPU zone:nothere,cpu0-thermal 0.001 - - - - - -\n", &s, &err));
    resolve(&s, zones);
    CHECK(s[0].path == root + "/class/thermal/thermal_zone3/temp");   // cpu0 -> cpu0-thermal
    CHECK(s[1].path == root + "/class/thermal/thermal_zone12/temp");  // gpu-thermal -> gpu
    CHECK(s[2].path == root + "/batt");
    CHECK(s[3].path.empty());
    CHECK(s[4].path == s[0].path);                                     // second candidate
    float t = 0;
    CHECK(readTemp(s[0].path, s[0].scale, &t) && std::fabs(t - 45.0f) < 1e-3);
    CHECK(readTemp(s[1].path, s[1].scale, &t) && std::fabs(t - 51.234f) < 1e-3);
    CHECK(readTemp(s[2].path, s[2].scale, &t) && std::fabs(t - 30.5f) < 1e-3);
    put(root + "/neg", "-52\n");
    CHECK(readTemp(root + "/neg", 0.1f, &t) && std::fabs(t + 5.2f) < 1e-3);  // cold battery
    put(root + "/bad", "abc\n");
    CHECK(!readTemp(root + "/bad", 1, &t));
    put(root + "/empty", "");
    CHECK(!readTemp(root + "/empty", 1, &t));
    CHECK(!readTemp(root + "/missing", 1, &t));
    CHECK(scanZones(root + "/nonexistent").empty());

    // ---- severity + hysteresis
    float h[kSeverities] = {NAN, 40, 45, NAN, 50, 55, 60};
    CHECK(severityFor(30, h, 0, 2) == 0);
    CHECK(severityFor(40, h, 0, 2) == 1);
    CHECK(severityFor(47, h, 0, 2) == 2);   // 45 MODERATE (SEVERE unset)
    CHECK(severityFor(52, h, 0, 2) == 4);
    CHECK(severityFor(61, h, 0, 2) == 6);
    CHECK(severityFor(49, h, 4, 2) == 4);   // held: 49 >= 50-2
    CHECK(severityFor(47.9f, h, 4, 2) == 2);  // falls to MODERATE (45 <= 47.9)
    CHECK(severityFor(43.5f, h, 2, 2) == 2);  // held at MODERATE: 43.5 >= 45-2
    CHECK(severityFor(42.9f, h, 2, 2) == 1);  // back to LIGHT
    CHECK(severityFor(10, h, 6, 2) == 0);
    CHECK(severityFor(NAN, h, 3, 2) == 3);    // unreadable: keep
    float none[kSeverities] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN};
    CHECK(severityFor(200, none, 0, 2) == 0);

    std::string cmd = "rm -rf '" + root + "'";
    if (system(cmd.c_str())) {}
    std::cout << "checks " << checks << " fails " << fails << "\nA6L_THERMAL_TEST " << (fails ? "FAIL" : "PASS") << "\n";
    return fails ? 1 : 0;
}
