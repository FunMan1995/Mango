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
 * Host PrBoom (prboom4android) drive — Test Lab local (research/54 uncover).
 * Flow: load libprboom_jni.so → Java_doom_util_Natives_DoomMain(String[] argv)
 * optional setVideoMode / keyEvent / motionEvent. Capture first mango: interp stop.
 * Exit 1 if stop seen; 0 if clean; timeout wrapper may kill if DoomMain blocks.
 *
 * Prefer libs/armeabi/libprboom_jni.so.
 * Env:
 *   MANGO_PRBOOM_DATA  — dir with prboom.wad / prboom.cfg (also MANGO_ASSET_ROOT)
 *   MANGO_PRBOOM_IWAD  — legal IWAD path (Freedoom / shareware doom1.wad)
 *   MANGO_PRBOOM_NOSOUND=1 (default) → pass -nosound -nomusic
 *   MANGO_PRBOOM_MINIMAL=1 → argv = ["prboom"] only (IWAD-check probe)
 *
 * JNI shortys (dex _31; env/thiz omitted):
 *   DoomMain IL · keyEvent III · motionEvent IIII
 *   setVideoMode VII (ELF orphan only — optional)
 *
 * Host String[] sentinel layout must match shim MangoHostStringArray (magic JSAR).
 */

#define MANGO_HOST_STRARR_MAGIC 0x4a534152u
#define MANGO_HOST_STRARR_MAX 16
typedef struct {
  uint32_t magic;
  int length;
  const char* items[MANGO_HOST_STRARR_MAX];
} MangoHostStringArray;

typedef jint (*mango_doom_main_fn)(JNIEnv* env, jobject thiz, jobjectArray args);
typedef void (*mango_set_video_fn)(JNIEnv* env, jobject thiz, jint w, jint h);
typedef jint (*mango_key_fn)(JNIEnv* env, jobject thiz, jint type, jint key);
typedef jint (*mango_motion_fn)(JNIEnv* env, jobject thiz, jint x, jint y, jint z);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <prboom-libs-dir> [data-dir]\n"
          "  libs-dir: directory with libprboom_jni.so (or armeabi/)\n"
          "  data-dir: optional; also MANGO_PRBOOM_DATA / MANGO_ASSET_ROOT\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_prboom_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_prboom_XXXXXX") >= (int)log_path_sz) {
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
    fprintf(stderr, "mango_host_prboom_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_prboom_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

static void* load_prboom(const char* libs) {
  const char* names[] = {"armeabi/libprboom_jni.so", "libprboom_jni.so",
                         "armeabi-v7a/libprboom_jni.so", NULL};
  for (int i = 0; names[i]; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), libs, names[i]) != 0) continue;
    if (access(path, R_OK) != 0) continue;
    void* h = load_abs(libs, names[i]);
    if (h) return h;
  }
  return NULL;
}

static void push_arg(MangoHostStringArray* a, const char* s) {
  if (!s || a->length >= MANGO_HOST_STRARR_MAX) return;
  a->items[a->length++] = s;
}

int main(int argc, char** argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc < 2 || argc > 3 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }
  const char* libs = argv[1];

  /* Resolve data root: argv[2] → MANGO_PRBOOM_DATA → MANGO_ASSET_ROOT → sibling. */
  if (argc >= 3 && argv[2][0]) {
    setenv("MANGO_PRBOOM_DATA", argv[2], 1);
    setenv("MANGO_ASSET_ROOT", argv[2], 1);
  } else if (getenv("MANGO_PRBOOM_DATA") && getenv("MANGO_PRBOOM_DATA")[0]) {
    setenv("MANGO_ASSET_ROOT", getenv("MANGO_PRBOOM_DATA"), 1);
  } else if (!getenv("MANGO_ASSET_ROOT")) {
    char gd[PATH_MAX], resolved[PATH_MAX];
    if (join_path(gd, sizeof(gd), libs, "../data") == 0 && realpath(gd, resolved)) {
      setenv("MANGO_PRBOOM_DATA", resolved, 1);
      setenv("MANGO_ASSET_ROOT", resolved, 1);
    } else if (join_path(gd, sizeof(gd), libs, "../_extract/prboom_zip") == 0 &&
               realpath(gd, resolved)) {
      setenv("MANGO_PRBOOM_DATA", resolved, 1);
      setenv("MANGO_ASSET_ROOT", resolved, 1);
    }
  }
  fprintf(stderr, "mango_host_prboom_drive: MANGO_PRBOOM_DATA=%s\n",
          getenv("MANGO_PRBOOM_DATA") ? getenv("MANGO_PRBOOM_DATA") : "(null)");
  fprintf(stderr, "mango_host_prboom_drive: MANGO_ASSET_ROOT=%s\n",
          getenv("MANGO_ASSET_ROOT") ? getenv("MANGO_ASSET_ROOT") : "(null)");

  setenv("HOME", "/tmp/prboom_home", 1);

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_prboom_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_prboom(libs);
  if (!h) {
    rc = 1;
    goto done;
  }

  void* doom_main = find_tramp(h, "Java_doom_util_Natives_DoomMain", "IL", 2);
  void* set_video = find_tramp(h, "Java_doom_util_Natives_setVideoMode", "VII", 3);
  void* key_ev = find_tramp(h, "Java_doom_util_Natives_keyEvent", "III", 3);
  void* motion_ev = find_tramp(h, "Java_doom_util_Natives_motionEvent", "IIII", 4);

  if (!doom_main) {
    fprintf(stderr, "mango_host_prboom_drive: missing DoomMain trampoline\n");
    rc = 1;
    goto unload;
  }

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;

  /* Optional orphan setVideoMode before DoomMain (pre-size framebuffer). */
  if (set_video) {
    fprintf(stderr, "mango_host_prboom_drive: calling setVideoMode(320,200)\n");
    ((mango_set_video_fn)set_video)(env, thiz, 320, 200);
    fprintf(stderr, "mango_host_prboom_drive: setVideoMode returned\n");
  }

  /* Build host String[] argv (shim magic JSAR). */
  static char iwad_buf[PATH_MAX];
  static char file_buf[PATH_MAX];
  static MangoHostStringArray argv_arr;
  memset(&argv_arr, 0, sizeof(argv_arr));
  argv_arr.magic = MANGO_HOST_STRARR_MAGIC;

  const char* data = getenv("MANGO_PRBOOM_DATA");
  const char* iwad_env = getenv("MANGO_PRBOOM_IWAD");
  int minimal = 0;
  {
    const char* m = getenv("MANGO_PRBOOM_MINIMAL");
    minimal = m && m[0] == '1' && m[1] == '\0';
  }
  int nosound = 1;
  {
    const char* ns = getenv("MANGO_PRBOOM_NOSOUND");
    if (ns && ns[0] == '0' && ns[1] == '\0') nosound = 0;
  }

  push_arg(&argv_arr, "prboom");
  if (!minimal) {
    const char* iwad = iwad_env;
    if (!iwad || !iwad[0]) {
      if (data && data[0]) {
        if (join_path(iwad_buf, sizeof(iwad_buf), data, "freedoom1.wad") == 0 &&
            access(iwad_buf, R_OK) == 0) {
          iwad = iwad_buf;
        } else if (join_path(iwad_buf, sizeof(iwad_buf), data, "doom1.wad") == 0 &&
                   access(iwad_buf, R_OK) == 0) {
          iwad = iwad_buf;
        }
      }
    }
    if (iwad && iwad[0] && access(iwad, R_OK) == 0) {
      push_arg(&argv_arr, "-iwad");
      push_arg(&argv_arr, iwad);
      fprintf(stderr, "mango_host_prboom_drive: IWAD=%s\n", iwad);
    } else {
      fprintf(stderr,
              "mango_host_prboom_drive: no legal IWAD found (set MANGO_PRBOOM_IWAD); "
              "probe without -iwad\n");
    }
    if (data && data[0]) {
      if (join_path(file_buf, sizeof(file_buf), data, "prboom.wad") == 0 &&
          access(file_buf, R_OK) == 0) {
        push_arg(&argv_arr, "-file");
        push_arg(&argv_arr, file_buf);
        fprintf(stderr, "mango_host_prboom_drive: -file %s\n", file_buf);
      }
    }
    if (nosound) {
      push_arg(&argv_arr, "-nosound");
      push_arg(&argv_arr, "-nomusic");
    }
  }

  fprintf(stderr, "mango_host_prboom_drive: DoomMain argc=%d\n", argv_arr.length);
  for (int i = 0; i < argv_arr.length; i++) {
    fprintf(stderr, "mango_host_prboom_drive:   argv[%d]=%s\n", i, argv_arr.items[i]);
  }

  fprintf(stderr, "mango_host_prboom_drive: calling DoomMain\n");
  jint dmr = ((mango_doom_main_fn)doom_main)(env, thiz, (jobjectArray)&argv_arr);
  fprintf(stderr, "mango_host_prboom_drive: DoomMain returned %d\n", (int)dmr);

  /* If main returns, optional single key/motion probe. */
  if (key_ev) {
    fprintf(stderr, "mango_host_prboom_drive: calling keyEvent(1, 13)\n");
    jint kr = ((mango_key_fn)key_ev)(env, thiz, 1, 13);
    fprintf(stderr, "mango_host_prboom_drive: keyEvent returned %d\n", (int)kr);
  }
  if (motion_ev) {
    fprintf(stderr, "mango_host_prboom_drive: calling motionEvent(160,100,0)\n");
    jint mr = ((mango_motion_fn)motion_ev)(env, thiz, 160, 100, 0);
    fprintf(stderr, "mango_host_prboom_drive: motionEvent returned %d\n", (int)mr);
  }

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_prboom_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_prboom_drive: saw mango: interp stop (paste that line for the ISA "
            "fix)\n");
    return 1;
  }
  return rc;
}
