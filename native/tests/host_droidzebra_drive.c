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

/*
 * Host DroidZebra (Othello/Zebra) drive helper for Test Lab (research/61).
 * Flow: load libdroidzebra.so → zeGlobalInit(files_dir) → optional zeSet*
 *   knobs → zeSetPlayerInfo×2 → zePlay(0,NULL) → zeGlobalTerminate if returns.
 * Capture stderr for first mango: interp stop. Exit 1 if stop seen; 0 if clean;
 * timeout wrapper / force-exit thread may kill if zePlay blocks (game loop +
 * post-game UI wait).
 *
 * Prefer libs/armeabi/libdroidzebra.so.
 * Env:
 *   MANGO_DROIDZEBRA_FILES / MANGO_ASSET_ROOT — dir with book.cmp.z + coeffs2.bin
 *     (writable; GlobalInit unpacks book.cmp.z → book.bin in-place)
 *   MANGO_DROIDZEBRA_USE_BOOK — 0/1 (default 1) → zeSetUseBook
 *   MANGO_DROIDZEBRA_SKILL — computer skill both sides (default 4; 0=human blocks)
 *   MANGO_DROIDZEBRA_EXACT / MANGO_DROIDZEBRA_WLD — exact/wld skills (default 12)
 *   MANGO_DROIDZEBRA_SKIP_PLAY=1 — stop after GlobalInit + SetPlayerInfo
 *   MANGO_DROIDZEBRA_FORCE_EXIT_MS — pthread zeForceExit after N ms (default 5000;
 *     0 disables; zePlay blocks until Exit/ForceExit)
 *
 * JNI shortys (dex _12; env/thiz omitted):
 *   zeGlobalInit VL · zeGlobalTerminate V · zePlay VIL ·
 *   zeForceExit V · zeForceReturn V · zeGameInProgress Z ·
 *   zeSetAutoMakeMoves VI · zeSetForcedOpening VL · zeSetHumanOpenings VI ·
 *   zeSetPerturbation VF · zeSetPlayerInfo VIIIIII · zeSetPracticeMode VI ·
 *   zeSetSlack VF · zeSetUseBook VI · zeJsonTest VL
 *
 * Host char* doubles as fake jstring (shim mango_fake_jstring_chars).
 */

#define DZ_BLACKSQ 0
#define DZ_WHITESQ 2
#define DZ_INFINIT_TIME 10000000

typedef void (*mango_void_fn)(JNIEnv* env, jobject thiz);
typedef void (*mango_str_fn)(JNIEnv* env, jobject thiz, jstring s);
typedef void (*mango_i_fn)(JNIEnv* env, jobject thiz, jint a);
typedef void (*mango_f_fn)(JNIEnv* env, jobject thiz, float a);
typedef void (*mango_player_fn)(JNIEnv* env, jobject thiz, jint player, jint skill,
                                jint exact, jint wld, jint t, jint tinc);
typedef void (*mango_play_fn)(JNIEnv* env, jobject thiz, jint count, jobject moves);
typedef jboolean (*mango_z_fn)(JNIEnv* env, jobject thiz);

typedef struct {
  JNIEnv* env;
  jobject thiz;
  void* force_exit;
  int delay_ms;
} mango_dz_force_args;

static void* mango_dz_force_thread(void* arg) {
  mango_dz_force_args* a = (mango_dz_force_args*)arg;
  if (a->delay_ms > 0) {
    fprintf(stderr, "mango_host_droidzebra_drive: force-exit thread sleep %d ms\n",
            a->delay_ms);
    usleep((useconds_t)a->delay_ms * 1000u);
  }
  if (a->force_exit) {
    fprintf(stderr, "mango_host_droidzebra_drive: force-exit thread zeForceExit\n");
    ((mango_void_fn)a->force_exit)(a->env, a->thiz);
    fprintf(stderr, "mango_host_droidzebra_drive: force-exit thread zeForceExit returned\n");
  }
  return NULL;
}

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <droidzebra-libs-dir> [files-dir]\n"
          "  libs-dir: directory with libdroidzebra.so (or armeabi/)\n"
          "  files-dir: optional; also MANGO_DROIDZEBRA_FILES / MANGO_ASSET_ROOT\n"
          "             must contain book.cmp.z + coeffs2.bin (writable)\n",
          argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_droidzebra_drive: trampoline %s %s\n", name,
          t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_droidzebra_XXXXXX") >=
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

static void* load_abs(const char* dir, const char* name) {
  char path[PATH_MAX], resolved[PATH_MAX];
  if (join_path(path, sizeof(path), dir, name) != 0 || realpath(path, resolved) == NULL) {
    fprintf(stderr, "mango_host_droidzebra_drive: cannot resolve %s/%s\n", dir, name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_droidzebra_drive: loadLibrary(%s) %s\n", name,
          h ? "ok" : "FAIL");
  return h;
}

static void* load_named(const char* libs, const char* so) {
  char nested[PATH_MAX];
  char nested7[PATH_MAX];
  if (snprintf(nested, sizeof(nested), "armeabi/%s", so) >= (int)sizeof(nested)) return NULL;
  if (snprintf(nested7, sizeof(nested7), "armeabi-v7a/%s", so) >= (int)sizeof(nested7))
    return NULL;
  const char* try_names[] = {nested, so, nested7, NULL};
  for (int i = 0; try_names[i]; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), libs, try_names[i]) != 0) continue;
    if (access(path, R_OK) != 0) continue;
    void* h = load_abs(libs, try_names[i]);
    if (h) return h;
  }
  return NULL;
}

static int env_int(const char* key, int def) {
  const char* e = getenv(key);
  if (!e || !e[0]) return def;
  char* end = NULL;
  long v = strtol(e, &end, 10);
  if (end == e) return def;
  return (int)v;
}

static int resolve_files_dir(const char* libs, const char* argv2, char* out, size_t out_sz) {
  const char* cand = NULL;
  if (argv2 && argv2[0]) {
    cand = argv2;
  } else if (getenv("MANGO_DROIDZEBRA_FILES") && getenv("MANGO_DROIDZEBRA_FILES")[0]) {
    cand = getenv("MANGO_DROIDZEBRA_FILES");
  } else if (getenv("MANGO_ASSET_ROOT") && getenv("MANGO_ASSET_ROOT")[0]) {
    cand = getenv("MANGO_ASSET_ROOT");
  } else {
    /* sibling files/ or assets/ next to libs/ (static so cand lifetime is safe) */
    static char try_files[PATH_MAX];
    static char try_assets[PATH_MAX];
    if (snprintf(try_files, sizeof(try_files), "%s/../files", libs) < (int)sizeof(try_files)) {
      if (access(try_files, R_OK) == 0) cand = try_files;
    }
    if (!cand &&
        snprintf(try_assets, sizeof(try_assets), "%s/../assets", libs) <
            (int)sizeof(try_assets)) {
      if (access(try_assets, R_OK) == 0) cand = try_assets;
    }
  }
  if (!cand) {
    fprintf(stderr, "mango_host_droidzebra_drive: no files-dir (need book.cmp.z+coeffs2.bin)\n");
    return -1;
  }
  if (!realpath(cand, out)) {
    /* realpath fails if path does not exist; copy as-is if accessible */
    if (access(cand, R_OK) != 0) {
      fprintf(stderr, "mango_host_droidzebra_drive: files-dir inaccessible: %s\n", cand);
      return -1;
    }
    if (snprintf(out, out_sz, "%s", cand) >= (int)out_sz) return -1;
  }
  /* Ensure required assets present */
  char book[PATH_MAX], coeffs[PATH_MAX];
  if (join_path(book, sizeof(book), out, "book.cmp.z") != 0 ||
      join_path(coeffs, sizeof(coeffs), out, "coeffs2.bin") != 0) {
    return -1;
  }
  if (access(book, R_OK) != 0) {
    fprintf(stderr, "mango_host_droidzebra_drive: missing %s\n", book);
    return -1;
  }
  if (access(coeffs, R_OK) != 0) {
    fprintf(stderr, "mango_host_droidzebra_drive: missing %s\n", coeffs);
    return -1;
  }
  setenv("MANGO_ASSET_ROOT", out, 1);
  setenv("MANGO_DROIDZEBRA_FILES", out, 1);
  return 0;
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
  fprintf(stderr, "mango_host_droidzebra_drive: libs=%s files=%s\n", libs, files_dir);

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_droidzebra_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h = load_named(libs, "libdroidzebra.so");
  if (!h) {
    fprintf(stderr, "mango_host_droidzebra_drive: libdroidzebra.so load FAIL\n");
    rc = 1;
    goto done;
  }

  /* Explicit Java_* exports (no JNI_OnLoad). Dex _12 shortys. */
  void* ze_init =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeGlobalInit", "VL", 2);
  void* ze_term =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeGlobalTerminate", "V", 1);
  void* ze_play =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zePlay", "VIL", 3);
  void* ze_force_exit =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeForceExit", "V", 1);
  void* ze_force_return =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeForceReturn", "V", 1);
  void* ze_in_progress =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeGameInProgress", "Z", 1);
  void* ze_set_book =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeSetUseBook", "VI", 2);
  void* ze_set_player =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeSetPlayerInfo", "VIIIIII", 7);
  void* ze_set_practice =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeSetPracticeMode", "VI", 2);
  void* ze_set_auto =
      find_tramp(h, "Java_com_shurik_droidzebra_ZebraEngine_zeSetAutoMakeMoves", "VI", 2);
  (void)ze_force_return;
  (void)ze_in_progress;
  (void)ze_set_practice;
  (void)ze_set_auto;

  if (!ze_init) {
    fprintf(stderr, "mango_host_droidzebra_drive: missing zeGlobalInit trampoline\n");
    rc = 1;
    goto unload;
  }

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;
  /* Host char* is fake jstring for GetStringUTFChars. */
  jstring files_js = (jstring)files_dir;

  fprintf(stderr, "mango_host_droidzebra_drive: calling zeGlobalInit(\"%s\")\n", files_dir);
  ((mango_str_fn)ze_init)(env, thiz, files_js);
  fprintf(stderr, "mango_host_droidzebra_drive: zeGlobalInit returned\n");

  int use_book = env_int("MANGO_DROIDZEBRA_USE_BOOK", 1);
  if (ze_set_book) {
    fprintf(stderr, "mango_host_droidzebra_drive: calling zeSetUseBook(%d)\n", use_book);
    ((mango_i_fn)ze_set_book)(env, thiz, use_book);
    fprintf(stderr, "mango_host_droidzebra_drive: zeSetUseBook returned\n");
  }

  int skill = env_int("MANGO_DROIDZEBRA_SKILL", 4);
  int exact = env_int("MANGO_DROIDZEBRA_EXACT", 12);
  int wld = env_int("MANGO_DROIDZEBRA_WLD", 12);
  if (ze_set_player) {
    fprintf(stderr,
            "mango_host_droidzebra_drive: calling zeSetPlayerInfo BLACK skill=%d "
            "exact=%d wld=%d\n",
            skill, exact, wld);
    ((mango_player_fn)ze_set_player)(env, thiz, DZ_BLACKSQ, skill, exact, wld,
                                     DZ_INFINIT_TIME, 0);
    fprintf(stderr, "mango_host_droidzebra_drive: zeSetPlayerInfo BLACK returned\n");
    fprintf(stderr,
            "mango_host_droidzebra_drive: calling zeSetPlayerInfo WHITE skill=%d "
            "exact=%d wld=%d\n",
            skill, exact, wld);
    ((mango_player_fn)ze_set_player)(env, thiz, DZ_WHITESQ, skill, exact, wld,
                                     DZ_INFINIT_TIME, 0);
    fprintf(stderr, "mango_host_droidzebra_drive: zeSetPlayerInfo WHITE returned\n");
  }

  int skip_play = env_int("MANGO_DROIDZEBRA_SKIP_PLAY", 0);
  if (!skip_play && ze_play) {
    int force_ms = env_int("MANGO_DROIDZEBRA_FORCE_EXIT_MS", 5000);
    if (force_ms < 0) force_ms = 0;
    if (force_ms > 600000) force_ms = 600000;
    pthread_t force_thr;
    mango_dz_force_args force_args;
    int have_force = 0;
    if (force_ms > 0 && ze_force_exit) {
      force_args.env = env;
      force_args.thiz = thiz;
      force_args.force_exit = ze_force_exit;
      force_args.delay_ms = force_ms;
      if (pthread_create(&force_thr, NULL, mango_dz_force_thread, &force_args) == 0) {
        have_force = 1;
        fprintf(stderr,
                "mango_host_droidzebra_drive: force-exit thread armed (%d ms)\n", force_ms);
      } else {
        fprintf(stderr, "mango_host_droidzebra_drive: force-exit pthread_create FAIL\n");
      }
    }
    fprintf(stderr, "mango_host_droidzebra_drive: calling zePlay(0, NULL)\n");
    ((mango_play_fn)ze_play)(env, thiz, 0, NULL);
    fprintf(stderr, "mango_host_droidzebra_drive: zePlay returned\n");
    if (have_force) {
      pthread_join(force_thr, NULL);
    }
  } else if (skip_play) {
    fprintf(stderr, "mango_host_droidzebra_drive: SKIP_PLAY=1 — not calling zePlay\n");
  } else {
    fprintf(stderr, "mango_host_droidzebra_drive: zePlay trampoline missing\n");
  }

  if (ze_term) {
    fprintf(stderr, "mango_host_droidzebra_drive: calling zeGlobalTerminate\n");
    ((mango_void_fn)ze_term)(env, thiz);
    fprintf(stderr, "mango_host_droidzebra_drive: zeGlobalTerminate returned\n");
  }

unload:
  (void)h;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_droidzebra_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_droidzebra_drive: saw mango: interp stop (paste that line for the "
            "ISA team)\n");
    return 1;
  }
  return rc;
}
