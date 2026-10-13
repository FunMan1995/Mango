#define _GNU_SOURCE
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mango/native_bridge.h"

extern struct NativeBridgeCallbacks NativeBridgeItf;

/*
 * Host Tux Rider drive — Test Lab local.
 * Flow: load libtuxrider.so → JNI_OnLoad (RegisterNatives resize/render) →
 * resize(640,480) [calls libtuxracer_main] → a few render() frames.
 * Surfaces mango: interp stop on stderr. Exit 1 if stop seen; 0 if clean.
 *
 * Prefer libs/libtuxrider_host.so (data_dir string patched to /tmp/tuxrider_gd/)
 * else libs/libtuxrider.so / libs/armeabi/libtuxrider.so.
 * Set MANGO_ASSET_ROOT to gamedata (or argv[2]).
 */

typedef jint (*mango_onload_fn)(void* vm, void* reserved);
typedef void (*mango_resize_fn)(JNIEnv* env, jobject thiz, jint w, jint h);
typedef jint (*mango_render_fn)(JNIEnv* env, jobject thiz, jint a, jint b, jlong dt_bits,
                                jint c, jint z0, jint z1);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <tuxrider-libs-dir> [gamedata-dir]\n"
          "  libs-dir: directory with libtuxrider.so (or libtuxrider_host.so)\n"
          "  gamedata: optional; also set via MANGO_ASSET_ROOT\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_tuxrider_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_tuxrider_XXXXXX") >= (int)log_path_sz) {
    return -1;
  }
  int fd = mkstemp(log_path);
  if (fd < 0) return -1;
  *saved_fd = dup(STDERR_FILENO);
  if (*saved_fd < 0) {
    close(fd);
    unlink(log_path);
    return -1;
  }
  if (dup2(fd, STDERR_FILENO) < 0) {
    close(fd);
    close(*saved_fd);
    unlink(log_path);
    return -1;
  }
  close(fd);
  return 0;
}

static int capture_stderr_end(int saved_fd, const char* log_path, int* saw_stop) {
  fflush(stderr);
  if (dup2(saved_fd, STDERR_FILENO) < 0) return -1;
  close(saved_fd);
  FILE* f = fopen(log_path, "r");
  if (!f) {
    unlink(log_path);
    return -1;
  }
  char buf[4096];
  size_t n;
  *saw_stop = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    fwrite(buf, 1, n, stderr);
    if (memmem(buf, n, "mango: interp stop", 18) != NULL) *saw_stop = 1;
  }
  fclose(f);
  unlink(log_path);
  fflush(stderr);
  return 0;
}

static int join_path(char* out, size_t out_sz, const char* dir, const char* name) {
  int n = snprintf(out, out_sz, "%s/%s", dir, name);
  return (n < 0 || (size_t)n >= out_sz) ? -1 : 0;
}

static void* load_abs(const char* dir, const char* name) {
  char path[PATH_MAX], resolved[PATH_MAX];
  if (join_path(path, sizeof(path), dir, name) != 0 || realpath(path, resolved) == NULL) {
    fprintf(stderr, "mango_host_tuxrider_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_tuxrider_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

static void* load_tuxrider(const char* libs) {
  const char* names[] = {"libtuxrider_host.so", "libtuxrider.so", "armeabi/libtuxrider.so", NULL};
  for (int i = 0; names[i]; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), libs, names[i]) != 0) continue;
    if (access(path, R_OK) != 0) continue;
    void* h = load_abs(libs, names[i]);
    if (h) return h;
  }
  return NULL;
}

int main(int argc, char** argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc < 2 || argc > 3 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }
  const char* libs = argv[1];
  if (argc >= 3 && argv[2][0]) {
    setenv("MANGO_ASSET_ROOT", argv[2], 1);
  } else if (!getenv("MANGO_ASSET_ROOT")) {
    /* Default: sibling ../gamedata from libs dir */
    char gd[PATH_MAX], resolved[PATH_MAX];
    if (join_path(gd, sizeof(gd), libs, "../gamedata") == 0 && realpath(gd, resolved)) {
      setenv("MANGO_ASSET_ROOT", resolved, 1);
    }
  }
  fprintf(stderr, "mango_host_tuxrider_drive: MANGO_ASSET_ROOT=%s\n",
          getenv("MANGO_ASSET_ROOT") ? getenv("MANGO_ASSET_ROOT") : "(null)");

  /* HOME for ~/.tuxracer options fallback (getenv guest is stubbed→NULL). */
  if (!getenv("HOME")) {
    setenv("HOME", "/tmp/tuxrider_home", 1);
  }

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_tuxrider_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_tuxrider(libs);
  if (!h) {
    rc = 1;
    goto done;
  }

  void* onload = find_tramp(h, "JNI_OnLoad", "IL", 2);
  if (!onload) {
    rc = 1;
    goto unload;
  }
  jint ver = ((mango_onload_fn)onload)((void*)(uintptr_t)1, NULL);
  fprintf(stderr, "mango_host_tuxrider_drive: JNI_OnLoad returned 0x%x\n", (unsigned)ver);

  /* After OnLoad, RegisterNatives binds resize/render. Try Java_* then short names. */
  void* resize = find_tramp(h, "Java_com_drodin_tuxrider_NativeLib_resize", "VII", 3);
  if (!resize) resize = find_tramp(h, "resize", "(II)V", 0);
  if (!resize) resize = find_tramp(h, "resize", "VII", 3);
  if (!resize) {
    fprintf(stderr, "mango_host_tuxrider_drive: no resize trampoline\n");
    rc = 1;
    goto unload;
  }

  void* render = find_tramp(h, "Java_com_drodin_tuxrider_NativeLib_render", "IIIDIZZ", 7);
  if (!render) render = find_tramp(h, "render", "(IIDIZZ)I", 0);
  if (!render) render = find_tramp(h, "render", "IIIDIZZ", 7);

  fprintf(stderr, "mango_host_tuxrider_drive: calling resize(640,480)\n");
  ((mango_resize_fn)resize)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2, 640, 480);
  fprintf(stderr, "mango_host_tuxrider_drive: resize returned\n");

  if (render) {
    /* dt as double bits via jlong (shim va_arg for D uses jlong). ~16ms frame. */
    union {
      double d;
      jlong j;
    } dt;
    dt.d = 0.016;
    for (int frame = 0; frame < 3; frame++) {
      fprintf(stderr, "mango_host_tuxrider_drive: calling render frame %d\n", frame);
      jint rr = ((mango_render_fn)render)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2, 0, 0, dt.j,
                                          0, 0, 0);
      fprintf(stderr, "mango_host_tuxrider_drive: render returned %d\n", (int)rr);
    }
  } else {
    fprintf(stderr, "mango_host_tuxrider_drive: no render trampoline (resize-only)\n");
  }

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_tuxrider_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_tuxrider_drive: saw mango: interp stop (paste that line for the ISA fix)\n");
    return 1;
  }
  return rc;
}
