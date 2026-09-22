/* Must come before any system header, see mango/native_bridge.h for why. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

/* Implements native/include/mango/native_bridge.h, exported as "NativeBridgeItf". */
#include <elf.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mango/cpu.h"
#include "mango/decoder.h"
#include "mango/elf32.h"
#include "mango/interp.h"
#include "mango/native_bridge.h"

#define MANGO_STACK_SIZE 0x10000u
#define MANGO_HEAP_SIZE 0x400000u
#define MANGO_JNI_TABLE_LEN 256u
#define MANGO_JVM_TABLE_LEN 8u
#define MANGO_JNI_THUNK_SIZE 16u
#define MANGO_JNI_SVC_BASE 0x1000u
#define MANGO_JVM_SVC_BASE 0x1800u
#define MANGO_LIBC_SVC_BASE 0x2000u
#define MANGO_JNI_STOP 0xFFFFFFF0u
#define MANGO_JNI_SLOTS 64
#define MANGO_HANDLE_MAX 4096

#define MANGO_JNI_GET_VERSION 4
#define MANGO_JNI_FIND_CLASS 6
#define MANGO_JNI_THROW 13
#define MANGO_JNI_THROW_NEW 14
#define MANGO_JNI_EXCEPTION_OCCURRED 15
#define MANGO_JNI_EXCEPTION_CLEAR 17
#define MANGO_JNI_FATAL_ERROR 18
#define MANGO_JNI_NEW_GLOBAL_REF 21
#define MANGO_JNI_DELETE_LOCAL_REF 23
#define MANGO_JNI_IS_SAME_OBJECT 24
#define MANGO_JNI_GET_OBJECT_CLASS 31
#define MANGO_JNI_GET_METHOD_ID 33
#define MANGO_JNI_NEW_OBJECT 28
#define MANGO_JNI_CALL_OBJECT_METHOD 34
#define MANGO_JNI_CALL_BOOLEAN_METHOD 37
#define MANGO_JNI_CALL_INT_METHOD 49
#define MANGO_JNI_CALL_VOID_METHOD 61
#define MANGO_JNI_GET_FIELD_ID 94
#define MANGO_JNI_GET_OBJECT_FIELD 95
#define MANGO_JNI_GET_BOOLEAN_FIELD 96
#define MANGO_JNI_GET_INT_FIELD 100
#define MANGO_JNI_SET_OBJECT_FIELD 104
#define MANGO_JNI_SET_INT_FIELD 109
#define MANGO_JNI_GET_STATIC_METHOD_ID 113
#define MANGO_JNI_CALL_STATIC_OBJECT_METHOD 114
#define MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD 117
#define MANGO_JNI_CALL_STATIC_INT_METHOD 129
#define MANGO_JNI_CALL_STATIC_VOID_METHOD 141
#define MANGO_JNI_GET_STATIC_FIELD_ID 144
#define MANGO_JNI_GET_STATIC_OBJECT_FIELD 145
#define MANGO_JNI_GET_STATIC_INT_FIELD 150
#define MANGO_JNI_NEW_STRING_UTF 167
#define MANGO_JNI_GET_STRING_UTF_LENGTH 168
#define MANGO_JNI_GET_STRING_UTF_CHARS 169
#define MANGO_JNI_RELEASE_STRING_UTF_CHARS 170
#define MANGO_JNI_REGISTER_NATIVES 215
#define MANGO_JNI_GET_JAVA_VM 219
#define MANGO_JNI_EXCEPTION_CHECK 228

#define MANGO_JVM_DESTROY 3
#define MANGO_JVM_ATTACH 4
#define MANGO_JVM_DETACH 5
#define MANGO_JVM_GET_ENV 6
#define MANGO_JVM_ATTACH_DAEMON 7

#define MANGO_LIBC_MALLOC 0
#define MANGO_LIBC_FREE 1
#define MANGO_LIBC_MEMCPY 2
#define MANGO_LIBC_MEMMOVE 3
#define MANGO_LIBC_MEMSET 4
#define MANGO_LIBC_MEMCMP 5
#define MANGO_LIBC_MEMCHR 6
#define MANGO_LIBC_STRLEN 7
#define MANGO_LIBC_STRCMP 8
#define MANGO_LIBC_STRNCMP 9
#define MANGO_LIBC_STRCPY 10
#define MANGO_LIBC_ABORT 11
#define MANGO_LIBC_MEMMEM 12
#define MANGO_LIBC_AEABI_MEMCPY 13
#define MANGO_LIBC_ERRNO 14
#define MANGO_LIBC_STRTOL 15
#define MANGO_LIBC_DLOPEN 16
#define MANGO_LIBC_DLSYM 17
#define MANGO_LIBC_DLCLOSE 18
#define MANGO_LIBC_DLERROR 19
#define MANGO_LIBC_ALOG 20
#define MANGO_LIBC_CALLOC 21
#define MANGO_LIBC_AEABI_MEMSET 22
#define MANGO_LIBC_AEABI_MEMMOVE 23
#define MANGO_LIBC_PRINTF 24
#define MANGO_LIBC_GETPID 25
#define MANGO_LIBC_PTHREAD_LOCK 26
#define MANGO_LIBC_PTHREAD_UNLOCK 27
#define MANGO_LIBC_CLOCK_GETTIME 28
#define MANGO_LIBC_SIN 29
#define MANGO_LIBC_COS 30
#define MANGO_LIBC_TAN 31
#define MANGO_LIBC_SQRT 32
#define MANGO_LIBC_POW 33
#define MANGO_LIBC_EXP 34
#define MANGO_LIBC_LOG 35
#define MANGO_LIBC_FLOOR 36
#define MANGO_LIBC_FABS 37
#define MANGO_LIBC_ATAN2 38
#define MANGO_LIBC_SINF 39
#define MANGO_LIBC_COSF 40
#define MANGO_LIBC_SQRTF 41
#define MANGO_LIBC_POWF 42
#define MANGO_LIBC_FLOORF 43
#define MANGO_LIBC_FABSF 44
#define MANGO_LIBC_PTHREAD_SELF 45
#define MANGO_LIBC_PTHREAD_CREATE 46
#define MANGO_LIBC_SNPRINTF 47
#define MANGO_LIBC_EGL_TRUE 48
#define MANGO_LIBC_EGL_SUCCESS 49
#define MANGO_LIBC_EGL_PROC 50
#define MANGO_LIBC_ANW_W 51
#define MANGO_LIBC_ANW_H 52
#define MANGO_LIBC_PTHREAD_ONCE 53
#define MANGO_LIBC_OPEN 54
#define MANGO_LIBC_READ 55
#define MANGO_LIBC_CLOSE 56
#define MANGO_LIBC_EGL_INIT 57
#define MANGO_LIBC_EGL_CHOOSE 58
#define MANGO_LIBC_EGL_SURFACE 59
#define MANGO_LIBC_EGL_CONTEXT 60
#define MANGO_LIBC_EGL_QUERY 61
#define MANGO_LIBC_ANW_FROM 62
#define MANGO_LIBC_PTHREAD_KEY_CREATE 63
#define MANGO_LIBC_PTHREAD_GETSPECIFIC 64
#define MANGO_LIBC_PTHREAD_SETSPECIFIC 65
#define MANGO_LIBC_PTHREAD_KEY_DELETE 66
#define MANGO_LIBC_COUNT 67
#define MANGO_TSD_KEYS 16

#define MANGO_AS_SIZE 0x2800000u
#define MANGO_LIB_CAP 0x2000000u
#define MANGO_MAX_LIBS 16

static bool mango_is_arm32_elf(const char* libpath) {
  int fd = open(libpath, O_RDONLY);
  if (fd < 0) {
    return false;
  }

  Elf32_Ehdr hdr;
  ssize_t n = read(fd, &hdr, sizeof(hdr));
  close(fd);

  if (n != (ssize_t)sizeof(hdr)) {
    return false;
  }
  if (memcmp(hdr.e_ident, ELFMAG, SELFMAG) != 0) {
    return false;
  }
  if (hdr.e_ident[EI_CLASS] != ELFCLASS32) {
    return false;
  }
  return hdr.e_machine == EM_ARM;
}

/* What loadLibrary actually hands getTrampoline: the whole file (elf32.c
 * reads straight out of it, doesn't copy) plus the guest memory its
 * PT_LOAD segments were copied into. Never freed, see unloadLibrary's
 * comment below for why that's a real, known, deliberate gap. */
typedef struct MangoLoadedLibrary {
  uint8_t* file_data;
  uint8_t* guest_mem;
  uint32_t guest_mem_size;
  uint32_t jni_env_addr;
  uint32_t jni_vm_addr;
  uint32_t libc_thunk_base;
  uint32_t stack_top;
  uint32_t heap_base;
  uint32_t heap_used;
  uint32_t guest_errno_addr;
  uint32_t load_bias;
  uint32_t stub_addr;
  void* host_vm;
  char path[512];
  MangoElf32Image image;
} MangoLoadedLibrary;

typedef struct MangoJniSlot {
  MangoLoadedLibrary* lib;
  uint32_t guest_pc;
  char shorty[32];
  char name[64];
} MangoJniSlot;

static const char* const kLibcNames[MANGO_LIBC_COUNT] = {
    "malloc",
    "free",
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "memchr",
    "strlen",
    "strcmp",
    "strncmp",
    "strcpy",
    "abort",
    "memmem",
    "__aeabi_memcpy",
    "__errno",
    "strtol",
    "dlopen",
    "dlsym",
    "dlclose",
    "dlerror",
    "__android_log_print",
    "calloc",
    "__aeabi_memset",
    "__aeabi_memmove",
    "printf",
    "getpid",
    "pthread_mutex_lock",
    "pthread_mutex_unlock",
    "clock_gettime",
    "sin",
    "cos",
    "tan",
    "sqrt",
    "pow",
    "exp",
    "log",
    "floor",
    "fabs",
    "atan2",
    "sinf",
    "cosf",
    "sqrtf",
    "powf",
    "floorf",
    "fabsf",
    "pthread_self",
    "pthread_create",
    "snprintf",
    "eglGetDisplay",
    "eglGetError",
    "eglGetProcAddress",
    "ANativeWindow_getWidth",
    "ANativeWindow_getHeight",
    "pthread_once",
    "open",
    "read",
    "close",
    "eglInitialize",
    "eglChooseConfig",
    "eglCreateWindowSurface",
    "eglCreateContext",
    "eglQueryString",
    "ANativeWindow_fromSurface",
    "pthread_key_create",
    "pthread_getspecific",
    "pthread_setspecific",
    "pthread_key_delete",
};

static MangoJniSlot g_slots[MANGO_JNI_SLOTS];
static int g_nslots;
static void* g_handles[MANGO_HANDLE_MAX];
static uint32_t g_nhandles = 1; /* 0 is JNI NULL */
static MangoLoadedLibrary* g_libs[MANGO_MAX_LIBS];
static int g_nlibs;
static uint8_t* g_as_mem;
static uint32_t g_as_size;
static uint32_t g_as_next;
static uint32_t g_as_jni_ready;
static char g_dlerror[128];
static uint32_t g_tsd[MANGO_TSD_KEYS];
static uint32_t g_nkeys = 1; /* key 0 is invalid, matching glibc */
static uint32_t g_nested_sp;
static const char kCpuInfo[] =
    "Processor\t: ARMv7 Processor rev 1 (v7l)\n"
    "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
    "CPU architecture: 7\n";
static const char kCpuList[] = "0-7\n";
static const uint32_t kAuxv[] = {16u, (1u << 6) | (1u << 12) | (1u << 13) | (1u << 16) | (1u << 17),
                                 0u, 0u};
static const char* g_fake_data;
static uint32_t g_fake_len;
static uint32_t g_fake_off;

static void* mango_load_library(const char* libpath, int flag);
static int mango_run_guest(MangoLoadedLibrary* lib, MangoCpu* cpu, JNIEnv* env);
static const char* mango_guest_cstr(MangoLoadedLibrary* lib, uint32_t addr);
static uint32_t mango_guest_alloc(MangoLoadedLibrary* lib, uint32_t n);

static int mango_alloc_slot(void) {
  for (int i = 0; i < g_nslots; i++) {
    if (g_slots[i].lib == NULL) {
      return i;
    }
  }
  if (g_nslots >= MANGO_JNI_SLOTS) {
    return -1;
  }
  return g_nslots++;
}

static uint32_t mango_handle_intern(void* p) {
  if (p == NULL) {
    return 0;
  }
  for (uint32_t i = 1; i < g_nhandles; i++) {
    if (g_handles[i] == p) {
      return i;
    }
  }
  if (g_nhandles >= MANGO_HANDLE_MAX) {
    return 0;
  }
  uint32_t id = g_nhandles++;
  g_handles[id] = p;
  return id;
}

static const char g_dummy_java[] = "";
static uint32_t mango_dummy_jobject(MangoLoadedLibrary* lib) {
  uint32_t a = mango_guest_alloc(lib, 32u);
  if (a == 0) {
    return mango_handle_intern((void*)g_dummy_java);
  }
  mango_handle_intern((void*)(uintptr_t)a);
  return a;
}

static const char* mango_fake_jstring_chars(MangoLoadedLibrary* lib, void* js) {
  uintptr_t p = (uintptr_t)js;
  if (p == 0) {
    return "";
  }
  if (p < lib->guest_mem_size) {
    const char* s = mango_guest_cstr(lib, (uint32_t)p);
    return s ? s : "";
  }
  return (const char*)js;
}

static uint32_t mango_guest_alloc(MangoLoadedLibrary* lib, uint32_t n) {
  n = (n + 7u) & ~7u;
  if (n == 0 || lib->heap_used + n > MANGO_HEAP_SIZE) {
    return 0;
  }
  uint32_t a = lib->heap_base + lib->heap_used;
  memset(lib->guest_mem + a, 0, n);
  lib->heap_used += n;
  return a;
}

static void* mango_handle_lookup(uint32_t id) {
  if (id == 0) {
    return NULL;
  }
  if (id < g_nhandles) {
    return g_handles[id];
  }
  for (uint32_t i = 1; i < g_nhandles; i++) {
    if ((uintptr_t)g_handles[i] == id) {
      return g_handles[i];
    }
  }
  return NULL;
}

static void mango_store_u32_guest(uint8_t* mem, uint32_t addr, uint32_t v) {
  mem[addr] = (uint8_t)(v & 0xFFu);
  mem[addr + 1] = (uint8_t)((v >> 8) & 0xFFu);
  mem[addr + 2] = (uint8_t)((v >> 16) & 0xFFu);
  mem[addr + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* A32 thunk preserves r4-r11 (AAPCS). SVC number lives in the word after
 * `bx lr` so we do not need to load it into r7.
 *   svc 0; bx lr; .word imm; nop */
static void mango_write_jni_thunk(uint8_t* mem, uint32_t addr, uint32_t imm) {
  mango_store_u32_guest(mem, addr, 0xEF000000u);
  mango_store_u32_guest(mem, addr + 4u, 0xE12FFF1Eu);
  mango_store_u32_guest(mem, addr + 8u, imm);
  mango_store_u32_guest(mem, addr + 12u, 0xE1A00000u);
}

static int mango_setup_guest_jni(MangoLoadedLibrary* lib) {
  uint32_t base = MANGO_LIB_CAP;
  uint32_t vm_table = base + 4u;
  uint32_t vm_thunks = vm_table + MANGO_JVM_TABLE_LEN * 4u;
  uint32_t env_ptr = vm_thunks + MANGO_JVM_TABLE_LEN * MANGO_JNI_THUNK_SIZE;
  uint32_t table = env_ptr + 4u;
  uint32_t thunks = table + MANGO_JNI_TABLE_LEN * 4u;
  uint32_t libc_thunks = thunks + MANGO_JNI_TABLE_LEN * MANGO_JNI_THUNK_SIZE;
  uint32_t stub = libc_thunks + MANGO_LIBC_COUNT * MANGO_JNI_THUNK_SIZE;
  uint32_t heap = stub + MANGO_JNI_THUNK_SIZE;
  uint32_t need = heap + MANGO_HEAP_SIZE + MANGO_STACK_SIZE;
  if (need > g_as_size) {
    return -1;
  }
  lib->guest_mem = g_as_mem;
  lib->guest_mem_size = g_as_size;
  if (g_as_jni_ready && g_nlibs > 0) {
    MangoLoadedLibrary* first = g_libs[0];
    lib->jni_vm_addr = first->jni_vm_addr;
    lib->jni_env_addr = first->jni_env_addr;
    lib->libc_thunk_base = first->libc_thunk_base;
    lib->stub_addr = first->stub_addr;
    lib->heap_base = first->heap_base;
    lib->heap_used = first->heap_used;
    lib->guest_errno_addr = first->guest_errno_addr;
    lib->stack_top = first->stack_top;
    lib->host_vm = first->host_vm;
    return 0;
  }
  uint8_t* mem = g_as_mem;
  lib->jni_vm_addr = base;
  lib->jni_env_addr = env_ptr;
  lib->libc_thunk_base = libc_thunks;
  lib->stub_addr = stub;
  lib->heap_base = heap;
  lib->heap_used = 8u; /* errno at +0, stack guard at +4 */
  lib->guest_errno_addr = heap;
  mango_store_u32_guest(mem, heap + 4u, 0xA5A5A5A5u);
  lib->stack_top = need - 16u;
  lib->host_vm = NULL;
  mango_store_u32_guest(mem, base, vm_table);
  for (uint32_t i = 0; i < MANGO_JVM_TABLE_LEN; i++) {
    uint32_t thunk = vm_thunks + i * MANGO_JNI_THUNK_SIZE;
    mango_write_jni_thunk(mem, thunk, MANGO_JVM_SVC_BASE + i);
    mango_store_u32_guest(mem, vm_table + i * 4u, thunk);
  }
  mango_store_u32_guest(mem, env_ptr, table);
  for (uint32_t i = 0; i < MANGO_JNI_TABLE_LEN; i++) {
    uint32_t thunk = thunks + i * MANGO_JNI_THUNK_SIZE;
    mango_write_jni_thunk(mem, thunk, MANGO_JNI_SVC_BASE + i);
    mango_store_u32_guest(mem, table + i * 4u, thunk);
  }
  for (uint32_t i = 0; i < MANGO_LIBC_COUNT; i++) {
    mango_write_jni_thunk(mem, libc_thunks + i * MANGO_JNI_THUNK_SIZE, MANGO_LIBC_SVC_BASE + i);
  }
  mango_store_u32_guest(mem, stub, 0xE3A00000u); /* mov r0, #0 */
  mango_store_u32_guest(mem, stub + 4u, 0xE12FFF1Eu); /* bx lr */
  g_as_jni_ready = 1;
  return 0;
}

static uint32_t mango_resolve_import(void* ctx, const char* name, uint32_t st_value,
                                     uint32_t st_shndx) {
  MangoLoadedLibrary* lib = ctx;
  (void)st_value;
  (void)st_shndx;
  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  if (strcmp(name, "gettid") == 0) {
    name = "getpid";
  }
  if (strcmp(name, "__stack_chk_guard") == 0) {
    return lib->guest_errno_addr + 4u;
  }
  if (strcmp(name, "eglGetError") == 0) {
    name = "eglGetError";
  } else if (strcmp(name, "eglGetProcAddress") == 0) {
    name = "eglGetProcAddress";
  } else if (strcmp(name, "eglInitialize") == 0) {
    name = "eglInitialize";
  } else if (strcmp(name, "eglChooseConfig") == 0 || strcmp(name, "eglGetConfigs") == 0) {
    name = "eglChooseConfig";
  } else if (strcmp(name, "eglCreateWindowSurface") == 0 ||
             strcmp(name, "eglCreatePbufferSurface") == 0) {
    name = "eglCreateWindowSurface";
  } else if (strcmp(name, "eglCreateContext") == 0) {
    name = "eglCreateContext";
  } else if (strcmp(name, "eglQueryString") == 0) {
    name = "eglQueryString";
  } else if (strcmp(name, "ANativeWindow_fromSurface") == 0) {
    name = "ANativeWindow_fromSurface";
  } else if (strcmp(name, "ANativeWindow_getWidth") == 0) {
    name = "ANativeWindow_getWidth";
  } else if (strcmp(name, "ANativeWindow_getHeight") == 0) {
    name = "ANativeWindow_getHeight";
  } else if (strncmp(name, "egl", 3) == 0 || strncmp(name, "ANative", 7) == 0 ||
             strncmp(name, "ALooper", 7) == 0 || strncmp(name, "AAsset", 6) == 0) {
    name = "eglGetDisplay";
  } else if (name[0] == 'g' && name[1] == 'l') {
    return lib->stub_addr;
  }
  for (uint32_t i = 0; i < MANGO_LIBC_COUNT; i++) {
    if (strcmp(name, kLibcNames[i]) == 0) {
      return lib->libc_thunk_base + i * MANGO_JNI_THUNK_SIZE;
    }
  }
  for (int i = 0; i < g_nlibs; i++) {
    uint32_t st = mango_elf32_find_symbol(&g_libs[i]->image, name);
    if (st != 0) {
      return st + g_libs[i]->load_bias;
    }
  }
  return lib->stub_addr;
}

static const char* mango_guest_cstr(MangoLoadedLibrary* lib, uint32_t addr) {
  if (addr == 0 || addr >= lib->guest_mem_size) {
    return NULL;
  }
  uint32_t i = addr;
  while (i < lib->guest_mem_size) {
    if (lib->guest_mem[i] == 0) {
      return (const char*)(lib->guest_mem + addr);
    }
    i++;
  }
  return NULL;
}

static uint32_t mango_load_u32_guest(const uint8_t* mem, uint32_t addr) {
  return (uint32_t)mem[addr] | ((uint32_t)mem[addr + 1u] << 8) | ((uint32_t)mem[addr + 2u] << 16) |
         ((uint32_t)mem[addr + 3u] << 24);
}

static int mango_guest_range_ok(const MangoLoadedLibrary* lib, uint32_t addr, uint32_t n) {
  return n == 0 || ((uint64_t)addr + n <= lib->guest_mem_size);
}

static uint32_t mango_guest_strdup(MangoLoadedLibrary* lib, const char* s) {
  if (s == NULL) {
    return 0;
  }
  size_t n = strlen(s) + 1;
  if (lib->heap_used + n > MANGO_HEAP_SIZE) {
    return 0;
  }
  uint32_t addr = lib->heap_base + lib->heap_used;
  memcpy(lib->guest_mem + addr, s, n);
  lib->heap_used += (uint32_t)((n + 3u) & ~3u);
  return addr;
}

static void mango_jvm_write_env(MangoLoadedLibrary* lib, uint32_t p_env) {
  if (p_env != 0 && mango_guest_range_ok(lib, p_env, 4u)) {
    mango_store_u32_guest(lib->guest_mem, p_env, lib->jni_env_addr);
  }
}

static void mango_jvm_svc(MangoLoadedLibrary* lib, MangoCpu* cpu, uint32_t fn) {
  (void)lib->host_vm; /* stashed by JNI_OnLoad for a later real Attach */
  switch (fn) {
    case MANGO_JVM_ATTACH:
    case MANGO_JVM_GET_ENV:
    case MANGO_JVM_ATTACH_DAEMON:
      mango_jvm_write_env(lib, cpu->r[1]);
      cpu->r[0] = 0; /* JNI_OK */
      break;
    case MANGO_JVM_DESTROY:
    case MANGO_JVM_DETACH:
      cpu->r[0] = 0;
      break;
    default:
      cpu->r[0] = (uint32_t)-1;
      break;
  }
}

static double mango_arg_d(const MangoCpu* cpu) {
  uint64_t u = (uint64_t)cpu->r[0] | ((uint64_t)cpu->r[1] << 32);
  double d;
  memcpy(&d, &u, 8);
  return d;
}

static double mango_arg_d2(const MangoCpu* cpu) {
  uint64_t u = (uint64_t)cpu->r[2] | ((uint64_t)cpu->r[3] << 32);
  double d;
  memcpy(&d, &u, 8);
  return d;
}

static void mango_ret_d(MangoCpu* cpu, double d) {
  uint64_t u;
  memcpy(&u, &d, 8);
  cpu->r[0] = (uint32_t)u;
  cpu->r[1] = (uint32_t)(u >> 32);
}

static float mango_arg_f(const MangoCpu* cpu) {
  float f;
  uint32_t u = cpu->r[0];
  memcpy(&f, &u, 4);
  return f;
}

static void mango_ret_f(MangoCpu* cpu, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  cpu->r[0] = u;
}

static void mango_libc_svc(MangoLoadedLibrary* lib, MangoCpu* cpu, uint32_t fn) {
  uint32_t r0 = cpu->r[0];
  uint32_t r1 = cpu->r[1];
  uint32_t r2 = cpu->r[2];
  switch (fn) {
    case MANGO_LIBC_MALLOC: {
      uint32_t n = (r0 + 7u) & ~7u;
      if (n == 0) {
        n = 8u;
      }
      if (lib->heap_used + n > MANGO_HEAP_SIZE) {
        cpu->r[0] = 0;
      } else {
        cpu->r[0] = lib->heap_base + lib->heap_used;
        lib->heap_used += n;
      }
      break;
    }
    case MANGO_LIBC_FREE:
      break;
    case MANGO_LIBC_MEMCPY:
    case MANGO_LIBC_AEABI_MEMCPY:
    case MANGO_LIBC_MEMMOVE:
    case MANGO_LIBC_AEABI_MEMMOVE:
      if (!mango_guest_range_ok(lib, r0, r2) || !mango_guest_range_ok(lib, r1, r2)) {
        cpu->r[0] = 0;
      } else {
        memmove(lib->guest_mem + r0, lib->guest_mem + r1, r2);
        cpu->r[0] = r0;
      }
      break;
    case MANGO_LIBC_MEMSET:
      if (!mango_guest_range_ok(lib, r0, r2)) {
        cpu->r[0] = 0;
      } else {
        memset(lib->guest_mem + r0, (int)(r1 & 0xFFu), r2);
        cpu->r[0] = r0;
      }
      break;
    case MANGO_LIBC_MEMCMP:
      if (!mango_guest_range_ok(lib, r0, r2) || !mango_guest_range_ok(lib, r1, r2)) {
        cpu->r[0] = (uint32_t)-1;
      } else {
        cpu->r[0] = (uint32_t)memcmp(lib->guest_mem + r0, lib->guest_mem + r1, r2);
      }
      break;
    case MANGO_LIBC_MEMCHR:
      if (!mango_guest_range_ok(lib, r0, r2)) {
        cpu->r[0] = 0;
      } else {
        const void* p = memchr(lib->guest_mem + r0, (int)(r1 & 0xFFu), r2);
        cpu->r[0] = p ? (uint32_t)((const uint8_t*)p - lib->guest_mem) : 0;
      }
      break;
    case MANGO_LIBC_STRLEN: {
      const char* s = mango_guest_cstr(lib, r0);
      cpu->r[0] = s ? (uint32_t)strlen(s) : 0;
      break;
    }
    case MANGO_LIBC_STRCMP: {
      const char* a = mango_guest_cstr(lib, r0);
      const char* b = mango_guest_cstr(lib, r1);
      cpu->r[0] = (a && b) ? (uint32_t)strcmp(a, b) : (uint32_t)-1;
      break;
    }
    case MANGO_LIBC_STRNCMP: {
      const char* a = mango_guest_cstr(lib, r0);
      const char* b = mango_guest_cstr(lib, r1);
      cpu->r[0] = (a && b) ? (uint32_t)strncmp(a, b, r2) : (uint32_t)-1;
      break;
    }
    case MANGO_LIBC_STRCPY: {
      const char* src = mango_guest_cstr(lib, r1);
      size_t n = src ? strlen(src) + 1u : 0;
      if (!src || !mango_guest_range_ok(lib, r0, (uint32_t)n)) {
        cpu->r[0] = 0;
      } else {
        memcpy(lib->guest_mem + r0, src, n);
        cpu->r[0] = r0;
      }
      break;
    }
    case MANGO_LIBC_MEMMEM: {
      if (!mango_guest_range_ok(lib, r0, r1) || !mango_guest_range_ok(lib, r2, cpu->r[3])) {
        cpu->r[0] = 0;
        break;
      }
      /* naive search; memmem is GNU and not in C11 */
      cpu->r[0] = 0;
      if (cpu->r[3] == 0) {
        cpu->r[0] = r0;
      } else if (r1 >= cpu->r[3]) {
        for (uint32_t i = 0; i + cpu->r[3] <= r1; i++) {
          if (memcmp(lib->guest_mem + r0 + i, lib->guest_mem + r2, cpu->r[3]) == 0) {
            cpu->r[0] = r0 + i;
            break;
          }
        }
      }
      break;
    }
    case MANGO_LIBC_ERRNO:
      cpu->r[0] = lib->guest_errno_addr;
      break;
    case MANGO_LIBC_STRTOL: {
      const char* s = mango_guest_cstr(lib, r0);
      cpu->r[0] = s ? (uint32_t)strtol(s, NULL, (int)r2) : 0;
      break;
    }
    case MANGO_LIBC_DLOPEN: {
      const char* nm = mango_guest_cstr(lib, r0);
      char path[512];
      if (!nm) {
        cpu->r[0] = 0;
        break;
      }
      if (nm[0] == '/') {
        snprintf(path, sizeof(path), "%s", nm);
      } else if (lib->path[0] && strrchr(lib->path, '/')) {
        const char* slash = strrchr(lib->path, '/');
        snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - lib->path), lib->path, nm);
      } else {
        snprintf(path, sizeof(path), "%s", nm);
      }
      {
        uint32_t saved_sp = g_nested_sp;
        g_nested_sp = cpu->r[MANGO_REG_SP];
        void* h = mango_load_library(path, (int)r1);
        g_nested_sp = saved_sp;
        uint32_t id = 0;
        if (h) {
          for (int i = 0; i < g_nlibs; i++) {
            if (g_libs[i] == h) {
              id = (uint32_t)(i + 1);
              break;
            }
          }
        }
        if (id == 0) {
          snprintf(g_dlerror, sizeof(g_dlerror), "dlopen failed: %s", path);
          fprintf(stderr, "mango: dlopen FAIL %s\n", path);
        } else {
          fprintf(stderr, "mango: dlopen OK %s -> id=%u\n", path, id);
        }
        cpu->r[0] = id;
      }
      break;
    }
    case MANGO_LIBC_DLSYM: {
      const char* nm = mango_guest_cstr(lib, r1);
      cpu->r[0] = 0;
      if (nm) {
        if (r0 == 0) {
          cpu->r[0] = mango_resolve_import(lib, nm, 0, 0);
        } else if (r0 >= 1u && r0 <= (uint32_t)g_nlibs) {
          MangoLoadedLibrary* other = g_libs[r0 - 1u];
          uint32_t st = mango_elf32_find_symbol(&other->image, nm);
          if (st) {
            cpu->r[0] = st + other->load_bias;
          }
        }
      }
      break;
    }
    case MANGO_LIBC_DLCLOSE:
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_DLERROR: {
      uint32_t a = mango_guest_strdup(lib, g_dlerror);
      cpu->r[0] = a;
      break;
    }
    case MANGO_LIBC_ALOG:
    case MANGO_LIBC_PRINTF:
    case MANGO_LIBC_PTHREAD_LOCK:
    case MANGO_LIBC_PTHREAD_UNLOCK:
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_GETPID:
      cpu->r[0] = 1;
      break;
    case MANGO_LIBC_CALLOC: {
      uint64_t n = (uint64_t)r0 * (uint64_t)r1;
      if (n == 0) {
        n = 8;
      }
      n = (n + 7ull) & ~7ull;
      if (n > MANGO_HEAP_SIZE || lib->heap_used + (uint32_t)n > MANGO_HEAP_SIZE) {
        cpu->r[0] = 0;
      } else {
        cpu->r[0] = lib->heap_base + lib->heap_used;
        memset(lib->guest_mem + cpu->r[0], 0, (size_t)n);
        lib->heap_used += (uint32_t)n;
      }
      break;
    }
    case MANGO_LIBC_SIN:
      mango_ret_d(cpu, sin(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_COS:
      mango_ret_d(cpu, cos(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_TAN:
      mango_ret_d(cpu, tan(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_SQRT:
      mango_ret_d(cpu, sqrt(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_POW:
      mango_ret_d(cpu, pow(mango_arg_d(cpu), mango_arg_d2(cpu)));
      break;
    case MANGO_LIBC_EXP:
      mango_ret_d(cpu, exp(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_LOG:
      mango_ret_d(cpu, log(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_FLOOR:
      mango_ret_d(cpu, floor(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_FABS:
      mango_ret_d(cpu, fabs(mango_arg_d(cpu)));
      break;
    case MANGO_LIBC_ATAN2:
      mango_ret_d(cpu, atan2(mango_arg_d(cpu), mango_arg_d2(cpu)));
      break;
    case MANGO_LIBC_SINF:
      mango_ret_f(cpu, sinf(mango_arg_f(cpu)));
      break;
    case MANGO_LIBC_COSF:
      mango_ret_f(cpu, cosf(mango_arg_f(cpu)));
      break;
    case MANGO_LIBC_SQRTF:
      mango_ret_f(cpu, sqrtf(mango_arg_f(cpu)));
      break;
    case MANGO_LIBC_POWF: {
      float a, b;
      uint32_t ua = cpu->r[0], ub = cpu->r[1];
      memcpy(&a, &ua, 4);
      memcpy(&b, &ub, 4);
      mango_ret_f(cpu, powf(a, b));
      break;
    }
    case MANGO_LIBC_FLOORF:
      mango_ret_f(cpu, floorf(mango_arg_f(cpu)));
      break;
    case MANGO_LIBC_FABSF:
      mango_ret_f(cpu, fabsf(mango_arg_f(cpu)));
      break;
    case MANGO_LIBC_PTHREAD_SELF:
      cpu->r[0] = 1;
      break;
    case MANGO_LIBC_PTHREAD_CREATE:
      cpu->r[0] = 11; /* EAGAIN: do not start a guest thread */
      break;
    case MANGO_LIBC_OPEN: {
      const char* path = mango_guest_cstr(lib, r0);
      g_fake_data = NULL;
      g_fake_len = 0;
      g_fake_off = 0;
      if (path && strstr(path, "cpuinfo") != NULL) {
        g_fake_data = kCpuInfo;
        g_fake_len = (uint32_t)sizeof(kCpuInfo) - 1u;
      } else if (path && (strstr(path, "cpu/present") != NULL || strstr(path, "cpu/possible") != NULL)) {
        g_fake_data = kCpuList;
        g_fake_len = (uint32_t)sizeof(kCpuList) - 1u;
      } else if (path && strstr(path, "auxv") != NULL) {
        g_fake_data = (const char*)kAuxv;
        g_fake_len = (uint32_t)sizeof(kAuxv);
      }
      cpu->r[0] = g_fake_data ? 100u : (uint32_t)-1;
      break;
    }
    case MANGO_LIBC_READ:
      if (r0 == 100 && g_fake_data != NULL) {
        if (g_fake_off >= g_fake_len) {
          cpu->r[0] = 0;
        } else if (!mango_guest_range_ok(lib, r1, r2)) {
          cpu->r[0] = (uint32_t)-1;
        } else {
          uint32_t n = g_fake_len - g_fake_off;
          if (n > r2) {
            n = r2;
          }
          memcpy(lib->guest_mem + r1, g_fake_data + g_fake_off, n);
          g_fake_off += n;
          cpu->r[0] = n;
        }
      } else {
        cpu->r[0] = (uint32_t)-1;
      }
      break;
    case MANGO_LIBC_CLOSE:
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_PTHREAD_KEY_CREATE:
      if (!mango_guest_range_ok(lib, r0, 4u) || g_nkeys >= MANGO_TSD_KEYS) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      mango_store_u32_guest(lib->guest_mem, r0, g_nkeys);
      g_tsd[g_nkeys] = 0;
      g_nkeys++;
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_PTHREAD_GETSPECIFIC:
      cpu->r[0] = (r0 > 0 && r0 < g_nkeys) ? g_tsd[r0] : 0;
      break;
    case MANGO_LIBC_PTHREAD_SETSPECIFIC:
      if (r0 == 0 || r0 >= g_nkeys) {
        cpu->r[0] = (uint32_t)-1;
      } else {
        g_tsd[r0] = r1;
        cpu->r[0] = 0;
      }
      break;
    case MANGO_LIBC_PTHREAD_KEY_DELETE:
      if (r0 > 0 && r0 < g_nkeys) {
        g_tsd[r0] = 0;
      }
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_PTHREAD_ONCE: {
      if (!mango_guest_range_ok(lib, r0, 4u) || r1 == 0) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      uint32_t once = mango_load_u32_guest(lib->guest_mem, r0);
      if (once == 0) {
        mango_store_u32_guest(lib->guest_mem, r0, 1u);
        MangoCpu nested = *cpu;
        nested.r[MANGO_REG_LR] = MANGO_JNI_STOP;
        if (r1 & 1u) {
          nested.cpsr |= MANGO_CPSR_T;
          nested.r[MANGO_REG_PC] = r1 & ~1u;
        } else {
          nested.cpsr &= ~MANGO_CPSR_T;
          nested.r[MANGO_REG_PC] = r1;
        }
        mango_run_guest(lib, &nested, NULL);
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SNPRINTF: {
      const char* fmt = mango_guest_cstr(lib, r2);
      if (!fmt || r1 == 0 || !mango_guest_range_ok(lib, r0, r1)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      char tmp[512];
      if (strcmp(fmt, "%s/%s") == 0) {
        const char* a = mango_guest_cstr(lib, cpu->r[3]);
        uint32_t baddr = mango_load_u32_guest(lib->guest_mem, cpu->r[MANGO_REG_SP]);
        const char* b = mango_guest_cstr(lib, baddr);
        snprintf(tmp, sizeof(tmp), "%s/%s", a ? a : "", b ? b : "");
      } else if (strcmp(fmt, "%s") == 0) {
        const char* a = mango_guest_cstr(lib, cpu->r[3]);
        snprintf(tmp, sizeof(tmp), "%s", a ? a : "");
      } else {
        snprintf(tmp, sizeof(tmp), "%s", fmt);
      }
      tmp[sizeof(tmp) - 1u] = 0;
      size_t n = strlen(tmp);
      if (n >= r1) {
        n = r1 - 1u;
      }
      memcpy(lib->guest_mem + r0, tmp, n);
      lib->guest_mem[r0 + (uint32_t)n] = 0;
      cpu->r[0] = (uint32_t)n;
      break;
    }
    case MANGO_LIBC_EGL_TRUE:
      cpu->r[0] = 1;
      break;
    case MANGO_LIBC_EGL_SUCCESS:
      cpu->r[0] = 0x3000u; /* EGL_SUCCESS */
      break;
    case MANGO_LIBC_EGL_PROC:
      cpu->r[0] = lib->stub_addr;
      break;
    case MANGO_LIBC_EGL_INIT:
      if (r1 && mango_guest_range_ok(lib, r1, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r1, 1u);
      }
      if (r2 && mango_guest_range_ok(lib, r2, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r2, 4u);
      }
      cpu->r[0] = 1;
      break;
    case MANGO_LIBC_EGL_CHOOSE: {
      uint32_t nptr = 0;
      if (mango_guest_range_ok(lib, cpu->r[MANGO_REG_SP], 4u)) {
        nptr = mango_load_u32_guest(lib->guest_mem, cpu->r[MANGO_REG_SP]);
      }
      if (nptr && mango_guest_range_ok(lib, nptr, 4u)) {
        mango_store_u32_guest(lib->guest_mem, nptr, 1u);
      }
      if (r2 && cpu->r[3] && mango_guest_range_ok(lib, r2, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r2, 1u);
      }
      cpu->r[0] = 1;
      break;
    }
    case MANGO_LIBC_EGL_SURFACE:
      cpu->r[0] = mango_guest_alloc(lib, 64u);
      if (cpu->r[0] == 0) {
        cpu->r[0] = 2;
      }
      break;
    case MANGO_LIBC_EGL_CONTEXT:
      cpu->r[0] = mango_guest_alloc(lib, 64u);
      if (cpu->r[0] == 0) {
        cpu->r[0] = 3;
      }
      break;
    case MANGO_LIBC_EGL_QUERY: {
      const char* s = "OpenGL_ES";
      if (r1 == 0x3053u) {
        s = "mango";
      } else if (r1 == 0x3054u) {
        s = "1.4";
      } else if (r1 == 0x3055u) {
        s = "";
      }
      cpu->r[0] = mango_guest_strdup(lib, s);
      break;
    }
    case MANGO_LIBC_ANW_FROM: {
      uint32_t a = mango_guest_alloc(lib, 256u);
      if (a) {
        mango_store_u32_guest(lib->guest_mem, a, 1280u);
        mango_store_u32_guest(lib->guest_mem, a + 4u, 720u);
      }
      cpu->r[0] = a;
      break;
    }
    case MANGO_LIBC_ANW_W:
      cpu->r[0] = 1280;
      break;
    case MANGO_LIBC_ANW_H:
      cpu->r[0] = 720;
      break;
    case MANGO_LIBC_CLOCK_GETTIME:
      if (!mango_guest_range_ok(lib, r1, 8u)) {
        cpu->r[0] = (uint32_t)-1;
      } else {
        mango_store_u32_guest(lib->guest_mem, r1, 1u);       /* tv_sec */
        mango_store_u32_guest(lib->guest_mem, r1 + 4u, 0u); /* tv_nsec */
        cpu->r[0] = 0;
      }
      break;
    case MANGO_LIBC_AEABI_MEMSET:
      /* dest, n, c — not the C memset order */
      if (!mango_guest_range_ok(lib, r0, r1)) {
        cpu->r[0] = 0;
      } else {
        memset(lib->guest_mem + r0, (int)(r2 & 0xFFu), r1);
        cpu->r[0] = r0;
      }
      break;
    case MANGO_LIBC_ABORT:
    default:
      cpu->r[0] = (uint32_t)-1;
      break;
  }
}

static int mango_host_jni_ok(JNIEnv* env) {
  if (env == NULL || (uintptr_t)env < 4096u) {
    return 0; /* tests pass a dummy; never dereference it */
  }
#if defined(__cplusplus)
  return 1;
#else
  return *(const void* const*)env != NULL;
#endif
}

static void mango_jni_svc(MangoLoadedLibrary* lib, MangoCpu* cpu, JNIEnv* env, uint32_t fn) {
  switch (fn) {
    case MANGO_JNI_GET_VERSION:
      cpu->r[0] = mango_host_jni_ok(env) ? (uint32_t)(*env)->GetVersion(env) : 0x00010006u;
      break;
    case MANGO_JNI_FIND_CLASS: {
      const char* name = mango_guest_cstr(lib, cpu->r[1]);
      jobject c = NULL;
      if (name && mango_host_jni_ok(env)) {
        c = (*env)->FindClass(env, name);
      } else if (name) {
        c = (jobject)(uintptr_t)(cpu->r[1] | 1u);
      }
      cpu->r[0] = mango_handle_intern(c);
      break;
    }
    case MANGO_JNI_THROW:
    case MANGO_JNI_THROW_NEW:
    case MANGO_JNI_FATAL_ERROR:
      cpu->r[0] = 0;
      break;
    case MANGO_JNI_EXCEPTION_OCCURRED:
      cpu->r[0] = mango_host_jni_ok(env) ? mango_handle_intern((*env)->ExceptionOccurred(env)) : 0;
      break;
    case MANGO_JNI_EXCEPTION_CLEAR:
      if (mango_host_jni_ok(env)) {
        (*env)->ExceptionClear(env);
      }
      break;
    case MANGO_JNI_NEW_GLOBAL_REF:
      cpu->r[0] =
          mango_host_jni_ok(env)
              ? mango_handle_intern((*env)->NewGlobalRef(env, mango_handle_lookup(cpu->r[1])))
              : cpu->r[1];
      break;
    case MANGO_JNI_DELETE_LOCAL_REF:
      if (mango_host_jni_ok(env)) {
        (*env)->DeleteLocalRef(env, mango_handle_lookup(cpu->r[1]));
      }
      break;
    case MANGO_JNI_IS_SAME_OBJECT:
      if (mango_host_jni_ok(env)) {
        cpu->r[0] = (*env)->IsSameObject(env, mango_handle_lookup(cpu->r[1]),
                                         mango_handle_lookup(cpu->r[2]));
      } else {
        cpu->r[0] = cpu->r[1] == cpu->r[2];
      }
      break;
    case MANGO_JNI_GET_OBJECT_CLASS:
      cpu->r[0] =
          mango_host_jni_ok(env)
              ? mango_handle_intern((*env)->GetObjectClass(env, mango_handle_lookup(cpu->r[1])))
              : (cpu->r[1] ? cpu->r[1] : mango_dummy_jobject(lib));
      break;
    case MANGO_JNI_GET_METHOD_ID:
    case MANGO_JNI_GET_STATIC_METHOD_ID: {
      const char* name = mango_guest_cstr(lib, cpu->r[2]);
      jmethodID mid = NULL;
#if defined(__ANDROID__) && !defined(MANGO_FAKE_JNI)
      const char* sig = mango_guest_cstr(lib, cpu->r[3]);
      if (name && sig && mango_host_jni_ok(env)) {
        jclass clazz = mango_handle_lookup(cpu->r[1]);
        mid = fn == MANGO_JNI_GET_STATIC_METHOD_ID
                  ? (*env)->GetStaticMethodID(env, clazz, name, sig)
                  : (*env)->GetMethodID(env, clazz, name, sig);
      }
#else
      (void)fn;
      if (name) {
        mid = (jmethodID)(uintptr_t)mango_handle_intern((void*)(uintptr_t)(cpu->r[2] | 1u));
      }
#endif
      cpu->r[0] = mango_handle_intern(mid);
      break;
    }
    case MANGO_JNI_NEW_OBJECT:
    case MANGO_JNI_NEW_OBJECT + 1:
    case MANGO_JNI_NEW_OBJECT + 2:
    case MANGO_JNI_CALL_OBJECT_METHOD:
    case MANGO_JNI_CALL_OBJECT_METHOD + 1:
    case MANGO_JNI_CALL_OBJECT_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_OBJECT_METHOD:
    case MANGO_JNI_CALL_STATIC_OBJECT_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_OBJECT_METHOD + 2:
    case MANGO_JNI_GET_OBJECT_FIELD:
    case MANGO_JNI_GET_STATIC_OBJECT_FIELD:
      cpu->r[0] = mango_dummy_jobject(lib);
      break;
    case MANGO_JNI_CALL_BOOLEAN_METHOD:
    case MANGO_JNI_CALL_BOOLEAN_METHOD + 1:
    case MANGO_JNI_CALL_BOOLEAN_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD + 2:
    case MANGO_JNI_GET_BOOLEAN_FIELD:
      cpu->r[0] = 1;
      break;
    case MANGO_JNI_CALL_INT_METHOD:
    case MANGO_JNI_CALL_INT_METHOD + 1:
    case MANGO_JNI_CALL_INT_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_INT_METHOD:
    case MANGO_JNI_CALL_STATIC_INT_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_INT_METHOD + 2:
    case MANGO_JNI_GET_INT_FIELD:
    case MANGO_JNI_GET_STATIC_INT_FIELD:
      cpu->r[0] = 1;
      break;
    case MANGO_JNI_CALL_VOID_METHOD:
    case MANGO_JNI_CALL_VOID_METHOD + 1:
    case MANGO_JNI_CALL_VOID_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD + 2:
    case MANGO_JNI_SET_OBJECT_FIELD:
    case MANGO_JNI_SET_INT_FIELD:
      cpu->r[0] = 0;
      break;
    case MANGO_JNI_GET_FIELD_ID:
    case MANGO_JNI_GET_STATIC_FIELD_ID: {
      const char* name = mango_guest_cstr(lib, cpu->r[2]);
      cpu->r[0] = name ? mango_handle_intern((void*)(uintptr_t)(cpu->r[2] | 1u)) : 0;
      break;
    }
    case MANGO_JNI_NEW_STRING_UTF: {
      const char* s = mango_guest_cstr(lib, cpu->r[1]);
      jobject js = NULL;
      if (s && mango_host_jni_ok(env)) {
        js = (*env)->NewStringUTF(env, s);
      } else if (s) {
        js = (jobject)(uintptr_t)mango_guest_strdup(lib, s);
      }
      cpu->r[0] = mango_handle_intern(js);
      break;
    }
    case MANGO_JNI_GET_STRING_UTF_LENGTH: {
      jstring js = mango_handle_lookup(cpu->r[1]);
      const char* s = NULL;
      if (js && mango_host_jni_ok(env)) {
        s = (*env)->GetStringUTFChars(env, js, NULL);
        cpu->r[0] = s ? (uint32_t)strlen(s) : 0;
        if (s) {
          (*env)->ReleaseStringUTFChars(env, js, s);
        }
      } else if (js) {
        cpu->r[0] = (uint32_t)strlen(mango_fake_jstring_chars(lib, js));
      } else {
        cpu->r[0] = 0;
      }
      break;
    }
    case MANGO_JNI_GET_STRING_UTF_CHARS: {
      jstring js = mango_handle_lookup(cpu->r[1]);
      const char* s = NULL;
      if (js && mango_host_jni_ok(env)) {
        s = (*env)->GetStringUTFChars(env, js, NULL);
      } else if (js) {
        s = mango_fake_jstring_chars(lib, js);
      }
      cpu->r[0] = mango_guest_strdup(lib, s ? s : "");
      if (s && mango_host_jni_ok(env)) {
        (*env)->ReleaseStringUTFChars(env, js, s);
      }
      break;
    }
    case MANGO_JNI_RELEASE_STRING_UTF_CHARS:
      break;
    case MANGO_JNI_REGISTER_NATIVES: {
      uint32_t n = cpu->r[3];
      uint32_t methods = cpu->r[2];
      if (n > 64u || !mango_guest_range_ok(lib, methods, n * 12u)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      /* Each guest JNINativeMethod is 3x uint32 (name, signature, fnPtr).
       * Bind each fnPtr as a trampoline slot so ART can call it later. */
      for (uint32_t i = 0; i < n; i++) {
        uint32_t name_a = mango_load_u32_guest(lib->guest_mem, methods + i * 12u);
        uint32_t sig_a = mango_load_u32_guest(lib->guest_mem, methods + i * 12u + 4u);
        uint32_t fn_a = mango_load_u32_guest(lib->guest_mem, methods + i * 12u + 8u);
        const char* nm = mango_guest_cstr(lib, name_a);
        const char* sg = mango_guest_cstr(lib, sig_a);
        if (fn_a != 0) {
          int si = mango_alloc_slot();
          if (si >= 0) {
            MangoLoadedLibrary* owner = lib;
            for (int li = g_nlibs - 1; li >= 0; li--) {
              if (fn_a >= g_libs[li]->load_bias) {
                owner = g_libs[li];
                break;
              }
            }
            g_slots[si].lib = owner;
            g_slots[si].guest_pc = fn_a;
            memset(g_slots[si].shorty, 0, sizeof(g_slots[si].shorty));
            memset(g_slots[si].name, 0, sizeof(g_slots[si].name));
            if (nm) {
              strncpy(g_slots[si].name, nm, sizeof(g_slots[si].name) - 1u);
            }
            if (sg) {
              strncpy(g_slots[si].shorty, sg, sizeof(g_slots[si].shorty) - 1u);
            }
            fprintf(stderr, "mango: RegisterNatives %s %s pc=0x%x\n", nm ? nm : "?",
                    sg ? sg : "?", fn_a);
          }
        }
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_JNI_GET_JAVA_VM:
      if (mango_guest_range_ok(lib, cpu->r[1], 4u)) {
        mango_store_u32_guest(lib->guest_mem, cpu->r[1], lib->jni_vm_addr);
      }
      cpu->r[0] = 0;
      break;
    case MANGO_JNI_EXCEPTION_CHECK:
      cpu->r[0] = mango_host_jni_ok(env) ? (uint32_t)(*env)->ExceptionCheck(env) : 0;
      break;
    default:
      cpu->r[0] = 0;
      break;
  }
}

static char mango_consume_jni_type(const char** pp) {
  const char* p = *pp;
  while (*p == '[') {
    p++;
  }
  if (*p == 'L') {
    while (*p && *p != ';') {
      p++;
    }
    if (*p == ';') {
      p++;
    }
    *pp = p;
    return 'L';
  }
  if (*p == '\0') {
    return 0;
  }
  char t = *p++;
  *pp = p;
  return t;
}

static int mango_parse_shorty(const char* shorty, char* ret, char* args, uint32_t args_max) {
  uint32_t n = 0;
  if (shorty == NULL || shorty[0] == '\0') {
    return -1;
  }
  if (shorty[0] == '(') {
    const char* p = shorty + 1;
    while (*p && *p != ')') {
      if (n + 1 >= args_max) {
        return -1;
      }
      char t = mango_consume_jni_type(&p);
      if (t == 0) {
        return -1;
      }
      args[n++] = t;
    }
    if (*p != ')' || p[1] == '\0') {
      return -1;
    }
    const char* r = p + 1;
    *ret = mango_consume_jni_type(&r);
  } else {
    const char* p = shorty;
    *ret = mango_consume_jni_type(&p);
    while (*p) {
      if (n + 1 >= args_max) {
        return -1;
      }
      char t = mango_consume_jni_type(&p);
      if (t == 0) {
        return -1;
      }
      args[n++] = t;
    }
  }
  args[n] = 0;
  return (int)n;
}

static int mango_is_jni_onload(const MangoJniSlot* slot) {
  if (strcmp(slot->name, "JNI_OnLoad") == 0 || strcmp(slot->name, "JNI_OnUnload") == 0) {
    return 1;
  }
  /* ART's JNI_OnLoad shorty; tests also use this with mango_add. */
  return slot->shorty[0] == 'I' && slot->shorty[1] == 'L' && slot->shorty[2] == '\0';
}

static int mango_run_guest(MangoLoadedLibrary* lib, MangoCpu* cpu, JNIEnv* env) {
  MangoMemory mem = {lib->guest_mem, lib->guest_mem_size};
  for (;;) {
    int rc = mango_interp_run(cpu, &mem, MANGO_JNI_STOP, 10000000u);
    if (rc == 0) {
      return 0;
    }
    if (rc != 1) {
      uint32_t pc = cpu->r[MANGO_REG_PC];
      uint32_t w = 0;
      if (pc + 4u <= mem.size) {
        w = mango_load_u32_guest(mem.bytes, pc);
      }
      MangoInsn ins;
      int dec = (cpu->cpsr & MANGO_CPSR_T) ? -2 : mango_decode(w, &ins);
      fprintf(stderr,
              "mango: interp stop pc=0x%x cpsr=0x%x word=0x%08x rc=%d decode=%d op=%d "
              "r0=%x r1=%x r2=%x r3=%x r4=%x r5=%x r6=%x r7=%x sp=%x lr=%x libbias=0x%x\n",
              pc, cpu->cpsr, w, rc, dec, dec == 0 ? (int)ins.op : -1, cpu->r[0], cpu->r[1],
              cpu->r[2], cpu->r[3], cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7],
              cpu->r[MANGO_REG_SP], cpu->r[MANGO_REG_LR], lib->load_bias);
      for (int i = 0; i < g_nlibs; i++) {
        fprintf(stderr, "mango: lib[%d] bias=0x%x %s\n", i, g_libs[i]->load_bias, g_libs[i]->path);
      }
      return -1;
    }
    uint32_t nr = cpu->r[7];
    {
      uint32_t pc = cpu->r[MANGO_REG_PC];
      if ((cpu->cpsr & MANGO_CPSR_T) == 0 && pc + 12u <= mem.size &&
          mango_load_u32_guest(mem.bytes, pc + 4u) == 0xE12FFF1Eu) {
        uint32_t imm = mango_load_u32_guest(mem.bytes, pc + 8u);
        if ((imm >= MANGO_JNI_SVC_BASE && imm < MANGO_JNI_SVC_BASE + MANGO_JNI_TABLE_LEN) ||
            (imm >= MANGO_JVM_SVC_BASE && imm < MANGO_JVM_SVC_BASE + MANGO_JVM_TABLE_LEN) ||
            (imm >= MANGO_LIBC_SVC_BASE && imm < MANGO_LIBC_SVC_BASE + MANGO_LIBC_COUNT)) {
          nr = imm;
        }
      }
    }
    if (nr >= MANGO_JNI_SVC_BASE && nr < MANGO_JNI_SVC_BASE + MANGO_JNI_TABLE_LEN) {
      mango_jni_svc(lib, cpu, env, nr - MANGO_JNI_SVC_BASE);
    } else if (nr >= MANGO_JVM_SVC_BASE && nr < MANGO_JVM_SVC_BASE + MANGO_JVM_TABLE_LEN) {
      mango_jvm_svc(lib, cpu, nr - MANGO_JVM_SVC_BASE);
    } else if (nr >= MANGO_LIBC_SVC_BASE && nr < MANGO_LIBC_SVC_BASE + MANGO_LIBC_COUNT) {
      mango_libc_svc(lib, cpu, nr - MANGO_LIBC_SVC_BASE);
    } else {
      cpu->r[0] = (uint32_t)-1;
    }
    cpu->r[MANGO_REG_PC] += (cpu->cpsr & MANGO_CPSR_T) ? 2u : 4u;
  }
}

static intptr_t mango_jni_invoke(MangoJniSlot* slot, JNIEnv* env, va_list ap) {
  MangoLoadedLibrary* lib = slot ? slot->lib : NULL;
  if (!lib) {
    return 0;
  }
  char ret = 'V';
  char args[24];
  int nargs = mango_parse_shorty(slot->shorty, &ret, args, sizeof(args));
  if (nargs < 0) {
    return 0;
  }

  uint32_t regs[4];
  uint32_t stack[16];
  uint32_t nstack = 0;
  uint32_t nreg = 2; /* r0 env/vm, r1 thiz/reserved */
  int onload = mango_is_jni_onload(slot);
  if (onload) {
    lib->host_vm = env; /* ART calls JNI_OnLoad(JavaVM*, void*) */
    regs[0] = lib->jni_vm_addr;
    (void)va_arg(ap, void*); /* reserved, typically NULL */
    regs[1] = 0;
    nargs = 0; /* JavaVM* / reserved are not JNIEnv method args */
  } else {
    jobject thiz = va_arg(ap, jobject);
    regs[0] = lib->jni_env_addr;
    regs[1] = mango_handle_intern(thiz);
  }

  for (int i = 0; i < nargs; i++) {
    char t = args[i];
    uint32_t lo, hi = 0;
    int wide = 0;
    if (t == 'J' || t == 'D') {
      jlong v = va_arg(ap, jlong);
      lo = (uint32_t)v;
      hi = (uint32_t)(v >> 32);
      wide = 1;
    } else if (t == 'F') {
      double d = va_arg(ap, double); /* float promotes */
      union {
        float f;
        uint32_t u;
      } u;
      u.f = (float)d;
      lo = u.u;
    } else if (t == 'L' || t == '[') {
      lo = mango_handle_intern(va_arg(ap, jobject));
    } else {
      lo = (uint32_t)va_arg(ap, int);
    }
    if (wide && (nreg & 1u)) {
      nreg++; /* AAPCS 8-byte align */
    }
    if (nreg < 4u) {
      regs[nreg++] = lo;
      if (wide) {
        if (nreg < 4u) {
          regs[nreg++] = hi;
        } else if (nstack + 1u < 16u) {
          stack[nstack++] = hi;
        }
      }
    } else if (nstack < 16u) {
      stack[nstack++] = lo;
      if (wide && nstack < 16u) {
        stack[nstack++] = hi;
      }
    }
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[0] = regs[0];
  cpu.r[1] = regs[1];
  cpu.r[2] = nreg > 2 ? regs[2] : 0;
  cpu.r[3] = nreg > 3 ? regs[3] : 0;
  cpu.r[MANGO_REG_SP] = lib->stack_top - nstack * 4u;
  cpu.r[MANGO_REG_LR] = MANGO_JNI_STOP;
  uint32_t pc = slot->guest_pc;
  if (pc & 1u) {
    cpu.cpsr = MANGO_CPSR_T;
    pc &= ~1u;
  }
  cpu.r[MANGO_REG_PC] = pc;
  for (uint32_t i = 0; i < nstack; i++) {
    mango_store_u32_guest(lib->guest_mem, cpu.r[MANGO_REG_SP] + i * 4u, stack[i]);
  }

  if (mango_run_guest(lib, &cpu, env) != 0) {
    return 0;
  }

  if (ret == 'J' || ret == 'D') {
    return (intptr_t)(((uint64_t)cpu.r[1] << 32) | cpu.r[0]);
  }
  if (ret == 'L' || ret == '[') {
    return (intptr_t)mango_handle_lookup(cpu.r[0]);
  }
  return (intptr_t)(int32_t)cpu.r[0];
}

#define MANGO_SLOT_FN(n)                                 \
  static intptr_t mango_jni_slot_##n(JNIEnv* env, ...) { \
    va_list ap;                                          \
    va_start(ap, env);                                   \
    intptr_t r = mango_jni_invoke(&g_slots[n], env, ap); \
    va_end(ap);                                          \
    return r;                                            \
  }

MANGO_SLOT_FN(0)
MANGO_SLOT_FN(1)
MANGO_SLOT_FN(2)
MANGO_SLOT_FN(3)
MANGO_SLOT_FN(4)
MANGO_SLOT_FN(5)
MANGO_SLOT_FN(6)
MANGO_SLOT_FN(7)
MANGO_SLOT_FN(8)
MANGO_SLOT_FN(9)
MANGO_SLOT_FN(10)
MANGO_SLOT_FN(11)
MANGO_SLOT_FN(12)
MANGO_SLOT_FN(13)
MANGO_SLOT_FN(14)
MANGO_SLOT_FN(15)
MANGO_SLOT_FN(16)
MANGO_SLOT_FN(17)
MANGO_SLOT_FN(18)
MANGO_SLOT_FN(19)
MANGO_SLOT_FN(20)
MANGO_SLOT_FN(21)
MANGO_SLOT_FN(22)
MANGO_SLOT_FN(23)
MANGO_SLOT_FN(24)
MANGO_SLOT_FN(25)
MANGO_SLOT_FN(26)
MANGO_SLOT_FN(27)
MANGO_SLOT_FN(28)
MANGO_SLOT_FN(29)
MANGO_SLOT_FN(30)
MANGO_SLOT_FN(31)
MANGO_SLOT_FN(32)
MANGO_SLOT_FN(33)
MANGO_SLOT_FN(34)
MANGO_SLOT_FN(35)
MANGO_SLOT_FN(36)
MANGO_SLOT_FN(37)
MANGO_SLOT_FN(38)
MANGO_SLOT_FN(39)
MANGO_SLOT_FN(40)
MANGO_SLOT_FN(41)
MANGO_SLOT_FN(42)
MANGO_SLOT_FN(43)
MANGO_SLOT_FN(44)
MANGO_SLOT_FN(45)
MANGO_SLOT_FN(46)
MANGO_SLOT_FN(47)
MANGO_SLOT_FN(48)
MANGO_SLOT_FN(49)
MANGO_SLOT_FN(50)
MANGO_SLOT_FN(51)
MANGO_SLOT_FN(52)
MANGO_SLOT_FN(53)
MANGO_SLOT_FN(54)
MANGO_SLOT_FN(55)
MANGO_SLOT_FN(56)
MANGO_SLOT_FN(57)
MANGO_SLOT_FN(58)
MANGO_SLOT_FN(59)
MANGO_SLOT_FN(60)
MANGO_SLOT_FN(61)
MANGO_SLOT_FN(62)
MANGO_SLOT_FN(63)

static void* const g_slot_fns[MANGO_JNI_SLOTS] = {
    (void*)mango_jni_slot_0,  (void*)mango_jni_slot_1,  (void*)mango_jni_slot_2,
    (void*)mango_jni_slot_3,  (void*)mango_jni_slot_4,  (void*)mango_jni_slot_5,
    (void*)mango_jni_slot_6,  (void*)mango_jni_slot_7,  (void*)mango_jni_slot_8,
    (void*)mango_jni_slot_9,  (void*)mango_jni_slot_10, (void*)mango_jni_slot_11,
    (void*)mango_jni_slot_12, (void*)mango_jni_slot_13, (void*)mango_jni_slot_14,
    (void*)mango_jni_slot_15, (void*)mango_jni_slot_16, (void*)mango_jni_slot_17,
    (void*)mango_jni_slot_18, (void*)mango_jni_slot_19, (void*)mango_jni_slot_20,
    (void*)mango_jni_slot_21, (void*)mango_jni_slot_22, (void*)mango_jni_slot_23,
    (void*)mango_jni_slot_24, (void*)mango_jni_slot_25, (void*)mango_jni_slot_26,
    (void*)mango_jni_slot_27, (void*)mango_jni_slot_28, (void*)mango_jni_slot_29,
    (void*)mango_jni_slot_30, (void*)mango_jni_slot_31, (void*)mango_jni_slot_32,
    (void*)mango_jni_slot_33, (void*)mango_jni_slot_34, (void*)mango_jni_slot_35,
    (void*)mango_jni_slot_36, (void*)mango_jni_slot_37, (void*)mango_jni_slot_38,
    (void*)mango_jni_slot_39, (void*)mango_jni_slot_40, (void*)mango_jni_slot_41,
    (void*)mango_jni_slot_42, (void*)mango_jni_slot_43, (void*)mango_jni_slot_44,
    (void*)mango_jni_slot_45, (void*)mango_jni_slot_46, (void*)mango_jni_slot_47,
    (void*)mango_jni_slot_48, (void*)mango_jni_slot_49, (void*)mango_jni_slot_50,
    (void*)mango_jni_slot_51, (void*)mango_jni_slot_52, (void*)mango_jni_slot_53,
    (void*)mango_jni_slot_54, (void*)mango_jni_slot_55, (void*)mango_jni_slot_56,
    (void*)mango_jni_slot_57, (void*)mango_jni_slot_58, (void*)mango_jni_slot_59,
    (void*)mango_jni_slot_60, (void*)mango_jni_slot_61, (void*)mango_jni_slot_62,
    (void*)mango_jni_slot_63,
};

static bool mango_initialize(const struct NativeBridgeRuntimeCallbacks* runtime_cbs,
                             const char* private_dir, const char* instruction_set) {
  (void)runtime_cbs;
  (void)private_dir;
  (void)instruction_set;
  return true; /* TODO: stash runtime_cbs for getTrampoline's JNI lookups */
}

static int mango_run_ctor(MangoLoadedLibrary* lib, uint32_t pc) {
  MangoCpu cpu;
  uint32_t sp;
  if (pc == 0 || pc == 0xFFFFFFFFu) {
    return 0;
  }
  memset(&cpu, 0, sizeof(cpu));
  sp = g_nested_sp ? g_nested_sp - 0x4000u : lib->stack_top;
  cpu.r[MANGO_REG_SP] = sp;
  cpu.r[MANGO_REG_LR] = MANGO_JNI_STOP;
  if (pc & 1u) {
    cpu.cpsr = MANGO_CPSR_T;
    pc &= ~1u;
  }
  cpu.r[MANGO_REG_PC] = pc;
  return mango_run_guest(lib, &cpu, NULL);
}

static void mango_run_constructors(MangoLoadedLibrary* lib) {
  uint32_t n = 0, ok = 0;
  if (lib->image.init_fn) {
    n++;
    if (mango_run_ctor(lib, lib->image.init_fn + lib->load_bias) == 0) {
      ok++;
    }
  }
  if (lib->image.init_array_vaddr && lib->image.init_array_size) {
    uint32_t addr = lib->image.init_array_vaddr + lib->load_bias;
    uint32_t count = lib->image.init_array_size / 4u;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t fn;
      n++;
      if (!mango_guest_range_ok(lib, addr + i * 4u, 4u)) {
        continue;
      }
      fn = mango_load_u32_guest(lib->guest_mem, addr + i * 4u);
      if (mango_run_ctor(lib, fn) == 0) {
        ok++;
      }
    }
  }
  if (n > 0) {
    fprintf(stderr, "mango: constructors %s %u/%u\n", lib->path, ok, n);
  }
}

static void* mango_load_library(const char* libpath, int flag) {
  (void)flag;
  if (!libpath || !mango_is_arm32_elf(libpath)) {
    return NULL;
  }
  for (int i = 0; i < g_nlibs; i++) {
    if (strcmp(g_libs[i]->path, libpath) == 0) {
      return g_libs[i];
    }
  }
  if (g_nlibs >= MANGO_MAX_LIBS) {
    return NULL;
  }

  int fd = open(libpath, O_RDONLY);
  if (fd < 0) {
    return NULL;
  }
  off_t end = lseek(fd, 0, SEEK_END);
  if (end <= 0 || (uint64_t)end > UINT32_MAX || lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return NULL;
  }
  uint32_t file_size = (uint32_t)end;

  uint8_t* file_data = malloc(file_size);
  if (!file_data) {
    close(fd);
    return NULL;
  }
  ssize_t n = read(fd, file_data, file_size);
  close(fd);
  if (n != (ssize_t)file_size) {
    free(file_data);
    return NULL;
  }

  MangoElf32Image image;
  if (mango_elf32_parse(file_data, file_size, EM_ARM, &image) != 0) {
    free(file_data); /* is_arm32_elf only checked the header; the rest of this one is malformed */
    return NULL;
  }

  uint64_t span = 0;
  for (uint32_t i = 0; i < image.segment_count; i++) {
    uint64_t seg_end = (uint64_t)image.segments[i].vaddr + image.segments[i].memsz;
    if (seg_end > span) {
      span = seg_end;
    }
  }
  if (span == 0 || span > MANGO_LIB_CAP) {
    free(file_data);
    return NULL;
  }

  if (g_as_mem == NULL) {
    g_as_mem = calloc(1, MANGO_AS_SIZE);
    if (!g_as_mem) {
      free(file_data);
      return NULL;
    }
    g_as_size = MANGO_AS_SIZE;
    g_as_next = 0;
  }

  uint32_t bias = (g_as_next + 0xFFFu) & ~0xFFFu;
  if ((uint64_t)bias + span > MANGO_LIB_CAP) {
    free(file_data);
    return NULL;
  }

  for (uint32_t i = 0; i < image.segment_count; i++) {
    memcpy(g_as_mem + bias + image.segments[i].vaddr, file_data + image.segments[i].file_offset,
           image.segments[i].filesz);
  }

  MangoLoadedLibrary* lib = calloc(1, sizeof(MangoLoadedLibrary));
  if (!lib) {
    free(file_data);
    return NULL;
  }
  lib->file_data = file_data;
  lib->image = image;
  lib->load_bias = bias;
  strncpy(lib->path, libpath, sizeof(lib->path) - 1u);
  if (mango_setup_guest_jni(lib) != 0) {
    free(lib->file_data);
    free(lib);
    return NULL;
  }
  g_libs[g_nlibs++] = lib;
  g_as_next = bias + (uint32_t)span;
  if (mango_elf32_apply_relocs(&lib->image, lib->guest_mem, lib->guest_mem_size, bias,
                               mango_resolve_import, lib) != 0) {
    g_nlibs--;
    free(lib->file_data);
    free(lib);
    return NULL;
  }
  mango_run_constructors(lib);
  return lib;
}

static void* mango_get_trampoline(void* handle, const char* name, const char* shorty,
                                  uint32_t len) {
  (void)len;
  MangoLoadedLibrary* lib = handle;
  if (!lib || name == NULL) {
    return NULL;
  }
  for (int i = 0; i < g_nslots; i++) {
    if (g_slots[i].lib == lib && strcmp(g_slots[i].name, name) == 0) {
      return g_slot_fns[i];
    }
  }
  for (int i = 0; i < g_nslots; i++) {
    if (g_slots[i].lib != NULL && g_slots[i].lib != lib && strcmp(g_slots[i].name, name) == 0) {
      return g_slot_fns[i];
    }
  }
  uint32_t addr = mango_elf32_find_symbol(&lib->image, name);
  if (addr == 0) {
    return NULL;
  }
  addr += lib->load_bias;
  int i = mango_alloc_slot();
  if (i < 0) {
    return NULL;
  }
  g_slots[i].lib = lib;
  g_slots[i].guest_pc = addr;
  memset(g_slots[i].shorty, 0, sizeof(g_slots[i].shorty));
  memset(g_slots[i].name, 0, sizeof(g_slots[i].name));
  strncpy(g_slots[i].name, name, sizeof(g_slots[i].name) - 1u);
  if (shorty) {
    strncpy(g_slots[i].shorty, shorty, sizeof(g_slots[i].shorty) - 1u);
  } else {
    g_slots[i].shorty[0] = 'V';
  }
  return g_slot_fns[i];
}

static bool mango_is_supported(const char* libpath) { return mango_is_arm32_elf(libpath); }

static const struct NativeBridgeRuntimeValues* mango_get_app_env(const char* instruction_set) {
  (void)instruction_set;
  return NULL;
}

/* Governs far more than its name suggests: libnativebridge's own
 * LoadNativeBridge() calls THIS (via its wrapper, which defers to us for
 * any bridge_version >= 2) with NAMESPACE_VERSION(3) *unconditionally*,
 * at load time, before anything else runs, and rejects the whole bridge
 * outright if it gets false back (confirmed against AOSP's
 * libnativebridge/native_bridge.cc). Returning false here isn't "we
 * don't support namespaces", it's "don't load this bridge at all",
 * which used to be true here by accident, not by design. Namespace
 * features are separately, individually gated by their own
 * isCompatibleWith(NAMESPACE_VERSION) check at the point they're
 * actually used, so claiming version 3 here obligates every v3 function
 * below to be real (or safely inert), never NULL. Not claiming
 * VENDOR_NAMESPACE_VERSION(4): NativeBridgeGetVendorNamespace() checks
 * compatibility itself before ever touching our function pointer, so
 * declining it is safe, unlike NAMESPACE_VERSION. */
static bool mango_is_compatible_with(uint32_t bridge_version) { return bridge_version <= 3; }

static NativeBridgeSignalHandlerFn mango_get_signal_handler(int signal) {
  (void)signal;
  return NULL;
}

/* Real now: frees what loadLibrary allocated. Only reachable because
 * mango_is_compatible_with(3) is honest above; it used to be dead code. */
static int mango_unload_library(void* handle) {
  MangoLoadedLibrary* lib = handle;
  if (!lib) {
    return -1;
  }
  for (int i = 0; i < g_nlibs; i++) {
    if (g_libs[i] == lib) {
      for (int s = 0; s < g_nslots; s++) {
        if (g_slots[s].lib == lib) {
          memset(&g_slots[s], 0, sizeof(g_slots[s]));
        }
      }
      free(lib->file_data);
      free(lib);
      for (int j = i; j + 1 < g_nlibs; j++) {
        g_libs[j] = g_libs[j + 1];
      }
      g_nlibs--;
      if (g_nlibs == 0) {
        g_as_next = 0;
        g_as_jni_ready = 0;
        g_nslots = 0;
        g_nkeys = 1;
        memset(g_tsd, 0, sizeof(g_tsd));
      }
      return 0;
    }
  }
  return -1;
}

/* No per-call error state tracked yet (loadLibrary/getTrampoline just
 * return NULL on failure); a fixed, generic string is honest about that
 * rather than inventing detail we don't actually have. */
static const char* mango_get_error(void) { return "mango: no detailed error reporting yet"; }

static bool mango_is_path_supported(const char* library_path) {
  return mango_is_arm32_elf(library_path);
}

/* We don't do real linker-level namespace isolation (loadLibrary/
 * loadLibraryExt both just parse and load the guest .so directly, see
 * above), so every namespace-related call below is a safe no-op that
 * says "sure" rather than actually creating separate load domains. That
 * matches what we actually do today: one guest library loaded is exactly
 * like another, regardless of which "namespace" ART thinks it's in. */
static bool mango_init_anonymous_namespace(const char* public_ns_sonames,
                                           const char* anon_ns_library_path) {
  (void)public_ns_sonames;
  (void)anon_ns_library_path;
  return true;
}

/* Not a real namespace object, just a distinct non-null token so callers
 * that check for NULL-on-failure don't treat this as having failed. */
static struct native_bridge_namespace_t* mango_create_namespace(
    const char* name, const char* ld_library_path, const char* default_library_path, uint64_t type,
    const char* permitted_when_isolated_path, struct native_bridge_namespace_t* parent_ns) {
  (void)name;
  (void)ld_library_path;
  (void)default_library_path;
  (void)type;
  (void)permitted_when_isolated_path;
  (void)parent_ns;
  static int dummy_namespace_token;
  return (struct native_bridge_namespace_t*)&dummy_namespace_token;
}

static bool mango_link_namespaces(struct native_bridge_namespace_t* from,
                                  struct native_bridge_namespace_t* to,
                                  const char* shared_libs_sonames) {
  (void)from;
  (void)to;
  (void)shared_libs_sonames;
  return true;
}

static void* mango_load_library_ext(const char* libpath, int flag,
                                    struct native_bridge_namespace_t* ns) {
  (void)ns; /* no real namespace-scoped path resolution, see the note above */
  return mango_load_library(libpath, flag);
}

__attribute__((visibility("default"))) struct NativeBridgeCallbacks NativeBridgeItf = {
    .version = 3,
    .initialize = mango_initialize,
    .loadLibrary = mango_load_library,
    .getTrampoline = mango_get_trampoline,
    .isSupported = mango_is_supported,
    .getAppEnv = mango_get_app_env,
    .isCompatibleWith = mango_is_compatible_with,
    .getSignalHandler = mango_get_signal_handler,
    .unloadLibrary = mango_unload_library,
    .getError = mango_get_error,
    .isPathSupported = mango_is_path_supported,
    .initAnonymousNamespace = mango_init_anonymous_namespace,
    .createNamespace = mango_create_namespace,
    .linkNamespaces = mango_link_namespaces,
    .loadLibraryExt = mango_load_library_ext,
    /* getVendorNamespace deliberately left NULL: we don't claim
     * VENDOR_NAMESPACE_VERSION(4), and that's checked before this would
     * ever be dereferenced, see mango_is_compatible_with's comment. */
};
