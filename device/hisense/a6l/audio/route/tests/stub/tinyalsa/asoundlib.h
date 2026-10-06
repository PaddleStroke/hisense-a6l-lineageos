/* host-test stub (r5 F7): the subset of tinyalsa used by a6l_audio_route.c */
#pragma once
struct mixer;
struct mixer_ctl;
enum mixer_ctl_type { MIXER_CTL_TYPE_BOOL, MIXER_CTL_TYPE_INT, MIXER_CTL_TYPE_ENUM, MIXER_CTL_TYPE_BYTE,
                      MIXER_CTL_TYPE_IEC958, MIXER_CTL_TYPE_INT64, MIXER_CTL_TYPE_UNKNOWN };
struct mixer *mixer_open(unsigned int card);
void mixer_close(struct mixer *mixer);
const char *mixer_get_name(struct mixer *mixer);
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *mixer, const char *name);
int mixer_ctl_get_value(const struct mixer_ctl *ctl, unsigned int id);
enum mixer_ctl_type mixer_ctl_get_type(const struct mixer_ctl *ctl);
const char *mixer_ctl_get_enum_string(struct mixer_ctl *ctl, unsigned int enum_id);
