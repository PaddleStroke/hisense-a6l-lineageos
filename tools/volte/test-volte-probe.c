#define main probe_main
#include "volte-probe.c"
#undef main
#include <assert.h>
int main(void) {
    struct msg m; m_init(&m, 7, 0x22); m_u32(&m, 0x01, 1); m_u32(&m, 0x10, 0xA6101);
    /* header: type 0, txn 7, id 0x22, len 14 */
    static const uint8_t exp[] = {0,7,0,0x22,0,14,0, 1,4,0,1,0,0,0, 0x10,4,0,0x01,0x61,0x0a,0};
    assert(m.n == sizeof(exp) && !memcmp(m.b, exp, sizeof(exp)));
    /* fake PDC Get Selected Config indication with active id "ab01" */
    static const uint8_t ind[] = {4,0,0,0x22,0, 21,0, 0x10,4,0,1,0x61,0x0a,0, 0x01,2,0,0,0, 0x11,3,0,2,0xab,0x01};
    uint16_t l; const uint8_t *p = tlv(ind, sizeof(ind), 0x11, &l);
    assert(p && l == 3 && p[0] == 2 && p[1] == 0xab);
    assert(tlv(ind, sizeof(ind), 0x12, &l) == NULL);
    /* truncated TLV must not be returned */
    static const uint8_t bad[] = {4,0,0,0x22,0,5,0, 0x11,9,0,1,2};
    assert(tlv(bad, sizeof(bad), 0x11, &l) == NULL);
    char ids[16]; put_id(ids, sizeof(ids), p + 1, 2); assert(!strcmp(ids, "ab01"));
    dump_tlvs("TEST", ind, sizeof(ind));
    puts("volte-probe unit tests PASS");
    return 0;
}
