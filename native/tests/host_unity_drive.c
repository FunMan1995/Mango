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
#include <sys/stat.h>
#include <unistd.h>

#include "mango/native_bridge.h"

extern struct NativeBridgeCallbacks NativeBridgeItf;

/*
 * Host-only driver for Test Lab: load OFDP/Unity ARM32 libmain.so through
 * the native bridge shim, run JNI_OnLoad + NativeLoader.load + optional
 * init/pause/render trampolines, and surface the first uncovered-opcode
 * "mango: interp stop" line on stderr.
 *
 * Guest dlopen (mango_libc_svc DLOPEN) resolves relative names against the
 * directory of the path passed to loadLibrary. Pass an absolute path to
 * libmain.so (this tool realpath()s) and keep libunity.so / libmono.so as
 * siblings in that directory. Host LD_LIBRARY_PATH is not consulted by the
 * guest loader; cwd only matters if you pass a relative libmain path and
 * skip resolution.
 */

typedef jint (*mango_onload_fn)(void* vm, void* reserved);

/* Enough trailing zeros for typical Unity JNI shapes (word-sized args). */
typedef intptr_t (*mango_poly_fn)(JNIEnv* env, jobject thiz, intptr_t a0, intptr_t a1,
                                  intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5,
                                  intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9,
                                  intptr_t a10, intptr_t a11, intptr_t a12, intptr_t a13,
                                  intptr_t a14, intptr_t a15);

static void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s <dir-or-libmain.so> [--skip-render]\n"
          "\n"
          "  Load libmain.so via NativeBridgeItf, call JNI_OnLoad, then\n"
          "  trampolines registered as NativeLoader.load / initJni /\n"
          "  nativePause / nativeRender (skip render with --skip-render).\n"
          "\n"
          "  Put libmain.so, libunity.so, and libmono.so in one directory.\n"
          "  Guest dlopen resolves relative names next to libmain's path.\n"
          "  Example:\n"
          "    %s /path/to/ofdp-libs\n"
          "  Exit 2 on usage errors; nonzero if an interp stop is seen or\n"
          "  a required step fails; 0 if every called trampoline returns.\n",
          argv0, argv0);
}

static int path_is_dir(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISDIR(st.st_mode);
}

static int resolve_libmain(const char* arg, char* out, size_t out_sz) {
  char candidate[PATH_MAX];
  if (path_is_dir(arg)) {
    int n = snprintf(candidate, sizeof(candidate), "%s/libmain.so", arg);
    if (n < 0 || (size_t)n >= sizeof(candidate)) {
      fprintf(stderr, "mango_host_unity_drive: path too long\n");
      return -1;
    }
  } else {
    if (snprintf(candidate, sizeof(candidate), "%s", arg) >= (int)sizeof(candidate)) {
      fprintf(stderr, "mango_host_unity_drive: path too long\n");
      return -1;
    }
  }
  if (access(candidate, R_OK) != 0) {
    fprintf(stderr, "mango_host_unity_drive: cannot read %s: %s\n", candidate, strerror(errno));
    return -1;
  }
  if (realpath(candidate, out) == NULL) {
    /* realpath needs the buffer to be PATH_MAX; fall back to candidate */
    if (strlen(candidate) + 1 > out_sz) {
      return -1;
    }
    memcpy(out, candidate, strlen(candidate) + 1);
  }
  return 0;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  if (snprintf(log_path, log_path_sz, "/tmp/mango_host_unity_drive_XXXXXX") >= (int)log_path_sz) {
    return -1;
  }
  int fd = mkstemp(log_path);
  if (fd < 0) {
    return -1;
  }
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
  if (dup2(saved_fd, STDERR_FILENO) < 0) {
    close(saved_fd);
    return -1;
  }
  close(saved_fd);

  FILE* f = fopen(log_path, "rb");
  unlink(log_path);
  if (!f) {
    return -1;
  }
  *saw_stop = 0;
  char buf[4096];
  size_t n;
  /* Sliding tail so a split "mango: interp stop" across reads is still found. */
  char tail[32];
  size_t tail_len = 0;
  memset(tail, 0, sizeof(tail));
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    (void)fwrite(buf, 1, n, stderr);
    if (tail_len > 0) {
      char window[sizeof(tail) + sizeof(buf)];
      memcpy(window, tail, tail_len);
      memcpy(window + tail_len, buf, n);
      window[tail_len + n] = '\0';
      if (strstr(window, "mango: interp stop") != NULL) {
        *saw_stop = 1;
      }
    } else {
      /* Ensure buf is searchable as C string in chunks: scan with memmem-ish loop */
      for (size_t i = 0; i + 18 <= n; i++) {
        if (memcmp(buf + i, "mango: interp stop", 18) == 0) {
          *saw_stop = 1;
          break;
        }
      }
    }
    if (n >= sizeof(tail) - 1) {
      memcpy(tail, buf + n - (sizeof(tail) - 1), sizeof(tail) - 1);
      tail_len = sizeof(tail) - 1;
      tail[tail_len] = '\0';
    } else {
      memcpy(tail, buf, n);
      tail_len = n;
      tail[tail_len] = '\0';
    }
  }
  fclose(f);
  fflush(stderr);
  return 0;
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  if (t) {
    fprintf(stderr, "mango_host_unity_drive: trampoline %s ok\n", name);
  } else {
    fprintf(stderr, "mango_host_unity_drive: trampoline %s missing\n", name);
  }
  return t;
}

static intptr_t call_poly(void* tramp, JNIEnv* env, jobject thiz, intptr_t a0) {
  return ((mango_poly_fn)tramp)(env, thiz, a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

int main(int argc, char** argv) {
  const char* path_arg = NULL;
  int skip_render = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--skip-render") == 0) {
      skip_render = 1;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else if (path_arg == NULL) {
      path_arg = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (path_arg == NULL) {
    usage(argv[0]);
    return 2;
  }

  char libmain[PATH_MAX];
  if (resolve_libmain(path_arg, libmain, sizeof(libmain)) != 0) {
    return 1;
  }
  fprintf(stderr, "mango_host_unity_drive: libmain=%s\n", libmain);

  int saved_err = -1;
  char log_path[64];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_unity_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* handle = NativeBridgeItf.loadLibrary(libmain, 0);
  if (!handle) {
    const char* err = NativeBridgeItf.getError ? NativeBridgeItf.getError() : NULL;
    fprintf(stderr, "mango_host_unity_drive: loadLibrary failed%s%s\n", err ? ": " : "",
            err ? err : "");
    rc = 1;
    goto done;
  }
  fprintf(stderr, "mango_host_unity_drive: loadLibrary ok\n");

  /*
   * Unity libmain exports JNI_OnLoad. Prefer that name with ART shorty "IL"
   * (JavaVM*, void* reserved). Fake JNIEnv/JavaVM: (void*)(uintptr_t)1.
   */
  void* onload = find_tramp(handle, "JNI_OnLoad", "IL", 2);
  if (!onload) {
    fprintf(stderr, "mango_host_unity_drive: no JNI_OnLoad export\n");
    rc = 1;
    goto unload;
  }
  jint ver = ((mango_onload_fn)onload)((void*)(uintptr_t)1, NULL);
  fprintf(stderr, "mango_host_unity_drive: JNI_OnLoad returned 0x%x\n", (unsigned)ver);

  JNIEnv* env = (JNIEnv*)(uintptr_t)1;
  jobject thiz = (jobject)(uintptr_t)2;

  /*
   * After OnLoad, RegisterNatives has bound method names with full JNI
   * signatures in the slot shorty field (see shim). Look up by name only;
   * getTrampoline returns the existing slot. Common Unity shape for load is
   * (Ljava/lang/String;)Z — pass a host C string as a fake jstring (shim's
   * GetStringUTFChars fake path returns the pointer as chars).
   */
  void* load = find_tramp(handle, "load", "(Ljava/lang/String;)Z", 0);
  if (load) {
    /* OFDP: NativeLoader.load takes a directory name; libs live in <dir>/unity/. */
    fprintf(stderr, "mango_host_unity_drive: calling load(unity)\n");
    intptr_t jstr = (intptr_t)(void*)"unity";
    intptr_t ok = call_poly(load, env, thiz, jstr);
    fprintf(stderr, "mango_host_unity_drive: load(unity) returned %ld\n", (long)ok);
  } else {
    fprintf(stderr,
            "mango_host_unity_drive: warning: no 'load' trampoline (RegisterNatives may use "
            "another name; check mango: RegisterNatives lines)\n");
  }

  void* init_jni = find_tramp(handle, "initJni", "(Landroid/content/Context;)V", 0);
  if (init_jni) {
    (void)call_poly(init_jni, env, thiz, (intptr_t)(uintptr_t)3);
    fprintf(stderr, "mango_host_unity_drive: initJni returned\n");
  }

  void* pause = find_tramp(handle, "nativePause", "()V", 0);
  if (pause) {
    (void)call_poly(pause, env, thiz, 0);
    fprintf(stderr, "mango_host_unity_drive: nativePause returned\n");
  }

  if (!skip_render) {
    void* render = find_tramp(handle, "nativeRender", "()V", 0);
    if (render) {
      (void)call_poly(render, env, thiz, 0);
      fprintf(stderr, "mango_host_unity_drive: nativeRender returned\n");
    } else {
      fprintf(stderr, "mango_host_unity_drive: warning: no nativeRender trampoline\n");
    }
  } else {
    fprintf(stderr, "mango_host_unity_drive: skipping nativeRender (--skip-render)\n");
  }

unload:
  if (handle) {
    NativeBridgeItf.unloadLibrary(handle);
  }

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_unity_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_unity_drive: saw mango: interp stop (paste that line for the ISA fix)\n");
    return 1;
  }
  return rc;
}
