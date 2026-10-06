/* host stub of <linux/soc/qcom/apr.h> for the btcall test */
#ifndef A6L_APR_STUB_BTCALL_H
#define A6L_APR_STUB_BTCALL_H
#include "../../kstub-btcall.h"
struct apr_hdr { u16 hdr_field, pkt_size; u8 src_svc, src_domain; u16 src_port; u8 dest_svc, dest_domain; u16 dest_port; u32 token, opcode; };
struct apr_pkt { struct apr_hdr hdr; };
struct apr_resp_pkt { struct apr_hdr hdr; void *payload; int payload_size; };
struct apr_device { struct device dev; int id; };
struct apr_driver { int (*probe)(struct apr_device *); void (*remove)(struct apr_device *);
	int (*callback)(struct apr_device *, const struct apr_resp_pkt *); struct { const char *name; const void *of_match_table; } driver; };
struct of_device_id { char compatible[128]; };
#define module_apr_driver(d) static struct apr_driver *a6l_unused_drv __attribute__((unused)) = &(d)
#define APR_HDR_SIZE sizeof(struct apr_hdr)
#endif
