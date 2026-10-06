/* host stub of <linux/soc/qcom/apr.h> for the q6voice-common SSR test */
#ifndef A6L_APR_STUB_H
#define A6L_APR_STUB_H
#include "../../kstub.h"
struct apr_hdr { u16 hdr_field, pkt_size; u8 src_svc, src_domain; u16 src_port; u8 dest_svc, dest_domain; u16 dest_port; u32 token, opcode; };
struct apr_pkt { struct apr_hdr hdr; };
struct apr_resp_pkt { struct apr_hdr hdr; void *payload; int payload_size; };
struct aprv2_ibasic_rsp_result_t { u32 opcode, status; };
struct apr_device { struct device dev; int id; };
#define APR_BASIC_RSP_RESULT 0x000110E8
#define APR_MSG_TYPE_SEQ_CMD 1
#define APR_HDR_SIZE sizeof(struct apr_hdr)
#define APR_HDR_LEN(l) ((l) / 4)
#define APR_PKT_VER 0
#define APR_HDR_FIELD(t, h, v) (((t) << 8) | ((h) << 4) | (v))
int apr_send_pkt(struct apr_device *adev, struct apr_pkt *pkt);	/* provided by the test */
#endif
