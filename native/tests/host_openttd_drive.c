#define _GNU_SOURCE
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <dirent.h>
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
 * Host OpenTTD (SDL/JNI, pelya org.openttd.sdl (OpenTTD 1.6.0.56)) driver.
 * Patterned on mango_host_meritous_drive. First goal: load → JNI_OnLoad /
 * ctors → nativeInit → SDL_main and capture first mango: interp stop.
 */

typedef jint (*mango_onload_fn)(void* vm, void* reserved);
typedef void (*mango_void2_fn)(JNIEnv* env, jobject thiz);
typedef void (*mango_native_init_fn)(JNIEnv* env, jobject thiz, jobject jstr0,
                                     jobject jstr1, jint w, jint h);
typedef int (*mango_sdl_main_fn)(int argc, char** argv);

static void usage(const char* argv0) {
  fprintf(stderr, "usage: %s <openttd-libs-dir>\n", argv0);
}

static void* find_tramp(void* handle, const char* name, const char* shorty, uint32_t len) {
  void* t = NativeBridgeItf.getTrampoline(handle, name, shorty, len);
  fprintf(stderr, "mango_host_openttd_drive: trampoline %s %s\n", name, t ? "ok" : "missing");
  return t;
}

static int capture_stderr_begin(int* saved_fd, char* log_path, size_t log_path_sz) {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL || tmpdir[0] == '\0') tmpdir = "/tmp";
  if (snprintf(log_path, log_path_sz, "%s/mango_host_openttd_XXXXXX", tmpdir) >= (int)log_path_sz) {
    return -1;
  }
  int fd = mkstemp(log_path);
  if (fd < 0) return -1;
  *saved_fd = dup(STDERR_FILENO);
  if (*saved_fd < 0) { close(fd); unlink(log_path); return -1; }
  if (dup2(fd, STDERR_FILENO) < 0) { close(fd); close(*saved_fd); unlink(log_path); return -1; }
  close(fd);
  return 0;
}

static int capture_stderr_end(int saved_fd, const char* log_path, int* saw_stop) {
  fflush(stderr);
  if (dup2(saved_fd, STDERR_FILENO) < 0) return -1;
  close(saved_fd);
  FILE* f = fopen(log_path, "r");
  if (!f) { unlink(log_path); return -1; }
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
    fprintf(stderr, "mango_host_openttd_drive: cannot resolve %s\n", name);
    return NULL;
  }
  void* h = NativeBridgeItf.loadLibrary(resolved, 0);
  fprintf(stderr, "mango_host_openttd_drive: loadLibrary(%s) %s\n", name, h ? "ok" : "FAIL");
  return h;
}

/* Prefetch every lib*.so in the libs dir (OpenTTD has many DT_NEEDED siblings). */
static void prefetch_libs(const char* dir) {
  DIR* d = opendir(dir);
  if (!d) return;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    const char* n = ent->d_name;
    size_t len = strlen(n);
    if (len < 7 || strncmp(n, "lib", 3) != 0) continue;
    if (strcmp(n + len - 3, ".so") != 0) continue;
    if (strcmp(n, "libsdl-1.2.so") == 0) continue; /* already loaded */
    (void)load_abs(dir, n);
  }
  closedir(d);
}

int main(int argc, char** argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  if (argc != 2 || argv[1][0] == '-') { usage(argv[0]); return 2; }
  const char* libs = argv[1];

  int saved_err = -1;
  char log_path[PATH_MAX];
  if (capture_stderr_begin(&saved_err, log_path, sizeof(log_path)) != 0) {
    fprintf(stderr, "mango_host_openttd_drive: could not capture stderr\n");
    return 1;
  }

  int rc = 0;
  void* h_sdl = load_abs(libs, "libsdl-1.2.so");
  if (!h_sdl) { rc = 1; goto done; }

  void* onload = find_tramp(h_sdl, "JNI_OnLoad", "IL", 2);
  if (!onload) { rc = 1; goto unload; }
  jint ver = ((mango_onload_fn)onload)((void*)(uintptr_t)1, NULL);
  fprintf(stderr, "mango_host_openttd_drive: JNI_OnLoad returned 0x%x\n", (unsigned)ver);

  void* cb = find_tramp(h_sdl, "Java_org_openttd_sdl_DemoRenderer_nativeInitJavaCallbacks", "V", 0);
  if (cb) {
    ((mango_void2_fn)cb)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2);
    fprintf(stderr, "mango_host_openttd_drive: nativeInitJavaCallbacks returned\n");
  }

  prefetch_libs(libs);
  void* h_app = load_abs(libs, "libapplication.so");
  void* h_main = load_abs(libs, "libsdl_main.so");
  if (!h_app || !h_main) { rc = 1; goto unload; }

  void* init = find_tramp(h_main, "Java_org_openttd_sdl_DemoRenderer_nativeInit",
                          "(Ljava/lang/String;Ljava/lang/String;II)V", 0);
  if (init) {
    fprintf(stderr, "mango_host_openttd_drive: calling nativeInit\n");
    ((mango_native_init_fn)init)((JNIEnv*)(uintptr_t)1, (jobject)(uintptr_t)2,
                                 (jobject)(void*)libs, (jobject)(void*)libs, 640, 480);
    fprintf(stderr, "mango_host_openttd_drive: nativeInit returned\n");
  }

  void* sm = find_tramp(h_app, "SDL_main", "*III", 4);
  if (!sm) sm = find_tramp(h_app, "SDL_main", "II", 2);
  if (!sm) {
    fprintf(stderr, "mango_host_openttd_drive: no SDL_main trampoline\n");
    rc = 1;
    goto unload;
  }
  fprintf(stderr, "mango_host_openttd_drive: calling SDL_main(0, NULL)\n");
  int smrc = ((mango_sdl_main_fn)sm)(0, NULL);
  fprintf(stderr, "mango_host_openttd_drive: SDL_main returned %d\n", smrc);

unload:
  (void)h_main;
  (void)h_sdl;
  (void)h_app;

done:;
  int saw_stop = 0;
  if (capture_stderr_end(saved_err, log_path, &saw_stop) != 0) {
    fprintf(stderr, "mango_host_openttd_drive: failed to restore/dump stderr\n");
    return 1;
  }
  if (saw_stop) {
    fprintf(stderr,
            "mango_host_openttd_drive: saw mango: interp stop (paste that line for the ISA fix)\n");
    return 1;
  }
  return rc;
}
