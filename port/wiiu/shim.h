// Force-included for the Wii U build: small stand-ins for macOS-only calls in the runtime.
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <pthread.h>
#define pthread_setname_np(name) ((void)0)
#define pthread_set_qos_class_self_np(a, b) ((void)0)
#define QOS_CLASS_USER_INTERACTIVE 0
#define QOS_CLASS_USER_INITIATED 0
#define QOS_CLASS_DEFAULT 0
#endif
