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
 * Host Liquid Wars OS drive — Test Lab local.
 * Flow: load libnativeinterface.so → Java_* init(AssetManager) →
 * onSurfaceCreated → onSurfaceChanged(w,h) → createGame → a few onDrawFrame.
 * Surfaces mango: interp stop on stderr. Exit 1 if stop seen; 0 if clean.
 *
 * Prefer libs/armeabi/libnativeinterface.so (ARMv5TE+Thumb-1), else
 * libs/libnativeinterface.so / libs/armeabi-v7a/...
 * Set MANGO_ASSET_ROOT to extracted assets/ (maps/*.png).
 * Frame count: MANGO_LW_FRAMES (default 3).
 */

typedef void (*mango_void_fn)(JNIEnv* env, jobject thiz);
typedef void (*mango_init_fn)(JNIEnv* env, jobject thiz, jobject asset_mgr);
typedef void (*mango_surface_changed_fn)(JNIEnv* env, jobject thiz, jint w, jint h);
typedef void (*mango_create_game_fn)(JNIEnv* env, jobject thiz, jint a, jint b, jint c, jint d);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <liquidwars-libs-dir> [assets-dir]\n"
          "  libs-dir: directory with libnativeinterface.so (or armeabi/)\n"
          "  assets: optional; also set via MANGO_ASSET_ROOT\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_liquidwars_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_liquidwars_XXXXXX") >= (int)log_path_sz) {
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
    fprintf(stderr, "mango_host_liquidwars_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_liquidwars_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

static void* load_lw(const char* libs) {
  const char* names[] = {"armeabi/libnativeinterface.so", "libnativeinterface.so",
                         "armeabi-v7a/libnativeinterface.so", NULL};
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
    char gd[PATH_MAX], resolved[PATH_MAX];
    if (join_path(gd, sizeof(gd), libs, "../_extract/assets") == 0 && realpath(gd, resolved)) {
      setenv("MANGO_ASSET_ROOT", resolved, 1);
    }
  }
  fprintf(stderr, "mango_host_liquidwars_drive: MANGO_ASSET_ROOT=%s\n",
          getenv("MANGO_ASSET_ROOT") ? getenv("MANGO_ASSET_ROOT") : "(null)");

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_liquidwars_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_lw(libs);
  if (!h) {
    rc = 1;
    goto done;
  }

  /* Explicit Java_* exports (no JNI_OnLoad / RegisterNatives). */
  void* init = find_tramp(h, "Java_com_xenris_liquidwarsos_NativeInterface_init", "VL", 2);
  void* on_created =
      find_tramp(h, "Java_com_xenris_liquidwarsos_NativeInterface_onSurfaceCreated", "V", 1);
  void* on_changed =
      find_tramp(h, "Java_com_xenris_liquidwarsos_NativeInterface_onSurfaceChanged", "VII", 3);
  void* create_game =
      find_tramp(h, "Java_com_xenris_liquidwarsos_NativeInterface_createGame", "VIIII", 5);
  void* on_draw =
      find_tramp(h, "Java_com_xenris_liquidwarsos_NativeInterface_onDrawFrame", "V", 1);

  if (!init || !on_draw) {
    fprintf(stderr, "mango_host_liquidwars_drive: missing required trampolines\n");
    rc = 1;
    goto unload;
  }

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;
  jobject asset_mgr = (jobject)(uintptr_t)3; /* fake; AAsset* → soft egl stub today */

  fprintf(stderr, "mango_host_liquidwars_drive: calling init(AssetManager)\n");
  ((mango_init_fn)init)(env, thiz, asset_mgr);
  fprintf(stderr, "mango_host_liquidwars_drive: init returned\n");

  if (on_created) {
    fprintf(stderr, "mango_host_liquidwars_drive: calling onSurfaceCreated\n");
    ((mango_void_fn)on_created)(env, thiz);
    fprintf(stderr, "mango_host_liquidwars_drive: onSurfaceCreated returned\n");
  }
  if (on_changed) {
    fprintf(stderr, "mango_host_liquidwars_drive: calling onSurfaceChanged(640,480)\n");
    ((mango_surface_changed_fn)on_changed)(env, thiz, 640, 480);
    fprintf(stderr, "mango_host_liquidwars_drive: onSurfaceChanged returned\n");
  }
  if (create_game) {
    /* Heuristic args: map=0, players/teams=2, extras 0,0 — adjust if first stop is earlier. */
    fprintf(stderr, "mango_host_liquidwars_drive: calling createGame(0,2,0,0)\n");
    ((mango_create_game_fn)create_game)(env, thiz, 0, 2, 0, 0);
    fprintf(stderr, "mango_host_liquidwars_drive: createGame returned\n");
  }

  int nframes = 3;
  const char* frames_env = getenv("MANGO_LW_FRAMES");
  if (frames_env && frames_env[0]) {
    char* end = NULL;
    long v = strtol(frames_env, &end, 10);
    if (end != frames_env && v > 0 && v <= 1000000) nframes = (int)v;
  }
  fprintf(stderr, "mango_host_liquidwars_drive: MANGO_LW_FRAMES=%d\n", nframes);
  for (int frame = 0; frame < nframes; frame++) {
    fprintf(stderr, "mango_host_liquidwars_drive: calling onDrawFrame %d\n", frame);
    ((mango_void_fn)on_draw)(env, thiz);
    fprintf(stderr, "mango_host_liquidwars_drive: onDrawFrame %d returned\n", frame);
  }

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_liquidwars_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_liquidwars_drive: saw mango: interp stop (paste that line for the ISA "
            "fix)\n");
    return 1;
  }
  return rc;
}
