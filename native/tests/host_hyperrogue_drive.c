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
 * Host HyperRogue (hyperroid) drive — Test Lab local (research/52 uncover).
 * Flow: load libhyper.so → Java_* initGame → loadMap → update×N
 * (research/52 pin). Capture stderr for first mango: interp stop.
 * Exit 1 if stop seen; 0 if clean.
 *
 * Prefer libs/armeabi/libhyper.so.
 * No assets in APK 3.7 (canvas path; NEEDED stdc++/m/c/dl only).
 * Frame count: MANGO_HYPER_FRAMES (default 3; alias MANGO_HYPERROGUE_FRAMES).
 * Input (per-frame update args; env-controllable):
 *   MANGO_HYPER_CLICKED — 0/1 (default 0); mouse at (320,240) when 1
 *   MANGO_HYPER_KEY — unsigned char / uint16_t code (default 0);
 *     try printable e.g. 32 space, 'w'/'a'/'s'/'d' (119/97/115/100), 13/'\n' enter
 *
 * JNI shortys (dex _370 only; env/thiz omitted like LW/Pacman):
 *   initGame I · loadMap L ([I) · update VIIIIIZC (xres,yres,ticks,mx,my,clicked,key)
 * Suggested update: 640,480,ticks=0..N-1, mouse=(320,240), clicked/key from env.
 */

typedef jint (*mango_init_game_fn)(JNIEnv* env, jobject thiz);
typedef jobject (*mango_load_map_fn)(JNIEnv* env, jobject thiz);
typedef void (*mango_update_fn)(JNIEnv* env, jobject thiz, jint xres, jint yres, jint ticks,
                                jint mousex, jint mousey, jboolean clicked, uint16_t key);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <hyperrogue-libs-dir>\n"
          "  libs-dir: directory with libhyper.so (or armeabi/)\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_hyperrogue_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_hyperrogue_XXXXXX") >= (int)log_path_sz) {
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
    fprintf(stderr, "mango_host_hyperrogue_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_hyperrogue_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

static void* load_hyper(const char* libs) {
  const char* names[] = {"armeabi/libhyper.so", "libhyper.so", "armeabi-v7a/libhyper.so", NULL};
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
  if (argc < 2 || argc > 2 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }
  const char* libs = argv[1];
  fprintf(stderr, "mango_host_hyperrogue_drive: libs=%s (no assets in APK 3.7)\n", libs);

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_hyperrogue_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_hyper(libs);
  if (!h) {
    rc = 1;
    goto done;
  }

  /* Explicit Java_* exports (no JNI_OnLoad / RegisterNatives). Dex 3.7 shortys. */
  void* init_game =
      find_tramp(h, "Java_com_roguetemple_hyperroid_HyperRogue_initGame", "I", 1);
  void* load_map =
      find_tramp(h, "Java_com_roguetemple_hyperroid_HyperRogue_loadMap", "L", 1);
  void* update =
      find_tramp(h, "Java_com_roguetemple_hyperroid_HyperRogue_update", "VIIIIIZC", 8);

  if (!init_game || !update) {
    fprintf(stderr, "mango_host_hyperrogue_drive: missing required trampolines\n");
    rc = 1;
    goto unload;
  }

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;

  fprintf(stderr, "mango_host_hyperrogue_drive: calling initGame\n");
  jint ig = ((mango_init_game_fn)init_game)(env, thiz);
  fprintf(stderr, "mango_host_hyperrogue_drive: initGame returned %d\n", (int)ig);

  if (load_map) {
    fprintf(stderr, "mango_host_hyperrogue_drive: calling loadMap\n");
    jobject arr = ((mango_load_map_fn)load_map)(env, thiz);
    fprintf(stderr, "mango_host_hyperrogue_drive: loadMap returned %p\n", (void*)arr);
  }

  int nframes = 3;
  const char* frames_env = getenv("MANGO_HYPER_FRAMES");
  if ((!frames_env || !frames_env[0]) && getenv("MANGO_HYPERROGUE_FRAMES")) {
    frames_env = getenv("MANGO_HYPERROGUE_FRAMES"); /* alias */
  }
  if (frames_env && frames_env[0]) {
    char* end = NULL;
    long v = strtol(frames_env, &end, 10);
    if (end != frames_env && v > 0 && v <= 1000000) nframes = (int)v;
  }
  fprintf(stderr, "mango_host_hyperrogue_drive: MANGO_HYPER_FRAMES=%d\n", nframes);

  /* MANGO_HYPER_CLICKED: 0/1 (default 0). */
  jboolean clicked = (jboolean)0;
  const char* clicked_env = getenv("MANGO_HYPER_CLICKED");
  if (clicked_env && clicked_env[0]) {
    if (clicked_env[0] == '1' && clicked_env[1] == '\0') {
      clicked = (jboolean)1;
    } else if (!(clicked_env[0] == '0' && clicked_env[1] == '\0')) {
      char* end = NULL;
      long v = strtol(clicked_env, &end, 10);
      if (end != clicked_env && v != 0) clicked = (jboolean)1;
    }
  }
  fprintf(stderr, "mango_host_hyperrogue_drive: MANGO_HYPER_CLICKED=%d\n", (int)clicked);

  /* MANGO_HYPER_KEY: unsigned char / uint16_t code (default 0). */
  uint16_t key = 0;
  const char* key_env = getenv("MANGO_HYPER_KEY");
  if (key_env && key_env[0]) {
    char* end = NULL;
    unsigned long v = strtoul(key_env, &end, 0); /* accept 119, 0x77, etc. */
    if (end != key_env && v <= 0xffffUL) key = (uint16_t)v;
  }
  fprintf(stderr, "mango_host_hyperrogue_drive: MANGO_HYPER_KEY=%u\n", (unsigned)key);

  /* research/52: update(xres,yres,ticks,mx,my,clicked,key) × N; ticks = 0..N-1. */
  for (int frame = 0; frame < nframes; frame++) {
    jint ticks = (jint)frame;
    fprintf(stderr,
            "mango_host_hyperrogue_drive: calling update(640,480,ticks=%d,mx=320,my=240,"
            "clicked=%d,key=%u) frame %d\n",
            (int)ticks, (int)clicked, (unsigned)key, frame);
    ((mango_update_fn)update)(env, thiz, 640, 480, ticks, 320, 240, clicked, key);
    fprintf(stderr, "mango_host_hyperrogue_drive: update %d returned\n", frame);
  }

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_hyperrogue_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_hyperrogue_drive: saw mango: interp stop (paste that line for the ISA "
            "fix)\n");
    return 1;
  }
  return rc;
}
