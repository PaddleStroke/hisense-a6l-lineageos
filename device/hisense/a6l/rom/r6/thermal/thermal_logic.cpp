// SPDX-License-Identifier: Apache-2.0
#include "thermal_logic.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace a6l::thermal {

static bool parseType(const std::string& s, int* t) {
    static const std::map<std::string, int> m = {{"CPU", CPU}, {"GPU", GPU}, {"BATTERY", BATTERY},
                                                 {"SKIN", SKIN}, {"USB_PORT", USB_PORT}, {"SOC", SOC},
                                                 {"POWER_AMPLIFIER", POWER_AMPLIFIER}, {"MODEM", MODEM},
                                                 {"AMBIENT", AMBIENT}};
    auto it = m.find(s);
    if (it == m.end()) return false;
    *t = it->second;
    return true;
}

static bool parseFloat(const std::string& s, float* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    float f = std::strtof(s.c_str(), &end);
    if (errno || end == s.c_str() || *end || !std::isfinite(f)) return false;
    *v = f;
    return true;
}

bool parseConfig(const std::string& text, std::vector<Sensor>* out, std::string* err) {
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    out->clear();
    auto fail = [&](const std::string& m) {
        if (err) *err = "line " + std::to_string(ln) + ": " + m;
        out->clear();
        return false;
    };
    while (std::getline(in, line)) {
        ++ln;
        auto h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream ls(line);
        std::vector<std::string> f;
        for (std::string w; ls >> w;) f.push_back(w);
        if (f.empty()) continue;
        if (f.size() != 10) return fail("expected 10 fields, got " + std::to_string(f.size()));
        Sensor s;
        s.name = f[0];
        for (const Sensor& o : *out)
            if (o.name == s.name) return fail("duplicate sensor " + s.name);
        if (!parseType(f[1], &s.type)) return fail("bad type " + f[1]);
        std::stringstream alts(f[2]);
        for (std::string a; std::getline(alts, a, '|');) {
            Source src;
            // "@<number>" at the end = per-source scale. A non-numeric tail is part of the name (sysfs paths like
            // .../pmic@0:battery@4000/... contain '@').
            auto at = a.rfind('@');
            float sc;
            if (at != std::string::npos && parseFloat(a.substr(at + 1), &sc)) {
                if (sc <= 0) return fail("bad source scale " + a);
                src.scale = sc;
                a.resize(at);
            }
            auto list = [&](size_t skip) {
                std::stringstream zs(a.substr(skip));
                for (std::string z; std::getline(zs, z, ',');)
                    if (!z.empty()) src.names.push_back(z);
            };
            if (a.rfind("zone:", 0) == 0) {
                src.kind = Source::ZONE;
                list(5);
                if (src.names.empty()) return fail("empty zone list");
                if (s.zones.empty()) s.zones = src.names;
            } else if (a.rfind("iio:", 0) == 0) {
                src.kind = Source::IIO;
                list(4);
                if (src.names.empty()) return fail("empty iio label list");
            } else if (a.rfind("file:/", 0) == 0) {
                src.kind = Source::FILE;
                src.names = {a.substr(5)};
                if (s.file.empty()) s.file = src.names[0];
            } else {
                return fail("source must be zone:<types>, iio:<labels> or file:/<path>");
            }
            s.sources.push_back(src);
        }
        if (s.sources.empty()) return fail("no source");
        if (!parseFloat(f[3], &s.scale) || s.scale <= 0) return fail("bad scale " + f[3]);
        float last = -INFINITY;
        for (int i = 1; i < kSeverities; ++i) {
            const std::string& v = f[3 + i];
            if (v == "-") continue;
            float t;
            if (!parseFloat(v, &t)) return fail("bad threshold " + v);
            if (t <= last) return fail("thresholds must increase (" + v + ")");
            s.hot[i] = last = t;
        }
        out->push_back(s);
    }
    if (out->empty()) return fail("no sensors");
    return true;
}

static bool readLine(const std::string& path, std::string* v) {
    std::ifstream f(path);
    if (!f || !std::getline(f, *v)) return false;
    while (!v->empty() && (v->back() == '\n' || v->back() == '\r' || v->back() == ' ')) v->pop_back();
    return true;
}

std::map<std::string, std::string> scanZones(const std::string& sysRoot) {
    std::map<std::string, std::string> zones;
    const std::string base = sysRoot + "/class/thermal";
    DIR* d = opendir(base.c_str());
    if (!d) return zones;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("thermal_zone", 0) != 0) continue;
        std::string type;
        if (!readLine(base + "/" + n + "/type", &type) || type.empty()) continue;
        // duplicates (two zones with one type): keep the lowest zone number, deterministic across boots
        auto it = zones.find(type);
        std::string p = base + "/" + n + "/temp";
        if (it == zones.end() || std::atoi(n.c_str() + 12) < std::atoi(it->second.c_str() + base.size() + 13))
            zones[type] = p;
    }
    closedir(d);
    return zones;
}

std::map<std::string, std::string> scanIio(const std::string& sysRoot) {
    std::map<std::string, std::string> out;
    const std::string base = sysRoot + "/bus/iio/devices";
    DIR* d = opendir(base.c_str());
    if (!d) return out;
    std::vector<std::string> devs;
    while (dirent* e = readdir(d))
        if (std::string(e->d_name).rfind("iio:device", 0) == 0) devs.push_back(e->d_name);
    closedir(d);
    std::sort(devs.begin(), devs.end());  // deterministic when two ADCs carry one label: first device wins
    for (const std::string& dev : devs) {
        const std::string dir = base + "/" + dev;
        DIR* dd = opendir(dir.c_str());
        if (!dd) continue;
        std::vector<std::string> files;
        while (dirent* e = readdir(dd)) files.push_back(e->d_name);
        closedir(dd);
        std::sort(files.begin(), files.end());
        const std::string pre = "in_temp", in = "_input", lb = "_label";
        for (const std::string& f : files) {
            if (f.rfind(pre, 0) != 0) continue;
            auto ends = [&](const std::string& x) {
                return f.size() > pre.size() + x.size() && f.compare(f.size() - x.size(), x.size(), x) == 0;
            };
            std::string label, input;
            if (ends(in) && f[pre.size()] == '_') {              // in_temp_<label>_input
                label = f.substr(pre.size() + 1, f.size() - pre.size() - 1 - in.size());
                input = f;
            } else if (ends(lb)) {                               // in_tempN_label -> in_tempN_input
                std::string idx = f.substr(pre.size(), f.size() - pre.size() - lb.size());
                if (idx.empty() || idx.find_first_not_of("0123456789") != std::string::npos) continue;
                if (!readLine(dir + "/" + f, &label)) continue;
                input = pre + idx + in;
                if (!std::binary_search(files.begin(), files.end(), input)) continue;
            }
            if (!label.empty() && !out.count(label)) out[label] = dir + "/" + input;
        }
    }
    return out;
}

void resolve(std::vector<Sensor>* sensors, const std::map<std::string, std::string>& zones,
             const std::map<std::string, std::string>& iio) {
    const std::string sfx = "-thermal";
    for (Sensor& s : *sensors) {
        s.path.clear();
        s.resolvedScale = s.scale;
        for (size_t k = 0; k < s.sources.size() && s.path.empty(); ++k) {
            const Source& src = s.sources[k];
            std::string hit;
            if (src.kind == Source::ZONE) {
                for (const std::string& z : src.names) {
                    std::vector<std::string> alts = {z, z + sfx};
                    if (z.size() > sfx.size() && z.compare(z.size() - sfx.size(), sfx.size(), sfx) == 0)
                        alts.push_back(z.substr(0, z.size() - sfx.size()));
                    for (const std::string& a : alts) {
                        auto it = zones.find(a);
                        if (it != zones.end()) { hit = it->second; break; }
                    }
                    if (!hit.empty()) break;
                }
            } else if (src.kind == Source::IIO) {
                for (const std::string& l : src.names) {
                    auto it = iio.find(l);
                    if (it != iio.end()) { hit = it->second; break; }
                }
            } else if (k + 1 == s.sources.size() || access(src.names[0].c_str(), R_OK) == 0) {
                hit = src.names[0];
            }
            if (hit.empty()) continue;
            s.path = hit;
            s.resolvedScale = std::isnan(src.scale) ? s.scale : src.scale;
        }
    }
}

bool readTemp(const std::string& path, float scale, float* out) {
    std::string v;
    if (!readLine(path, &v) || v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long raw = std::strtol(v.c_str(), &end, 10);
    if (errno || end == v.c_str() || *end) return false;
    *out = static_cast<float>(raw) * scale;
    return true;
}

int severityFor(float t, const float hot[kSeverities], int prev, float hyst) {
    if (!std::isfinite(t)) return prev;
    int sev = 0;
    for (int i = 1; i < kSeverities; ++i)
        if (std::isfinite(hot[i]) && t >= hot[i]) sev = i;
    // hysteresis: stay at a higher previous severity until t < threshold(prev) - hyst
    while (prev > sev) {
        if (std::isfinite(hot[prev]) && t >= hot[prev] - hyst) return prev;
        --prev;
    }
    return sev;
}

}  // namespace a6l::thermal
