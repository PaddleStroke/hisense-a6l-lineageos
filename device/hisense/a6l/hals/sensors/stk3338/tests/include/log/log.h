/* host-test stub of <log/log.h> */
#pragma once
#include <stdio.h>
#ifndef LOG_TAG
#define LOG_TAG "?"
#endif
#define ALOGE(...) (fprintf(stderr, "E " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGW(...) (fprintf(stderr, "W " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGI(...) (fprintf(stderr, "I " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGD(...) ((void)0)
