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
 * Host Pacman drive — Test Lab local (PRIMARY lock research/46).
 * Flow: load libpacman.so → Java_* init(w,h,PngManager,AssetManager,StoreManager)
 * → a few step frames (optional actionDown/Move/Up) → capture stderr for first
 * mango: interp stop. Exit 1 if stop seen; 0 if clean.
 *
 * Prefer libs/armeabi/libpacman.so.
 * Set MANGO_ASSET_ROOT to extracted assets/ (levels/textures/audio/shaders).
 * Frame count: MANGO_PACMAN_FRAMES (default 3).
 *
 * JNI shortys (dex; env/thiz omitted from shorty like LW):
 *   init VIILLL · step V · stop Z · free V · action{Down,Move,Up} VFF
 */

typedef void (*mango_init_fn)(JNIEnv* env, jobject thiz, jint w, jint h, jobject png,
                              jobject asset_mgr, jobject store);
typedef void (*mango_void_fn)(JNIEnv* env, jobject thiz);
typedef jboolean (*mango_stop_fn)(JNIEnv* env, jobject thiz);
typedef void (*mango_action_fn)(JNIEnv* env, jobject thiz, float x, float y);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <pacman-libs-dir> [assets-dir]\n"
          "  libs-dir: directory with libpacman.so (or armeabi/)\n"
          "  assets: optional; also set via MANGO_ASSET_ROOT\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_pacman_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_pacman_XXXXXX") >= (int)log_path_sz) {
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
    fprintf(stderr, "mango_host_pacman_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_pacman_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

static void* load_pacman(const char* libs) {
  const char* names[] = {"armeabi/libpacman.so", "libpacman.so",
                         "armeabi-v7a/libpacman.so", NULL};
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
  fprintf(stderr, "mango_host_pacman_drive: MANGO_ASSET_ROOT=%s\n",
          getenv("MANGO_ASSET_ROOT") ? getenv("MANGO_ASSET_ROOT") : "(null)");

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_pacman_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_pacman(libs);
  if (!h) {
    rc = 1;
    goto done;
  }

  /* Explicit Java_* exports (no JNI_OnLoad / RegisterNatives). */
  void* init = find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_init", "VIILLL", 6);
  void* step = find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_step", "V", 1);
  void* stop = find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_stop", "Z", 1);
  void* free_fn = find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_free", "V", 1);
  void* action_down =
      find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_actionDown", "VFF", 3);
  void* action_move =
      find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_actionMove", "VFF", 3);
  void* action_up = find_tramp(h, "Java_com_zagayevskiy_pacman_PacmanLib_actionUp", "VFF", 3);

  if (!init || !step) {
    fprintf(stderr, "mango_host_pacman_drive: missing required trampolines\n");
    rc = 1;
    goto unload;
  }

  /* Host PngManager sentinel — JNI Call* dispatches open/getWidth/getHeight/
   * getPixels/close in native_bridge_shim (Pacman Q1 research/48). */
  static char png_mgr_obj = 'P';
  /* Host StoreManager sentinel — in-memory prefs honour defValue (Pacman Q3
   * research/50). Critical: loadBoolean("Engine_saved", false) → false. */
  static char store_mgr_obj = 'S';
  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;
  jobject png_mgr = (jobject)&png_mgr_obj;
  jobject asset_mgr = (jobject)(uintptr_t)4; /* fake; AAsset* host I/O live */
  jobject store_mgr = (jobject)&store_mgr_obj;

  fprintf(stderr, "mango_host_pacman_drive: calling init(640,480,Png,Asset,Store)\n");
  ((mango_init_fn)init)(env, thiz, 640, 480, png_mgr, asset_mgr, store_mgr);
  fprintf(stderr, "mango_host_pacman_drive: init returned\n");

  int nframes = 3;
  const char* frames_env = getenv("MANGO_PACMAN_FRAMES");
  if (frames_env && frames_env[0]) {
    char* end = NULL;
    long v = strtol(frames_env, &end, 10);
    if (end != frames_env && v > 0 && v <= 1000000) nframes = (int)v;
  }
  fprintf(stderr, "mango_host_pacman_drive: MANGO_PACMAN_FRAMES=%d\n", nframes);

  /* Optional: one tap near center before stepping (may help enter gameplay). */
  if (action_down) {
    fprintf(stderr, "mango_host_pacman_drive: calling actionDown(320,240)\n");
    ((mango_action_fn)action_down)(env, thiz, 320.0f, 240.0f);
    fprintf(stderr, "mango_host_pacman_drive: actionDown returned\n");
  }
  if (action_up) {
    fprintf(stderr, "mango_host_pacman_drive: calling actionUp(320,240)\n");
    ((mango_action_fn)action_up)(env, thiz, 320.0f, 240.0f);
    fprintf(stderr, "mango_host_pacman_drive: actionUp returned\n");
  }

  for (int frame = 0; frame < nframes; frame++) {
    fprintf(stderr, "mango_host_pacman_drive: calling step %d\n", frame);
    ((mango_void_fn)step)(env, thiz);
    fprintf(stderr, "mango_host_pacman_drive: step %d returned\n", frame);
  }

  if (stop) {
    fprintf(stderr, "mango_host_pacman_drive: calling stop\n");
    jboolean s = ((mango_stop_fn)stop)(env, thiz);
    fprintf(stderr, "mango_host_pacman_drive: stop returned %d\n", (int)s);
  }
  if (free_fn) {
    fprintf(stderr, "mango_host_pacman_drive: calling free\n");
    ((mango_void_fn)free_fn)(env, thiz);
    fprintf(stderr, "mango_host_pacman_drive: free returned\n");
  }
  (void)action_move;

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_pacman_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_pacman_drive: saw mango: interp stop (paste that line for the ISA "
            "fix)\n");
    return 1;
  }
  return rc;
}
