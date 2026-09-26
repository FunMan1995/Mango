#define _GNU_SOURCE
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mango/native_bridge.h"

extern struct NativeBridgeCallbacks NativeBridgeItf;

/* Must match shim MangoHostGamePtr / StringArray / ByteArray / ObjectArray. */
#define MANGO_HOST_GAMEPTR_MAGIC 0x47505452u /* GPTR */
#define MANGO_HOST_STRARR_MAGIC 0x4a534152u  /* JSAR */
#define MANGO_HOST_STRARR_MAX 16
#define MANGO_HOST_BYTEARR_MAGIC 0x4a425954u /* JBYT */
#define MANGO_HOST_OBJARR_MAGIC 0x4a4f424au  /* JOBJ */
#define MANGO_HOST_OBJARR_MAX 16

typedef struct {
  uint32_t magic;
  uint32_t ptr;
} MangoHostGamePtr;

typedef struct {
  uint32_t magic;
  int length;
  const char* items[MANGO_HOST_STRARR_MAX];
} MangoHostStringArray;

typedef struct {
  uint32_t magic;
  int length;
  const uint8_t* data;
} MangoHostByteArray;

typedef struct {
  uint32_t magic;
  int length;
  void* items[MANGO_HOST_OBJARR_MAX];
} MangoHostObjectArray;

/*
 * Host CrossWords (xw4) drive — Test Lab PRIMARY (research/63 → first-stop → 64*).
 * Tip pin: org.eehouse.android.xw4_136 / libxwjni.so.
 *
 * Flow (Java/pin order — initGlobals before initJNI; SO LDR [globalJNI,#24]):
 *   stage .xwd → loadLibrary → initGlobals(ILL) → initJNI(III=(II)I) →
 *   dict_ref(VI) → game_makeNewGame(VLLLLLLLLLL) soft jobjects →
 *   board_setDraw(VLL) → board_invalAll / board_draw / pen / board_commitTurn →
 *   game_dispose / cleanGlobals / envDone.
 *
 * Do NOT trampoline (absent from this SO): board_getActiveRect, board_handleKey,
 *   comms_getAddrDisabled, comms_setAddrDisabled.
 *
 * ELF mangles '_' → '_1' in Java_* symbols (board_draw → board_1draw).
 *
 * Env:
 *   MANGO_CROSSWORDS_FILES / MANGO_ASSET_ROOT — filesDir with *.xwd
 *   MANGO_CROSSWORDS_DICT — dict basename (default BasEnglish2to8.xwd)
 *   MANGO_CROSSWORDS_SKIP_DRAW=1 — stop after makeNewGame
 *   MANGO_CROSSWORDS_PEN=1 — fire handlePenDown/Move/Up after draw
 *   MANGO_CROSSWORDS_COMMIT=1 — board_commitTurn after draw/pen
 *   MANGO_CROSSWORDS_FORCE_EXIT_MS — pthread abort after N ms (default 8000; 0=off)
 *
 * Exit 1 if stderr contains "mango: interp stop"; 0 if clean; 2 usage.
 */

typedef jint (*mango_ill_fn)(JNIEnv* env, jclass clazz, jobject a, jobject b);
typedef jint (*mango_iii_fn)(JNIEnv* env, jclass clazz, jint a, jint b);
typedef void (*mango_vi_fn)(JNIEnv* env, jclass clazz, jint a);
typedef void (*mango_vl_fn)(JNIEnv* env, jclass clazz, jobject a);
typedef void (*mango_vll_fn)(JNIEnv* env, jclass clazz, jobject a, jobject b);
typedef jboolean (*mango_zl_fn)(JNIEnv* env, jclass clazz, jobject a);
typedef jboolean (*mango_zlii_fn)(JNIEnv* env, jclass clazz, jobject a, jint x, jint y);
typedef jboolean (*mango_zliil_fn)(JNIEnv* env, jclass clazz, jobject a, jint x, jint y,
                                  jobject arr);
typedef jboolean (*mango_zlzzl_fn)(JNIEnv* env, jclass clazz, jobject a, jboolean b,
                                  jboolean c, jobject arr);
typedef void (*mango_make_fn)(JNIEnv* env, jclass clazz, jobject gamePtr, jobject gi,
                              jobject dictNames, jobject dictBytes, jobject dictPaths,
                              jobject langName, jobject util, jobject draw, jobject cp,
                              jobject procs);

typedef struct {
  int delay_ms;
} mango_xw_force_args;

static void* mango_xw_force_thread(void* arg) {
  mango_xw_force_args* a = (mango_xw_force_args*)arg;
  if (a->delay_ms > 0) {
    fprintf(stderr, "mango_host_crosswords_drive: force-exit sleep %d ms\n", a->delay_ms);
    usleep((useconds_t)a->delay_ms * 1000u);
  }
  fprintf(stderr, "mango_host_crosswords_drive: force-exit abort\n");
  _exit(124);
  return NULL;
}

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <crosswords-libs-dir> [files-dir]\n"
          "  libs-dir: directory with libxwjni.so (or armeabi/)\n"
          "  files-dir: optional; also MANGO_CROSSWORDS_FILES / MANGO_ASSET_ROOT\n"
          "             must contain staged *.xwd (e.g. BasEnglish2to8.xwd)\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_crosswords_drive: trampoline %s %s\n", name,
          t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_crosswords_XXXXXX") >=
      (int)log_path_sz) {
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

static uint8_t* load_file_bytes(const char* path, int* out_len) {
  FILE* f = fopen(path, "rb");
  long sz;
  uint8_t* buf;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  sz = ftell(f);
  if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
  rewind(f);
  buf = (uint8_t*)malloc((size_t)sz);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_len = (int)sz;
  return buf;
}

static int env_int(const char* key, int dflt) {
  const char* v = getenv(key);
  if (!v || !v[0]) return dflt;
  char* end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v) return dflt;
  if (n > 1000000) n = 1000000;
  if (n < -1000000) n = -1000000;
  return (int)n;
}

static void* load_abs(const char* dir, const char* name) {
  char path[PATH_MAX], resolved[PATH_MAX];
  if (join_path(path, sizeof(path), dir, name) != 0 || realpath(path, resolved) == NULL) {
    fprintf(stderr, "mango_host_crosswords_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_crosswords_drive: loadLibrary(%s) %s\n", name,
          h ? "ok" : "FAIL");
  return h;
}

static void* load_xwjni(const char* libs) {
  const char* names[] = {"armeabi/libxwjni.so", "libxwjni.so",
                         "armeabi-v7a/libxwjni.so", NULL};
  for (int i = 0; names[i]; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), libs, names[i]) != 0) continue;
    if (access(path, R_OK) != 0) continue;
    void* h = load_abs(libs, names[i]);
    if (h) return h;
  }
  return NULL;
}

static int resolve_files_dir(const char* libs, const char* argv_files, char* out,
                             size_t out_sz) {
  const char* env = getenv("MANGO_CROSSWORDS_FILES");
  if (!env || !env[0]) env = getenv("MANGO_ASSET_ROOT");
  if (argv_files && argv_files[0]) env = argv_files;
  if (env && env[0]) {
    if (!realpath(env, out)) {
      fprintf(stderr, "mango_host_crosswords_drive: files-dir resolve FAIL (%s)\n", env);
      return -1;
    }
    return 0;
  }
  char cand[PATH_MAX];
  if (join_path(cand, sizeof(cand), libs, "../files") == 0 && realpath(cand, out)) {
    return 0;
  }
  if (join_path(cand, sizeof(cand), libs, "../assets") == 0 && realpath(cand, out)) {
    return 0;
  }
  fprintf(stderr, "mango_host_crosswords_drive: no files-dir (set MANGO_CROSSWORDS_FILES)\n");
  return -1;
}

int main(int argc, char** argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc < 2 || argc > 3 || argv[1][0] == '-') {
    usage(argv[0]);
    return 2;
  }
  const char* libs = argv[1];
  const char* argv_files = (argc >= 3) ? argv[2] : NULL;

  char files_dir[PATH_MAX];
  if (resolve_files_dir(libs, argv_files, files_dir, sizeof(files_dir)) != 0) {
    return 2;
  }
  setenv("MANGO_CROSSWORDS_FILES", files_dir, 1);
  setenv("MANGO_ASSET_ROOT", files_dir, 1);

  const char* dict_name = getenv("MANGO_CROSSWORDS_DICT");
  if (!dict_name || !dict_name[0]) dict_name = "BasEnglish2to8.xwd";
  char dict_path[PATH_MAX];
  if (join_path(dict_path, sizeof(dict_path), files_dir, dict_name) != 0 ||
      access(dict_path, R_OK) != 0) {
    fprintf(stderr, "mango_host_crosswords_drive: dict missing %s/%s\n", files_dir,
            dict_name);
    return 2;
  }
  fprintf(stderr, "mango_host_crosswords_drive: libs=%s files=%s dict=%s\n", libs,
          files_dir, dict_name);

  int force_ms = env_int("MANGO_CROSSWORDS_FORCE_EXIT_MS", 8000);
  if (force_ms < 0) force_ms = 0;
  if (force_ms > 600000) force_ms = 600000;
  pthread_t force_thr;
  mango_xw_force_args force_args;
  int have_force = 0;
  if (force_ms > 0) {
    force_args.delay_ms = force_ms;
    if (pthread_create(&force_thr, NULL, mango_xw_force_thread, &force_args) == 0) {
      have_force = 1;
      fprintf(stderr, "mango_host_crosswords_drive: force-exit armed (%d ms)\n", force_ms);
    }
  }

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_crosswords_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  uint8_t* dict_buf = NULL;
  void* h = load_xwjni(libs);
  if (!h) {
    fprintf(stderr, "mango_host_crosswords_drive: libxwjni.so load FAIL\n");
    rc = 1;
    goto done;
  }

  /* Explicit Java_* (no JNI_OnLoad). Dex shortys; ELF _ → _1. */
  void* init_globals =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_initGlobals", "ILL", 3);
  void* init_jni =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_initJNI", "III", 3);
  void* dict_ref =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_dict_1ref", "VI", 2);
  void* make_new = find_tramp(
      h, "Java_org_eehouse_android_xw4_jni_XwJNI_game_1makeNewGame", "VLLLLLLLLLL", 11);
  void* set_draw =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1setDraw", "VLL", 3);
  void* inval_all =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1invalAll", "VL", 2);
  void* board_draw =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1draw", "ZL", 2);
  void* pen_down = find_tramp(
      h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1handlePenDown", "ZLIIL", 5);
  void* pen_move = find_tramp(
      h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1handlePenMove", "ZLII", 4);
  void* pen_up = find_tramp(
      h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1handlePenUp", "ZLII", 4);
  void* commit = find_tramp(
      h, "Java_org_eehouse_android_xw4_jni_XwJNI_board_1commitTurn", "ZLZZL", 5);
  void* dispose =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_game_1dispose", "VL", 2);
  void* clean =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_cleanGlobals", "VI", 2);
  void* env_done =
      find_tramp(h, "Java_org_eehouse_android_xw4_jni_XwJNI_envDone", "VI", 2);

  if (!init_globals || !init_jni) {
    fprintf(stderr, "mango_host_crosswords_drive: missing initGlobals/initJNI\n");
    rc = 1;
    goto unload;
  }

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jclass clazz = (jclass)(uintptr_t)2;
  /* Soft jobject sentinels for DUtilCtxt / JNIUtils / CurGame / Draw / etc. */
  static char dutil_obj = 'D';
  static char jniu_obj = 'J';
  static char gi_obj = 'I';
  static char util_obj = 'U';
  static char draw_obj = 'W';
  static char cp_obj = 'C';
  static char procs_obj = 'T';
  static char pen_arr_obj = 'A';
  static char commit_arr_obj = 'K';
  /* Host char* doubles as fake jstring (shim mango_fake_jstring_chars). */
  const char* lang_name = "English";

  /* Dict name / path String[] and byte[][] from fixtures .xwd. */
  static MangoHostStringArray names_arr;
  static MangoHostStringArray paths_arr;
  static MangoHostByteArray dict_bytes;
  static MangoHostObjectArray bytes_arr;
  static char dict_name_storage[256];
  int dict_len = 0;
  dict_buf = load_file_bytes(dict_path, &dict_len);
  memset(&names_arr, 0, sizeof(names_arr));
  memset(&paths_arr, 0, sizeof(paths_arr));
  memset(&dict_bytes, 0, sizeof(dict_bytes));
  memset(&bytes_arr, 0, sizeof(bytes_arr));
  names_arr.magic = MANGO_HOST_STRARR_MAGIC;
  paths_arr.magic = MANGO_HOST_STRARR_MAGIC;
  dict_bytes.magic = MANGO_HOST_BYTEARR_MAGIC;
  bytes_arr.magic = MANGO_HOST_OBJARR_MAGIC;
  snprintf(dict_name_storage, sizeof(dict_name_storage), "%s", dict_name);
  /* Strip .xwd suffix for dict name if present (Java often uses basename). */
  {
    char* dot = strrchr(dict_name_storage, '.');
    if (dot && strcmp(dot, ".xwd") == 0) *dot = '\0';
  }
  names_arr.length = 1;
  names_arr.items[0] = dict_name_storage;
  paths_arr.length = 1;
  paths_arr.items[0] = dict_path;
  if (dict_buf && dict_len > 0) {
    dict_bytes.length = dict_len;
    dict_bytes.data = dict_buf;
    bytes_arr.length = 1;
    bytes_arr.items[0] = &dict_bytes;
    fprintf(stderr, "mango_host_crosswords_drive: dict bytes %d from %s\n", dict_len,
            dict_path);
  } else {
    fprintf(stderr, "mango_host_crosswords_drive: dict bytes load FAIL %s\n", dict_path);
  }

  fprintf(stderr, "mango_host_crosswords_drive: calling initGlobals(DUtil,JNIUtils)\n");
  jint globals = ((mango_ill_fn)init_globals)(env, clazz, (jobject)&dutil_obj,
                                              (jobject)&jniu_obj);
  fprintf(stderr, "mango_host_crosswords_drive: initGlobals returned %d (0x%x)\n",
          (int)globals, (unsigned)globals);

  int seed = env_int("MANGO_CROSSWORDS_SEED", 0x5151);
  fprintf(stderr, "mango_host_crosswords_drive: calling initJNI(globals=%d, seed=%d)\n",
          (int)globals, seed);
  jint game_state = ((mango_iii_fn)init_jni)(env, clazz, globals, seed);
  fprintf(stderr, "mango_host_crosswords_drive: initJNI returned %d (0x%x)\n",
          (int)game_state, (unsigned)game_state);

  /* dict_ref after staging .xwd — ptr 0 is a documented no-op; real dict comes
   * via makeNewGame soft byte[][] / paths. */
  if (dict_ref) {
    fprintf(stderr, "mango_host_crosswords_drive: calling dict_ref(0) after stage %s\n",
            dict_path);
    ((mango_vi_fn)dict_ref)(env, clazz, 0);
    fprintf(stderr, "mango_host_crosswords_drive: dict_ref returned\n");
  }

  /* Soft GamePtr — CallIntMethod("ptr") returns initJNI guest state (research/64). */
  static MangoHostGamePtr game_ptr_obj;
  memset(&game_ptr_obj, 0, sizeof(game_ptr_obj));
  game_ptr_obj.magic = MANGO_HOST_GAMEPTR_MAGIC;
  game_ptr_obj.ptr = (uint32_t)game_state;
  jobject game_ptr = (jobject)&game_ptr_obj;
  fprintf(stderr, "mango_host_crosswords_drive: GamePtr.ptr wired to 0x%x\n",
          game_ptr_obj.ptr);

  if (make_new) {
    fprintf(stderr,
            "mango_host_crosswords_drive: calling game_makeNewGame "
            "(GamePtr+dict arrays)\n");
    ((mango_make_fn)make_new)(env, clazz, game_ptr, (jobject)&gi_obj,
                              (jobject)&names_arr, (jobject)&bytes_arr,
                              (jobject)&paths_arr, (jobject)lang_name,
                              (jobject)&util_obj, (jobject)&draw_obj, (jobject)&cp_obj,
                              (jobject)&procs_obj);
    fprintf(stderr, "mango_host_crosswords_drive: game_makeNewGame returned\n");
  } else {
    fprintf(stderr, "mango_host_crosswords_drive: game_makeNewGame trampoline missing\n");
  }

  int skip_draw = env_int("MANGO_CROSSWORDS_SKIP_DRAW", 0);
  if (!skip_draw) {
    if (set_draw) {
      fprintf(stderr, "mango_host_crosswords_drive: calling board_setDraw\n");
      ((mango_vll_fn)set_draw)(env, clazz, game_ptr, (jobject)&draw_obj);
      fprintf(stderr, "mango_host_crosswords_drive: board_setDraw returned\n");
    }
    if (inval_all) {
      fprintf(stderr, "mango_host_crosswords_drive: calling board_invalAll\n");
      ((mango_vl_fn)inval_all)(env, clazz, game_ptr);
      fprintf(stderr, "mango_host_crosswords_drive: board_invalAll returned\n");
    }
    if (board_draw) {
      fprintf(stderr, "mango_host_crosswords_drive: calling board_draw\n");
      jboolean drew = ((mango_zl_fn)board_draw)(env, clazz, game_ptr);
      fprintf(stderr, "mango_host_crosswords_drive: board_draw returned %d\n", (int)drew);
    }
    if (env_int("MANGO_CROSSWORDS_PEN", 1)) {
      int x = env_int("MANGO_CROSSWORDS_PEN_X", 80);
      int y = env_int("MANGO_CROSSWORDS_PEN_Y", 80);
      if (pen_down) {
        fprintf(stderr, "mango_host_crosswords_drive: calling board_handlePenDown(%d,%d)\n",
                x, y);
        ((mango_zliil_fn)pen_down)(env, clazz, game_ptr, x, y, (jobject)&pen_arr_obj);
        fprintf(stderr, "mango_host_crosswords_drive: board_handlePenDown returned\n");
      }
      if (pen_move) {
        fprintf(stderr, "mango_host_crosswords_drive: calling board_handlePenMove(%d,%d)\n",
                x + 4, y + 4);
        ((mango_zlii_fn)pen_move)(env, clazz, game_ptr, x + 4, y + 4);
        fprintf(stderr, "mango_host_crosswords_drive: board_handlePenMove returned\n");
      }
      if (pen_up) {
        fprintf(stderr, "mango_host_crosswords_drive: calling board_handlePenUp(%d,%d)\n",
                x + 4, y + 4);
        ((mango_zlii_fn)pen_up)(env, clazz, game_ptr, x + 4, y + 4);
        fprintf(stderr, "mango_host_crosswords_drive: board_handlePenUp returned\n");
      }
    }
    if (env_int("MANGO_CROSSWORDS_COMMIT", 1) && commit) {
      fprintf(stderr, "mango_host_crosswords_drive: calling board_commitTurn\n");
      ((mango_zlzzl_fn)commit)(env, clazz, game_ptr, (jboolean)0, (jboolean)0,
                               (jobject)&commit_arr_obj);
      fprintf(stderr, "mango_host_crosswords_drive: board_commitTurn returned\n");
    }
  } else {
    fprintf(stderr, "mango_host_crosswords_drive: SKIP_DRAW=1\n");
  }

  if (dispose) {
    fprintf(stderr, "mango_host_crosswords_drive: calling game_dispose\n");
    ((mango_vl_fn)dispose)(env, clazz, game_ptr);
    fprintf(stderr, "mango_host_crosswords_drive: game_dispose returned\n");
  }
  if (clean) {
    fprintf(stderr, "mango_host_crosswords_drive: calling cleanGlobals(%d)\n", (int)globals);
    ((mango_vi_fn)clean)(env, clazz, globals);
    fprintf(stderr, "mango_host_crosswords_drive: cleanGlobals returned\n");
  }
  if (env_done) {
    fprintf(stderr, "mango_host_crosswords_drive: calling envDone(%d)\n", (int)globals);
    ((mango_vi_fn)env_done)(env, clazz, globals);
    fprintf(stderr, "mango_host_crosswords_drive: envDone returned\n");
  }

unload:
  (void)h;

done:;
  free(dict_buf);
  if (have_force) {
    /* drive finished before timeout — cancel is best-effort; join may race abort */
    pthread_detach(force_thr);
  }
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_crosswords_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_crosswords_drive: saw mango: interp stop (paste for research/65*)\n");
    return 1;
  }
  return rc;
}
