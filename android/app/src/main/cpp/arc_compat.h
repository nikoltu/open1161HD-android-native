// Forced include for deps/unrar on Android: bionic declares lutimes only from
// API 26 (minSdk is 24). unrar calls it only when restoring symlinks, which the
// importer never extracts; arc_import_jni.cpp defines it via utimensat.
#ifndef OPENBW_ARC_COMPAT_H
#define OPENBW_ARC_COMPAT_H
#if defined(__ANDROID__) && __ANDROID_API__ < 26
#include <sys/time.h>
#ifdef __cplusplus
extern "C"
#endif
int lutimes(const char* path, const struct timeval tv[2]);
#endif
#endif
