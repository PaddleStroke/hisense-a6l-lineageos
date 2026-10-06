/* host-test stub (r5 F7) */
#pragma once
#include <stdio.h>
#define ALOGI(...) (fprintf(stderr, "I " __VA_ARGS__), fputc('\n', stderr))
#define ALOGW(...) (fprintf(stderr, "W " __VA_ARGS__), fputc('\n', stderr))
#define ALOGE(...) (fprintf(stderr, "E " __VA_ARGS__), fputc('\n', stderr))
#define ALOGD(...) ((void)0)
#define ALOGV(...) ((void)0)
#define LOG_ALWAYS_FATAL(...) (fprintf(stderr, "F " __VA_ARGS__), abort())
#include <stdlib.h>
