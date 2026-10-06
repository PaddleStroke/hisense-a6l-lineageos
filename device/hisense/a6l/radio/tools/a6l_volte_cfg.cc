// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): a6l-volte-cfg = modem configuration tool for the VoLTE attempts.
//   A: EFS items (IMS_enable, IMS settings) over DIAG EFS2 (diag-router socket), with backup/verify/rollback/restore
//   B: PDC select + activate of an MBN already in the modem (with the previous selection saved for revert)
//   C: PDC load of an MBN file (chunked), then B
// Every WRITE needs A6L_MODEM_CFG_APPROVED=1 (Pierre types it) AND the RAM-EFS guard (rmtfs on /tmp copies);
// A6L_VC_DRYRUN=1 turns every write into a no-op plan. Reads need nothing. Output lines: A6L_VC_*.
//
// usage: a6l-volte-cfg <cmd> [args]
//   guard                         RAM-EFS guard verdict (read-only)
//   efs-hello                     EFS2 HELLO over diag (read-only)
//   efs-get <path>...             read items (GET, then file interface)            (read-only)
//   efs-stat <path> | efs-ls <dir>                                                  (read-only)
//   efs-selftest                  PUT/GET/UNLINK of /nv/item_files/volte4_selftest (write; proves the PUT layout)
//   nv-plan <patch.nvp>           current vs new values, nothing written            (read-only)
//   nv-apply <patch.nvp> <dir>    backup to <dir>/backup.txt, PUT, read back, sync (write)
//   nv-restore <dir>              replay <dir>/backup.txt, verify, sync            (write)
//   mbn-info <file.mbn>           offline: MCFG type/items/name/version + SHA-1 id
//   pdc-list                      selected/pending + every SW config (read-only)
//   pdc-select <id-hex|name> [activate]   save the current SW id, Set Selected (+ Activate = modem reset) (write)
//   pdc-activate                  Activate the pending SW config (modem reset)     (write)
//   pdc-load <file.mbn>           Load Config (1024 B chunks, id = SHA-1 of the file), then list/verify (write)
//   pdc-revert                    select + activate the SW id saved by the first pdc-select/pdc-load (write)
// env: A6L_VC_STATE (default /tmp/a6l-volte4/pdc-prev.txt), A6L_VC_CHUNK (1024), A6L_VC_DIAG (diag), A6L_VC_PROC (/proc)
#include "../volte4/efs2.h"
#include "../volte4/nvpatch.h"
#include "../volte4/pdc.h"

#include <a6lqmi/client.h>
#include <a6lqmi/log.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::volte4;

static void out(const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); }
static std::string env(const char* k, const char* d) { const char* v = getenv(k); return v && *v ? v : d; }

static bool readAll(const std::string& p, std::string* s) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    s->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}
static bool writeSync(const std::string& p, const std::string& s) {
    FILE* f = fopen(p.c_str(), "wx");  // never overwrite an existing backup/state file
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
    return fclose(f) == 0 && ok;
}
static void mkdirs(const std::string& d) {
    for (size_t i = 1; i <= d.size(); i++)
        if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0700);
}

static bool writeAllowed(const char* what) {
    std::string why;
    bool guard = ramEfsGuard(env("A6L_VC_PROC", "/proc"), &why);
    bool approved = env("A6L_MODEM_CFG_APPROVED", "0") == "1";
    bool dry = env("A6L_VC_DRYRUN", "0") == "1";
    if (!approved || !guard || dry) {
        out(std::string("A6L_VC_REFUSED ") + what + ": " + (dry ? "A6L_VC_DRYRUN=1" : !approved ? "needs A6L_MODEM_CFG_APPROVED=1" : "RAM-EFS guard: " + why));
        return false;
    }
    out(std::string("A6L_VC_WRITE_OK ") + what + " (" + why + ")");
    return true;
}

// ---------------------------------------------------------------- EFS
static std::unique_ptr<DiagTransport> diagOpen() {
    auto t = makeDiagUnixTransport(env("A6L_VC_DIAG", "diag"));
    if (!t) out("A6L_VC_EFS_FAIL cannot connect to the diag-router socket @" + env("A6L_VC_DIAG", "diag") + " (is diag-router running?)");
    return t;
}

static int cmdEfs(const std::string& cmd, int argc, char** argv) {
    auto t = diagOpen();
    if (!t) return 4;
    Efs2 efs(t.get());
    uint32_t ver = 0;
    int h = efs.hello(&ver);
    out("A6L_VC_EFS_HELLO rc=" + std::to_string(h) + (h ? " " + efs.lastError() : " version=" + std::to_string(ver)));
    if (h) return 5;
    if (cmd == "efs-hello") return 0;
    if (cmd == "efs-get") {
        int fails = 0;
        for (int i = 0; i < argc; i++) {
            std::vector<uint8_t> d;
            int e = efs.read(argv[i], &d);
            out(std::string("A6L_VC_EFS_GET ") + argv[i] + (e == 0 ? " len=" + std::to_string(d.size()) + " " + hexs(d) : e == efs::kEnoent ? " absent" : " rc=" + std::to_string(e) +
