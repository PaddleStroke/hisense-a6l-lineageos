// SPDX-License-Identifier: GPL-2.0
/*
 * btcall (29 Sep 2026): host test of the in-call record / in-call music state machine of the q6voice series
 * (kvoice/a6l-btcall-incall-v75.patch). The real q6voice.c and q6cvs.c (rebuilt from the series patches by
 * run-btcall-tests.sh) are compiled against stub kernel headers; MVM/CVP and q6voice-common are stubbed and every
 * CVS packet is recorded. Checks the APR payloads against msm-4.4 voice.c/q6voice.h (opcodes, tap points, port ids,
 * packed sizes, passive session name) and the ordering rules: requests before a call are applied after MVM start,
 * record/playback stop before MVM stop, a record mode change is stop + start, DSP errors never fail the call.
 */
#include "q6voice.c"
#include "q6cvs.c"

static int g_pass, g_fail;
#define EXPECT(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- recorded command log ---- */
struct ev { char what[16]; u32 opcode; u32 a, b, c, d; u16 size; };
static struct ev log_[256];
static int nlog;
static void ev(const char *w, u32 op, u32 a, u32 b, u32 c, u32 d, u16 size)
{
	struct ev *e = &log_[nlog++];

	snprintf(e->what, sizeof(e->what), "%s", w);
	e->opcode = op; e->a = a; e->b = b; e->c = c; e->d = d; e->size = size;
}
static int find(const char *w, u32 op, int from)
{
	for (int i = from; i < nlog; i++)
		if (!strcmp(log_[i].what, w) && (!op || log_[i].opcode == op))
			return i;
	return -1;
}
static int count(const char *w, u32 op)
{
	int n = 0;

	for (int i = 0; i < nlog; i++)
		n += !strcmp(log_[i].what, w) && (!op || log_[i].opcode == op);
	return n;
}

/* ---- stubs: q6afe, q6mvm, q6cvp, q6voice-common ---- */
int q6afe_get_port_id(int index) { return 0x1000 + index; }
static char mvm_name[24] = "11C05000";
static int cvs_fail_create;	/* the next CVS session create fails */
static int cvs_fail_next;	/* DSP error for the next CVS command */
static int sessions_live;

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path)
{
	struct q6voice_session *s = calloc(1, sizeof(*s));

	s->port = path; s->handle = 0x100; sessions_live++;
	ev("mvm_create", 0, path, 0, 0, 0, 0);
	return s;
}
int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp, bool state) { ev("mvm_attach", 0, state, 0, 0, 0, 0); return 0; }
int q6mvm_start(struct q6voice_session *mvm, bool state) { ev(state ? "mvm_start" : "mvm_stop", 0, 0, 0, 0, 0, 0); return 0; }
int q6mvm_get_cvd_version(char *buf, size_t len) { snprintf(buf, len, "2.2"); return 0; }
int q6mvm_get_session_name(enum q6voice_path_type path, char *buf, size_t len)
{
	if (path != Q6VOICE_PATH_VOICEMMODE1)
		return -EINVAL;
	snprintf(buf, len, "%s", mvm_name);
	return 0;
}
static struct q6voice_session *cvp_new(void)
{
	struct q6voice_session *s = calloc(1, sizeof(*s));

	s->handle = 0x200; sessions_live++;
	ev("cvp_create", 0, 0, 0, 0, 0, 0);
	return s;
}
struct q6voice_session *q6cvp_session_create(enum q6voice_path_type p, u16 tx, u16 rx, u32 tt, u32 rt) { return cvp_new(); }
struct q6voice_session *q6cvp_session_create_v3(enum q6voice_path_type p, u16 tx, u16 rx, u32 tt, u32 rt) { return cvp_new(); }
int q6cvp_send_channel_info(struct q6voice_session *cvp, bool is_tx) { return 0; }
int q6cvp_send_media_format(struct q6voice_session *cvp, int port_id, bool is_tx) { return 0; }
int q6cvp_topology_commit(struct q6voice_session *cvp) { return 0; }
int q6cvp_enable(struct q6voice_session *cvp, bool enable) { ev(enable ? "cvp_enable" : "cvp_disable", 0, 0, 0, 0, 0, 0); return 0; }
int q6cvp_set_mute(struct q6voice_session *cvp, bool tx, bool mute, u16 ramp_ms) { return 0; }
int q6cvp_set_rx_vol_step(struct q6voice_session *cvp, u32 step, u16 ramp_ms) { return 0; }

/* CVS session create + every CVS command; payload decoded like the ADSP would */
struct q6voice_session *q6voice_session_create(enum q6voice_service_type type, enum q6voice_path_type path,
					       struct apr_hdr *hdr)
{
	struct q6voice_session *s;
	char *name = (char *)(hdr + 1);

	EXPECT(type == Q6VOICE_SERVICE_CVS);
	EXPECT(hdr->opcode == 0x00011140);	/* VSS_ISTREAM_CMD_CREATE_PASSIVE_CONTROL_SESSION */
	EXPECT(hdr->pkt_size == APR_HDR_SIZE + 20);
	ev("cvs_create", hdr->opcode, path, 0, 0, 0, hdr->pkt_size);
	EXPECT(!strcmp(name, mvm_name));
	if (cvs_fail_create) {
		cvs_fail_create = 0;
		return ERR_PTR(-EIO);
	}
	s = calloc(1, sizeof(*s));
	s->port = path; s->handle = 0x300; sessions_live++;
	return s;
}
void q6voice_session_release(struct q6voice_session *s) { ev("release", 0, s->handle, 0, 0, 0, 0); sessions_live--; free(s); }
int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr)
{
	u8 *p = (u8 *)(hdr + 1);
	u32 a = 0, b = 0, c = 0, d = 0;

	EXPECT(s->handle == 0x300);	/* only CVS commands reach this stub */
	switch (hdr->opcode) {
	case 0x000112BE:	/* VSS_IRECORD_CMD_START: u32 rx_tap, u32 tx_tap, u16 port_id, u32 mode (packed) */
		memcpy(&a, p, 4); memcpy(&b, p + 4, 4); memcpy(&c, p + 8, 2); memcpy(&d, p + 10, 4);
		EXPECT(hdr->pkt_size == APR_HDR_SIZE + 14);
		break;
	case 0x000112BD:	/* VSS_IPLAYBACK_CMD_START: u16 port_id */
		memcpy(&a, p, 2);
		EXPECT(hdr->pkt_size == APR_HDR_SIZE + 2);
		break;
	case 0x00011237: case 0x00011239:	/* STOPs: header only */
		EXPECT(hdr->pkt_size == APR_HDR_SIZE);
		break;
	default:
		EXPECT(!"unexpected CVS opcode");
	}
	ev("cvs", hdr->opcode, a, b, c, d, hdr->pkt_size);
	if (cvs_fail_next) {
		cvs_fail_next = 0;
		return -EIO;
	}
	return 0;
}

#define REC_START 0x000112BE
#define REC_STOP 0x00011237
#define PLAY_START 0x000112BD
#define PLAY_STOP 0x00011239
#define TAP_NONE 0x00010F78
#define TAP_END 0x00010F79
#define P Q6VOICE_PATH_VOICEMMODE1

static struct device dev;

static void call_start(struct q6voice *v)
{
	EXPECT(q6voice_start(v, P, false) == 0);
	EXPECT(q6voice_start(v, P, true) == 0);
}
static void call_stop(struct q6voice *v)
{
	q6voice_stop(v, P, true);
	q6voice_stop(v, P, false);
}

int main(void)
{
	struct q6voice *v = q6voice_create(&dev, Q6VOICE_CVD_LEGACY);
	int i, j;

	/* packed payload sizes = msm-4.4 cvs_start_record_cmd / cvs_start_playback_cmd */
	EXPECT(sizeof(struct vss_irecord_cmd_start_cmd) == APR_HDR_SIZE + 14);
	EXPECT(sizeof(struct vss_iplayback_cmd_start_cmd) == APR_HDR_SIZE + 2);
	EXPECT(sizeof(struct vss_istream_cmd_create_passive_control_session_cmd) == APR_HDR_SIZE + 20);

	/* 1. no request: a call creates no CVS session and sends nothing to CVS */
	call_start(v);
	call_stop(v);
	EXPECT(count("cvs_create", 0) == 0 && count("cvs", 0) == 0);
	EXPECT(sessions_live == 0);

	/* 2. downlink record requested before the call: nothing now, at call start after MVM start */
	nlog = 0;
	EXPECT(q6voice_set_incall_record(v, P, false, true) == 0);
	EXPECT(count("cvs", 0) == 0);
	call_start(v);
	i = find("mvm_start", 0, 0); j = find("cvs", REC_START, 0);
	EXPECT(i >= 0 && j > i && find("cvs_create", 0, 0) > i);
	EXPECT(j >= 0 && log_[j].a == TAP_END && log_[j].b == TAP_NONE && log_[j].c == 0xFFFF && log_[j].d == 0x00010F7A);
	/* 3. in-call music during the call: VSS_IPLAYBACK_CMD_START port 0x8005, one CVS session */
	EXPECT(q6voice_set_incall_music(v, P, true) == 0);
	j = find("cvs", PLAY_START, 0);
	EXPECT(j >= 0 && log_[j].a == 0x8005);
	EXPECT(count("cvs_create", 0) == 1);
	/* idempotent: prepare may run twice */
	EXPECT(q6voice_set_incall_music(v, P, true) == 0);
	EXPECT(q6voice_set_incall_record(v, P, false, true) == 0);
	EXPECT(count("cvs", PLAY_START) == 1 && count("cvs", REC_START) == 1);
	/* 4. uplink added: record mode change = STOP then START rx+tx */
	nlog = 0;
	EXPECT(q6voice_set_incall_record(v, P, true, true) == 0);
	i = find("cvs", REC_STOP, 0); j = find("cvs", REC_START, 0);
	EXPECT(i >= 0 && j > i && log_[j].a == TAP_END && log_[j].b == TAP_END);
	/* 5. call end: playback + record stop BEFORE MVM stop; CVS session released with the path */
	nlog = 0;
	call_stop(v);
	i = find("cvs", PLAY_STOP, 0); j = find("cvs", REC_STOP, 0);
	EXPECT(i >= 0 && j >= 0 && i < find("mvm_stop", 0, 0) && j < find("mvm_stop", 0, 0));
	EXPECT(count("release", 0) == 3 && sessions_live == 0);
	/* 6. next call: the kept requests resume (rx+tx record, music) */
	nlog = 0;
	call_start(v);
	j = find("cvs", REC_START, 0);
	EXPECT(j >= 0 && log_[j].a == TAP_END && log_[j].b == TAP_END && find("cvs", PLAY_START, 0) >= 0);
	/* 7. requests withdrawn during the call (back ends shut down): STOPs, nothing left at call end */
	nlog = 0;
	EXPECT(q6voice_set_incall_music(v, P, false) == 0);
	EXPECT(q6voice_set_incall_record(v, P, true, false) == 0);	/* rx only: stop + start rx */
	j = find("cvs", REC_START, 0);
	EXPECT(find("cvs", PLAY_STOP, 0) >= 0 && j >= 0 && log_[j].a == TAP_END && log_[j].b == TAP_NONE);
	EXPECT(q6voice_set_incall_record(v, P, false, false) == 0);
	EXPECT(count("cvs", REC_STOP) == 2);
	nlog = 0;
	call_stop(v);
	EXPECT(count("cvs", 0) == 0 && sessions_live == 0);
	/* 8. DSP error on start during a call: returned to the caller, call unaffected, retried on the next request */
	call_start(v);
	nlog = 0;
	cvs_fail_create = 1;	/* CVS session create fails */
	EXPECT(q6voice_set_incall_record(v, P, false, true) == -EIO);
	EXPECT(count("cvs", 0) == 0);
	EXPECT(q6voice_set_incall_music(v, P, true) == 0);	/* retry creates the session and starts both */
	EXPECT(count("cvs", REC_START) == 1 && count("cvs", PLAY_START) == 1);
	call_stop(v);
	/* 9. error at call start is not fatal: the call starts, the record is not marked running */
	nlog = 0;
	cvs_fail_next = 1;	/* the record START is refused by the DSP, the music START succeeds */
	call_start(v);	/* EXPECTs start == 0 */
	EXPECT(count("cvs", REC_START) == 1 && count("cvs", PLAY_START) == 1);
	EXPECT(find("mvm_start", 0, 0) >= 0);
	nlog = 0;
	call_stop(v);	/* the failed record start is not stopped; music (started) is */
	EXPECT(count("cvs", REC_STOP) == 0 && count("cvs", PLAY_STOP) == 1);
	/* 10. mmode1_session override (q6mvm_get_session_name) also names the CVS session */
	snprintf(mvm_name, sizeof(mvm_name), "default volte voice");
	nlog = 0;
	call_start(v);
	EXPECT(count("cvs_create", 0) == 1);	/* name checked in the stub */
	call_stop(v);
	q6voice_set_incall_music(v, P, false);
	q6voice_set_incall_record(v, P, false, false);
	EXPECT(sessions_live == 0);
	/* 11. invalid path */
	EXPECT(q6voice_set_incall_record(v, Q6VOICE_PATH_COUNT, false, true) == -EINVAL);
	EXPECT(q6voice_set_incall_music(v, Q6VOICE_PATH_COUNT, true) == -EINVAL);

	printf("BTCALL_Q6VOICE_TEST %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
