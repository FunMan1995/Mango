#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L /* mkstemp; still satisfies native_bridge.h's own floor */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* glibc gates mkstemp's declaration behind this too */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mango/native_bridge.h"

extern struct NativeBridgeCallbacks NativeBridgeItf;

/*
 * Same fixture as native/tests/test_elf32.c (see that file for how it was
 * built and independently verified with readelf): a minimal ELF32 shared
 * object exporting mango_add and mango_answer. loadLibrary takes a file
 * path, not a buffer, so this gets written to a temp file first.
 */
static const uint8_t kSynthElf[] = {
    0x7Fu, 0x45u, 0x4Cu, 0x46u, 0x01u, 0x01u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x28u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x34u, 0x00u, 0x00u, 0x00u, 0x9Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x34u, 0x00u, 0x20u, 0x00u, 0x01u, 0x00u, 0x28u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u,
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x14u, 0x01u, 0x00u, 0x00u, 0x14u, 0x01u, 0x00u, 0x00u, 0x05u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x10u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
    0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u, 0x01u, 0x00u, 0x0Bu,
    0x00u, 0x00u, 0x00u, 0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x12u, 0x00u,
    0x01u, 0x00u, 0x00u, 0x6Du, 0x61u, 0x6Eu, 0x67u, 0x6Fu, 0x5Fu, 0x61u, 0x64u, 0x64u, 0x00u,
    0x6Du, 0x61u, 0x6Eu, 0x67u, 0x6Fu, 0x5Fu, 0x61u, 0x6Eu, 0x73u, 0x77u, 0x65u, 0x72u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x54u, 0x00u, 0x00u, 0x00u, 0x54u, 0x00u, 0x00u, 0x00u, 0x30u, 0x00u, 0x00u, 0x00u, 0x02u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x84u, 0x00u, 0x00u, 0x00u, 0x84u, 0x00u, 0x00u, 0x00u, 0x18u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u,
};

int main(void) {
  /* The catastrophic bug this whole file exists to catch: libnativebridge's
   * own LoadNativeBridge() calls isCompatibleWith(NAMESPACE_VERSION=3)
   * unconditionally at load time and rejects the ENTIRE bridge if it's
   * false, confirmed against AOSP's libnativebridge/native_bridge.cc.
   * This isn't gated by .version; it's an independent check we implement. */
  if (!NativeBridgeItf.isCompatibleWith(2) || !NativeBridgeItf.isCompatibleWith(3)) {
    fprintf(stderr,
            "FAIL: isCompatibleWith(2) and (3) must both be true, or ART rejects the whole "
            "bridge at load time\n");
    return 1;
  }
  if (NativeBridgeItf.isCompatibleWith(4)) {
    fprintf(stderr,
            "FAIL: isCompatibleWith(4) should be false, we don't implement getVendorNamespace\n");
    return 1;
  }
  printf("ok: isCompatibleWith(3) is true, the load-time acceptance gate is fixed\n");

  if (NativeBridgeItf.version != 3 || !NativeBridgeItf.unloadLibrary || !NativeBridgeItf.getError ||
      !NativeBridgeItf.isPathSupported || !NativeBridgeItf.initAnonymousNamespace ||
      !NativeBridgeItf.createNamespace || !NativeBridgeItf.linkNamespaces ||
      !NativeBridgeItf.loadLibraryExt) {
    fprintf(stderr, "FAIL: version=3 but not every v3 field it obligates is filled in\n");
    return 1;
  }
  if (NativeBridgeItf.getVendorNamespace != NULL) {
    fprintf(stderr, "FAIL: getVendorNamespace should be NULL, version 4 isn't claimed\n");
    return 1;
  }
  printf(
      "ok: every v3 field claimed by version=3 is filled in, getVendorNamespace correctly isn't\n");

  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  char path[256];
  if (snprintf(path, sizeof(path), "%s/mango_shim_test_XXXXXX", tmpdir) >= (int)sizeof(path)) {
    fprintf(stderr, "FAIL: TMPDIR is too long to build a fixture path\n");
    return 1;
  }
  int fd = mkstemp(path);
  if (fd < 0 || write(fd, kSynthElf, sizeof(kSynthElf)) != (ssize_t)sizeof(kSynthElf)) {
    fprintf(stderr, "FAIL: couldn't write the test fixture to a temp file\n");
    return 1;
  }
  close(fd);

  void* handle = NativeBridgeItf.loadLibrary(path, 0);
  if (!handle) {
    fprintf(stderr, "FAIL: loadLibrary rejected a valid ARM32 .so\n");
    unlink(path);
    return 1;
  }
  printf("ok: loadLibrary succeeds on a real ARM32 .so\n");

  void* trampoline = NativeBridgeItf.getTrampoline(handle, "mango_add", "(II)I", 5);
  if (trampoline == NULL) {
    fprintf(stderr, "FAIL: getTrampoline returned NULL for an exported ARM32 symbol\n");
    unlink(path);
    return 1;
  }
  printf("ok: getTrampoline returns a bindable JNI stub for an exported symbol\n");

  if (NativeBridgeItf.getTrampoline(handle, "no_such_symbol", "()V", 3) != NULL) {
    fprintf(stderr, "FAIL: getTrampoline returned non-NULL for a missing symbol\n");
    unlink(path);
    return 1;
  }
  printf("ok: getTrampoline is NULL for a symbol the guest .so does not export\n");

  NativeBridgeItf.unloadLibrary(handle);
  unlink(path);

  /* Put a real A32 JNI body at 0x100: add r0, r2, r3; bx lr. ART shorty
   * III = returns int, two int args (JNIEnv* and jobject are implicit). */
  uint8_t jni_elf[sizeof(kSynthElf)];
  memcpy(jni_elf, kSynthElf, sizeof(jni_elf));
  for (size_t i = 4; i + 4 < sizeof(jni_elf); i++) {
    if (jni_elf[i] == 0x40 && jni_elf[i + 1] == 0 && jni_elf[i + 2] == 0 && jni_elf[i + 3] == 0 &&
        jni_elf[i - 4] == 0x01 && jni_elf[i - 3] == 0 && jni_elf[i - 2] == 0 &&
        jni_elf[i - 1] == 0) {
      jni_elf[i] = 0x00;
      jni_elf[i + 1] = 0x01;
      break;
    }
  }
  jni_elf[0x100] = 0x03;
  jni_elf[0x101] = 0x00;
  jni_elf[0x102] = 0x82;
  jni_elf[0x103] = 0xE0; /* add r0, r2, r3 */
  jni_elf[0x104] = 0x1E;
  jni_elf[0x105] = 0xFF;
  jni_elf[0x106] = 0x2F;
  jni_elf[0x107] = 0xE1; /* bx lr */

  if (snprintf(path, sizeof(path), "%s/mango_shim_test_XXXXXX", tmpdir) >= (int)sizeof(path)) {
    fprintf(stderr, "FAIL: TMPDIR is too long for the JNI fixture\n");
    return 1;
  }
  fd = mkstemp(path);
  if (fd < 0 || write(fd, jni_elf, sizeof(jni_elf)) != (ssize_t)sizeof(jni_elf)) {
    fprintf(stderr, "FAIL: couldn't write the JNI test fixture\n");
    return 1;
  }
  close(fd);

  handle = NativeBridgeItf.loadLibrary(path, 0);
  if (!handle) {
    fprintf(stderr, "FAIL: loadLibrary rejected the JNI add fixture\n");
    unlink(path);
    return 1;
  }
  void* add = NativeBridgeItf.getTrampoline(handle, "mango_add", "III", 3);
  if (!add) {
    fprintf(stderr, "FAIL: getTrampoline(mango_add, III) returned NULL\n");
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    return 1;
  }
  typedef jint (*mango_add_fn)(JNIEnv*, jobject, jint, jint);
  jint sum = ((mango_add_fn)add)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2, 20, 22);
  if (sum != 42) {
    fprintf(stderr, "FAIL: interpreter JNI trampoline returned %d, want 42\n", (int)sum);
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    return 1;
  }
  printf("ok: JNI trampoline runs guest add (20+22=42) through the interpreter\n");

  NativeBridgeItf.unloadLibrary(handle);
  unlink(path);

  /* Guest dlopen of a second ARM32 .so via SVC. */
  char path_b[256];
  if (snprintf(path_b, sizeof(path_b), "%s/mango_shim_test_XXXXXX", tmpdir) >= (int)sizeof(path_b)) {
    fprintf(stderr, "FAIL: TMPDIR is too long for the dlopen fixture B\n");
    return 1;
  }
  int fd_b = mkstemp(path_b);
  if (fd_b < 0 || write(fd_b, kSynthElf, sizeof(kSynthElf)) != (ssize_t)sizeof(kSynthElf)) {
    fprintf(stderr, "FAIL: couldn't write dlopen fixture B\n");
    return 1;
  }
  close(fd_b);

  uint8_t dl_elf[512];
  memset(dl_elf, 0, sizeof(dl_elf));
  memcpy(dl_elf, kSynthElf, sizeof(kSynthElf));
  dl_elf[0x44] = 0x00;
  dl_elf[0x45] = 0x02; /* p_filesz = 0x200 */
  dl_elf[0x48] = 0x00;
  dl_elf[0x49] = 0x02;
  for (size_t i = 4; i + 4 < sizeof(kSynthElf); i++) {
    if (dl_elf[i] == 0x40 && dl_elf[i + 1] == 0 && dl_elf[i + 2] == 0 && dl_elf[i + 3] == 0 &&
        dl_elf[i - 4] == 0x01 && dl_elf[i - 3] == 0 && dl_elf[i - 2] == 0 && dl_elf[i - 1] == 0) {
      dl_elf[i] = 0x00;
      dl_elf[i + 1] = 0x01;
      break;
    }
  }
  static const uint32_t kDlopen[] = {
      0xE3000180u, /* movw r0, #0x180 */
      0xE3A01000u, /* mov r1, #0 */
      0xE3027010u, /* movw r7, #0x2010  dlopen svc */
      0xEF000000u, /* svc 0 */
      0xE12FFF1Eu, /* bx lr */
  };
  for (size_t i = 0; i < sizeof(kDlopen) / sizeof(kDlopen[0]); i++) {
    uint32_t w = kDlopen[i];
    dl_elf[0x100 + i * 4 + 0] = (uint8_t)(w & 0xFFu);
    dl_elf[0x100 + i * 4 + 1] = (uint8_t)((w >> 8) & 0xFFu);
    dl_elf[0x100 + i * 4 + 2] = (uint8_t)((w >> 16) & 0xFFu);
    dl_elf[0x100 + i * 4 + 3] = (uint8_t)((w >> 24) & 0xFFu);
  }
  memcpy(dl_elf + 0x180, path_b, strlen(path_b) + 1);

  if (snprintf(path, sizeof(path), "%s/mango_shim_test_XXXXXX", tmpdir) >= (int)sizeof(path)) {
    fprintf(stderr, "FAIL: TMPDIR is too long for the dlopen fixture A\n");
    unlink(path_b);
    return 1;
  }
  fd = mkstemp(path);
  if (fd < 0 || write(fd, dl_elf, 0x200) != 0x200) {
    fprintf(stderr, "FAIL: couldn't write dlopen fixture A\n");
    unlink(path_b);
    return 1;
  }
  close(fd);
  handle = NativeBridgeItf.loadLibrary(path, 0);
  if (!handle) {
    fprintf(stderr, "FAIL: loadLibrary rejected the dlopen fixture\n");
    unlink(path);
    unlink(path_b);
    return 1;
  }
  void* dlfn = NativeBridgeItf.getTrampoline(handle, "mango_add", "I", 1);
  if (!dlfn) {
    fprintf(stderr, "FAIL: getTrampoline for dlopen fixture returned NULL\n");
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    unlink(path_b);
    return 1;
  }
  typedef jint (*mango_dl_fn)(JNIEnv*, jobject);
  jint hid = ((mango_dl_fn)dlfn)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2);
  if (hid < 2) {
    fprintf(stderr, "FAIL: guest dlopen returned %d, want handle >= 2\n", (int)hid);
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    unlink(path_b);
    return 1;
  }
  printf("ok: guest dlopen loaded a second ARM32 .so (handle %d)\n", (int)hid);
  NativeBridgeItf.unloadLibrary(handle);
  unlink(path);
  unlink(path_b);

  /* JNI_OnLoad shape from Unity's libmain.so: AttachCurrentThread via the
   * JavaVM vtable, RegisterNatives, return JNI_VERSION_1_6 with MOVW/MOVT. */
  uint8_t onload_elf[512];
  memset(onload_elf, 0, sizeof(onload_elf));
  memcpy(onload_elf, kSynthElf, sizeof(kSynthElf));
  onload_elf[0x44] = 0x80;
  onload_elf[0x45] = 0x01; /* p_filesz = 0x180 */
  onload_elf[0x48] = 0x80;
  onload_elf[0x49] = 0x01; /* p_memsz = 0x180 */
  for (size_t i = 4; i + 4 < sizeof(kSynthElf); i++) {
    if (onload_elf[i] == 0x40 && onload_elf[i + 1] == 0 && onload_elf[i + 2] == 0 &&
        onload_elf[i + 3] == 0 && onload_elf[i - 4] == 0x01 && onload_elf[i - 3] == 0 &&
        onload_elf[i - 2] == 0 && onload_elf[i - 1] == 0) {
      onload_elf[i] = 0x00;
      onload_elf[i + 1] = 0x01;
      break;
    }
  }
  static const uint32_t kOnLoad[] = {
      0xE92D4000u, /* push {lr} */
      0xE24DD008u, /* sub sp, sp, #8 */
      0xE3A01000u, /* mov r1, #0 */
      0xE58D1004u, /* str r1, [sp, #4] */
      0xE5901000u, /* ldr r1, [r0] */
      0xE5913010u, /* ldr r3, [r1, #0x10] AttachCurrentThread */
      0xE28D1004u, /* add r1, sp, #4 */
      0xE3A02000u, /* mov r2, #0 */
      0xE12FFF33u, /* blx r3 */
      0xE59D0004u, /* ldr r0, [sp, #4] */
      0xE5901000u, /* ldr r1, [r0] */
      0xE591435Cu, /* ldr r4, [r1, #0x35c] RegisterNatives */
      0xE3A01000u, /* mov r1, #0 */
      0xE3A02000u, /* mov r2, #0 */
      0xE3A03000u, /* mov r3, #0 */
      0xE12FFF34u, /* blx r4 */
      0xE3000006u, /* movw r0, #6 */
      0xE3400001u, /* movt r0, #1 */
      0xE28DD008u, /* add sp, sp, #8 */
      0xE8BD8000u, /* pop {pc} */
  };
  for (size_t i = 0; i < sizeof(kOnLoad) / sizeof(kOnLoad[0]); i++) {
    uint32_t w = kOnLoad[i];
    onload_elf[0x100 + i * 4 + 0] = (uint8_t)(w & 0xFFu);
    onload_elf[0x100 + i * 4 + 1] = (uint8_t)((w >> 8) & 0xFFu);
    onload_elf[0x100 + i * 4 + 2] = (uint8_t)((w >> 16) & 0xFFu);
    onload_elf[0x100 + i * 4 + 3] = (uint8_t)((w >> 24) & 0xFFu);
  }

  if (snprintf(path, sizeof(path), "%s/mango_shim_test_XXXXXX", tmpdir) >= (int)sizeof(path)) {
    fprintf(stderr, "FAIL: TMPDIR is too long for the JNI_OnLoad fixture\n");
    return 1;
  }
  fd = mkstemp(path);
  if (fd < 0 || write(fd, onload_elf, 0x180) != 0x180) {
    fprintf(stderr, "FAIL: couldn't write the JNI_OnLoad fixture\n");
    return 1;
  }
  close(fd);

  handle = NativeBridgeItf.loadLibrary(path, 0);
  if (!handle) {
    fprintf(stderr, "FAIL: loadLibrary rejected the JNI_OnLoad fixture\n");
    unlink(path);
    return 1;
  }
  void* onload = NativeBridgeItf.getTrampoline(handle, "mango_add", "IL", 2);
  if (!onload) {
    fprintf(stderr, "FAIL: getTrampoline(JNI_OnLoad shorty IL) returned NULL\n");
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    return 1;
  }
  typedef jint (*mango_onload_fn)(void*, void*);
  jint ver = ((mango_onload_fn)onload)((void*)(uintptr_t)1, NULL);
  if (ver != 0x00010006) {
    fprintf(stderr, "FAIL: JNI_OnLoad returned 0x%x, want 0x00010006\n", (int)ver);
    NativeBridgeItf.unloadLibrary(handle);
    unlink(path);
    return 1;
  }
  printf("ok: JNI_OnLoad runs AttachCurrentThread + RegisterNatives, returns 0x00010006\n");

  if (NativeBridgeItf.unloadLibrary(handle) != 0) {
    fprintf(stderr, "FAIL: unloadLibrary returned nonzero for a real handle\n");
    unlink(path);
    return 1;
  }
  printf("ok: unloadLibrary succeeds (leak-checked by the ASan build in CI)\n");

  if (!NativeBridgeItf.initAnonymousNamespace("libfoo.so", "/vendor/lib") ||
      !NativeBridgeItf.createNamespace("ns", NULL, NULL, 0, NULL, NULL) ||
      !NativeBridgeItf.linkNamespaces(NULL, NULL, "libfoo.so")) {
    fprintf(stderr,
            "FAIL: a namespace stub returned failure; they're all supposed to be safe no-ops\n");
    unlink(path);
    return 1;
  }
  void* handle2 = NativeBridgeItf.loadLibraryExt(path, 0, NULL);
  if (!handle2 || NativeBridgeItf.unloadLibrary(handle2) != 0) {
    fprintf(stderr, "FAIL: loadLibraryExt/unloadLibrary round trip failed\n");
    unlink(path);
    return 1;
  }
  printf("ok: namespace stubs behave safely (no crash, sane return values)\n");

  unlink(path);
  printf("all native_bridge_shim tests passed\n");
  return 0;
}
