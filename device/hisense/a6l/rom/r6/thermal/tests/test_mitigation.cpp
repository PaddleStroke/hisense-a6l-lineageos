// SPDX-License-Identifier: Apache-2.0
// Host tests (thermal-20261012): stock-like skin mitigation in mitigation_logic.cpp (rule parser, cpufreq/devfreq scan,
// ss controller, sysfs writes, release, stale-cap reset). Built by tests/run-tests.sh.
// usage: test_mitigation <thermal-a6l-mitigation.conf> <thermal-a6l.conf>. Prints A6L_THERMAL_MIT_TEST PASS|FAIL.
#include "../mitigation_logic.h"
#include "../thermal_logic.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace a6l::thermal;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)

static void put(const std::string& p, const std::string& v) { std::ofstream(p) << v; }
static void md(const std::string& p) { mkdir(p.c_str(), 0755); }
static std::string slurp(const std::string& p) { std::ifstream f(p); return std::string(std::istreambuf_iterator<char>(f), {}); }
static long rd(const std::string& p) { return std::atol(slurp(p).c_str()); }

static const char* kGold = "300000 1113600 1401600 1747200 1958400 2150400 2208000 \n";
static const char* kSilver = "300000 633600 902400 1113600 1401600 1536000 1747200 1843200 \n";
static const char* kGpu = "647000000 588000000 465000000 370000000 266000000 160000000\n";  // devfreq lists descending

int main(int argc, char** argv) {
    std::string err;
    std::vector<MitigationRule> r;

    // ---- shipped config = the stock thermal-engine quiet_therm ss sections (+ SS-GPU), sensors known to thermal-a6l.conf
    {
        CHECK(argc > 2);
        CHECK(parseMitigation(slurp(argc > 1 ? argv[1] : "../thermal-a6l-mitigation.conf"), &r, &err));
        if (!err.empty()) std::cerr << err << "\n";
        std::vector<Sensor> s;
        CHECK(parseConfig(slurp(argc > 2 ? argv[2] : "../thermal-a6l.conf"), &s, &err));
        struct Stock { const char* name; const char* dev; float set, clr; long max; } stock[] = {
                {"SS-QUIET-THERM-GOLD", "cpu-big", 45, 40, 1747200},
                {"SS-QUIET-THERM-SILVER", "cpu-little", 47, 40, 1536000},
                {"SS-QUIET-THERM-GPU", "gpu", 47, 40, 588000000},
                {"SS-GPU", "gpu", 95, 65, 588000000}};
        CHECK(r.size() == 4);
        for (const auto& st : stock) {
            const MitigationRule* m = nullptr;
            for (const auto& x : r)
                if (x.name == st.name) m = &x;
            CHECK(m != nullptr);
            if (!m) continue;
            CHECK(m->device == st.dev && m->set == st.set && m->clear == st.clr && m->maxLimit == st.max);
            CHECK(m->floor > 0 && m->floor < m->maxLimit);
            bool known = false;
            for (const auto& x : s) known |= x.name == m->sensor;
            CHECK(known);
            if (m->name.rfind("SS-QUIET", 0) == 0) CHECK(m->sensor == "skin");
        }
        // Android status follows the mitigation: LIGHT = gold set point, MODERATE above the silver/GPU set point
        for (const auto& x : s)
            if (x.type == SKIN) CHECK(x.hot[1] == 45.0f && x.hot[2] > 47.0f && x.hot[4] == 50.0f && x.hot[6] == 58.0f);
    }

    // ---- parser rejects
    CHECK(!parseMitigation("a skin cpu-big 45 40 1747200\n", &r, &err));                 // field count
    CHECK(!parseMitigation("a skin cpu-big 45 45 1747200 -\n", &r, &err));               // clear == set
    CHECK(!parseMitigation("a skin cpu-mid 45 40 1747200 -\n", &r, &err));               // device
    CHECK(!parseMitigation("a skin cpu:policyx 45 40 1747200 -\n", &r, &err));           // device
    CHECK(!parseMitigation("a skin devfreq: 45 40 1747200 -\n", &r, &err));              // device
    CHECK(!parseMitigation("a skin gpu 47 40 588000000 600000000\n", &r, &err));         // floor > max
    CHECK(!parseMitigation("a skin gpu 47 40 0 -\n", &r, &err));                          // max
    CHECK(!parseMitigation("a skin gpu 4x 40 588000000 -\n", &r, &err));                 // temperature
    CHECK(!parseMitigation("a skin gpu 47 40 5e8 -\n", &r, &err));                        // integer
    CHECK(!parseMitigation("a skin gpu 47 40 1 -\na skin cpu-big 45 40 1 -\n", &r, &err));  // duplicate
    CHECK(!parseMitigation("# nothing\n", &r, &err) && err.find("no rules") != std::string::npos);
    CHECK(parseMitigation("a skin cpu:policy4 45.5 40 1747200 - # tail\nb gpu devfreq:x 95 65 1 1\n", &r, &err));
    CHECK(r.size() == 2 && r[0].set == 45.5f && r[0].floor == 0 && r[1].device == "devfreq:x" && r[1].floor == 1);

    // ---- fake sysfs: A6L topology (policy0 = silver cpus 0,5-7; policy1 = gold cpus 1-4), msm GPU + a bus devfreq
    char tmpl[] = "/tmp/a6l-thermal-mit-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    md(root + "/devices"); md(root + "/devices/system"); md(root + "/devices/system/cpu");
    md(root + "/devices/system/cpu/cpufreq"); md(root + "/class"); md(root + "/class/devfreq");
    auto policy = [&](int n, long max, const char* table) {
        std::string p = root + "/devices/system/cpu/cpufreq/policy" + std::to_string(n);
        md(p);
        put(p + "/cpuinfo_max_freq", std::to_string(max) + "\n");
        put(p + "/scaling_available_frequencies", table);
        put(p + "/scaling_max_freq", std::to_string(max) + "\n");
        return p + "/scaling_max_freq";
    };
    const std::string silver = policy(0, 1843200, kSilver), gold = policy(1, 2208000, kGold);
    md(root + "/class/devfreq/soc:qcom,cpubw");
    put(root + "/class/devfreq/soc:qcom,cpubw/available_frequencies", "100 200\n");
    put(root + "/class/devfreq/soc:qcom,cpubw/max_freq", "200\n");
    md(root + "/class/devfreq/5000000.gpu");
    put(root + "/class/devfreq/5000000.gpu/available_frequencies", kGpu);
    const std::string gpu = root + "/class/devfreq/5000000.gpu/max_freq";
    put(gpu, "647000000\n");

    auto cpus = scanCpufreq(root);
    auto dfs = scanDevfreq(root);
    CHECK(cpus.size() == 2 && dfs.size() == 2);
    FreqDevice d;
    CHECK(resolveDevice("cpu-big", cpus, dfs, &d) && d.name == "policy1" && d.hwMax == 2208000 && d.freqs.size() == 7);
    CHECK(resolveDevice("cpu-little", cpus, dfs, &d) && d.name == "policy0" && d.maxPath == silver);
    CHECK(resolveDevice("cpu:policy0", cpus, dfs, &d) && d.hwMax == 1843200);
    CHECK(!resolveDevice("cpu:policy4", cpus, dfs, &d));
    CHECK(resolveDevice("gpu", cpus, dfs, &d) && d.name == "5000000.gpu" && d.devfreq && d.hwMax == 647000000 &&
          d.freqs.front() == 160000000);
    CHECK(resolveDevice("devfreq:soc:qcom,cpubw", cpus, dfs, &d) && d.hwMax == 200);
    CHECK(!resolveDevice("cpu-big", {cpus[0]}, dfs, &d));          // one policy: no big/little
    CHECK(scanCpufreq(root + "/none").empty() && scanDevfreq(root + "/none").empty());

    // ---- ss controller on the gold table (stock SS-QUIET-THERM-GOLD + A6L floor 1113600)
    {
        FreqDevice g;
        resolveDevice("cpu-big", cpus, dfs, &g);
        SsController c({"G", "skin", "cpu-big", 45, 40, 1747200, 1113600});
        CHECK(c.update(44.9f, g) == 2208000 && !c.active());
        CHECK(c.update(NAN, g) == 2208000 && !c.active());
        CHECK(c.update(45.0f, g) == 1747200 && c.active());       // stock first step = device_max_limit
        CHECK(c.update(45.3f, g) == 1747200);                       // dwell 1
        CHECK(c.update(45.3f, g) == 1747200);                       // dwell 2
        CHECK(c.update(45.3f, g) == 1401600);                       // dwell 3, rising since engage -> one step down
        CHECK(c.update(45.4f, g) == 1401600);
        CHECK(c.update(45.4f, g) == 1401600);
        CHECK(c.update(45.4f, g) == 1401600);                       // not rising (+0.1) and below set+1: hold
        CHECK(c.update(NAN, g) == 1401600);                         // unreadable: hold
        CHECK(c.update(46.0f, g) == 1113600);                       // set+1: down
        for (int i = 0; i < 10; ++i) CHECK(c.update(47.0f + i, g) == 1113600);  // floor
        CHECK(c.update(44.5f, g) == 1113600);                       // between set-1 and set: hold
        CHECK(c.update(43.5f, g) == 1401600);                       // <= set-1, >= 5 samples since the last change: up
        for (int i = 0; i < 4; ++i) CHECK(c.update(43.8f, g) == 1401600);  // dwell
        CHECK(c.update(43.8f, g) == 1401600);                       // rising (+0.3 since the change): no step up
        CHECK(c.update(43.0f, g) == 1747200);                       // back to the first cap...
        for (int i = 0; i < 20; ++i) CHECK(c.update(41.0f, g) == 1747200);  // ...never above it until clear
        CHECK(c.update(40.0f, g) == 2208000 && !c.active());        // set_point_clr: released
        CHECK(c.update(44.0f, g) == 2208000);
        // no floor: walks down to the lowest step (stock ss)
        SsController n({"N", "skin", "cpu-big", 45, 40, 1747200, 0});
        long v = 0;
        for (int i = 0; i < 40; ++i) v = n.update(50.0f, g);
        CHECK(v == 300000);
        // floor above the first cap is clamped to it (parser forbids it; table without the exact step)
        SsController f({"F", "skin", "cpu-big", 45, 40, 1500000, 1500000});
        CHECK(f.update(46.0f, g) == 1401600);
        for (int i = 0; i < 10; ++i) CHECK(f.update(50.0f, g) == 1401600);
    }

    // ---- Mitigator: writes, min over rules, levels, release, daemon vs owner (claim) mode, write failure
    {
        std::vector<MitigationRule> rules;
        CHECK(parseMitigation(slurp(argv[1]), &rules, &err));
        float skin = 30, gpuT = 50;
        auto temp = [&](const std::string& n) { return n == "skin" ? skin : n == "gpu" ? gpuT : NAN; };
        put(gold, "1401600\n");                                     // somebody else's cap
        {
            Mitigator m(rules, root);                               // daemon mode: leaves foreign values alone
            CHECK(m.step(temp).empty() && rd(gold) == 1401600);
        }
        Mitigator m(rules, root, false, true);                      // owner (HAL) mode: resets it once
        auto ch = m.step(temp);
        CHECK(ch.size() == 1 && ch[0].device == "policy1" && ch[0].from == 1401600 && ch[0].to == 2208000 && ch[0].ok);
        CHECK(rd(gold) == 2208000 && rd(silver) == 1843200 && rd(gpu) == 647000000);
        CHECK(m.step(temp).empty());
        skin = 45.2f;
        ch = m.step(temp);
        CHECK(ch.size() == 1 && ch[0].spec == "cpu-big" && ch[0].to == 1747200 && ch[0].level == 3 &&
              ch[0].why.find("SS-QUIET-THERM-GOLD@45.20") != std::string::npos);
        CHECK(rd(gold) == 1747200 && rd(silver) == 1843200 && rd(gpu) == 647000000);
        skin = 47.0f;
        ch = m.step(temp);
        CHECK(ch.size() == 2 && rd(silver) == 1536000 && rd(gpu) == 588000000 && rd(gold) == 1747200);
        CHECK(m.levels()["cpu-little"] == 2 && m.levels()["gpu"] == 1 && m.levels()["cpu-big"] == 3);
        gpuT = 96;                                                  // SS-GPU engages too: the lowest cap wins
        for (int i = 0; i < 4; ++i) m.step(temp);                   // engage + 3 samples dwell, 96 >= 95 + 1 -> down
        CHECK(rd(gpu) == 465000000);
        gpuT = 50;
        // re-assert after ~30 s while capped (a re-created policy lost the value)
        put(silver, "1843200\n");
        for (int i = 0; i < 15; ++i) m.step(temp);
        CHECK(rd(silver) <= 1536000);
        // write failure: reported once, retried every sample, recovered
        unlink(gold.c_str());
        md(gold);                                                   // open(O_WRONLY) on a directory fails
        skin = 39.0f;                                               // clear: the release write to policy1 fails
        int failed = 0;
        for (int i = 0; i < 10; ++i)
            for (const auto& c : m.step(temp)) failed += !c.ok && c.device == "policy1";
        CHECK(failed == 1);
        CHECK(rd(silver) == 1843200 && rd(gpu) == 647000000);       // the other devices were released
        rmdir(gold.c_str());
        put(gold, "1113600\n");
        bool recovered = false;
        for (const auto& c : m.step(temp)) recovered |= c.ok && c.device == "policy1" && c.to == 2208000;
        CHECK(recovered && rd(gold) == 2208000);
        // release on stop (daemon SIGTERM)
        CHECK(m.releaseAll().empty());
        skin = 47.5f;
        CHECK(m.step(temp).size() == 3);
        ch = m.releaseAll();
        CHECK(ch.size() == 3 && rd(gold) == 2208000 && rd(silver) == 1843200 && rd(gpu) == 647000000);
        CHECK(m.levels()["cpu-big"] == 0 && m.releaseAll().empty());
        // cool-down to clear: idle again, nothing written
        skin = 39.0f;
        m.step(temp);
        CHECK(m.step(temp).empty() && rd(gold) == 2208000);
        // dry run never writes
        Mitigator dry(rules, root, true);
        skin = 46;
        CHECK(dry.step(temp).size() == 1 && rd(gold) == 2208000);
        // a device that disappears is forgotten (no writes to a missing knob)
        Mitigator late(rules, root + "/none");
        CHECK(late.step(temp).empty() && late.levels().empty());
    }

    std::cout << "checks " << checks << " fails " << fails << "\nA6L_THERMAL_MIT_TEST " << (fails ? "FAIL" : "PASS") << "\n";
    return fails ? 1 : 0;
}
