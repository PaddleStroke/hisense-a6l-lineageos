// SPDX-License-Identifier: Apache-2.0
// Host tests (thermal-r5prep, 29 Sep 2026): source alternatives / per-source scale / IIO scan in thermal_logic.cpp and the
// cooling-device scan in cooling_logic.cpp. Built by tests/run-tests.sh. Prints A6L_THERMAL_SRC_TEST PASS|FAIL.
#include "../cooling_logic.h"
#include "../thermal_logic.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace a6l::thermal;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)

static void put(const std::string& p, const std::string& v) { std::ofstream(p) << v; }
static void md(const std::string& p) { mkdir(p.c_str(), 0755); }
static std::string slurp(const char* p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); }

int main(int argc, char** argv) {
    char tmpl[] = "/tmp/a6l-thermal-src-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    md(root + "/class"); md(root + "/class/thermal"); md(root + "/bus"); md(root + "/bus/iio"); md(root + "/bus/iio/devices");
    auto zone = [&](int n, const std::string& type, const std::string& temp) {
        std::string z = root + "/class/thermal/thermal_zone" + std::to_string(n);
        md(z); put(z + "/type", type + "\n"); put(z + "/temp", temp + "\n");
    };
    // r5 DT names (thermal_of registers the node name, "-thermal" included)
    zone(0, "aoss-thermal", "31000"); zone(9, "pm660l-thermal", "37000"); zone(11, "qcom-battery", "25000");
    // PM660 adc5 (non-indexed, extend_name = DT label) + an indexed ADC with *_label files
    std::string a = root + "/bus/iio/devices/iio:device0";
    md(a);
    put(a + "/in_temp_quiet_therm_input", "33512\n");
    put(a + "/in_temp_xo_therm_input", "30125\n");
    put(a + "/in_voltage_vph_pwr_input", "3900000\n");  // not a temperature
    std::string b = root + "/bus/iio/devices/iio:device1";
    md(b);
    put(b + "/in_temp3_label", "skin_therm\n"); put(b + "/in_temp3_input", "40000\n");
    put(b + "/in_temp4_label", "no_input\n");                                   // label without input: ignored
    put(b + "/in_tempx_label", "bogus\n");                                       // non-numeric index: ignored
    put(b + "/in_temp_quiet_therm_input", "99000\n");                            // duplicate label: device0 wins
    put(root + "/batt", "305\n");

    auto iio = scanIio(root);
    CHECK(iio.size() == 3);
    CHECK(iio["quiet_therm"] == a + "/in_temp_quiet_therm_input");
    CHECK(iio["xo_therm"] == a + "/in_temp_xo_therm_input");
    CHECK(iio["skin_therm"] == b + "/in_temp3_input");
    CHECK(!iio.count("no_input") && !iio.count("bogus") && !iio.count("vph_pwr"));
    CHECK(scanIio(root + "/none").empty());

    std::vector<Sensor> s;
    std::string err;
    // alternatives + per-source scale; '@' inside a sysfs path is not a scale
    CHECK(parseConfig("skin SKIN iio:quiet_therm@0.001|file:" + root + "/batt 0.1 41 45 47 50 53 58\n"
                      "skin2 SKIN iio:absent@0.001|file:" + root + "/batt 0.1 - - - - - -\n"
                      "pm SOC zone:pm660-thermal|zone:pm660l 0.001 - - 95 105 115 -\n"
                      "at BATTERY file:/sys/devices/x/pmic@0:battery@4000/temp 0.1 - - - - - -\n"
                      "none SOC iio:nothing 0.001 - - - - - -\n"
                      "mid SOC file:/definitely/absent|zone:aoss 0.001 - - - - - -\n"
                      "psy BATTERY zone:qcom-battery|file:/x 0.001 - - - - - -\n", &s, &err));
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(s.size() == 7);
    CHECK(s[0].sources.size() == 2 && s[0].sources[0].kind == Source::IIO && s[0].sources[0].scale == 0.001f);
    CHECK(std::isnan(s[0].sources[1].scale) && s[0].file == root + "/batt");
    CHECK(s[3].file == "/sys/devices/x/pmic@0:battery@4000/temp" && std::isnan(s[3].sources[0].scale));
    CHECK(s[2].zones.size() == 1 && s[2].zones[0] == "pm660-thermal");
    resolve(&s, scanZones(root), iio);
    float t = 0;
    CHECK(s[0].path == a + "/in_temp_quiet_therm_input" && s[0].resolvedScale == 0.001f);
    CHECK(readTemp(s[0].path, s[0].resolvedScale, &t) && std::fabs(t - 33.512f) < 1e-3);
    CHECK(s[1].path == root + "/batt" && s[1].resolvedScale == 0.1f);          // IIO absent -> battery fallback
    CHECK(readTemp(s[1].path, s[1].resolvedScale, &t) && std::fabs(t - 30.5f) < 1e-3);
    CHECK(s[2].path == root + "/class/thermal/thermal_zone9/temp");            // second alternative (pm660l)
    CHECK(s[3].path == "/sys/devices/x/pmic@0:battery@4000/temp");            // last FILE taken unchecked
    CHECK(s[4].path.empty());
    CHECK(s[5].path == root + "/class/thermal/thermal_zone0/temp");            // absent non-last FILE skipped
    CHECK(s[6].path == root + "/class/thermal/thermal_zone11/temp");           // psy tripless zone
    // late module load: quiet_therm disappears -> re-resolve falls back; comes back -> IIO again
    auto iio2 = iio; iio2.erase("quiet_therm");
    resolve(&s, scanZones(root), iio2);
    CHECK(s[0].path == root + "/batt" && s[0].resolvedScale == 0.1f);
    resolve(&s, scanZones(root), iio);
    CHECK(s[0].path == a + "/in_temp_quiet_therm_input" && s[0].resolvedScale == 0.001f);
    // parser rejects
    CHECK(!parseConfig("x SOC iio: 0.001 - - - - - -\n", &s, &err));
    CHECK(!parseConfig("x SOC iio:a@0 0.001 - - - - - -\n", &s, &err));
    CHECK(!parseConfig("x SOC iio:a@-1 0.001 - - - - - -\n", &s, &err));
    CHECK(!parseConfig("x SOC zone:a|bad:b 0.001 - - - - - -\n", &s, &err));
    CHECK(!parseConfig("x SOC | 0.001 - - - - - -\n", &s, &err));
    CHECK(!parseConfig("x NOPE iio:a 0.001 - - - - - -\n", &s, &err));
    CHECK(parseConfig("x AMBIENT iio:a 0.001 - - - - - -\ny POWER_AMPLIFIER iio:b 0.001 - - - - - -\n"
                      "z MODEM zone:m 0.001 - - - - - -\n", &s, &err) && s[0].type == 18 && s[1].type == 5 && s[2].type == 12);

    // shipped config: skin prefers quiet_therm, falls back to the battery; every IIO label is one the r5 DT declares
    std::string conf = slurp(argc > 1 ? argv[1] : "../thermal-a6l.conf");
    CHECK(parseConfig(conf, &s, &err));
    int skins = 0;
    const std::string dtLabels = " ref_gnd vref_1p25 die_temp xo_therm msm_therm emmc_therm pa_therm0 pa_therm1 quiet_therm "
                                 "vph_pwr vcoin "   // pm660_adc channel labels, /home/a6l/rom-v2/kit-r5/rom-v2.dts
                                 "epd_therm ";      // + stock label of channel 0x50 (stock vadc chan@50; r5 DT: pa_therm1)
    for (const Sensor& x : s) {
        if (x.type == SKIN) {
            ++skins;
            CHECK(x.sources.size() == 2 && x.sources[0].kind == Source::IIO && x.sources[0].names[0] == "quiet_therm");
            CHECK(x.sources[1].kind == Source::FILE && x.sources[1].names[0] == "/sys/class/power_supply/qcom-battery/temp");
            CHECK(x.sources[0].scale == 0.001f && x.scale == 0.1f);
        }
        for (const Source& src : x.sources)
            if (src.kind == Source::IIO)
                for (const std::string& l : src.names) CHECK(dtLabels.find(" " + l + " ") != std::string::npos);
    }
    CHECK(skins == 1);

    // ---- thermal follow-up (29 Sep 2026): the shipped config against a simulated PM660 adc5 (qcom-spmi-adc5.ko now in
    // misc.txt): non-indexed in_temp_<DT label>_input for every r5 DT channel, before and after the module loads.
    {
        const std::string r2 = root + "/adc5";
        md(r2); md(r2 + "/class"); md(r2 + "/class/thermal"); md(r2 + "/bus"); md(r2 + "/bus/iio");
        md(r2 + "/bus/iio/devices");
        auto z2 = [&](int n, const std::string& type, const std::string& temp) {
            std::string z = r2 + "/class/thermal/thermal_zone" + std::to_string(n);
            md(z); put(z + "/type", type + "\n"); put(z + "/temp", temp + "\n");
        };
        const char* tz[] = {"aoss-thermal", "cpuss0-thermal", "cpuss1-thermal", "cpu0-thermal", "cpu1-thermal", "cpu2-thermal",
                            "cpu3-thermal", "pwr-cluster-thermal", "gpu-thermal", "pm660l-thermal"};
        for (int i = 0; i < 10; ++i) z2(i, tz[i], "40000");
        std::vector<Sensor> c2;
        CHECK(parseConfig(conf, &c2, &err));
        auto find = [&](const std::string& n) -> const Sensor* {
            for (const Sensor& x : c2) if (x.name == n) return &x;
            return nullptr;
        };
        // before adc5: skin = battery proxy, thermistors and pm660 absent (the HAL reports only what exists)
        resolve(&c2, scanZones(r2), scanIio(r2));
        const Sensor* sk = find("skin");
        CHECK(sk && sk->path == "/sys/class/power_supply/qcom-battery/temp" && sk->resolvedScale == 0.1f);
        for (const char* n : {"xo-therm", "msm-therm", "emmc-therm", "pa-therm0", "epd-therm", "pm660"})
            CHECK(find(n) && find(n)->path.empty());
        // adc5 loaded: iio:device0 = pm660 adc@3100 with the r5 DT labels; pm660-thermal (die_temp consumer) registers
        const std::string d = r2 + "/bus/iio/devices/iio:device0";
        md(d);
        const char* temps[] = {"die_temp", "xo_therm", "msm_therm", "emmc_therm", "pa_therm0", "pa_therm1", "quiet_therm"};
        for (int i = 0; i < 7; ++i) put(d + "/in_temp_" + temps[i] + "_input", std::to_string(25000 + 1000 * i) + "\n");
        for (const char* v : {"ref_gnd", "vref_1p25", "vph_pwr", "vcoin"}) put(d + "/in_voltage_" + std::string(v) + "_input", "1\n");
        z2(10, "pm660-thermal", "41500");
        auto io = scanIio(r2);
        CHECK(io.size() == 7 && !io.count("vph_pwr"));
        resolve(&c2, scanZones(r2), io);
        CHECK(sk->path == d + "/in_temp_quiet_therm_input" && sk->resolvedScale == 0.001f);
        CHECK(readTemp(sk->path, sk->resolvedScale, &t) && std::fabs(t - 31.0f) < 1e-3);
        CHECK(find("xo-therm")->path == d + "/in_temp_xo_therm_input");
        CHECK(find("msm-therm")->path == d + "/in_temp_msm_therm_input");
        CHECK(find("emmc-therm")->path == d + "/in_temp_emmc_therm_input");
        CHECK(find("pa-therm0")->path == d + "/in_temp_pa_therm0_input" && find("pa-therm0")->type == POWER_AMPLIFIER);
        CHECK(find("epd-therm")->path == d + "/in_temp_pa_therm1_input");       // r5 DT label for channel 0x50
        CHECK(readTemp(find("epd-therm")->path, find("epd-therm")->resolvedScale, &t) && std::fabs(t - 30.0f) < 1e-3);
        CHECK(find("pm660") && find("pm660")->path == r2 + "/class/thermal/thermal_zone10/temp");
        CHECK(readTemp(find("pm660")->path, find("pm660")->resolvedScale, &t) && std::fabs(t - 41.5f) < 1e-3);
        // a DT relabelled to the stock name wins over the mainline one
        put(d + "/in_temp_epd_therm_input", "29000\n");
        resolve(&c2, scanZones(r2), scanIio(r2));
        CHECK(find("epd-therm")->path == d + "/in_temp_epd_therm_input");
        // skin severity from quiet_therm (stock gold throttle 45 -> MODERATE, 58 -> SHUTDOWN)
        CHECK(severityFor(45.0f, sk->hot, 0, 2.0f) == 2 && severityFor(58.0f, sk->hot, 0, 2.0f) == 6);
    }

    // ---- safe shutdown path: no cpufreq cooling (no DT cooling-maps) and, on r5, a kernel critical trip that does not
    // power off -> every CPU/GPU sensor has a SHUTDOWN threshold below the kernel critical trip (110 C), so Android's
    // ThermalManagerService (shuts down on SHUTDOWN for CPU/GPU/SKIN/BATTERY) acts first; thresholds strictly increase.
    {
        int cpu = 0, gpu = 0;
        for (const Sensor& x : s) {
            if (x.type != CPU && x.type != GPU && x.type != SKIN && x.type != BATTERY) continue;
            CHECK(!std::isnan(x.hot[6]));
            if (x.type == CPU || x.type == GPU) {
                CHECK(x.hot[6] < 110.0f && x.hot[6] >= 100.0f);
                CHECK(severityFor(x.hot[6], x.hot, 0, 2.0f) == 6);
                CHECK(severityFor(x.hot[6] - 0.5f, x.hot, 0, 2.0f) == 5);
                CHECK(severityFor(x.hot[6] - 1.0f, x.hot, 6, 2.0f) == 6);       // hysteresis: stays SHUTDOWN
                ++(x.type == CPU ? cpu : gpu);
            }
            float last = -1000;
            for (int k = 1; k < kSeverities; ++k)
                if (!std::isnan(x.hot[k])) { CHECK(x.hot[k] > last); last = x.hot[k]; }
        }
        CHECK(cpu == 6 && gpu == 1);
    }

    // ---- cooling devices
    CHECK(coolingKindFor("cpufreq-cpu0") == C_CPU && coolingKindFor("cpufreq-cpu4") == C_CPU);
    CHECK(coolingKindFor("thermal-cpufreq-1") == C_CPU);
    CHECK(coolingKindFor("devfreq-5000000.gpu") == C_GPU && coolingKindFor("devfreq") == C_GPU);
    CHECK(coolingKindFor("devfreq-soc:cpubw") == C_COMPONENT);
    CHECK(coolingKindFor("qcom-battery") == C_BATTERY && coolingKindFor("pmi8998_charger") == C_BATTERY);
    CHECK(coolingKindFor("qcom-smbx-charger") == C_BATTERY);
    CHECK(coolingKindFor("modem") == C_MODEM && coolingKindFor("pa") == C_POWER_AMPLIFIER);
    CHECK(coolingKindFor("panel0-backlight") == C_DISPLAY && coolingKindFor("wlan") == C_WIFI);
    CHECK(coolingKindFor("something-else") == C_COMPONENT);
    auto cdev = [&](const std::string& n, const std::string& type, const std::string& cur, const std::string& max) {
        std::string d = root + "/class/thermal/" + n;
        md(d); put(d + "/type", type + "\n"); put(d + "/cur_state", cur + "\n");
        if (!max.empty()) put(d + "/max_state", max + "\n");
    };
    cdev("cooling_device10", "devfreq-5000000.gpu", "2", "6");
    cdev("cooling_device2", "cpufreq-cpu4", "0", "12");
    cdev("cooling_device0", "cpufreq-cpu0", "1", "");
    cdev("cooling_device3", "devfreq", "0", "3");
    cdev("cooling_device4", "devfreq", "0", "3");
    md(root + "/class/thermal/cooling_devicex");                               // junk entries ignored
    md(root + "/class/thermal/cooling_device7");                               // no type file
    auto c = scanCooling(root);
    CHECK(c.size() == 5);
    CHECK(c.size() == 5 && c[0].name == "cpufreq-cpu0" && c[0].maxState == -1 && c[0].type == C_CPU);
    CHECK(c.size() == 5 && c[1].name == "cpufreq-cpu4" && c[1].maxState == 12);
    CHECK(c.size() == 5 && c[2].name == "devfreq" && c[3].name == "devfreq-1");  // unique names
    CHECK(c.size() == 5 && c[4].name == "devfreq-5000000.gpu" && c[4].type == C_GPU);  // numeric order: 10 last
    long v = -5;
    CHECK(c.size() == 5 && readState(c[4].curPath, &v) && v == 2);
    put(root + "/bad", "x\n");
    CHECK(!readState(root + "/bad", &v) && !readState(root + "/missing", &v));
    CHECK(scanCooling(root + "/none").empty());

    std::string cmd = "rm -rf '" + root + "'";
    if (system(cmd.c_str())) {}
    std::cout << "checks " << checks << " fails " << fails << "\nA6L_THERMAL_SRC_TEST " << (fails ? "FAIL" : "PASS") << "\n";
    return fails ? 1 : 0;
}
