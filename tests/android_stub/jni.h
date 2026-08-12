/* Заглушка jni.h — только для проверки компиляции Android-ветки net.c
 * на машине без NDK. В сборке APK используется настоящий заголовок NDK. */
#ifndef JNI_H_STUB
#define JNI_H_STUB

#include <stdarg.h>
#include <stdint.h>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef int32_t jint;
typedef int64_t jlong;
typedef jint jsize;

typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef jarray jbyteArray;
typedef jobject jthrowable;

typedef struct _jmethodID *jmethodID;
typedef struct _jfieldID *jfieldID;

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_VERSION_1_6 0x00010006

struct JNINativeInterface;
struct JNIInvokeInterface;

typedef const struct JNINativeInterface *JNIEnv;
typedef const struct JNIInvokeInterface *JavaVM;

struct JNINativeInterface {
    jclass (*FindClass)(JNIEnv *, const char *);
    jclass (*GetObjectClass)(JNIEnv *, jobject);
    jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*NewObject)(JNIEnv *, jclass, jmethodID, ...);
    jobject (*CallObjectMethod)(JNIEnv *, jobject, jmethodID, ...);
    void (*CallVoidMethod)(JNIEnv *, jobject, jmethodID, ...);
    jint (*CallIntMethod)(JNIEnv *, jobject, jmethodID, ...);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    jbyteArray (*NewByteArray)(JNIEnv *, jsize);
    void (*SetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *);
    void (*GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *);
    jboolean (*ExceptionCheck)(JNIEnv *);
    void (*ExceptionClear)(JNIEnv *);
    jint (*PushLocalFrame)(JNIEnv *, jint);
    jobject (*PopLocalFrame)(JNIEnv *, jobject);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
};

struct JNIInvokeInterface {
    jint (*GetEnv)(JavaVM *, void **, jint);
    jint (*AttachCurrentThread)(JavaVM *, JNIEnv **, void *);
    jint (*DetachCurrentThread)(JavaVM *);
};

#endif /* JNI_H_STUB */
