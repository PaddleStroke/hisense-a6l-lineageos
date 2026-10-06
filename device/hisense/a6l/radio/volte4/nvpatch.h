// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): EFS item patches with backup, read-back verification, rollback and
// restore, plus the "RAM EFS only" guard used before any modem-config write.
//
// Patch file (.nvp):  "put <efs path> <hex>" lines, '#' comments, blank lines ignored.
// Backup manifest   :  "A6L_VOLTE4_BACKUP 1" header, then "present <path> <hex>" / "absent <path> -" lines, written
//                      and fsync'ed BEFORE the first write. nv-restore replays it (present -> PUT old bytes,
//                      absent -> UNLINK) and verifies every item.
#pragma once

#include "efs2.h"

#include <functional>
#include <string>
#include <vector>

namespace a6l::volte4 {

struct PatchEntry {
    std::string path;
    std::vector<uint8_t> data;
};
// nullopt-style: returns false and sets *err ("line N: ...") on a malformed file.
bool parsePatch(const std::string& text, std::vector<PatchEntry>* out, std::string* err);

struct BackupEntry {
    std::string path;
    bool present = false;
    std::vector<uint8_t> data;
};
std::string formatBackup(const std::vector<BackupEntry>& b);
bool parseBackup(const std::string& text, std::vector<BackupEntry>* out, std::string* err);

using Out = std::function<void(const std::string&)>;

enum class ApplyResult { Ok, NothingToDo, ReadFailed, BackupFailed, WriteFailed, VerifyFailed };
const char* applyResultName(ApplyResult r);

// Reads every item, prints A6L_VC_NV_PLAN lines. With commit=false nothing is written.
// With commit=true: backup (via saveBackup; must return true) -> PUT -> GET read-back per item; on the first
// failure the items already written are rolled back from the backup. Then EFS sync.
ApplyResult applyPatch(Efs2& efs, const std::vector<PatchEntry>& patch, bool commit,
                       const std::function<bool(const std::string&)>& saveBackup, const Out& out);
// Restores a backup. Returns the number of items that failed (0 = all restored and verified).
int restoreBackup(Efs2& efs, const std::vector<BackupEntry>& b, const Out& out);

// RAM-EFS guard: true only if exactly one rmtfs process runs and it serves RAM copies: "-o <dir>" with <dir> under
// /tmp/ (the V74/V75 test recovery: dd copies of modemst1/2/fsg/fsc in tmpfs) or "-r" (read-only mode: writes go to
// an in-memory shadow). procRoot is "/proc" on the phone, a fake tree in the host tests.
bool ramEfsGuard(const std::string& procRoot, std::string* why);

std::string hexs(const std::vector<uint8_t>& v, size_t max = 0);
bool unhexs(const std::string& s, std::vector<uint8_t>* out);

}  // namespace a6l::volte4
