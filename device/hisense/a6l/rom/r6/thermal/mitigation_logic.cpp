// SPDX-License-Identifier: Apache-2.0
#include "mitigation_logic.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace a6l::thermal {

static bool readLine(const std::string& path, std::string* v) {
    std::ifstream f(path);
    if (!f || !std::getline(f, *v)) return false;
    while (!v->empty() && (v->back() == '\n' || v->back() == '\r' || v->back() == ' ')) v->pop_back();
    return true;
}

static bool parseLong(const std::string& s, long* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long r = std::strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || *end) return false;
    *v = r;
    return true;
}

static bool parseDeg(const std::string& s, float* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    float f = std::strtof(s.c_str(), &end);
    if (errno || end == s.c_str() || *end || !std::isfinite(f)) return false;
    *v = f;
    return true;
}

static bool validSpec(const std::string& d) {
    if (d == "cpu-big" || d == "cpu-little" || d == "gpu") return true;
    if (d.rfind("cpu:policy", 0) == 0) {
        std::string n = d.substr(10);
        return !n.empty() && n.find_first_not_of("0123456789") == std::string::npos;
    }
    return d.rfind("devfreq:", 0) == 0 && d.size() > 8;
}

bool parseMitigation(const std::string& text, std::vector<MitigationRule>* out, std::string* err) {
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
        if (f.size() != 7) return fail("expected 7 fields, got " + std::to_string(f.size()));
        MitigationRule r;
        r.name = f[0];
        r.sensor = f[1];
        r.device = f[2];
        for (const MitigationRule& o : *out)
            if (o.name == r.name) return fail("duplicate rule " + r.name);
        if (!validSpec(r.device)) return fail("bad device " + r.device);
        if (!parseDeg(f[3], &r.set) || !parseDeg(f[4], &r.clear)) return fail("bad temperature");
        if (r.clear >= r.set) return fail("clear must be below set");
        if (!parseLong(f[5], &r.maxLimit) || r.maxLimit <= 0) return fail("bad device_max_limit " + f[5]);
        if (f[6] == "-") {
            r.floor = 0;
        } else if (!parseLong(f[6], &r.floor) || r.floor <= 0 || r.floor > r.maxLimit) {
            return fail("bad floor " + f[6]);
        }
        out->push_back(r);
    }
    if (out->empty()) return fail("no rules");
    return true;
}

static std::vector<long> parseFreqs(const std::string& s) {
    std::vector<long> v;
    std::istringstream in(s);
    for (std::string w; in >> w;) {
        long x;
        if (parseLong(w, &x) && x > 0) v.push_back(x);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

static std::vector<std::pair<std::string, std::string>> listDir(const std::string& base, const std::string& prefix) {
    std::vector<std::pair<std::string, std::string>> r;  // (name, path)
    DIR* d = opendir(base.c_str());
    if (!d) return r;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (!prefix.empty() && n.rfind(prefix, 0) != 0) continue;
        r.emplace_back(n, base + "/" + n);
    }
    closedir(d);
    return r;
}

std::vector<FreqDevice> scanCpufreq(const std::string& sysRoot) {
    std::vector<std::pair<long, FreqDevice>> found;
    for (auto& [n, p] : listDir(sysRoot + "/devices/system/cpu/cpufreq", "policy")) {
        std::string idx = n.substr(6);
        long k;
        if (!parseLong(idx, &k)) continue;
        FreqDevice d;
        std::string v;
        if (!readLine(p + "/cpuinfo_max_freq", &v) || !parseLong(v, &d.hwMax) || d.hwMax <= 0) continue;
        if (!readLine(p + "/scaling_available_frequencies", &v)) continue;
        d.freqs = parseFreqs(v);
        if (d.freqs.empty()) continue;
        if (access((p + "/scaling_max_freq").c_str(), F_OK) != 0) continue;
        d.name = n;
        d.maxPath = p + "/scaling_max_freq";
        found.emplace_back(k, d);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<FreqDevice> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

std::vector<FreqDevice> scanDevfreq(const std::string& sysRoot) {
    std::vector<FreqDevice> out;
    auto dirs = listDir(sysRoot + "/class/devfreq", "");
    std::sort(dirs.begin(), dirs.end());
    for (auto& [n, p] : dirs) {
        FreqDevice d;
        std::string v;
        if (!readLine(p + "/available_frequencies", &v)) continue;
        d.freqs = parseFreqs(v);
        if (d.freqs.empty() || access((p + "/max_freq").c_str(), F_OK) != 0) continue;
        d.hwMax = d.freqs.back();
        d.name = n;
        d.maxPath = p + "/max_freq";
        d.devfreq = true;
        out.push_back(d);
    }
    return out;
}

bool resolveDevice(const std::string& spec, const std::vector<FreqDevice>& cpus, const std::vector<FreqDevice>& devfreqs,
                   FreqDevice* out) {
    if (spec == "cpu-big" || spec == "cpu-little") {
        if (cpus.size() < 2) return false;
        auto [lo, hi] = std::minmax_element(cpus.begin(), cpus.end(),
                                            [](const FreqDevice& a, const FreqDevice& b) { return a.hwMax < b.hwMax; });
        if (lo->hwMax == hi->hwMax) return false;
        *out = spec == "cpu-big" ? *hi : *lo;
        return true;
    }
    if (spec.rfind("cpu:", 0) == 0) {
        for (const auto& c : cpus)
            if (c.name == spec.substr(4)) { *out = c; return true; }
        return false;
    }
    for (const auto& d : devfreqs) {
        bool hit = spec == "gpu" ? (d.name.find("gpu") != std::string::npos || d.name.find("kgsl") != std::string::npos)
                                 : d.name == spec.substr(8);
        if (hit) { *out = d; return true; }
    }
    return false;
}

long SsController::update(float t, const FreqDevice& dev) {
    const std::vector<long>& f = dev.freqs;
    if (f.empty()) return dev.hwMax;
    int top = 0;
    for (int i = 0; i < static_cast<int>(f.size()); ++i)
        if (f[i] <= r_.maxLimit) top = i;
    int bottom = 0;
    if (r_.floor > 0) {
        bottom = static_cast<int>(f.size()) - 1;
        for (int i = static_cast<int>(f.size()) - 1; i >= 0; --i)
            if (f[i] >= r_.floor) bottom = i;
    }
    bottom = std::min(bottom, top);
    if (!active_) {
        if (!std::isfinite(t) || t < r_.set) return dev.hwMax;
        active_ = true;
        idx_ = top;
        ref_ = t;
        dwell_ = 0;
        return f[idx_];
    }
    idx_ = std::clamp(idx_, bottom, top);
    if (!std::isfinite(t)) return f[idx_];
    ++dwell_;
    if (t <= r_.clear) {
        reset();
        return dev.hwMax;
    }
    if (t >= r_.set) {
        if (dwell_ >= kDownDwell && idx_ > bottom && (t >= ref_ + kRise || t >= r_.set + kAbove)) {
            --idx_;
            ref_ = t;
            dwell_ = 0;
        }
    } else if (dwell_ >= kUpDwell && idx_ < top && t <= r_.set - kBelow && t < ref_ + kRise) {
        ++idx_;
        ref_ = t;
        dwell_ = 0;
    }
    return f[idx_];
}

Mitigator::Mitigator(std::vector<MitigationRule> rules, std::string sysRoot, bool dryRun, bool claim)
    : rules_(std::move(rules)), sysRoot_(std::move(sysRoot)), dryRun_(dryRun), claim_(claim) {
    for (const auto& r : rules_) ctl_.emplace_back(r);
    devOf_.assign(rules_.size(), -1);
    rescan();
}

void Mitigator::rescan() {
    std::vector<FreqDevice> cpus = scanCpufreq(sysRoot_), dfs = scanDevfreq(sysRoot_);
    for (size_t i = 0; i < rules_.size(); ++i) {
        FreqDevice fd;
        if (!resolveDevice(rules_[i].device, cpus, dfs, &fd)) {
            // device gone (module unloaded / policy removed): forget it, the controller restarts from idle
            if (devOf_[i] >= 0) ctl_[i].reset();
            devOf_[i] = -1;
            continue;
        }
        int k = -1;
        for (size_t j = 0; j < devs_.size(); ++j)
            if (devs_[j].dev.maxPath == fd.maxPath) k = static_cast<int>(j);
        if (k < 0) {
            Dev d;
            d.dev = fd;
            d.spec = rules_[i].device;
            devs_.push_back(d);
            k = static_cast<int>(devs_.size()) - 1;
        } else {
            devs_[k].dev.freqs = fd.freqs;  // table / hwMax may change (e.g. a policy re-created)
            devs_[k].dev.hwMax = fd.hwMax;
        }
        devOf_[i] = k;
    }
}

bool Mitigator::write(Dev& d, long v) {
    if (dryRun_) return true;
    int fd = open(d.dev.maxPath.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string s = std::to_string(v) + "\n";
    bool ok = ::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
    if (close(fd) != 0) ok = false;
    return ok;
}

static int levelOf(const FreqDevice& d, long cap) {
    int n = 0;
    for (long f : d.freqs)
        if (f > cap && f <= d.hwMax) ++n;
    return n;
}

static std::string fmtTemp(float t) {
    if (!std::isfinite(t)) return "nan";
    char b[16];
    snprintf(b, sizeof(b), "%.2f", t);
    return b;
}

std::vector<Mitigator::Change> Mitigator::step(const std::function<float(const std::string&)>& temp) {
    std::map<std::string, float> cache;  // one read per sensor per sample
    std::vector<long> cap(devs_.size());
    std::vector<std::string> why(devs_.size());
    std::vector<bool> used(devs_.size(), false);  // a device no rule resolves to any more (gone) is left alone
    for (size_t j = 0; j < devs_.size(); ++j) cap[j] = devs_[j].dev.hwMax;
    for (size_t i = 0; i < rules_.size(); ++i) {
        int k = devOf_[i];
        if (k < 0) continue;
        used[k] = true;
        auto it = cache.find(rules_[i].sensor);
        float t = it != cache.end() ? it->second : (cache[rules_[i].sensor] = temp(rules_[i].sensor));
        long c = ctl_[i].update(t, devs_[k].dev);
        if (ctl_[i].active()) why[k] += (why[k].empty() ? "" : ",") + rules_[i].name + "@" + fmtTemp(t);
        cap[k] = std::min(cap[k], c);
    }
    std::vector<Change> out;
    for (size_t j = 0; j < devs_.size(); ++j) {
        Dev& d = devs_[j];
        if (!used[j]) continue;
        long want = cap[j];
        long from = d.applied < 0 ? d.dev.hwMax : d.applied;
        d.wanted = want;
        ++d.sinceWrite;
        // never written and nothing to cap: leave the knob alone (daemon), or reset a stale cap once (owner mode)
        if (d.applied < 0 && want >= d.dev.hwMax) {
            std::string v;
            long cur;
            if (!claim_ || !readLine(d.dev.maxPath, &v) || !parseLong(v, &cur) || cur >= d.dev.hwMax) continue;
            from = cur;
            if (write(d, want)) {
                d.applied = want;
                d.sinceWrite = 0;
                out.push_back(Change{d.dev.name, d.spec, from, want, d.dev.hwMax, 0, true, "stale cap reset"});
            } else if (++d.failures == 1 || d.failures % 150 == 0) {
                out.push_back(Change{d.dev.name, d.spec, from, want, d.dev.hwMax, 0, false, "stale cap reset"});
            }
            continue;
        }
        bool reassert = want < d.dev.hwMax && d.sinceWrite >= 15;  // ~30 s: a re-created policy loses the cap
        if (want == d.applied && !reassert) continue;
        bool ok = write(d, want);
        d.sinceWrite = 0;
        if (ok) {
            bool changed = want != d.applied;
            d.applied = want;
            d.failures = 0;
            if (!changed) continue;
        } else if (++d.failures != 1 && d.failures % 150 != 0) {
            continue;  // log the first failure and then every ~5 min
        }
        out.push_back(Change{d.dev.name, d.spec, from, want, d.dev.hwMax, levelOf(d.dev, want), ok,
                             why[j].empty() ? "clear" : why[j]});
    }
    return out;
}

std::vector<Mitigator::Change> Mitigator::releaseAll() {
    std::vector<Change> out;
    for (Dev& d : devs_) {
        if (d.applied < 0 || d.applied >= d.dev.hwMax) continue;
        long from = d.applied;
        bool ok = write(d, d.dev.hwMax);
        if (ok) d.applied = d.dev.hwMax;
        out.push_back(Change{d.dev.name, d.spec, from, d.dev.hwMax, d.dev.hwMax, 0, ok, "release"});
    }
    for (auto& c : ctl_) c.reset();
    return out;
}

std::map<std::string, int> Mitigator::levels() const {
    std::map<std::string, int> m;
    for (const Dev& d : devs_) m[d.spec] = d.applied < 0 ? 0 : levelOf(d.dev, d.applied);
    return m;
}

}  // namespace a6l::thermal
