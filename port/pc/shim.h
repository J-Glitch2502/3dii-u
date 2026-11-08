// Force-included for the PC build: small Linux stand-ins for macOS-only calls in the runtime.
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <pthread.h>
#define pthread_setname_np(name) pthread_setname_np(pthread_self(), name)
#define pthread_set_qos_class_self_np(a, b) ((void)0)
#define QOS_CLASS_USER_INTERACTIVE 0
#define QOS_CLASS_USER_INITIATED 0
#define QOS_CLASS_DEFAULT 0
#define MAP_ANON 0x20
#ifdef _WIN32
#include "compat.h"
#endif
#endif
