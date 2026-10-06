#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
int kd_probe(void); void kd_remove(void); int kd_led_registered(void); const char *kd_led_name(void); int kd_gpio(void);
unsigned long kd_on_count(void); int kd_has_attr(const char *name); ssize_t kd_store(const char *name, const char *buf, size_t n);
int kd_show(const char *name, char *out); void kd_advance(long ms); int kd_timed_pending(void); int kd_suspend(void);
int kd_resume(void); int kd_brightness(int b); void kd_ff(int on); void kd_set_led_register_err(int e);
#ifdef __cplusplus
}
#endif
