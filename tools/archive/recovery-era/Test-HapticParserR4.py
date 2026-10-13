"""Compile and execute the exact candidate parser, including error propagation."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/haptics-brake-prep-20260918-r4'
source=(OUT/'candidate.c').read_text()
a=source.index('\thaptics->brake_pat[0] = 0x3;')
b=source.index('\n\thaptics->current_limit =',a)
block=source[a:b]
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
typedef uint32_t u32;
struct property { const unsigned char *data; int length; };
struct device_node { struct property *prop; };
struct test_haptics { unsigned char brake_pat[4]; };
static struct property *of_find_property(struct device_node *n,const char *s,int *len) {
    (void)s; if (!n->prop) return NULL; *len=n->prop->length; return n->prop;
}
static int of_property_read_u32_array(struct device_node *n,const char *s,u32 *v,int count) {
    (void)s; if (!n->prop) return -EINVAL;
    if (!n->prop->data) return -ENODATA;
    if (n->prop->length<count*4) return -EOVERFLOW;
    for(int i=0;i<count;i++) {const unsigned char *d=n->prop->data+4*i; v[i]=((u32)d[0]<<24)|((u32)d[1]<<16)|((u32)d[2]<<8)|d[3];} return 0;
}
static int of_property_read_u8_array(struct device_node *n,const char *s,unsigned char *v,int count) {
    (void)s; if (!n->prop) return -EINVAL;
    if (!n->prop->data) return -ENODATA;
    if(n->prop->length<count)return -EOVERFLOW;
    memcpy(v,n->prop->data,count);return 0;
}
#define dev_err(...) ((void)0)
static int actual_parser(struct device_node *node,struct test_haptics *haptics) {
    int ret=0,i;
'''
suffix=r'''
    return 0;
register_fail:
    return ret;
}
static int cases=0;
static void check(int present,const unsigned char *data,int length,int expected,const unsigned char *value) {
    struct property p={data,length}; struct device_node n={present?&p:NULL};struct test_haptics h;
    memset(&h,0xff,sizeof h);int r=actual_parser(&n,&h);assert(r==expected);
    if(value) { assert(!memcmp(h.brake_pat,value,4)); }
    cases++;
}
int main(void) {
    unsigned char good4[]={3,3,0,0}, defaults[]={3,3,2,1};
    unsigned char data[24]={0,0,0,3,0,0,0,3,0,0,0,0,0,0,0,0};
    check(0,NULL,0,0,defaults);check(1,good4,4,0,good4);check(1,data,16,0,good4);
    int lengths[]={0,1,3,5,8,12,20};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++)check(1,data,lengths[i],-EOVERFLOW,NULL);
    check(1,NULL,4,-ENODATA,NULL);check(1,NULL,16,-ENODATA,NULL);
    for(int i=0;i<4;i++){
        unsigned char invalid4[4]={3,3,0,0};invalid4[i]=4;check(1,invalid4,4,-ERANGE,NULL);
        unsigned char invalid16[16];memcpy(invalid16,data,16);invalid16[4*i+3]=4;check(1,invalid16,16,-ERANGE,NULL);
    }
    printf("A6L_EXACT_C_PARSER_PASS cases=%d\n",cases);return 0;
}
'''
harness=OUT/'actual-parser-test.c';harness.write_text(prefix+block+suffix)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(harness),'-o',str(OUT/'actual-parser-test')],check=True)
p=subprocess.run([str(OUT/'actual-parser-test')],capture_output=True,text=True,check=True)
assert p.stdout.strip()=='A6L_EXACT_C_PARSER_PASS cases=20',p.stdout
report={'passed':True,'cases':20,'stdout':p.stdout,'method':'Exact candidate C parser including defaults and register_fail decision; OF access stubs','candidate_sha256':hashlib.sha256(source.encode()).hexdigest()}
(OUT/'parser-behavior.json').write_text(json.dumps(report,indent=2)+'\n')
print(p.stdout.strip())
