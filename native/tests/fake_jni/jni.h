/*
 * NOT a real JNI implementation, or even a real subset of one. This
 * exists solely so native_bridge_shim.c's *host* test build has
 * something to satisfy its `#include <jni.h>`, since the real one only
 * comes from the Android NDK (see docs/BUILDING.md), which this host
 * build deliberately isn't using, the same reason native_bridge_shim.c
 * itself isn't part of mango_core. Never included by the real Android
 * build: android.toolchain.cmake's own jni.h wins there since this
 * directory is never on that build's include path. Do not use this for
 * anything other than compiling native_bridge_shim.c's tests.
 */
#ifndef MANGO_TESTS_FAKE_JNI_H_
#define MANGO_TESTS_FAKE_JNI_H_
#define MANGO_FAKE_JNI 1

#include <stdint.h>

typedef void* jclass;
typedef void* jobject;
typedef void* jobjectArray;
typedef void* jstring;
typedef void* jthrowable;
typedef void* jmethodID;
typedef int32_t jint;
typedef int64_t jlong;
typedef uint8_t jboolean;
typedef union jvalue {
  jboolean z;
  jint i;
  jlong j;
  void* l;
} jvalue;
typedef struct {
  const char* name;
  const char* signature;
  void* fnPtr;
} JNINativeMethod;

struct JNINativeInterface;
typedef const struct JNINativeInterface* JNIEnv;

struct JNINativeInterface {
  void* reserved0;
  void* reserved1;
  void* reserved2;
  void* reserved3;
  jint (*GetVersion)(JNIEnv*);
  void* reserved_defineclass;
  jclass (*FindClass)(JNIEnv*, const char*);
  void* pad_a[8];
  jthrowable (*ExceptionOccurred)(JNIEnv*);
  void* reserved_describe;
  void (*ExceptionClear)(JNIEnv*);
  void* pad_b[3];
  jobject (*NewGlobalRef)(JNIEnv*, jobject);
  void (*DeleteGlobalRef)(JNIEnv*, jobject);
  void (*DeleteLocalRef)(JNIEnv*, jobject);
  jboolean (*IsSameObject)(JNIEnv*, jobject, jobject);
  void* pad_c[6];
  jclass (*GetObjectClass)(JNIEnv*, jobject);
  void* pad_d[135];
  jstring (*NewStringUTF)(JNIEnv*, const char*);
  void* reserved_getstringlen;
  const char* (*GetStringUTFChars)(JNIEnv*, jstring, jboolean*);
  void (*ReleaseStringUTFChars)(JNIEnv*, jstring, const char*);
  void* pad_e[57];
  jboolean (*ExceptionCheck)(JNIEnv*);
};

#endif /* MANGO_TESTS_FAKE_JNI_H_ */
