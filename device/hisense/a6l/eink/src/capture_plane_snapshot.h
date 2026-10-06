// SPDX-License-Identifier: Apache-2.0
#ifndef A6L_CAPTURE_PLANE_SNAPSHOT_H
#define A6L_CAPTURE_PLANE_SNAPSHOT_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <xf86drmMode.h>
/* Each returned property array is one kernel modeset-lock transaction. This
 * is neither a multi-plane atomic snapshot nor ownership of producer pixels. */
enum a6l_pf { PF_FB, PF_CRTC, PF_CX, PF_CY, PF_CW, PF_CH, PF_SX, PF_SY,
 PF_SW, PF_SH, PF_ZPOS, PF_ROT, PF_ALPHA, PF_BLEND, PF_ACTIVE, PF_MODE, PF_N };
static const char *const a6l_pf_names[PF_N] = {"FB_ID","CRTC_ID","CRTC_X","CRTC_Y",
 "CRTC_W","CRTC_H","SRC_X","SRC_Y","SRC_W","SRC_H","zpos","rotation",
 "alpha","pixel blend mode","ACTIVE","MODE_ID"};
struct a6l_object_tuple { uint32_t id, present; uint64_t v[PF_N]; };
struct a6l_plane_snapshot {
 struct a6l_object_tuple crtc, planes[16]; unsigned count;
 uint32_t x,y,width,height;
};
struct a6l_name_cache {uint32_t id;int field;};
static struct a6l_name_cache a6l_property_names[128];
static unsigned a6l_property_name_count;
static int a6l_property_cache_fd=-1;
static int a6l_property_field(int fd,uint32_t id) {
 if(fd!=a6l_property_cache_fd){a6l_property_name_count=0;a6l_property_cache_fd=fd;}
 for(unsigned i=0;i<a6l_property_name_count;i++)
  if(a6l_property_names[i].id==id)return a6l_property_names[i].field;
 drmModePropertyRes *p=drmModeGetProperty(fd,id);if(!p)return -2;
 int field=-1;
 for(int i=0;i<PF_N;i++)if(!strcmp(p->name,a6l_pf_names[i])){field=i;break;}
 drmModeFreeProperty(p);
 if(a6l_property_name_count<128)
  a6l_property_names[a6l_property_name_count++]=(struct a6l_name_cache){id,field};
 return field;
}
static int a6l_object_tuple_read(int fd,uint32_t id,uint32_t type,
                                uint32_t required,unsigned zpos,
                                struct a6l_object_tuple *out) {
 drmModeObjectProperties *p=drmModeObjectGetProperties(fd,id,type);
 if(!p)return -1;
 struct a6l_object_tuple t={.id=id};
 t.v[PF_ZPOS]=zpos;t.v[PF_ROT]=1;t.v[PF_ALPHA]=0xffff;t.v[PF_BLEND]=0;
 int ok=1;
 for(unsigned i=0;i<p->count_props;i++) {
  int field=a6l_property_field(fd,p->props[i]);
  if(field==-2){ok=0;break;}
  if(field<0)continue;
  uint32_t bit=1u<<field;
  if(t.present&bit){ok=0;break;}
  t.present|=bit;t.v[field]=p->prop_values[i];
 }
 drmModeFreeObjectProperties(p);
 if(!ok||(t.present&required)!=required){errno=EPROTO;return -1;}
 *out=t;return 0;
}
static int a6l_plane_snapshot_read(int fd,uint32_t crtc,
                                   struct a6l_plane_snapshot *out) {
 struct a6l_plane_snapshot s={0};
 drmModeCrtc *c=drmModeGetCrtc(fd,crtc);if(!c)return -1;
 int valid=c->mode_valid;s.x=c->x;s.y=c->y;
 s.width=c->mode.hdisplay;s.height=c->mode.vdisplay;
 drmModeFreeCrtc(c);
 if(a6l_object_tuple_read(fd,crtc,DRM_MODE_OBJECT_CRTC,
     (1u<<PF_ACTIVE)|(1u<<PF_MODE),0,&s.crtc))return -1;
 if(!valid||!s.crtc.v[PF_ACTIVE])return 1; /* actual inactive CRTC */
 if(!s.crtc.v[PF_MODE]||!s.width||!s.height){errno=EPROTO;return -1;}
 drmModePlaneRes *p=drmModeGetPlaneResources(fd);if(!p)return -1;
 if(p->count_planes>16){drmModeFreePlaneResources(p);errno=E2BIG;return -1;}
 s.count=p->count_planes;
 int ok=1;
 for(unsigned i=0;i<s.count;i++)
  if(a6l_object_tuple_read(fd,p->planes[i],DRM_MODE_OBJECT_PLANE,
      (1u<<10)-1,i,&s.planes[i])){ok=0;break;}
 drmModeFreePlaneResources(p);if(!ok)return -1;
 *out=s;return 0;
}
static int a6l_object_tuple_equal(const struct a6l_object_tuple *a,
                                  const struct a6l_object_tuple *b) {
 return a->id==b->id&&a->present==b->present&&!memcmp(a->v,b->v,sizeof a->v);
}
static int a6l_plane_snapshot_equal(const struct a6l_plane_snapshot *a,
                                    const struct a6l_plane_snapshot *b) {
 if(a->count!=b->count||a->x!=b->x||a->y!=b->y||
    a->width!=b->width||a->height!=b->height||
    !a6l_object_tuple_equal(&a->crtc,&b->crtc))return 0;
 for(unsigned i=0;i<a->count;i++)
  if(!a6l_object_tuple_equal(&a->planes[i],&b->planes[i]))return 0;
 return 1;
}
#endif
