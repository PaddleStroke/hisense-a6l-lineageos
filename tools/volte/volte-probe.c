/*
 * volte-probe: READ-ONLY QMI-over-QRTR probe for VoLTE/IMS readiness (Hisense A6L, SDM660).
 *
 * Sends only query messages (no set/activate/load/delete, no dial, no data call, no mode change):
 *   QRTR name-service lookup (all services), PDC Register(reporting)+Get Selected Config+List Configs+
 *   Get Config Info, NAS Get System Info, WDS Get Profile List/Settings, DSD Get System Status,
 *   and, only when the modem publishes them, IMSA Get Registration/Services Status, IMS settings
 *   Get Services Enabled, IMSP Get Enabler State. `imsa-watch` registers IMSA indications and prints them.
 * It never talks to unknown services (54, 68, 74, 4098 are only named).
 * Output lines start with A6L_VOLTE_. Build: see build-volte-probe.sh (static aarch64, NDK r27c).
 */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_NEW_LOOKUP 10

struct sockaddr_qrtr_ { unsigned short sq_family; uint32_t sq_node; uint32_t sq_port; };
struct qrtr_ctrl_ { uint32_t cmd; uint32_t service, instance, node, port; };

struct svc { uint32_t service, version, instance, node, port; };
static struct svc g_svc[128];
static int g_nsvc;

static void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("A6L_VOLTE_"); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout);
}

static const char *svc_name(uint32_t s) {
    switch (s) {
    case 1: return "WDS"; case 2: return "DMS"; case 3: return "NAS"; case 4: return "QOS"; case 5: return "WMS";
    case 7: return "AUTH"; case 8: return "AT"; case 9: return "VOICE"; case 10: return "CAT2"; case 11: return "UIM";
    case 12: return "PBM"; case 14: return "RMTFS"; case 15: return "TEST"; case 16: return "LOC"; case 17: return "SAR";
    case 18: return "IMSS(ims-settings)"; case 21: return "MFS"; case 22: return "TIME"; case 23: return "TS"; case 24: return "TMD";
    case 26: return "WDA"; case 29: return "CSVT"; case 31: return "IMSP(presence)"; case 32: return "IMSVT";
    case 33: return "IMSA(ims-application)"; case 34: return "COEX"; case 36: return "PDC"; case 40: return "IMSRTP(AP-hosted)";
    case 41: return "RFRPE"; case 42: return "DSD"; case 43: return "SSCTL"; case 46: return "ATP"; case 47: return "DPM";
    case 48: return "DFS"; case 49: return "IPA-ctrl"; case 50: return "UIMRMT"; case 54: return "unknown-0x36(no AP client in stock vendor)";
    case 55: return "SLIM"; case 66: return "servreg-notif"; case 68: return "OTT(ott_qmi, stock libqmiservices)";
    case 69: return "WLFW"; case 71: return "UIMHTTP"; case 74: return "ANTSWITCH(antswitch_qmi, stock libqmiservices)";
    case 77: return "IMS-private"; case 770: return "IMSDCM(AP-hosted by imsdatadaemon)";
    case 4096: return "TFTP"; case 4097: return "DIAG"; case 4098: return "HISENSE-OEM(hisense_qmi, stock libqmiservices)";
    default: return "?";
    }
}

static int qrtr_socket(uint32_t *node) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (fd < 0) { out("ERROR socket(AF_QIPCRTR) errno=%d (%s)", errno, strerror(errno)); return -1; }
    struct sockaddr_qrtr_ sq; socklen_t sl = sizeof(sq);
    if (getsockname(fd, (struct sockaddr *)&sq, &sl) < 0) { out("ERROR getsockname errno=%d", errno); close(fd); return -1; }
    if (node) *node = sq.sq_node;
    return fd;
}

static int do_lookup(void) {
    uint32_t local; int fd = qrtr_socket(&local);
    if (fd < 0) return -1;
    struct qrtr_ctrl_ pkt = { QRTR_TYPE_NEW_LOOKUP, 0, 0, 0, 0 };
    struct sockaddr_qrtr_ to = { AF_QIPCRTR, local, QRTR_PORT_CTRL };
    if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) { out("ERROR lookup sendto errno=%d", errno); close(fd); return -1; }
    g_nsvc = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 3000) <= 0) { out("WARN lookup: no end marker within 3 s"); break; }
        struct qrtr_ctrl_ r; ssize_t n = recv(fd, &r, sizeof(r), 0);
        if (n < (ssize_t)sizeof(r) || r.cmd != QRTR_TYPE_NEW_SERVER) continue;
        if (!r.service && !r.instance && !r.node && !r.port) break;
        if (g_nsvc < 128) {
            struct svc *s = &g_svc[g_nsvc++];
            s->service = r.service; s->version = r.instance & 0xff; s->instance = r.instance >> 8; s->node = r.node; s->port = r.port;
        }
    }
    close(fd);
    return g_nsvc;
}

static struct svc *find_svc(uint32_t service) {
    for (int i = 0; i < g_nsvc; i++) if (g_svc[i].service == service && g_svc[i].node == 0) return &g_svc[i];
    for (int i = 0; i < g_nsvc; i++) if (g_svc[i].service == service) return &g_svc[i];
    return NULL;
}

/* ---- QMI message building / parsing ---- */
struct msg { uint8_t b[4096]; size_t n; };
static void m_init(struct msg *m, uint16_t txn, uint16_t id) {
    m->b[0] = 0x00; m->b[1] = txn & 0xff; m->b[2] = txn >> 8; m->b[3] = id & 0xff; m->b[4] = id >> 8; m->b[5] = m->b[6] = 0; m->n = 7;
}
static void m_tlv(struct msg *m, uint8_t t, const void *v, uint16_t l) {
    if (m->n + 3 + l > sizeof(m->b)) return;
    m->b[m->n] = t; m->b[m->n + 1] = l & 0xff; m->b[m->n + 2] = l >> 8; memcpy(m->b + m->n + 3, v, l); m->n += 3 + l;
    uint16_t pl = (uint16_t)(m->n - 7); m->b[5] = pl & 0xff; m->b[6] = pl >> 8;
}
static void m_u8(struct msg *m, uint8_t t, uint8_t v) { m_tlv(m, t, &v, 1); }
static void m_u32(struct msg *m, uint8_t t, uint32_t v) { uint8_t x[4] = { v, v >> 8, v >> 16, v >> 24 }; m_tlv(m, t, x, 4); }

/* find TLV in a received QMI message (header 7 bytes) */
static const uint8_t *tlv(const uint8_t *b, size_t n, uint8_t t, uint16_t *len) {
    size_t o = 7;
    while (o + 3 <= n) {
        uint16_t l = b[o + 1] | (b[o + 2] << 8);
        if (o + 3 + l > n) return NULL;
        if (b[o] == t) { if (len) *len = l; return b + o + 3; }
        o += 3 + l;
    }
    return NULL;
}
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void dump_tlvs(const char *tag, const uint8_t *b, size_t n) {
    size_t o = 7;
    while (o + 3 <= n) {
        uint16_t l = b[o + 1] | (b[o + 2] << 8);
        if (o + 3 + l > n) { out("%s TLV-TRUNCATED at %zu", tag, o); return; }
        char hex[3 * 48 + 1]; size_t k, w = 0;
        for (k = 0; k < l && k < 48; k++) w += snprintf(hex + w, sizeof(hex) - w, "%02x", b[o + 3 + k]);
        hex[w] = 0;
        out("%s tlv=0x%02x len=%u %s%s", tag, b[o], l, hex, l > 48 ? ".." : "");
        o += 3 + l;
    }
}

struct cl { int fd; struct sockaddr_qrtr_ to; uint16_t txn; const char *name; };
static int cl_open(struct cl *c, uint32_t service, const char *name) {
    struct svc *s = find_svc(service);
    c->name = name;
    if (!s) { out("%s ABSENT (service %u not published)", name, service); return -1; }
    c->fd = qrtr_socket(NULL);
    if (c->fd < 0) return -1;
    c->to.sq_family = AF_QIPCRTR; c->to.sq_node = s->node; c->to.sq_port = s->port; c->txn = 1;
    return 0;
}
static void cl_close(struct cl *c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }

/* receive one packet with timeout; returns length */
static ssize_t cl_recv(struct cl *c, uint8_t *buf, size_t cap, int ms) {
    struct pollfd p = { c->fd, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return 0;
    return recv(c->fd, buf, cap, 0);
}

/* request/response: returns response length, fills result/error. Indications seen meanwhile are dumped. */
static ssize_t cl_req(struct cl *c, struct msg *m, uint8_t *rsp, size_t cap, uint16_t *res, uint16_t *err) {
    uint16_t txn = c->txn++;
    m->b[1] = txn & 0xff; m->b[2] = txn >> 8;
    uint16_t id = le16(m->b + 3);
    if (sendto(c->fd, m->b, m->n, 0, (struct sockaddr *)&c->to, sizeof(c->to)) < 0) { out("%s ERROR sendto msg=0x%04x errno=%d", c->name, id, errno); return -1; }
    for (int tries = 0; tries < 20; tries++) {
        ssize_t n = cl_recv(c, rsp, cap, 3000);
        if (n <= 0) break;
        if (n < 7) continue;
        if (rsp[0] == 0x02 && le16(rsp + 1) == txn) {
            uint16_t l; const uint8_t *r = tlv(rsp, n, 0x02, &l);
            *res = r && l >= 4 ? le16(r) : 0xffff; *err = r && l >= 4 ? le16(r + 2) : 0xffff;
            return n;
        }
        if (rsp[0] == 0x04) { char tag[64]; snprintf(tag, sizeof(tag), "%s IND msg=0x%04x", c->name, le16(rsp + 3)); dump_tlvs(tag, rsp, n); }
    }
    out("%s TIMEOUT msg=0x%04x", c->name, id);
    return -1;
}

/* wait for an indication with msg id; returns length */
static ssize_t cl_wait_ind(struct cl *c, uint16_t id, uint8_t *buf, size_t cap, int ms) {
    struct timeval t0, t; gettimeofday(&t0, NULL);
    for (;;) {
        gettimeofday(&t, NULL);
        int left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000);
        if (left <= 0) return 0;
        ssize_t n = cl_recv(c, buf, cap, left);
        if (n <= 0) return 0;
        if (n >= 7 && buf[0] == 0x04 && le16(buf + 3) == id) return n;
    }
}

static void put_id(char *dst, size_t cap, const uint8_t *p, int l) {
    size_t w = 0; dst[0] = 0;
    for (int i = 0; i < l && w + 3 < cap; i++) w += snprintf(dst + w, cap - w, "%02x", p[i]);
}

/* ---- probes ---- */
static void probe_lookup(void) {
    int n = do_lookup();
    out("LOOKUP services=%d", n);
    for (int i = 0; i < g_nsvc; i++)
        out("SVC service=%u (0x%x) name=%s version=%u instance=%u node=%u port=%u", g_svc[i].service, g_svc[i].service,
            svc_name(g_svc[i].service), g_svc[i].version, g_svc[i].instance, g_svc[i].node, g_svc[i].port);
    static const uint32_t ims[] = { 18, 31, 33, 40, 77, 770 };
    for (unsigned k = 0; k < sizeof(ims) / sizeof(ims[0]); k++)
        out("IMS_SVC %u %s %s", ims[k], svc_name(ims[k]), find_svc(ims[k]) ? "PRESENT" : "absent");
}

static void pdc_config_info(struct cl *c, uint32_t type, const uint8_t *id, int idl, uint32_t token) {
    struct msg m; uint8_t rsp[4096]; uint16_t res, err; uint8_t v[4 + 1 + 255];
    m_init(&m, 0, 0x28);
    v[0] = type; v[1] = type >> 8; v[2] = type >> 16; v[3] = type >> 24; v[4] = (uint8_t)idl; memcpy(v + 5, id, idl);
    m_tlv(&m, 0x01, v, 5 + idl); m_u32(&m, 0x10, token);
    char ids[520]; put_id(ids, sizeof(ids), id, idl);
    if (cl_req(c, &m, rsp, sizeof(rsp), &res, &err) < 0 || res) { out("PDC_INFO id=%s request res=%u err=%u", ids, res, err); return; }
    ssize_t n = cl_wait_ind(c, 0x28, rsp, sizeof(rsp), 5000);
    if (n <= 0) { out("PDC_INFO id=%s no indication", ids); return; }
    uint16_t l; const uint8_t *p; uint32_t size = 0, ver = 0; char desc[256] = "";
    if ((p = tlv(rsp, n, 0x11, &l)) && l >= 4) size = le32(p);
    if ((p = tlv(rsp, n, 0x13, &l)) && l >= 4) ver = le32(p);
    if ((p = tlv(rsp, n, 0x12, &l)) && l >= 1) { int sl = p[0] < l - 1 ? p[0] : l - 1; memcpy(desc, p + 1, sl); desc[sl] = 0; }
    for (char *q = desc; *q; q++) if (*q < 32 || *q > 126) *q = '.';
    out("PDC_INFO type=%s id=%s size=%u version=0x%08x desc='%s'", type ? "SW" : "HW", ids, size, ver, desc);
}

static void probe_pdc(void) {
    struct cl c; if (cl_open(&c, 36, "PDC") < 0) return;
    struct msg m; uint8_t rsp[4096]; uint16_t res, err;
    m_init(&m, 0, 0x20); m_u8(&m, 0x10, 1);
    if (cl_req(&c, &m, rsp, sizeof(rsp), &res, &err) >= 0) out("PDC_REGISTER res=%u err=%u", res, err);
    for (uint32_t type = 0; type <= 1; type++) {
        uint32_t token = 0xA6100 + type;
        m_init(&m, 0, 0x22); m_u32(&m, 0x01, type); m_u32(&m, 0x10, token);
        if (cl_req(&c, &m, rsp, sizeof(rsp), &res, &err) < 0) continue;
        if (res) { out("PDC_SELECTED type=%s request res=%u err=%u (err 0x%x: no config selected?)", type ? "SW" : "HW", res, err, err); continue; }
        ssize_t n = cl_wait_ind(&c, 0x22, rsp, sizeof(rsp), 5000);
        if (n <= 0) { out("PDC_SELECTED type=%s no indication", type ? "SW" : "HW"); continue; }
        uint16_t l; const uint8_t *p = tlv(rsp, n, 0x01, &l);
        out("PDC_SELECTED type=%s ind_result=%d", type ? "SW" : "HW", p && l >= 2 ? le16(p) : -1);
        const uint8_t *a = tlv(rsp, n, 0x11, &l); char ids[520];
        if (a && l >= 1) { put_id(ids, sizeof(ids), a + 1, a[0] < l - 1 ? a[0] : l - 1); out("PDC_ACTIVE type=%s id=%s", type ? "SW" : "HW", ids); pdc_config_info(&c, type, a + 1, a[0] < l - 1 ? a[0] : l - 1, token + 0x10); }
        else out("PDC_ACTIVE type=%s none", type ? "SW" : "HW");
        const uint8_t *pe = tlv(rsp, n, 0x12, &l);
        if (pe && l >= 1) { put_id(ids, sizeof(ids), pe + 1, pe[0] < l - 1 ? pe[0] : l - 1); out("PDC_PENDING type=%s id=%s", type ? "SW" : "HW", ids); }
    }
    /* list SW configs stored in the modem EFS */
    m_init(&m, 0, 0x24); m_u32(&m, 0x10, 0xA6124); m_u32(&m, 0x11, 1);
    if (cl_req(&c, &m, rsp, sizeof(rsp), &res, &err) >= 0 && !res) {
        uint8_t ind[4096]; ssize_t n = cl_wait_ind(&c, 0x24, ind, sizeof(ind), 5000);
        uint16_t l; const uint8_t *a = n > 0 ? tlv(ind, n, 0x11, &l) : NULL;
        if (!a || l < 1) out("PDC_LIST SW none (n=%zd)", n);
        else {
            int cnt = a[0]; size_t o = 1; out("PDC_LIST SW count=%d", cnt);
            for (int i = 0; i < cnt && o + 5 <= l && i < 64; i++) {
                uint32_t t = le32(a + o); int idl = a[o + 4];
                if (o + 5 + idl > l) break;
                pdc_config_info(&c, t, a + o + 5, idl, 0xA6200 + i);
                o += 5 + idl;
            }
        }
    } else out("PDC_LIST request res=%u err=%u", res, err);
    cl_close(&c);
}

static void probe_nas(void) {
    struct cl c; if (cl_open(&c, 3, "NAS") < 0) return;
    struct msg m; uint8_t rsp[4096]; uint16_t res, err; ssize_t n;
    m_init(&m, 0, 0x4D);
    if ((n = cl_req(&c, &m, rsp, sizeof(rsp), &res, &err)) > 0) {
        out("NAS_SYSINFO res=%u err=%u", res, err);
        uint16_t l; const uint8_t *p;
        if ((p = tlv(rsp, n, 0x14, &l)) && l >= 2) out("NAS_SYSINFO lte_srv_status=%u true=%u (2=full service)", p[0], p[1]);
        if ((p = tlv(rsp, n, 0x29, &l)) && l >= 1) out("NAS_SYSINFO ims_voice_support=%u (TLV 0x29 IMS Voice Support, boolean: 1 = the LTE network indicates IMS voice over PS, 0 = not indicated/not supported)", p[0]);
        else out("NAS_SYSINFO ims_voice_support=absent");
        if ((p = tlv(rsp, n, 0x2A, &l)) && l >= 4) out("NAS_SYSINFO lte_voice_domain=%u (TLV 0x2A LTE Voice Domain: 0 none, 1 IMS, 2 1X, 3 3GPP CS; libqmi QmiNasLteVoiceDomain)", le32(p));
        else out("NAS_SYSINFO lte_voice_domain=absent");
        dump_tlvs("NAS_SYSINFO", rsp, n);
    }
    cl_close(&c);
}

static void probe_wds(void) {
    struct cl c; if (cl_open(&c, 1, "WDS") < 0) return;
    struct msg m; uint8_t rsp[4096]; uint16_t res, err; ssize_t n;
    m_init(&m, 0, 0x2A); m_u8(&m, 0x10, 0);
    if ((n = cl_req(&c, &m, rsp, sizeof(rsp), &res, &err)) <= 0 || res) { out("WDS_PROFILES res=%u err=%u", res, err); cl_close(&c); return; }
    uint16_t l; const uint8_t *a = tlv(rsp, n, 0x01, &l);
    uint8_t idx[64]; int cnt = 0;
    if (a && l >= 1) {
        size_t o = 1;
        for (int i = 0; i < a[0] && o + 3 <= l && cnt < 64; i++) {
            int nl = a[o + 2]; if (o + 3 + nl > l) break;
            char name[256]; memcpy(name, a + o + 3, nl); name[nl] = 0;
            out("WDS_PROFILE type=%u index=%u name='%s'", a[o], a[o + 1], name);
            idx[cnt++] = a[o + 1]; o += 3 + nl;
        }
    }
    for (int i = 0; i < cnt; i++) {
        uint8_t v[2] = { 0, idx[i] };
        m_init(&m, 0, 0x2B); m_tlv(&m, 0x01, v, 2);
        if ((n = cl_req(&c, &m, rsp, sizeof(rsp), &res, &err)) <= 0) continue;
        const uint8_t *p; char apn[160] = ""; int pdp = -1;
        if ((p = tlv(rsp, n, 0x14, &l))) { int k = l < 159 ? l : 159; memcpy(apn, p, k); apn[k] = 0; }
        if ((p = tlv(rsp, n, 0x11, &l)) && l >= 1) pdp = p[0];
        out("WDS_PROFILE_SETTINGS index=%u res=%u apn='%s' pdp_type=%d", idx[i], res, apn, pdp);
        char tag[48]; snprintf(tag, sizeof(tag), "WDS_PROFILE_%u", idx[i]); dump_tlvs(tag, rsp, n);
    }
    cl_close(&c);
}

static void probe_simple(uint32_t service, const char *name, uint16_t id, const char *tag) {
    struct cl c; if (cl_open(&c, service, name) < 0) return;
    struct msg m; uint8_t rsp[4096]; uint16_t res, err; ssize_t n;
    m_init(&m, 0, id);
    if ((n = cl_req(&c, &m, rsp, sizeof(rsp), &res, &err)) > 0) { out("%s res=%u err=%u", tag, res, err); dump_tlvs(tag, rsp, n); }
    cl_close(&c);
}

static void imsa_watch(int secs) {
    struct cl c; if (cl_open(&c, 33, "IMSA") < 0) return;
    struct msg m; uint8_t rsp[4096]; uint16_t res, err;
    m_init(&m, 0, 0x22); m_u8(&m, 0x10, 1); m_u8(&m, 0x11, 1);
    if (cl_req(&c, &m, rsp, sizeof(rsp), &res, &err) >= 0) out("IMSA_REGISTER_IND res=%u err=%u", res, err);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        ssize_t n = cl_recv(&c, rsp, sizeof(rsp), 1000);
        if (n >= 7 && rsp[0] == 0x04) { char tag[48]; snprintf(tag, sizeof(tag), "IMSA_IND msg=0x%04x", le16(rsp + 3)); dump_tlvs(tag, rsp, n); }
    }
    cl_close(&c);
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "all";
    out("START cmd=%s (read-only queries)", cmd);
    if (do_lookup() < 0) { out("FAIL no QRTR"); return 2; }
    if (!strcmp(cmd, "lookup")) probe_lookup();
    else if (!strcmp(cmd, "pdc")) probe_pdc();
    else if (!strcmp(cmd, "imsa-watch")) imsa_watch(argc > 2 ? atoi(argv[2]) : 60);
    else if (!strcmp(cmd, "all")) {
        probe_lookup();
        probe_pdc();
        probe_nas();
        probe_wds();
        probe_simple(42, "DSD", 0x24, "DSD_SYSTEM_STATUS");
        probe_simple(33, "IMSA", 0x20, "IMSA_REG_STATUS");
        probe_simple(33, "IMSA", 0x21, "IMSA_SERVICES_STATUS");
        probe_simple(18, "IMSS", 0x90, "IMSS_SERVICES_ENABLED");
        probe_simple(31, "IMSP", 0x24, "IMSP_ENABLER_STATE");
    } else { out("usage: volte-probe [all|lookup|pdc|imsa-watch <secs>]"); return 1; }
    out("DONE");
    return 0;
}
