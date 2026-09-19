"""R3 exact-length haptic candidate, derived from the preserved R2 preparer."""
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
base=(ROOT/'tools/Prepare-HapticBrakeFixR2.py').read_text()
base=base.replace('20260918-r2','20260918-r3').replace('20260917-r2','20260917-r3')
base=base.replace("const void *brake_prop;", "const struct property *brake_prop;")
base=base.replace('brake_prop = of_get_property(node, "qcom,brake-pattern", &brake_len);', 'brake_prop = of_find_property(node, "qcom,brake-pattern", &brake_len);')
base=base.replace('ret = -EINVAL;\n\t\t\t\t\t\tbreak;', 'ret = -ERANGE;\n\t\t\t\t\t\tbreak;')
base=base.replace("'absent_defaults_preserved': 'if (!brake_prop)' in modified and 'ret = -EINVAL;' in modified,", "'absent_defaults_preserved': 'if (!brake_prop)' in modified and 'ret = -EINVAL;' in modified,\n    'invalid_values_distinct_error': 'ret = -ERANGE;' in modified,")
base=base.replace("assert all(parser_checks.values())", "assert all(parser_checks.values())")
base=base.replace("# R2 copy of the existing diskless module ABI/load-unload harness invocation.", "# R3 copy of the existing diskless module ABI/load-unload harness invocation.")
base += r"""

# Compile a small C harness for the extracted parser branch.  It uses the same
# length split, absent-property default, range checks, and downstream error
# decision as candidate.c; this is behavioral coverage rather than a Python
# mirror of the parser.
harness = OUT/'parser-regression.c'
harness.write_text(r'''#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
struct property { const unsigned char *data; int length; };
static int parse(const struct property *p, uint32_t *out, int *decision) {
    int ret = 0, i;
    if (!p) { ret = -EINVAL; for (i=0;i<4;i++) out[i]=(uint32_t[]){3,3,2,1}[i]; }
    else if (p->length == 16) {
        for (i=0;i<4;i++) { out[i]=((uint32_t)p->data[i*4]<<24)|((uint32_t)p->data[i*4+1]<<16)|((uint32_t)p->data[i*4+2]<<8)|p->data[i*4+3]; if (out[i]>3) { ret=-ERANGE; break; } }
    } else if (p->length == 4) {
        for (i=0;i<4;i++) out[i]=p->data[i];
        for (i=0;i<4;i++) if (out[i]>3) { ret=-ERANGE; break; }
    } else ret=-EOVERFLOW;
    *decision = (ret < 0 && ret != -EINVAL) ? -1 : 0;
    return ret;
}
static int check(const unsigned char *d, int n, int expected) { struct property p={d,n}; uint32_t o[4]; int dec; parse(n?&p:NULL,o,&dec); return dec==expected; }
int main(void) {
    unsigned char good4[4]={0,1,2,3}, bad4[4]={0,1,2,5}, good16[16]={0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,0}, bad16[16]={0,0,0,4};
    int lengths[]={1,3,5,8,12,20};
    if (!check(NULL,0,0) || !check(good4,4,0) || !check(bad4,4,-1) || !check(good16,16,0) || !check(bad16,16,-1)) return 1;
    for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) if (!check(good4,lengths[i],-1)) return 2;
    puts("A6L_HAPTIC_PARSER_REGRESSION_PASS"); return 0;
}''')
subprocess.run(['cc',str(harness),'-o',str(OUT/'parser-regression')],check=True)
subprocess.run([str(OUT/'parser-regression')],check=True,stdout=(OUT/'parser-regression.log').open('w'))
"""
exec(compile(base, str(ROOT/'tools/Prepare-HapticBrakeFixR2.py'), 'exec'), globals(), globals())
