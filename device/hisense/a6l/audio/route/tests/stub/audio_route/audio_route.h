/* host-test stub (r5 F7): the subset of libaudioroute used by a6l_audio_route.c */
#pragma once
struct audio_route;
struct audio_route *audio_route_init(unsigned int card, const char *xml_path);
void audio_route_free(struct audio_route *ar);
void audio_route_reset(struct audio_route *ar);
int audio_route_apply_path(struct audio_route *ar, const char *name);
int audio_route_update_mixer(struct audio_route *ar);
