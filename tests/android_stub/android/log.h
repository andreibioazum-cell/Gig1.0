#ifndef ANDROID_LOG_STUB
#define ANDROID_LOG_STUB
#include <stdarg.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
static inline int __android_log_print(int p, const char *t, const char *f, ...) { (void)p;(void)t;(void)f; return 0; }
static inline int __android_log_vprint(int p, const char *t, const char *f, va_list a) {
    (void)p; (void)t; (void)f; (void)a; return 0;
}
#endif
