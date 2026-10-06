// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): EFS item patch / backup / restore / RAM-EFS guard.
#include "nvpatch.h"

#include <dirent.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace a6l::volte4 {

std::string hexs(const std::vector<uint8_t>& v, size_t max) {
    static const char* d = "0123456789abcdef";
    std::string s;
    size_t n = max && v.size() > max ? max : v.size();
    for (size_t i = 0; i < n; i++) { s += d[v[i] >> 4]; s += d[v[i] & 15]; }
    if (n < v.size()) s += "...(" + std::to_string(v.size()) + " B)";
    if (v.empty()) s = "(empty)";
    return s;
}

bool unhexs(const std::string& s, std::vector<uint8_t>* out) {
    out->clear();
    if (s.size() % 2) return false;
    for (size_t i = 0; i < s.size(); i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[i + k];
            int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (x < 0) return false;
            v = v * 16 + x;
        }
        out->push_back(static_cast<uint8_t>(v));
    }
    return true;
}

static bool validPath(const std::string& p) {
    if (p.size() < 2 || p[0] != '/' || p.size() > 240) return false;
    for (char c : p) if (c <= ' ' || c > '~') return false;
    return p.find("..") == std::string::npos;
}

bool parsePatch(const std::string& text, std::vector<PatchEntry>* out, std::string* err) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    out->clear();
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        std::istringstream ls(line);
        std::string verb, path, hx, extra;
        ls >> verb >> path >> hx;
        if (verb != "put" || path.empty() || hx.empty() || (ls >> extra)) {
            *err = "line " + std::to_string(n) + ": expected 'put <path> <hex>'";
            return false;
        }
        PatchEntry e;
        e.path = path;
        if (!validPath(path)) { *err = "line " + std::to_string(n) + ": bad EFS path"; return false; }
        if (!unhexs(hx, &e.data) || e.data.empty() || e.data.size() > 6144) {
            *err = "line " + std::to_string(n) + ": bad hex / size";
            return false;
        }
        for (auto& o : *out)
            if (o.path == path) { *err = "line " + std::to_string(n) + ": duplicate path " + path; return false; }
        out->push_back(std::move(e));
    }
    if (out->empty()) { *err = "no 'put' line"; return false; }
    return true;
}

std::string formatBackup(const std::vector<BackupEntry>& b) {
    std::string s = "A6L_VOLTE4_BACKUP 1\n";
    for (auto& e : b) s += (e.present ? "present " : "absent ") + e.path + " " + (e.present ? (e.data.empty() ? "=" : hexs(e.data)) : "-") + "\n";
    return s;
}

bool parseBackup(const std::string& text, std::vector<BackupEntry>* out, std::string* err) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    out->clear();
    if (!std::getline(in, line) || line.rfind("A6L_VOLTE4_BACKUP 1", 0) != 0) { *err = "not a volte4 backup"; return false; }
    while (std::getline(in, line)) {
        n++;
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string st, path, hx;
        ls >> st >> path >> hx;
        BackupEntry e;
        e.path = path;
        if (!validPath(path) || hx.empty()) { *err = "entry " + std::to_string(n) + ": malformed"; return false; }
        if (st == "present") {
            e.present = true;
            if (hx != "=" && !unhexs(hx, &e.data)) { *err = "entry " + std::to_string(n) + ": bad hex"; return false; }
        } else if (st != "absent") { *err = "entry " + std::to_string(n) + ": bad state"; return false; }
        out->push_back(std::move(e));
    }
    return true;
}

const char* applyResultName(ApplyResult r) {
    switch (r) {
        case ApplyResult::Ok: return "ok";
        case ApplyResult::NothingToDo: return "nothing-to-do";
        case ApplyResult::ReadFailed: return "read-failed";
        case ApplyResult::BackupFailed: return "backup-failed";
        case ApplyResult::WriteFailed: return "write-failed";
        case ApplyResult::VerifyFailed: return "verify-failed";
    }
    return "?";
}

static bool restoreOne(Efs2& efs, const BackupEntry& e, const Out& out) {
    int rc = e.present ? efs.put(e.path, e.data) : efs.unlink(e.path);
    if (!e.present && rc == efs::kEnoent) rc = 0;
    std::vector<uint8_t> now;
    int g = efs.read(e.path, &now);
    bool ok = rc == 0 && (e.present ? (g == 0 && now == e.data) : g == efs::kEnoent);
    out("A6L_VC_NV_RESTORE " + e.path + (e.present ? " -> " + hexs(e.data, 16) : " -> absent") +
        (ok ? " verified" : " FAILED rc=" + std::to_string(rc) + " read=" + std::to_string(g) + " " + efs.lastError()));
    return ok;
}

ApplyResult applyPatch(Efs2& efs, const std::vector<PatchEntry>& patch, bool commit,
                       const std::function<bool(const std::string&)>& saveBackup, const Out& out) {
    std::vector<BackupEntry> backup;
    std::vector<const PatchEntry*> todo;
    for (auto& p : patch) {
        BackupEntry b;
        b.path = p.path;
        int e = efs.read(p.path, &b.data);
        if (e == 0) b.present = true;
        else if (e == efs::kEnoent) b.data.clear();
        else {
            out("A6L_VC_NV_PLAN " + p.path + " READ_FAILED rc=" + std::to_string(e) + " " + efs.lastError());
            return ApplyResult::ReadFailed;
        }
        bool same = b.present && b.data == p.data;
        out("A6L_VC_NV_PLAN " + p.path + " cur=" + (b.present ? hexs(b.data, 16) : "absent") + " new=" +
            hexs(p.data, 16) + (same ? " same" : " change"));
        if (!same) { todo.push_back(&p); backup.push_back(std::move(b)); }
    }
    out("A6L_VC_NV_PLAN_SUMMARY items=" + std::to_string(patch.size()) + " to_change=" + std::to_string(todo.size()) +
        (commit ? " mode=commit" : " mode=dry-run (nothing written)"));
    if (todo.empty()) return ApplyResult::NothingToDo;
    if (!commit) return ApplyResult::Ok;
    if (!saveBackup(formatBackup(backup))) {
        out("A6L_VC_NV_BACKUP_FAILED (nothing written)");
        return ApplyResult::BackupFailed;
    }
    for (size_t i = 0; i < todo.size(); i++) {
        const PatchEntry& p = *todo[i];
        int e = efs.put(p.path, p.data);
        std::vector<uint8_t> rb;
        int g = e == 0 ? efs.read(p.path, &rb) : e;
        bool ok = e == 0 && g == 0 && rb == p.data;
        out("A6L_VC_NV_PUT " + p.path + " " + hexs(p.data, 16) + (ok ? " verified" : " FAILED put=" + std::to_string(e) +
            " read=" + std::to_string(g) + " got=" + hexs(rb, 16) + " " + efs.lastError()));
        if (!ok) {
            out("A6L_VC_NV_ROLLBACK items=" + std::to_string(i + 1));
            for (size_t k = 0; k <= i; k++) restoreOne(efs, backup[k], out);
            efs.sync();
            return e ? ApplyResult::WriteFailed : ApplyResult::VerifyFailed;
        }
    }
    int s = efs.sync();
    out("A6L_VC_NV_SYNC rc=" + std::to_string(s) + (s ? " " + efs.lastError() : ""));
    return ApplyResult::Ok;
}

int restoreBackup(Efs2& efs, const std::vector<BackupEntry>& b, const Out& out) {
    int fails = 0;
    for (auto& e : b) if (!restoreOne(efs, e, out)) fails++;
    int s = efs.sync();
    out("A6L_VC_NV_SYNC rc=" + std::to_string(s));
    return fails;
}

bool ramEfsGuard(const std::string& procRoot, std::string* why) {
    DIR* d = opendir(procRoot.c_str());
    if (!d) { *why = "cannot read " + procRoot; return false; }
    int found = 0;
    bool ok = false;
    std::string detail;
    while (dirent* de = readdir(d)) {
        std::string pid = de->d_name;
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream f(procRoot + "/" + pid + "/cmdline", std::ios::binary);
        if (!f) continue;
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> argv;
        size_t a = 0;
        while (a < all.size()) { size_t z = all.find('\0', a); if (z == std::string::npos) z = all.size(); argv.push_back(all.substr(a, z - a)); a = z + 1; }
        if (argv.empty()) continue;
        std::string base = argv[0].substr(argv[0].rfind('/') == std::string::npos ? 0 : argv[0].rfind('/') + 1);
        if (base != "rmtfs") continue;
        found++;
        bool thisOk = false;
        for (size_t i = 1; i < argv.size(); i++) {
            if (argv[i] == "-r") thisOk = true;
            if (argv[i] == "-o" && i + 1 < argv.size() && argv[i + 1].rfind("/tmp/", 0) == 0 &&
                argv[i + 1].find("..") == std::string::npos) thisOk = true;
        }
        detail += " pid" + pid + (thisOk ? "=ram" : "=PARTITIONS");
        ok = thisOk;
    }
    closedir(d);
    if (found != 1) { *why = "expected exactly one rmtfs, found " + std::to_string(found) + detail; return false; }
    *why = ok ? "rmtfs serves RAM copies" + detail : "rmtfs is NOT on RAM copies" + detail;
    return ok;
}

}  // namespace a6l::volte4
