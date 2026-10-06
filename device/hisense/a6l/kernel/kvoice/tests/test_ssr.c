// SPDX-License-Identifier: GPL-2.0
/*
 * r5 bug hunt round2 audio (29 Sep 2026): host test of sound/soc/qcom/qdsp6/q6voice-common.c across an ADSP restart.
 * The real source (extracted from the kernel series by run-tests.sh) is compiled against stub kernel headers; devm
 * memory is freed at "unbind" and the APR device when its last reference goes, like the kernel, so a use after the
 * service removal is an AddressSanitizer error. Sequence of apr_remove(): CVP, CVS, MVM (overlay child order) while a
 * call owns MVM and CVP sessions; afterwards the voice path is stopped and destroyed (q6voice_path_stop/destroy).
 */
#include "q6voice-common.c"

static int g_pass, g_fail;
#define EXPECT(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct fake_apr { struct apr_device adev; bool gone; };	/* gone: the APR router of this device was removed */
static int sends, sends_to_gone, released;

static void apr_dev_release(struct device *d)
{
	released++;
	free(container_of(container_of(d, struct apr_device, dev), struct fake_apr, adev));
}

static struct fake_apr *apr_register(int id)
{
	struct fake_apr *a = calloc(1, sizeof(*a));

	a->adev.id = id;
	a->adev.dev.refs = 1;
	a->adev.dev.release = apr_dev_release;
	return a;
}

/* apr_remove_device(): driver remove, devres released, registration reference dropped */
static void apr_unregister(struct fake_apr *a)
{
	a->gone = true;
	q6voice_common_remove(&a->adev);
	devres_release_all(&a->adev.dev);
	put_device(&a->adev.dev);
}

int apr_send_pkt(struct apr_device *adev, struct apr_pkt *pkt)
{
	struct fake_apr *a = container_of(adev, struct fake_apr, adev);
	struct aprv2_ibasic_rsp_result_t r = { pkt->hdr.opcode, 0 };
	struct apr_resp_pkt resp = { .payload = &r };

	if (a->gone) {		/* kernel: adev->dev.parent drvdata (the router) is freed memory */
		sends_to_gone++;
		return -EIO;
	}
	sends++;
	resp.hdr.opcode = APR_BASIC_RSP_RESULT;
	resp.hdr.dest_port = pkt->hdr.src_port;
	resp.hdr.src_port = pkt->hdr.dest_port ? pkt->hdr.dest_port : 0x55;	/* handle of a new session */
	q6voice_common_callback(adev, &resp);
	return pkt->hdr.pkt_size;
}

static int cmd(struct q6voice_session *s, u32 opcode)
{
	struct apr_pkt p = { .hdr = { .pkt_size = sizeof(struct apr_hdr), .opcode = opcode } };

	return q6voice_common_send(s, &p.hdr);
}

static struct q6voice_session *create(enum q6voice_service_type t, enum q6voice_path_type path)
{
	struct apr_pkt p = { .hdr = { .pkt_size = sizeof(struct apr_hdr), .opcode = 0x100 + t } };

	return q6voice_session_create(t, path, &p.hdr);
}

int main(void)
{
	struct fake_apr *mvm = apr_register(9), *cvs = apr_register(10), *cvp = apr_register(11), *cvp2;
	struct q6voice_session *sm, *sp, *busy;

	EXPECT(q6voice_common_probe(&mvm->adev, Q6VOICE_SERVICE_MVM) == 0);
	EXPECT(q6voice_common_probe(&cvs->adev, Q6VOICE_SERVICE_CVS) == 0);
	EXPECT(q6voice_common_probe(&cvp->adev, Q6VOICE_SERVICE_CVP) == 0);

	/* call: MVM + CVP sessions of VoiceMMode1 (q6voice_path_start) */
	sm = create(Q6VOICE_SERVICE_MVM, Q6VOICE_PATH_VOICEMMODE1);
	sp = create(Q6VOICE_SERVICE_CVP, Q6VOICE_PATH_VOICEMMODE1);
	EXPECT(!IS_ERR(sm) && !IS_ERR(sp) && sp->handle == 0x55);
	EXPECT(cmd(sp, 0x100C6) == 0 && cmd(sm, 0x1100C) == 0);
	busy = create(Q6VOICE_SERVICE_CVP, Q6VOICE_PATH_VOICEMMODE1);	/* second session on the same path */
	EXPECT(IS_ERR(busy) && PTR_ERR(busy) == -EBUSY);

	/* ADSP restart: apr_remove() takes CVP and CVS first, the call still owns its sessions */
	apr_unregister(cvp);
	apr_unregister(cvs);
	EXPECT(released == 1);				/* CVS (no session) gone, CVP pinned by its session */
	/* in-call commands (mute/volume/stop) on the removed CVP: refused, nothing reaches the removed router */
	EXPECT(cmd(sp, 0x0001138B) == -ENODEV);
	EXPECT(sends_to_gone == 0);
	/* a new CVP session cannot be created while no CVP service exists */
	busy = create(Q6VOICE_SERVICE_CVP, Q6VOICE_PATH_VOICE);
	EXPECT(IS_ERR(busy) && PTR_ERR(busy) == -ENODEV);

	/* MVM goes last; ASoC waits for userspace to close the PCMs, then the path is stopped and destroyed */
	apr_unregister(mvm);
	EXPECT(cmd(sm, 0x000110FF) == -ENODEV);	/* STOP_VOICE */
	EXPECT(cmd(sp, 0x00011357) == -ENODEV);	/* DISABLE */
	q6voice_session_release(sm);			/* DESTROY_SESSION refused, memory released */
	q6voice_session_release(sp);
	EXPECT(sends_to_gone == 0);
	EXPECT(released == 3);				/* every APR device freed once, after its last user */

	/* the ADSP is back: services probe again (no stale -EEXIST) and a new call works */
	cvp2 = apr_register(11);
	EXPECT(q6voice_common_probe(&cvp2->adev, Q6VOICE_SERVICE_CVP) == 0);
	sp = create(Q6VOICE_SERVICE_CVP, Q6VOICE_PATH_VOICEMMODE1);
	EXPECT(!IS_ERR(sp) && cmd(sp, 0x100C6) == 0);
	q6voice_session_release(sp);
	/* normal unbind without sessions frees immediately */
	apr_unregister(cvp2);
	EXPECT(released == 4 && sends_to_gone == 0);
	printf("A6L_Q6VOICE_SSR_TESTS %s pass=%d fail=%d sends=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail, sends);
	return g_fail ? 1 : 0;
}
