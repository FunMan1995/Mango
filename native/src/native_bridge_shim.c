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
#include <time.h>
#ifndef ANDROID
#include <png.h>
#endif

#include "mango/cpu.h"
#include "mango/decoder.h"
#include "mango/elf32.h"
#include "mango/interp.h"
#include "mango/native_bridge.h"

#define MANGO_STACK_SIZE 0x10000u
#define MANGO_HEAP_SIZE 0x1000000u
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
#define MANGO_LIBC_REALLOC 67
#define MANGO_LIBC_WRITE 68
#define MANGO_LIBC_SYSCONF 69
#define MANGO_LIBC_MMAP 70
#define MANGO_LIBC_MUNMAP 71
#define MANGO_LIBC_MPROTECT 72
#define MANGO_LIBC_GETPAGESIZE 73
#define MANGO_LIBC_PTHREAD_EQUAL 74
#define MANGO_LIBC_EXIT 75
#define MANGO_LIBC_EXIT_UNDERSCORE 76
#define MANGO_LIBC_AEABI_IDIV 77
#define MANGO_LIBC_AEABI_IDIVMOD 78
#define MANGO_LIBC_AEABI_UIDIV 79
#define MANGO_LIBC_AEABI_UIDIVMOD 80
#define MANGO_LIBC_FOPEN 81
#define MANGO_LIBC_FCLOSE 82
#define MANGO_LIBC_FREAD 83
#define MANGO_LIBC_FWRITE 84
#define MANGO_LIBC_FSEEK 85
#define MANGO_LIBC_FTELL 86
#define MANGO_LIBC_IMG_LOAD 87
#define MANGO_LIBC_AEABI_I2D 88
#define MANGO_LIBC_AEABI_D2IZ 89
#define MANGO_LIBC_SDL_GETTICKS 90
#define MANGO_LIBC_SDL_DELAY 91
#define MANGO_LIBC_SDL_POLL_EVENT 92
#define MANGO_LIBC_SDL_UPPER_BLIT 93
#define MANGO_LIBC_SDL_UPDATE_RECT 94
#define MANGO_LIBC_SDL_SET_PALETTE 95
#define MANGO_LIBC_SDL_SET_COLORS 96
#define MANGO_LIBC_SDL_FLIP 97
#define MANGO_LIBC_SDL_FILL_RECT 98
#define MANGO_LIBC_SDL_FREE_SURFACE 99
#define MANGO_LIBC_SDL_WM_SET_CAPTION 100
#define MANGO_LIBC_SDL_WM_SET_ICON 101
#define MANGO_LIBC_SDL_QUIT 102
#define MANGO_LIBC_MIX_OPEN_AUDIO 103
#define MANGO_LIBC_MIX_ALLOCATE_CHANNELS 104
#define MANGO_LIBC_MIX_VOLUME_MUSIC 105
#define MANGO_LIBC_MIX_HALT_MUSIC 106
#define MANGO_LIBC_MIX_FREE_MUSIC 107
#define MANGO_LIBC_MIX_LOAD_MUS 108
#define MANGO_LIBC_MIX_PLAY_MUSIC 109
#define MANGO_LIBC_MIX_LOADWAV_RW 110
#define MANGO_LIBC_MIX_PLAY_CHANNEL_TIMED 111
#define MANGO_LIBC_MIX_HALT_CHANNEL 112
#define MANGO_LIBC_MIX_FREE_CHUNK 113
#define MANGO_LIBC_MIX_VOLUME 114
#define MANGO_LIBC_MIX_PLAYING 115
#define MANGO_LIBC_SPRINTF 116
#define MANGO_LIBC_AEABI_DADD 117
#define MANGO_LIBC_AEABI_DSUB 118
#define MANGO_LIBC_AEABI_DMUL 119
#define MANGO_LIBC_AEABI_DDIV 120
#define MANGO_LIBC_AEABI_FADD 121
#define MANGO_LIBC_AEABI_FSUB 122
#define MANGO_LIBC_AEABI_FMUL 123
#define MANGO_LIBC_AEABI_FDIV 124
#define MANGO_LIBC_AEABI_F2D 125
#define MANGO_LIBC_AEABI_D2F 126
#define MANGO_LIBC_AEABI_I2F 127
#define MANGO_LIBC_AEABI_D2UIZ 128
#define MANGO_LIBC_AEABI_F2IZ 129
#define MANGO_LIBC_AEABI_F2UIZ 130
#define MANGO_LIBC_AEABI_DCMPLT 131
#define MANGO_LIBC_AEABI_DCMPLE 132
#define MANGO_LIBC_AEABI_DCMPGE 133
#define MANGO_LIBC_AEABI_FCMPEQ 134
#define MANGO_LIBC_AEABI_FCMPLT 135
#define MANGO_LIBC_AEABI_FCMPLE 136
#define MANGO_LIBC_AEABI_FCMPGT 137
#define MANGO_LIBC_LRAND48 138
#define MANGO_LIBC_SRAND48 139
#define MANGO_LIBC_TIME 140
#define MANGO_LIBC_COUNT 141
#define MANGO_TSD_KEYS 16

#define MANGO_AS_SIZE 0x4000000u
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
  uint32_t page_size_addr;
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
    "realloc",
    "write",
    "sysconf",
    "mmap",
    "munmap",
    "mprotect",
    "getpagesize",
    "pthread_equal",
    "exit",
    "_exit",
    "__aeabi_idiv",
    "__aeabi_idivmod",
    "__aeabi_uidiv",
    "__aeabi_uidivmod",
    "fopen",
    "fclose",
    "fread",
    "fwrite",
    "fseek",
    "ftell",
    "IMG_Load",
    "__aeabi_i2d",
    "__aeabi_d2iz",
    "SDL_GetTicks",
    "SDL_Delay",
    "SDL_PollEvent",
    "SDL_UpperBlit",
    "SDL_UpdateRect",
    "SDL_SetPalette",
    "SDL_SetColors",
    "SDL_Flip",
    "SDL_FillRect",
    "SDL_FreeSurface",
    "SDL_WM_SetCaption",
    "SDL_WM_SetIcon",
    "SDL_Quit",
    "Mix_OpenAudio",
    "Mix_AllocateChannels",
    "Mix_VolumeMusic",
    "Mix_HaltMusic",
    "Mix_FreeMusic",
    "Mix_LoadMUS",
    "Mix_PlayMusic",
    "Mix_LoadWAV_RW",
    "Mix_PlayChannelTimed",
    "Mix_HaltChannel",
    "Mix_FreeChunk",
    "Mix_Volume",
    "Mix_Playing",
    "sprintf",
    "__aeabi_dadd",
    "__aeabi_dsub",
    "__aeabi_dmul",
    "__aeabi_ddiv",
    "__aeabi_fadd",
    "__aeabi_fsub",
    "__aeabi_fmul",
    "__aeabi_fdiv",
    "__aeabi_f2d",
    "__aeabi_d2f",
    "__aeabi_i2f",
    "__aeabi_d2uiz",
    "__aeabi_f2iz",
    "__aeabi_f2uiz",
    "__aeabi_dcmplt",
    "__aeabi_dcmple",
    "__aeabi_dcmpge",
    "__aeabi_fcmpeq",
    "__aeabi_fcmplt",
    "__aeabi_fcmple",
    "__aeabi_fcmpgt",
    "lrand48",
    "srand48",
    "time",
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
static uint64_t g_sdl_ticks_start_ms;
static uint64_t g_lrand48_state = 0x1234abcd330eull; /* POSIX drand48 seed */
/* Soft-float host-stub call histogram (Meritous mapgen / title plasma). */
static uint64_t g_aeabi_calls[MANGO_LIBC_COUNT];
static uint64_t g_aeabi_calls_total;
static uint64_t g_pc_hist_steps;
enum { MANGO_PC_HIST_BUCKETS = 4096 };
static uint32_t g_pc_hist[MANGO_PC_HIST_BUCKETS];
static int g_meritous_progress_armed;
static uint32_t g_meritous_bias;
static uint32_t g_meritous_seen_mask; /* bit0 DungeonPlay, bit1 Generate, bit2 RandomGenerateMap */

static void mango_store_u32_guest(uint8_t* mem, uint32_t addr, uint32_t v);
static int mango_guest_range_ok(const MangoLoadedLibrary* lib, uint32_t addr, uint32_t n);

static void mango_aeabi_note(uint32_t which) {
  if (which < MANGO_LIBC_COUNT) {
    g_aeabi_calls[which]++;
  }
  g_aeabi_calls_total++;
  if ((g_aeabi_calls_total & 0xfffffu) == 0u) {
    fprintf(stderr, "mango: softfloat host calls=%llu\n",
            (unsigned long long)g_aeabi_calls_total);
  }
}

static void mango_dump_aeabi_top(void) {
  fprintf(stderr, "mango: softfloat host total=%llu\n",
          (unsigned long long)g_aeabi_calls_total);
  for (uint32_t i = 0; i < MANGO_LIBC_COUNT; i++) {
    if (g_aeabi_calls[i] == 0) continue;
    if (strncmp(kLibcNames[i], "__aeabi_", 8) != 0) continue;
    fprintf(stderr, "mango: softfloat %-20s %llu\n", kLibcNames[i],
            (unsigned long long)g_aeabi_calls[i]);
  }
}

static void mango_pc_hist_sample(uint32_t pc) {
  g_pc_hist[(pc >> 4) & (MANGO_PC_HIST_BUCKETS - 1u)]++;
  g_pc_hist_steps++;
}

static void mango_dump_pc_hist(void) {
  if (g_pc_hist_steps == 0) return;
  fprintf(stderr, "mango: pc-hist samples=%llu (bucket=pc>>4 & 0xfff)\n",
          (unsigned long long)g_pc_hist_steps);
  for (int pass = 0; pass < 8; pass++) {
    uint32_t best_i = 0, best_c = 0;
    for (uint32_t i = 0; i < MANGO_PC_HIST_BUCKETS; i++) {
      if (g_pc_hist[i] > best_c) {
        best_c = g_pc_hist[i];
        best_i = i;
      }
    }
    if (best_c == 0) break;
    fprintf(stderr, "mango: pc-hist top pc~0x%x count=%u\n", best_i << 4, best_c);
    g_pc_hist[best_i] = 0;
  }
}

static void mango_meritous_progress(uint32_t pc) {
  if (!g_meritous_progress_armed) return;
  if (pc < g_meritous_bias) return;
  uint32_t va = (pc & ~1u) - g_meritous_bias;
  /* Ranges from nm -D on libapplication.so (Thumb). */
  if (!(g_meritous_seen_mask & 1u) && va >= 0x1af28u && va < 0x1c8f0u) {
    g_meritous_seen_mask |= 1u;
    fprintf(stderr, "mango: Meritous in DungeonPlay (va=0x%x)\n", va);
  } else if (!(g_meritous_seen_mask & 2u) && va >= 0x1e4f8u && va < 0x1e638u) {
    g_meritous_seen_mask |= 2u;
    fprintf(stderr, "mango: Meritous in Generate (va=0x%x)\n", va);
  } else if (!(g_meritous_seen_mask & 4u) && va >= 0x1e638u && va < 0x1f000u) {
    g_meritous_seen_mask |= 4u;
    fprintf(stderr, "mango: Meritous in RandomGenerateMap (va=0x%x)\n", va);
  } else if (va >= 0x1d404u && va < 0x1e4f8u) {
    if (!(g_meritous_seen_mask & 8u)) {
      g_meritous_seen_mask |= 8u;
      fprintf(stderr, "mango: Meritous in DestroyDungeon/map IO (va=0x%x)\n", va);
    }
    /* DestroyDungeon after DungeonPlay ⇒ run finished; arm title→exit. */
    if ((g_meritous_seen_mask & 1u) && !(g_meritous_seen_mask & 32u)) {
      g_meritous_seen_mask |= 32u;
      fprintf(stderr, "mango: Meritous post-dungeon DestroyDungeon\n");
    }
  }
  /* After a completed DungeonPlay then DestroyDungeon, rewrite title head. */
  if ((g_meritous_seen_mask & 32u) && !(g_meritous_seen_mask & 16u)) {
    uint32_t th = g_meritous_bias + 0x1ccd8u;
    /* Find exit libc thunk; fall back to title Quit path: movs r0,#0; bl exit
     * via writing bx to a tiny A32 thunk we place is hard — use Thumb:
     *   ldr r0, [pc, #4]; blx r0; .word exit_thunk
     * Simpler: store game_running=0 already; for title use SVC exit thunk addr. */
    for (uint32_t i = 0; i < MANGO_LIBC_COUNT; i++) {
      if (strcmp(kLibcNames[i], "exit") == 0) {
        /* Thumb: ldr r3,[pc,#4]; blx r3; .word thunk|1 is messy.
         * Write:  bx pc; nop; then ARM: ldr r0,=thunk; blx r0 — too long.
         * Overwrite with: movs r0,#0; ldr r1,[pc,#0]; bx r1; .word exit_thunk */
        uint32_t thunk = 0;
        /* libc thunk base lives on every lib; use first loaded. */
        if (g_nlibs > 0) {
          thunk = g_libs[0]->libc_thunk_base + i * MANGO_JNI_THUNK_SIZE;
        }
        if (thunk && mango_guest_range_ok(g_libs[0], th, 12u)) {
          /* movs r0,#0; ldr r1,[pc,#4]; bx r1; nop; .word thunk */
          g_libs[0]->guest_mem[th + 0u] = 0x00u;
          g_libs[0]->guest_mem[th + 1u] = 0x20u; /* movs r0,#0 */
          g_libs[0]->guest_mem[th + 2u] = 0x01u;
          g_libs[0]->guest_mem[th + 3u] = 0x49u; /* ldr r1,[pc,#4] */
          g_libs[0]->guest_mem[th + 4u] = 0x08u;
          g_libs[0]->guest_mem[th + 5u] = 0x47u; /* bx r1 */
          g_libs[0]->guest_mem[th + 6u] = 0x00u;
          g_libs[0]->guest_mem[th + 7u] = 0xbfu; /* nop */
          mango_store_u32_guest(g_libs[0]->guest_mem, th + 8u, thunk);
          g_meritous_seen_mask |= 16u;
          fprintf(stderr, "mango: Meritous title head -> exit after one dungeon\n");
        }
        break;
      }
    }
  }
}

/* Guest stdio: FILE* are small host-table ids (0 = NULL). Meritous IMG_Load
 * reaches SDL_RWFromFile → fopen("dat/i/title.png"); without this the NULL
 * surface is used as a guest object at address 0 (ELF header) and a 640×480
 * store loop clobbers libsdl .text including SDL_UpperBlit. */
#define MANGO_FILE_MAX 64
#define MANGO_FAKE_GLES_HANDLE 0x7e01u
static FILE* g_files[MANGO_FILE_MAX];

static FILE* mango_file_get(uint32_t id) {
  if (id == 0 || id >= MANGO_FILE_MAX) {
    return NULL;
  }
  return g_files[id];
}

static uint32_t mango_file_intern(FILE* fp) {
  if (fp == NULL) {
    return 0;
  }
  for (uint32_t i = 1; i < MANGO_FILE_MAX; i++) {
    if (g_files[i] == NULL) {
      g_files[i] = fp;
      return i;
    }
  }
  fclose(fp);
  return 0;
}

static void mango_file_release(uint32_t id) {
  if (id == 0 || id >= MANGO_FILE_MAX) {
    return;
  }
  if (g_files[id]) {
    fclose(g_files[id]);
    g_files[id] = NULL;
  }
}

/* Resolve path for fopen: as-is, then <libdir>/../gamedata/<path>, then
 * <libdir>/../<path>, then $MANGO_ASSET_ROOT/<path>. */
static FILE* mango_host_fopen(MangoLoadedLibrary* lib, const char* path, const char* mode) {
  char cand[768];
  FILE* fp;
  const char* root;
  if (!path || !mode) {
    return NULL;
  }
  fp = fopen(path, mode);
  if (fp) {
    return fp;
  }
  if (lib && lib->path[0] && strrchr(lib->path, '/')) {
    const char* slash = strrchr(lib->path, '/');
    int dlen = (int)(slash - lib->path);
    snprintf(cand, sizeof(cand), "%.*s/../gamedata/%s", dlen, lib->path, path);
    fp = fopen(cand, mode);
    if (fp) {
      return fp;
    }
    snprintf(cand, sizeof(cand), "%.*s/../%s", dlen, lib->path, path);
    fp = fopen(cand, mode);
    if (fp) {
      return fp;
    }
  }
  root = getenv("MANGO_ASSET_ROOT");
  if (root && root[0]) {
    snprintf(cand, sizeof(cand), "%s/%s", root, path);
    fp = fopen(cand, mode);
    if (fp) {
      return fp;
    }
  }
  return NULL;
}

/* Resolve guest asset path to a host filesystem path (same search order as
 * mango_host_fopen). Returns 1 on success with out filled. */
static int mango_host_resolve_path(MangoLoadedLibrary* lib, const char* path,
                                   char* out, size_t outsz) {
  const char* root;
  if (!path || !out || outsz == 0) {
    return 0;
  }
  if (access(path, R_OK) == 0) {
    snprintf(out, outsz, "%s", path);
    return 1;
  }
  if (lib && lib->path[0] && strrchr(lib->path, '/')) {
    const char* slash = strrchr(lib->path, '/');
    int dlen = (int)(slash - lib->path);
    snprintf(out, outsz, "%.*s/../gamedata/%s", dlen, lib->path, path);
    if (access(out, R_OK) == 0) {
      return 1;
    }
    snprintf(out, outsz, "%.*s/../%s", dlen, lib->path, path);
    if (access(out, R_OK) == 0) {
      return 1;
    }
  }
  root = getenv("MANGO_ASSET_ROOT");
  if (root && root[0]) {
    snprintf(out, outsz, "%s/%s", root, path);
    if (access(out, R_OK) == 0) {
      return 1;
    }
  }
  return 0;
}

#ifndef ANDROID
/* Decode PNG via host libpng into tightly packed RGBA8888 (row-major).
 * Meritous assets are 8-bit grayscale; expand to RGB + opaque A. */
static int mango_png_decode_rgba(const char* host_path, uint8_t** out_rgba,
                                 uint32_t* out_w, uint32_t* out_h) {
  FILE* fp;
  png_structp png = NULL;
  png_infop info = NULL;
  png_bytep* rows = NULL;
  uint8_t* rgba = NULL;
  png_uint_32 w = 0, h = 0;
  size_t rowbytes, nbytes;
  png_uint_32 y;
  if (!host_path || !out_rgba || !out_w || !out_h) {
    return -1;
  }
  *out_rgba = NULL;
  *out_w = 0;
  *out_h = 0;
  fp = fopen(host_path, "rb");
  if (!fp) {
    return -1;
  }
  png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(fp);
    return -1;
  }
  info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    return -1;
  }
  if (setjmp(png_jmpbuf(png))) {
    free(rgba);
    if (rows) {
      free(rows);
    }
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return -1;
  }
  png_init_io(png, fp);
  png_read_info(png, info);
  w = png_get_image_width(png, info);
  h = png_get_image_height(png, info);
  if (w == 0 || h == 0 || w > 4096u || h > 4096u) {
    longjmp(png_jmpbuf(png), 1);
  }
  {
    int bit_depth = png_get_bit_depth(png, info);
    int color_type = png_get_color_type(png, info);
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
      png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
      png_set_tRNS_to_alpha(png);
    }
    if (bit_depth == 16) {
      png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
      png_set_gray_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    }
  }
  png_read_update_info(png, info);
  rowbytes = png_get_rowbytes(png, info);
  if (rowbytes < (size_t)w * 4u) {
    longjmp(png_jmpbuf(png), 1);
  }
  nbytes = (size_t)w * (size_t)h * 4u;
  rgba = (uint8_t*)malloc(nbytes);
  rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
  if (!rgba || !rows) {
    longjmp(png_jmpbuf(png), 1);
  }
  for (y = 0; y < h; y++) {
    rows[y] = rgba + (size_t)y * (size_t)w * 4u;
  }
  /* If libpng rowbytes > w*4 (padding), read into temp then compact — Meritous
   * assets are tightly packed after expand, so direct rows are fine when equal. */
  if (rowbytes == (size_t)w * 4u) {
    png_read_image(png, rows);
  } else {
    png_bytep tmp = (png_bytep)malloc(rowbytes);
    if (!tmp) {
      longjmp(png_jmpbuf(png), 1);
    }
    for (y = 0; y < h; y++) {
      png_read_row(png, tmp, NULL);
      memcpy(rows[y], tmp, (size_t)w * 4u);
    }
    free(tmp);
  }
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  free(rows);
  *out_rgba = rgba;
  *out_w = (uint32_t)w;
  *out_h = (uint32_t)h;
  return 0;
}
#endif


static void* mango_load_library(const char* libpath, int flag);
static int mango_run_guest(MangoLoadedLibrary* lib, MangoCpu* cpu, JNIEnv* env);
static const char* mango_guest_cstr(MangoLoadedLibrary* lib, uint32_t addr);
static uint32_t mango_guest_alloc(MangoLoadedLibrary* lib, uint32_t n);
static uint32_t mango_guest_sdl_surface(MangoLoadedLibrary* lib, uint32_t w, uint32_t h);
static int mango_host_resolve_path(MangoLoadedLibrary* lib, const char* path,
                                   char* out, size_t outsz);
#ifndef ANDROID
static int mango_png_decode_rgba(const char* host_path, uint8_t** out_rgba,
                                 uint32_t* out_w, uint32_t* out_h);
#endif
static void mango_patch_meritous_img_load(MangoLoadedLibrary* lib);


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

/* Page-aligned anonymous mapping for Boehm GC GET_MEM (mmap). */
static uint32_t mango_guest_alloc_pages(MangoLoadedLibrary* lib, uint32_t n) {
  const uint32_t page = 4096u;
  uint32_t cur, aligned, pad, total;
  if (n == 0) {
    return 0;
  }
  n = (n + page - 1u) & ~(page - 1u);
  cur = lib->heap_base + lib->heap_used;
  aligned = (cur + page - 1u) & ~(page - 1u);
  pad = aligned - cur;
  total = pad + n;
  if (lib->heap_used + total > MANGO_HEAP_SIZE) {
    return 0;
  }
  lib->heap_used += total;
  memset(lib->guest_mem + aligned, 0, n);
  return aligned;
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
    lib->page_size_addr = first->page_size_addr;
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
  lib->heap_used = 12u; /* errno +0, stack guard +4, __page_size +8 */
  lib->guest_errno_addr = heap;
  lib->page_size_addr = heap + 8u;
  mango_store_u32_guest(mem, heap + 4u, 0xA5A5A5A5u);
  mango_store_u32_guest(mem, heap + 8u, 4096u); /* bionic __page_size */
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
  if (strcmp(name, "__page_size") == 0) {
    /* Bionic data symbol; Boehm GC reads *(__page_size). Must not be the
     * mov-r0-#0 stub (that made GC_page_size=0xe3a00000 → Bad GET_MEM arg). */
    return lib->page_size_addr;
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


/* Guest printf-family helper: walk %s/%d/%i/%u/%x/%X/%c/%% with AAPCS
 * varargs starting in the given register window then the stack.
 * Meritous TitleScreenMusic does sprintf(buf, "dat/m/track%d.%s", n, ext);
 * without this the default mov-r0-#0 stub leaves the stack buffer as 0xFF
 * and fopen logs "FAIL <garbled>". */
typedef struct MangoGuestVA {
  MangoLoadedLibrary* lib;
  uint32_t regs[4];
  int nregs;
  uint32_t sp;
} MangoGuestVA;

static uint32_t mango_va_u32(MangoGuestVA* va) {
  if (va->nregs > 0) {
    uint32_t v = va->regs[0];
    for (int i = 1; i < va->nregs; i++) {
      va->regs[i - 1] = va->regs[i];
    }
    va->nregs--;
    return v;
  }
  if (!mango_guest_range_ok(va->lib, va->sp, 4u)) {
    return 0;
  }
  uint32_t v = mango_load_u32_guest(va->lib->guest_mem, va->sp);
  va->sp += 4u;
  return v;
}

static int mango_guest_format(MangoLoadedLibrary* lib, char* out, size_t out_sz, const char* fmt,
                              MangoGuestVA* va) {
  size_t o = 0;
  if (!fmt || !out || out_sz == 0) {
    return -1;
  }
  for (const char* p = fmt; *p && o + 1 < out_sz; p++) {
    if (*p != '%') {
      out[o++] = *p;
      continue;
    }
    p++;
    if (*p == '%') {
      out[o++] = '%';
      continue;
    }
    /* skip simple flags/width/precision */
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
      p++;
    }
    while (*p >= '0' && *p <= '9') {
      p++;
    }
    if (*p == '.') {
      p++;
      while (*p >= '0' && *p <= '9') {
        p++;
      }
    }
    if (*p == 'l' || *p == 'z' || *p == 't' || *p == 'h') {
      p++;
      if (*p == 'l') {
        p++;
      }
    }
    char tmp[64];
    size_t n = 0;
    if (*p == 's') {
      uint32_t a = mango_va_u32(va);
      const char* s = mango_guest_cstr(lib, a);
      if (!s) {
        s = "(null)";
      }
      n = strlen(s);
      if (o + n >= out_sz) {
        n = out_sz - 1 - o;
      }
      memcpy(out + o, s, n);
      o += n;
    } else if (*p == 'd' || *p == 'i') {
      int32_t v = (int32_t)mango_va_u32(va);
      n = (size_t)snprintf(tmp, sizeof(tmp), "%d", v);
      if (o + n >= out_sz) {
        n = out_sz - 1 - o;
      }
      memcpy(out + o, tmp, n);
      o += n;
    } else if (*p == 'u') {
      uint32_t v = mango_va_u32(va);
      n = (size_t)snprintf(tmp, sizeof(tmp), "%u", v);
      if (o + n >= out_sz) {
        n = out_sz - 1 - o;
      }
      memcpy(out + o, tmp, n);
      o += n;
    } else if (*p == 'x' || *p == 'X') {
      uint32_t v = mango_va_u32(va);
      n = (size_t)snprintf(tmp, sizeof(tmp), (*p == 'x') ? "%x" : "%X", v);
      if (o + n >= out_sz) {
        n = out_sz - 1 - o;
      }
      memcpy(out + o, tmp, n);
      o += n;
    } else if (*p == 'c') {
      out[o++] = (char)(mango_va_u32(va) & 0xffu);
    } else if (*p == 'p') {
      uint32_t v = mango_va_u32(va);
      n = (size_t)snprintf(tmp, sizeof(tmp), "%x", v);
      if (o + n >= out_sz) {
        n = out_sz - 1 - o;
      }
      memcpy(out + o, tmp, n);
      o += n;
    } else if (*p == '\0') {
      break;
    } else {
      /* Unknown conversion: copy literally. */
      if (o + 2 < out_sz) {
        out[o++] = '%';
        out[o++] = *p;
      }
    }
  }
  out[o] = 0;
  return (int)o;
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
    case MANGO_LIBC_REALLOC: {
      /* Unity MemoryManager realloc: r0=old (may be 0), r1=new bytes. */
      uint32_t old = r0;
      uint32_t n = (r1 + 7u) & ~7u;
      uint32_t neu;
      if (n == 0) {
        n = 8u;
      }
      if (lib->heap_used + n > MANGO_HEAP_SIZE) {
        cpu->r[0] = 0;
        break;
      }
      neu = lib->heap_base + lib->heap_used;
      if (old != 0 && old >= lib->heap_base && old < lib->heap_base + lib->heap_used) {
        uint32_t avail = (lib->heap_base + lib->heap_used) - old;
        uint32_t copy = n < avail ? n : avail;
        memmove(lib->guest_mem + neu, lib->guest_mem + old, copy);
      }
      lib->heap_used += n;
      cpu->r[0] = neu;
      break;
    }
    case MANGO_LIBC_FREE:
      break;
    case MANGO_LIBC_MEMCPY:
    case MANGO_LIBC_AEABI_MEMCPY:
    case MANGO_LIBC_MEMMOVE:
    case MANGO_LIBC_AEABI_MEMMOVE:
      /* Unity's MemoryManager can return NULL before it is constructed; refuse to
       * smash guest address 0 (seen as ASCII "DIRECTIONAL_COOKIE" / NAL_ OOB). */
      if (r0 == 0 || !mango_guest_range_ok(lib, r0, r2) || !mango_guest_range_ok(lib, r1, r2)) {
        cpu->r[0] = 0;
      } else {
        memmove(lib->guest_mem + r0, lib->guest_mem + r1, r2);
        cpu->r[0] = r0;
      }
      break;
    case MANGO_LIBC_MEMSET:
      if (r0 == 0 || !mango_guest_range_ok(lib, r0, r2)) {
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
      if (r0 == 0 || !src || !mango_guest_range_ok(lib, r0, (uint32_t)n)) {
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
      const char* base;
      if (!nm) {
        cpu->r[0] = 0;
        break;
      }
      base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      /* Host has no GLES .so; libsdl NEEDED/dlopen's it for Android GL video.
       * Return a fake handle so init continues; gl* already resolve to stubs. */
      if (strcmp(base, "libGLESv1_CM.so") == 0 || strcmp(base, "libGLESv2.so") == 0 ||
          strcmp(base, "libEGL.so") == 0) {
        fprintf(stderr, "mango: dlopen STUB %s -> id=%u\n", nm, MANGO_FAKE_GLES_HANDLE);
        cpu->r[0] = MANGO_FAKE_GLES_HANDLE;
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
        if (r0 == 0 || r0 == MANGO_FAKE_GLES_HANDLE) {
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
    case MANGO_LIBC_WRITE:
      /* Discard sink: report full count so write-all loops advance. Unresolved
       * write used to hit the mov-r0-#0 stub and spin forever (libmono). */
      if (r2 != 0 && !mango_guest_range_ok(lib, r1, r2)) {
        cpu->r[0] = (uint32_t)-1;
      } else {
        cpu->r[0] = r2;
      }
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
      /* snprintf(buf, n, fmt, ...) — varargs start in r3 then stack. */
      const char* fmt = mango_guest_cstr(lib, r2);
      if (!fmt || r1 == 0 || !mango_guest_range_ok(lib, r0, r1)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      char tmp[512];
      MangoGuestVA va;
      va.lib = lib;
      va.nregs = 1;
      va.regs[0] = cpu->r[3];
      va.sp = cpu->r[MANGO_REG_SP];
      int wrote = mango_guest_format(lib, tmp, sizeof(tmp), fmt, &va);
      if (wrote < 0) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      size_t n = (size_t)wrote;
      if (n >= r1) {
        n = r1 - 1u;
      }
      memcpy(lib->guest_mem + r0, tmp, n);
      lib->guest_mem[r0 + (uint32_t)n] = 0;
      cpu->r[0] = (uint32_t)wrote;
      break;
    }
    case MANGO_LIBC_SPRINTF: {
      /* sprintf(buf, fmt, ...) — varargs start in r2,r3 then stack.
       * Meritous: sprintf(buf, "dat/m/track%d.%s", track, ext). */
      const char* fmt = mango_guest_cstr(lib, r1);
      if (!fmt || !mango_guest_range_ok(lib, r0, 1u)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      char tmp[512];
      MangoGuestVA va;
      va.lib = lib;
      va.nregs = 2;
      va.regs[0] = r2;
      va.regs[1] = cpu->r[3];
      va.sp = cpu->r[MANGO_REG_SP];
      int wrote = mango_guest_format(lib, tmp, sizeof(tmp), fmt, &va);
      if (wrote < 0) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      size_t n = (size_t)wrote;
      /* Cap to remaining guest space from buf. */
      uint32_t max = lib->guest_mem_size - r0;
      if (n >= max) {
        n = max ? max - 1u : 0u;
      }
      if (n > 0) {
        memcpy(lib->guest_mem + r0, tmp, n);
      }
      if (r0 < lib->guest_mem_size) {
        lib->guest_mem[r0 + (uint32_t)n] = 0;
      }
      {
        static int s_sp;
        if (s_sp < 6) {
          fprintf(stderr, "mango: sprintf -> '%s'\n", tmp);
          s_sp++;
        }
      }
      cpu->r[0] = (uint32_t)wrote;
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
      if (r0 == 0 || !mango_guest_range_ok(lib, r0, r1)) {
        cpu->r[0] = 0;
      } else {
        memset(lib->guest_mem + r0, (int)(r2 & 0xFFu), r1);
        cpu->r[0] = r0;
      }
      break;
    case MANGO_LIBC_SYSCONF:
      /* bionic: _SC_PAGESIZE=0x27, _SC_CLK_TCK=0x28, _SC_NPROCESSORS_*=0x60/0x61,
       * _SC_PHYS_PAGES=0x62; glibc pagesize=30. Unresolved/zero pagesize left
       * Boehm GC_page_size as garbage (saw 0xe3a00000) → "Bad GET_MEM arg". */
      if (r0 == 0x27u || r0 == 30u || r0 == 39u) {
        cpu->r[0] = 4096u;
      } else if (r0 == 0x28u || r0 == 40u || r0 == 2u) {
        cpu->r[0] = 100u; /* CLK_TCK */
      } else if (r0 == 0x60u || r0 == 0x61u || r0 == 83u || r0 == 84u || r0 == 97u) {
        cpu->r[0] = 4u; /* NPROCESSORS */
      } else if (r0 == 0x62u || r0 == 0x63u || r0 == 98u || r0 == 99u) {
        cpu->r[0] = 256u * 1024u; /* PHYS/AVPHYS pages (~1GiB) */
      } else {
        cpu->r[0] = (uint32_t)-1;
      }
      break;
    case MANGO_LIBC_MMAP: {
      /* Anon guest pages for Boehm GC GET_MEM. Reject absurd lengths (seen
       * 0xe3a00000 when GC_page_size was an ARM 'mov r0,#0' opcode). */
      uint32_t a = 0;
      if (r1 != 0 && r1 <= (MANGO_HEAP_SIZE / 2u)) {
        a = mango_guest_alloc_pages(lib, r1);
      }
      cpu->r[0] = a ? a : (uint32_t)-1; /* MAP_FAILED */
      break;
    }
    case MANGO_LIBC_MUNMAP:
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_MPROTECT:
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_GETPAGESIZE:
      cpu->r[0] = 4096u;
      break;
    case MANGO_LIBC_PTHREAD_EQUAL:
      /* Default stub returned 0 (not equal), so Boehm GC_lookup_thread never
       * matched the registered main thread → "Collecting from unknown thread." */
      cpu->r[0] = (r0 == r1) ? 1u : 0u;
      break;
    case MANGO_LIBC_ABORT:
    case MANGO_LIBC_EXIT:
    case MANGO_LIBC_EXIT_UNDERSCORE:
      /* Noreturn: mono/Unity BL exit/abort with LR on the next literal pool.
       * The default mov-r0-#0 stub returned there (OFDP pc=0x17101c word=0x0022ff98).
       * Point LR at the JNI stop sentinel so the thunk's bx lr ends the trampoline. */
      cpu->r[MANGO_REG_LR] = MANGO_JNI_STOP;
      break;
    case MANGO_LIBC_AEABI_IDIV:
    case MANGO_LIBC_AEABI_IDIVMOD: {
      /* Meritous SDL_main init loops call soft-div via PLT hundreds of thousands
       * of times; interpreting libsdl's __aeabi_idivmod burns the step budget.
       * Host-implement EABI: quot in r0, rem in r1 (idivmod). */
      int32_t num = (int32_t)r0;
      int32_t den = (int32_t)r1;
      if (den == 0) {
        cpu->r[0] = 0;
        cpu->r[1] = (uint32_t)num;
      } else if (num == (int32_t)0x80000000 && den == -1) {
        cpu->r[0] = (uint32_t)0x80000000u; /* INT_MIN / -1 */
        cpu->r[1] = 0;
      } else {
        cpu->r[0] = (uint32_t)(num / den);
        cpu->r[1] = (uint32_t)(num % den);
      }
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_UIDIV:
    case MANGO_LIBC_AEABI_UIDIVMOD: {
      if (r1 == 0) {
        cpu->r[0] = 0;
        cpu->r[1] = r0;
      } else {
        cpu->r[0] = r0 / r1;
        cpu->r[1] = r0 % r1;
      }
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_FOPEN: {
      const char* path = mango_guest_cstr(lib, r0);
      const char* mode = mango_guest_cstr(lib, r1);
      FILE* fp = mango_host_fopen(lib, path, mode ? mode : "rb");
      cpu->r[0] = mango_file_intern(fp);
      if (cpu->r[0] == 0 && path) {
        static int s_fail;
        int printable = 1;
        for (const char* p = path; *p; p++) {
          if ((unsigned char)*p < 0x20u || (unsigned char)*p >= 0x7fu) {
            printable = 0;
            break;
          }
        }
        if (s_fail < 12) {
          if (printable) {
            fprintf(stderr, "mango: fopen FAIL %s\n", path);
          } else {
            fprintf(stderr, "mango: fopen FAIL <garbled len=%zu hex=", strlen(path));
            size_t lim = strlen(path);
            if (lim > 16u) lim = 16u;
            for (size_t i = 0; i < lim; i++) {
              fprintf(stderr, "%02x", (unsigned char)path[i]);
            }
            fprintf(stderr, ">\n");
          }
          s_fail++;
        }
      }
      break;
    }
    case MANGO_LIBC_FCLOSE:
      mango_file_release(r0);
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_FREAD: {
      /* fread(ptr, size, nmemb, fp) — r0,r1,r2,r3 */
      FILE* fp = mango_file_get(cpu->r[3]);
      uint64_t bytes = (uint64_t)r1 * (uint64_t)r2;
      if (!fp || r1 == 0 || r2 == 0 || bytes > 0xffffffffu ||
          !mango_guest_range_ok(lib, r0, (uint32_t)bytes)) {
        cpu->r[0] = 0;
      } else {
        size_t n = fread(lib->guest_mem + r0, (size_t)r1, (size_t)r2, fp);
        cpu->r[0] = (uint32_t)n;
      }
      break;
    }
    case MANGO_LIBC_FWRITE: {
      FILE* fp = mango_file_get(cpu->r[3]);
      uint64_t bytes = (uint64_t)r1 * (uint64_t)r2;
      if (!fp || r1 == 0 || r2 == 0 || bytes > 0xffffffffu ||
          !mango_guest_range_ok(lib, r0, (uint32_t)bytes)) {
        cpu->r[0] = 0;
      } else {
        size_t n = fwrite(lib->guest_mem + r0, (size_t)r1, (size_t)r2, fp);
        cpu->r[0] = (uint32_t)n;
      }
      break;
    }
    case MANGO_LIBC_FSEEK: {
      FILE* fp = mango_file_get(r0);
      cpu->r[0] = (fp && fseek(fp, (long)(int32_t)r1, (int)r2) == 0) ? 0u : (uint32_t)-1;
      break;
    }
    case MANGO_LIBC_FTELL: {
      FILE* fp = mango_file_get(r0);
      long pos = fp ? ftell(fp) : -1L;
      cpu->r[0] = (pos < 0) ? (uint32_t)-1 : (uint32_t)pos;
      break;
    }
    case MANGO_LIBC_IMG_LOAD: {
      /* Meritous libsdl_image IMG_Load (basename-gated thunk). Prefer real
       * host libpng decode into an RGBA8888 guest SDL_Surface so DrawLevel
       * sees real tileset/mons pixels; fall back to a blank 256x256 surface. */
      const char* path = mango_guest_cstr(lib, r0);
      uint32_t surf = 0;
      char host_path[768];
      static int s_img;
#ifndef ANDROID
      uint8_t* rgba = NULL;
      uint32_t pw = 0, ph = 0;
#endif
      if (path && mango_host_resolve_path(lib, path, host_path, sizeof(host_path))) {
#ifndef ANDROID
        if (mango_png_decode_rgba(host_path, &rgba, &pw, &ph) == 0 && rgba) {
          surf = mango_guest_sdl_surface(lib, pw, ph);
          if (surf) {
            uint32_t pixels = 0;
            uint32_t nbytes = pw * ph * 4u;
            uint32_t nonzero = 0;
            uint32_t i;
            pixels = mango_load_u32_guest(lib->guest_mem, surf + 20u);
            if (pixels && mango_guest_range_ok(lib, pixels, nbytes)) {
              memcpy(lib->guest_mem + pixels, rgba, nbytes);
              for (i = 0; i < nbytes; i++) {
                if (rgba[i] != 0) {
                  nonzero++;
                }
              }
            }
            if (s_img < 40) {
              fprintf(stderr,
                      "mango: IMG_Load '%s' -> %ux%u png (%u nonzero bytes)\n",
                      path, (unsigned)pw, (unsigned)ph, (unsigned)nonzero);
              s_img++;
            }
          }
          free(rgba);
          rgba = NULL;
        }
#endif
      }
      if (!surf) {
        if (s_img < 40) {
          fprintf(stderr, "mango: IMG_Load '%s' (blank fallback)\n",
                  path ? path : "(null)");
          s_img++;
        }
        surf = mango_guest_sdl_surface(lib, 256u, 256u);
      }
      cpu->r[0] = surf;
      break;
    }
    case MANGO_LIBC_AEABI_I2D: {
      /* int32 -> double in r0:r1 (soft-ABI little-endian). Meritous title
       * plasma loop calls this ~300k× via interpreted libgcc otherwise. */
      union { double d; uint32_t u[2]; } v;
      v.d = (double)(int32_t)r0;
      cpu->r[0] = v.u[0];
      cpu->r[1] = v.u[1];
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_D2IZ: {
      union { double d; uint32_t u[2]; } v;
      v.u[0] = r0;
      v.u[1] = r1;
      cpu->r[0] = (uint32_t)(int32_t)v.d;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_DADD:
    case MANGO_LIBC_AEABI_DSUB:
    case MANGO_LIBC_AEABI_DMUL:
    case MANGO_LIBC_AEABI_DDIV: {
      /* softfp: double args in r0:r1 and r2:r3, result in r0:r1 */
      union { double d; uint32_t u[2]; } a, b, out;
      a.u[0] = r0; a.u[1] = r1;
      b.u[0] = r2; b.u[1] = cpu->r[3];
      if (fn == MANGO_LIBC_AEABI_DADD) out.d = a.d + b.d;
      else if (fn == MANGO_LIBC_AEABI_DSUB) out.d = a.d - b.d;
      else if (fn == MANGO_LIBC_AEABI_DMUL) out.d = a.d * b.d;
      else out.d = (b.d == 0.0) ? 0.0 : (a.d / b.d);
      cpu->r[0] = out.u[0];
      cpu->r[1] = out.u[1];
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_FADD:
    case MANGO_LIBC_AEABI_FSUB:
    case MANGO_LIBC_AEABI_FMUL:
    case MANGO_LIBC_AEABI_FDIV: {
      union { float f; uint32_t u; } a, b, out;
      a.u = r0; b.u = r1;
      if (fn == MANGO_LIBC_AEABI_FADD) out.f = a.f + b.f;
      else if (fn == MANGO_LIBC_AEABI_FSUB) out.f = a.f - b.f;
      else if (fn == MANGO_LIBC_AEABI_FMUL) out.f = a.f * b.f;
      else out.f = (b.f == 0.0f) ? 0.0f : (a.f / b.f);
      cpu->r[0] = out.u;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_F2D: {
      union { float f; uint32_t u; } a;
      union { double d; uint32_t u[2]; } out;
      a.u = r0;
      out.d = (double)a.f;
      cpu->r[0] = out.u[0];
      cpu->r[1] = out.u[1];
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_D2F: {
      union { double d; uint32_t u[2]; } a;
      union { float f; uint32_t u; } out;
      a.u[0] = r0; a.u[1] = r1;
      out.f = (float)a.d;
      cpu->r[0] = out.u;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_I2F: {
      union { float f; uint32_t u; } out;
      out.f = (float)(int32_t)r0;
      cpu->r[0] = out.u;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_D2UIZ: {
      union { double d; uint32_t u[2]; } v;
      v.u[0] = r0; v.u[1] = r1;
      cpu->r[0] = (uint32_t)v.d;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_F2IZ: {
      union { float f; uint32_t u; } v;
      v.u = r0;
      cpu->r[0] = (uint32_t)(int32_t)v.f;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_F2UIZ: {
      union { float f; uint32_t u; } v;
      v.u = r0;
      cpu->r[0] = (uint32_t)v.f;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_DCMPLT:
    case MANGO_LIBC_AEABI_DCMPLE:
    case MANGO_LIBC_AEABI_DCMPGE: {
      union { double d; uint32_t u[2]; } a, b;
      a.u[0] = r0; a.u[1] = r1;
      b.u[0] = r2; b.u[1] = cpu->r[3];
      int rel;
      if (fn == MANGO_LIBC_AEABI_DCMPLT) rel = (a.d < b.d);
      else if (fn == MANGO_LIBC_AEABI_DCMPLE) rel = (a.d <= b.d);
      else rel = (a.d >= b.d);
      cpu->r[0] = rel ? 1u : 0u;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_AEABI_FCMPEQ:
    case MANGO_LIBC_AEABI_FCMPLT:
    case MANGO_LIBC_AEABI_FCMPLE:
    case MANGO_LIBC_AEABI_FCMPGT: {
      union { float f; uint32_t u; } a, b;
      a.u = r0; b.u = r1;
      int rel;
      if (fn == MANGO_LIBC_AEABI_FCMPEQ) rel = (a.f == b.f);
      else if (fn == MANGO_LIBC_AEABI_FCMPLT) rel = (a.f < b.f);
      else if (fn == MANGO_LIBC_AEABI_FCMPLE) rel = (a.f <= b.f);
      else rel = (a.f > b.f);
      cpu->r[0] = rel ? 1u : 0u;
      mango_aeabi_note(fn);
      break;
    }
    case MANGO_LIBC_LRAND48: {
      /* Meritous mapgen: rndnum/rndval → lrand48() % n. Default mov-r0-#0 stub
       * made every placement identical → AddChild always collided → Generate
       * never reached 3000 rooms (idivmod storm). POSIX-ish LCG. */
      g_lrand48_state = (0x5deece66dull * g_lrand48_state + 0xbull) & 0xffffffffffffull;
      cpu->r[0] = (uint32_t)((g_lrand48_state >> 17) & 0x7fffffffu);
      break;
    }
    case MANGO_LIBC_SRAND48: {
      /* srand48(long seed) — low 32 bits of 48-bit state; match glibc. */
      g_lrand48_state = (((uint64_t)(uint32_t)r0) << 16) | 0x330eull;
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_TIME: {
      /* time(NULL) for srand(time(NULL)); non-zero so seed varies. */
      uint32_t t = (uint32_t)(g_sdl_ticks_start_ms / 1000u) + 1700000000u;
      if (r0 && mango_guest_range_ok(lib, r0, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r0, t);
      }
      cpu->r[0] = t;
      break;
    }
    case MANGO_LIBC_SDL_GETTICKS: {
      /* Guest Delay busy-waits on GetTicks; advance a virtual clock so waits
       * finish in a few interpreter steps instead of wall-clock time. */
      g_sdl_ticks_start_ms += 50u;
      cpu->r[0] = (uint32_t)g_sdl_ticks_start_ms;
      break;
    }
    case MANGO_LIBC_SDL_DELAY: {
      /* Title uses GetTicks busy-wait; Delay itself can be a no-op. */
      (void)r0;
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SDL_POLL_EVENT: {
      /* Title waits for KEYDOWN after logo. Logo SkipLogoEvents also polls, so
       * inject SPACE (keysym.sym at event+8 per Meritous HandleEvents) repeatedly. */
      static int s_inject_left = 64;
      uint32_t ev = r0;
      if (s_inject_left > 0 && ev != 0 && mango_guest_range_ok(lib, ev, 16u)) {
        lib->guest_mem[ev + 0u] = 2u; /* SDL_KEYDOWN */
        lib->guest_mem[ev + 1u] = 0u; /* which */
        lib->guest_mem[ev + 2u] = 1u; /* SDL_PRESSED */
        lib->guest_mem[ev + 3u] = 0u;
        /* SDLK_SPACE = 32 at keysym.sym (offset 8 on ARM SDL 1.2). */
        mango_store_u32_guest(lib->guest_mem, ev + 8u, 32u);
        mango_store_u32_guest(lib->guest_mem, ev + 12u, 0u); /* mod */
        s_inject_left--;
        cpu->r[0] = 1;
      } else {
        cpu->r[0] = 0;
      }
      break;
    }
    case MANGO_LIBC_SDL_UPPER_BLIT: {
      /* Software CalculateBlit of 640x480 is millions of interpreted ops.
       * Report success without copying; enough to advance Meritous. */
      (void)r0; (void)r1; (void)r2;
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SDL_UPDATE_RECT: {
      static int s_ur;
      (void)r0; (void)r1; (void)r2;
      if (s_ur < 5) {
        fprintf(stderr, "mango: SDL_UpdateRect stub\n");
        s_ur++;
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SDL_SET_PALETTE:
    case MANGO_LIBC_SDL_SET_COLORS: {
      cpu->r[0] = 1; /* success */
      break;
    }
    case MANGO_LIBC_SDL_FLIP:
    case MANGO_LIBC_SDL_FILL_RECT:
    case MANGO_LIBC_SDL_FREE_SURFACE:
    case MANGO_LIBC_SDL_WM_SET_CAPTION:
    case MANGO_LIBC_SDL_WM_SET_ICON:
    case MANGO_LIBC_SDL_QUIT: {
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_MIX_OPEN_AUDIO: {
      cpu->r[0] = 0; /* Mix_OpenAudio: 0 = success */
      break;
    }
    case MANGO_LIBC_MIX_ALLOCATE_CHANNELS:
    case MANGO_LIBC_MIX_VOLUME_MUSIC:
    case MANGO_LIBC_MIX_VOLUME: {
      cpu->r[0] = r0 ? r0 : 128u;
      break;
    }
    case MANGO_LIBC_MIX_HALT_MUSIC:
    case MANGO_LIBC_MIX_FREE_MUSIC:
    case MANGO_LIBC_MIX_HALT_CHANNEL:
    case MANGO_LIBC_MIX_FREE_CHUNK: {
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_MIX_LOAD_MUS:
    case MANGO_LIBC_MIX_LOADWAV_RW: {
      /* Non-NULL fake chunk/music so callers do not NPE on Free*. */
      cpu->r[0] = mango_guest_alloc(lib, 16u);
      if (cpu->r[0] == 0) {
        cpu->r[0] = 1u;
      }
      break;
    }
    case MANGO_LIBC_MIX_PLAY_MUSIC:
    case MANGO_LIBC_MIX_PLAY_CHANNEL_TIMED: {
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_MIX_PLAYING: {
      cpu->r[0] = 0;
      break;
    }
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

/* Leading '*' = raw AAPCS (no JNIEnv/jobject). Used for SDL_main etc.
 * Example: "*II" → returns int, args in r0 then r1 (first trampoline
 * fixed arg is r0; remaining from va_list). */
static int mango_is_raw_aapcs(const MangoJniSlot* slot) {
  return slot->shorty[0] == '*';
}

static int mango_run_guest(MangoLoadedLibrary* lib, MangoCpu* cpu, JNIEnv* env) {
  MangoMemory mem = {lib->guest_mem, lib->guest_mem_size};
  for (;;) {
    /* Meritous: shorter quanta so PC/progress sampling sees mapgen; on quantum
     * expiry continue (same as a fresh 100M window after SVC). Fatal step-limit
     * only if a single quantum burns the full Meritous budget with no SVC. */
    uint32_t budget = g_meritous_progress_armed ? 2000000u : 100000000u;
    int rc = mango_interp_run(cpu, &mem, MANGO_JNI_STOP, budget);
    if (g_meritous_progress_armed) {
      uint32_t pc = cpu->r[MANGO_REG_PC];
      mango_pc_hist_sample(pc);
      mango_meritous_progress(pc);
      if ((g_pc_hist_steps & 0x3ffu) == 0u && g_aeabi_calls_total > 0) {
        static uint64_t s_last;
        if (g_aeabi_calls_total - s_last >= 500000u) {
          mango_dump_aeabi_top();
          s_last = g_aeabi_calls_total;
        }
      }
    }
    if (rc == 0) {
      if (g_meritous_progress_armed) {
        mango_dump_aeabi_top();
        mango_dump_pc_hist();
      }
      return 0;
    }
    if (rc == -3) {
      if (g_meritous_progress_armed) {
        /* Keep going — mapgen is long; sample already recorded. Cap wall via driver timeout. */
        continue;
      }
      fprintf(stderr, "mango: step-limit pc=0x%x lr=0x%x r0=%x r1=%x\n",
              cpu->r[MANGO_REG_PC], cpu->r[MANGO_REG_LR], cpu->r[0], cpu->r[1]);
      return -1;
    }
    if (rc != 1) {
      uint32_t pc = cpu->r[MANGO_REG_PC];
      uint32_t w = 0;
      if (pc + 4u <= mem.size) {
        w = mango_load_u32_guest(mem.bytes, pc);
      }
      MangoInsn ins;
      int dec;
      if (cpu->cpsr & MANGO_CPSR_T) {
        uint16_t hw = (uint16_t)(w & 0xFFFFu);
        uint16_t hw2 = (uint16_t)(w >> 16);
        if ((hw >> 11) >= 0x1Du) {
          dec = mango_decode_t32(hw, hw2, &ins);
        } else {
          dec = mango_decode_t16(hw, &ins);
        }
      } else {
        dec = mango_decode(w, &ins);
      }
      fprintf(stderr,
              "mango: interp stop pc=0x%x cpsr=0x%x word=0x%08x rc=%d decode=%d op=%d "
              "r0=%x r1=%x r2=%x r3=%x r4=%x r5=%x r6=%x r7=%x sp=%x lr=%x libbias=0x%x\n",
              pc, cpu->cpsr, w, rc, dec, dec == 0 ? (int)ins.op : -1, cpu->r[0], cpu->r[1],
              cpu->r[2], cpu->r[3], cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7],
              cpu->r[MANGO_REG_SP], cpu->r[MANGO_REG_LR], lib->load_bias);
      for (int i = 0; i < g_nlibs; i++) {
        fprintf(stderr, "mango: lib[%d] bias=0x%x %s\n", i, g_libs[i]->load_bias, g_libs[i]->path);
      }
      if (g_meritous_progress_armed) {
        mango_dump_aeabi_top();
        mango_dump_pc_hist();
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
      if (g_meritous_progress_armed) {
        /* LR is the guest caller (mapgen / DungeonPlay), more useful than thunk PC. */
        mango_meritous_progress(cpu->r[MANGO_REG_LR]);
        mango_pc_hist_sample(cpu->r[MANGO_REG_LR]);
      }
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
  int raw = mango_is_raw_aapcs(slot);
  const char* shorty = slot->shorty;
  if (raw) {
    shorty++; /* skip leading '*' */
  }
  int nargs = mango_parse_shorty(shorty, &ret, args, sizeof(args));
  if (nargs < 0) {
    return 0;
  }

  uint32_t regs[4];
  uint32_t stack[16];
  uint32_t nstack = 0;
  uint32_t nreg = 2; /* r0 env/vm, r1 thiz/reserved — overridden for raw */
  int onload = mango_is_jni_onload(slot);
  if (raw) {
    /* Raw AAPCS: fixed trampoline arg is r0; remaining args from va_list. */
    nreg = 0;
    if (nargs > 0) {
      char t0 = args[0];
      if (t0 == 'J' || t0 == 'D') {
        return 0; /* wide first arg cannot arrive in the fixed slot */
      }
      regs[nreg++] = (uint32_t)(uintptr_t)env;
      memmove(args, args + 1, (size_t)(nargs - 1) + 1u);
      nargs -= 1;
    }
  } else if (onload) {
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
      void* p = va_arg(ap, void*);
      /* Raw AAPCS: pass the pointer bits (guest addr). JNI: intern a handle. */
      lo = raw ? (uint32_t)(uintptr_t)p : mango_handle_intern(p);
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

/*
 * OFDP libunity: MemoryManager is not constructed under the shim, so the
 * internal allocator at VA 0x102b78 returns NULL and callers memcpy into
 * address 0 (clobbering the ELF header with strings like DIRECTIONAL_COOKIE).
 * That makes NULL object field loads at [0+8] yield ASCII "NAL_" and OOB.
 *
 * Also the keyword/shader tree header at BSS 0x12d3d80 needs an empty-tree
 * sentinel so nativeRender's first insert does not pass a NULL node pointer.
 *
 * After constructors, MemoryManager at BSS 0x12c1630 has list1 empty but list2
 * head/sentinel at +0x30/+0x34 left 0. Contains() then walks address 0 as a
 * node; [0+4] holds a label pointer into "ALLOC_TEMP_THREAD", whose +4 word
 * is ASCII "C_TE" and OOB-loads as a next pointer.
 *
 * nativeRender also probes a Hash128 interval tree via BSS object 0x12d7fa0.
 * Without a tree, Contains() treats address 0 as the root, loads a bogus head
 * from [8] (corrupted ELF header / prior NULL stores), and LDRDs OOB at node
 * 0x4000000. Seed an empty tree at obj+0x40, redirect ctor VA 0x87fa84's Unity
 * alloc onto that BSS tree (so method pointers still install), and disable
 * reset VA 0x87fb30 which would free/clear obj+4.
 *
 * nativeRender label lookup at VA 0x104ce8 can legitimately return NULL (two
 * sibling call sites null-check; the site at VA 0x103f54 does not). A NULL
 * result then does ldr r1,[r0] / ldr r2,[r1,#0x1c] / blx r2, so guest[0]
 * (0xffffff from earlier NULL stores) is treated as a vtable and slot +0x1c
 * (0x82ec07ee, a VFP word mid-image) becomes the branch target — PC far above
 * the 40 MiB guest AS. Redirect that site through a trampoline that skips the
 * virtual call when the lookup returns NULL (same as the sibling sites).
 *
 * Vector grow uses sibling realloc at VA 0x103a64 (r0=old, r1=bytes). With no
 * MemoryManager that path fails open and leaves dynamic_array data pointing at
 * a shader label string ("_Object2World") while size holds a near-pointer
 * (strlen into the same pool) — quicksort then LDR-reg-offsets OOB (ASCII
 * "Worl"). Route that realloc onto guest realloc like the malloc hook above.
 *
 * A second typed allocator at VA 0x102c48 (same prologue; call sites pass size
 * in r0, e.g. nativeRender VA 0x780b24/0x780b68) also returned NULL. That left
 * BSS object 0x12d3ee0 +8 as 0, so the helper at VA 0x7810fc took its empty-path
 * count++ on every call while the caller looped on that same count — interpreter
 * step-limit then looked like a failing LDMIA POP at VA 0x781280. Hook 0x102c48
 * to guest malloc as well.
 */


/* SDL 1.2 software surface large enough for Meritous title blit (640×480).
 * Used when we host-hook IMG_Load so a failed/partial decode cannot hand the
 * game a NULL surface (NULL→pixels load at addr 0x14 reads ELF magic and the
 * fill loop strb's through libsdl .text). */
static uint32_t mango_guest_sdl_surface(MangoLoadedLibrary* lib, uint32_t w, uint32_t h) {
  uint32_t fmt, surf, pixels, pitch, nbytes;
  if (w == 0 || h == 0 || w > 4096u || h > 4096u) {
    return 0;
  }
  pitch = w * 4u;
  nbytes = pitch * h;
  fmt = mango_guest_alloc(lib, 64u);
  surf = mango_guest_alloc(lib, 64u);
  pixels = mango_guest_alloc(lib, nbytes);
  if (!fmt || !surf || !pixels) {
    return 0;
  }
  /* SDL_PixelFormat: 32bpp RGBA8888-ish */
  mango_store_u32_guest(lib->guest_mem, fmt + 0u, 0u); /* palette */
  lib->guest_mem[fmt + 4u] = 32; /* BitsPerPixel */
  lib->guest_mem[fmt + 5u] = 4;  /* BytesPerPixel */
  mango_store_u32_guest(lib->guest_mem, fmt + 16u, 0x000000ffu); /* Rmask */
  mango_store_u32_guest(lib->guest_mem, fmt + 20u, 0x0000ff00u);
  mango_store_u32_guest(lib->guest_mem, fmt + 24u, 0x00ff0000u);
  mango_store_u32_guest(lib->guest_mem, fmt + 28u, 0xff000000u);
  /* SDL_Surface */
  mango_store_u32_guest(lib->guest_mem, surf + 0u, 0u); /* flags */
  mango_store_u32_guest(lib->guest_mem, surf + 4u, fmt);
  mango_store_u32_guest(lib->guest_mem, surf + 8u, w);
  mango_store_u32_guest(lib->guest_mem, surf + 12u, h);
  lib->guest_mem[surf + 16u] = (uint8_t)(pitch & 0xffu);
  lib->guest_mem[surf + 17u] = (uint8_t)((pitch >> 8) & 0xffu);
  mango_store_u32_guest(lib->guest_mem, surf + 20u, pixels);
  /* clip_rect at +32: x=0,y=0,w,h as Sint16/Uint16 */
  lib->guest_mem[surf + 32u] = 0;
  lib->guest_mem[surf + 33u] = 0;
  lib->guest_mem[surf + 34u] = 0;
  lib->guest_mem[surf + 35u] = 0;
  lib->guest_mem[surf + 36u] = (uint8_t)(w & 0xffu);
  lib->guest_mem[surf + 37u] = (uint8_t)((w >> 8) & 0xffu);
  lib->guest_mem[surf + 38u] = (uint8_t)(h & 0xffu);
  lib->guest_mem[surf + 39u] = (uint8_t)((h >> 8) & 0xffu);
  mango_store_u32_guest(lib->guest_mem, surf + 56u, 1u); /* refcount */
  return surf;
}

static int mango_is_meritous_sdl_image(const MangoLoadedLibrary* lib) {
  const char* base;
  if (lib == NULL || lib->path[0] == '\0') {
    return 0;
  }
  base = strrchr(lib->path, '/');
  base = base ? base + 1 : lib->path;
  return strcmp(base, "libsdl_image.so") == 0;
}


static int mango_is_meritous_app(const MangoLoadedLibrary* lib) {
  const char* base;
  if (lib == NULL || lib->path[0] == '\0') {
    return 0;
  }
  base = strrchr(lib->path, '/');
  base = base ? base + 1 : lib->path;
  return strcmp(base, "libapplication.so") == 0;
}

/* Title "plasma" fill at VA 0x69ad6 runs ~307k host-sqrt iterations under the
 * interpreter (minutes). Skip to the post-loop epilogue so Meritous can advance;
 * the buffer from malloc remains zeroed from guest calloc/malloc. */
static void mango_patch_meritous_skip_plasma(MangoLoadedLibrary* lib) {
  uint32_t addr;
  if (!mango_is_meritous_app(lib)) {
    return;
  }
  addr = lib->load_bias + 0x1caceu;
  if (!mango_guest_range_ok(lib, addr, 2u)) {
    return;
  }
  /* Thumb unconditional B to 0x69b0c: imm = (0x69b0c - (0x69ace+4))/2 = 0x1d */
  lib->guest_mem[addr] = 0x1du;
  lib->guest_mem[addr + 1u] = 0xe0u;
  /* Title PNG post-process loop VA 0x69d06 (~307k iters) → skip to 0x69d2e. */
  addr = lib->load_bias + 0x1cd06u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    /* imm=(0x69d2e-(0x69d06+4))/2=0x12 */
    lib->guest_mem[addr] = 0x12u;
    lib->guest_mem[addr + 1u] = 0xe0u;
  }

  /* wait-for-key at ELF VA 0x1656c: force immediate return 1 */
  addr = lib->load_bias + 0x1656cu;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    lib->guest_mem[addr] = 0x01u;
    lib->guest_mem[addr + 1u] = 0x20u; /* movs r0, #1 */
    lib->guest_mem[addr + 2u] = 0x70u;
    lib->guest_mem[addr + 3u] = 0x47u; /* bx lr */
  }
  /* custom Delay at ELF 0x161c4: return immediately */
  addr = lib->load_bias + 0x161c4u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    lib->guest_mem[addr] = 0x00u;
    lib->guest_mem[addr + 1u] = 0x20u; /* movs r0, #0 */
    lib->guest_mem[addr + 2u] = 0x70u;
    lib->guest_mem[addr + 3u] = 0x47u; /* bx lr */
  }

  /* HandleEvents @ VA 0x166d0: set enter_pressed, voluntary_exit, key_held[SPACE].
   * Game loop exits on (voluntary_exit && enter_pressed). */
  addr = lib->load_bias + 0x166d0u;
  if (mango_guest_range_ok(lib, addr, 32u)) {
    uint32_t enter = lib->load_bias + 0x401c0u;
    uint32_t vol = lib->load_bias + 0x32730u; /* voluntary_exit */
    uint32_t ksp = lib->load_bias + 0x326fcu + 16u;
    /* ldr r0,[pc,#16]; movs r1,#1; str r1,[r0];
     * ldr r0,[pc,#16]; str r1,[r0];
     * ldr r0,[pc,#16]; str r1,[r0]; bx lr;
     * .word enter; .word vol; .word ksp */
    lib->guest_mem[addr + 0u] = 0x03u;
    lib->guest_mem[addr + 1u] = 0x48u; /* ldr r0, [pc, #12] enter @+16 */
    lib->guest_mem[addr + 2u] = 0x01u;
    lib->guest_mem[addr + 3u] = 0x21u; /* movs r1, #1 */
    lib->guest_mem[addr + 4u] = 0x01u;
    lib->guest_mem[addr + 5u] = 0x60u; /* str r1, [r0] */
    lib->guest_mem[addr + 6u] = 0x03u;
    lib->guest_mem[addr + 7u] = 0x48u; /* ldr r0, [pc, #12] vol @+20 */
    lib->guest_mem[addr + 8u] = 0x01u;
    lib->guest_mem[addr + 9u] = 0x60u;
    lib->guest_mem[addr + 10u] = 0x03u;
    lib->guest_mem[addr + 11u] = 0x48u; /* ldr r0, [pc, #12] ksp @+24 */
    lib->guest_mem[addr + 12u] = 0x01u;
    lib->guest_mem[addr + 13u] = 0x60u;
    lib->guest_mem[addr + 14u] = 0x70u;
    lib->guest_mem[addr + 15u] = 0x47u; /* bx lr */
    mango_store_u32_guest(lib->guest_mem, addr + 16u, enter);
    mango_store_u32_guest(lib->guest_mem, addr + 20u, vol);
    mango_store_u32_guest(lib->guest_mem, addr + 24u, ksp);
    fprintf(stderr, "mango: Meritous HandleEvents -> enter/voluntary/SPACE\n");
  }

  /* SetTitlePalette2 @ ELF VA 0x173c8 (Thumb): skip float palette churn. */
  addr = lib->load_bias + 0x173c8u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    lib->guest_mem[addr + 0u] = 0x70u;
    lib->guest_mem[addr + 1u] = 0x47u; /* bx lr */
  }
  /* VideoUpdate @ ELF VA 0x16118: skip SDL_UpdateRect path. */
  addr = lib->load_bias + 0x16118u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    lib->guest_mem[addr + 0u] = 0x70u;
    lib->guest_mem[addr + 1u] = 0x47u; /* bx lr */
  }
  /* draw_text @ ELF VA 0x17f4c: title string blit is slow under interp. */
  addr = lib->load_bias + 0x17f4cu;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    lib->guest_mem[addr + 0u] = 0x70u;
    lib->guest_mem[addr + 1u] = 0x47u; /* bx lr */
  }

  /* Gameplay draw path: tile walk + DrawCircle sqrt dominate after mapgen. */
  {
    /* DrawLevel left live: one tile-walk frame is cheap with host UpperBlit
     * no-op; proves a real gameplay draw path. Keep circle/entity/arc stubs. */
    static const uint32_t kBx[] = {
        0x11db4u, /* DrawEntities */
        0x16a88u, /* DrawPlayer */
        0x18a90u, /* DrawCircle */
        0x18ba4u, /* DrawCircleEx */
        0x18cb8u, /* DrawShield */
        0x18878u, /* DrawCircuit */
        0x1a540u, /* DrawArtifacts */
        0x16c70u, /* SetTonedPalette */
        0x1a710u, /* Arc */
    };
    for (unsigned i = 0; i < sizeof(kBx) / sizeof(kBx[0]); i++) {
      addr = lib->load_bias + kBx[i];
      if (mango_guest_range_ok(lib, addr, 2u)) {
        lib->guest_mem[addr + 0u] = 0x70u;
        lib->guest_mem[addr + 1u] = 0x47u; /* bx lr */
      }
    }
    fprintf(stderr, "mango: Meritous stub gameplay draw helpers\n");
  }

  /* Title loop HEAD: first visit -> New Game; after one DungeonPlay the progress
   * hook rewrites this site to `bl exit` so SDL_main returns cleanly. */
  addr = lib->load_bias + 0x1ccd8u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x55u;
    lib->guest_mem[addr + 1u] = 0xe1u; /* b 0x1cf86 New Game */
    fprintf(stderr, "mango: Meritous title head -> force New Game\n");
  }

  /* DungeonPlay left intact — title head branches to New Game at 0x1cf86. */

  /* Generate() → 1000 rooms (boss index 999 exists). InitEnemies for all
   * 999 non-start rooms is the wall under interp (enemy place loops); keep
   * map topology large but only spawn into the first 256 rooms. Dist gate
   * >20 (vanilla >49) so 1k-room maps pass without ResetLevel storms. */
  addr = lib->load_bias + 0x1e61cu;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 1000u);
    fprintf(stderr, "mango: Meritous Generate room target 3000->1000\n");
  }
  addr = lib->load_bias + 0x1e5c2u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x14u;
    lib->guest_mem[addr + 1u] = 0x29u; /* cmp r1, #0x14 */
    fprintf(stderr, "mango: Meritous Generate dist gate >49 -> >20\n");
  }
  addr = lib->load_bias + 0x135c0u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 256u * 0x34u);
    fprintf(stderr, "mango: Meritous InitEnemies room loop ->256 (of 1000)\n");
  }

  g_meritous_progress_armed = 1;
  g_meritous_bias = lib->load_bias;
  g_meritous_seen_mask = 0;
  setvbuf(stderr, NULL, _IONBF, 0);

  fprintf(stderr, "mango: skip Meritous title fill loops\n");
}

static void mango_patch_meritous_img_load(MangoLoadedLibrary* lib) {
  uint32_t sym;
  if (!mango_is_meritous_sdl_image(lib)) {
    return;
  }
  sym = mango_elf32_find_symbol(&lib->image, "IMG_Load");
  if (sym == 0) {
    return;
  }
  sym += lib->load_bias;
  /* Thumb entry: clear bit0 for patch address. */
  if (sym & 1u) {
    sym &= ~1u;
  }
  if (!mango_guest_range_ok(lib, sym, MANGO_JNI_THUNK_SIZE)) {
    return;
  }
  /* A32 SVC thunk (IMG_Load is Thumb but BX to ARM SVC is fine via blx).
   * Callers use blx to PLT → ARM or Thumb; writing ARM SVC at the symbol
   * requires the symbol to be called as ARM. Safer: write Thumb stub:
   *   svc #0 ; bx lr  is awkward in Thumb. Use ARM thunk and rely on blx.
   * Meritous PLT uses ARM stubs that ldr pc from GOT — GOT holds symbol|1
   * for Thumb. Overwriting Thumb entry with ARM opcodes would fault.
   * Write Thumb:  svc 0; bx lr  via UDF/svc — Thumb SVC is `svc #imm` 11011111.
   * Simplest portable approach: point GOT (done via resolving) — instead
   * overwrite with Thumb branch to a nearby ARM thunk in libc area.
   * Easiest: replace first instructions with Thumb `bx pc; nop` then ARM SVC. */
  /* bx pc ; nop  (Thumb) then ARM svc thunk at sym+4 */
  mango_store_u32_guest(lib->guest_mem, sym, 0x46c04778u); /* bx pc; nop */
  mango_write_jni_thunk(lib->guest_mem, sym + 4u, MANGO_LIBC_SVC_BASE + MANGO_LIBC_IMG_LOAD);
}

static int mango_is_ofdp_mono(const MangoLoadedLibrary* lib) {
  const char* base;
  if (lib == NULL || lib->path[0] == '\0') {
    return 0;
  }
  base = strrchr(lib->path, '/');
  base = base ? base + 1 : lib->path;
  return strcmp(base, "libmono.so") == 0;
}

/* Boehm GC in OFDP libmono: GC_page_size BSS at ELF VA 0x3b7d90. Primary fix
 * is resolving bionic __page_size to a real 4096 word (see mango_resolve_import);
 * re-seed after ctors in case an early probe still ran before that reloc. */
static void mango_seed_ofdp_mono_gc(MangoLoadedLibrary* lib) {
  uint32_t addr;
  if (!mango_is_ofdp_mono(lib)) {
    return;
  }
  addr = lib->load_bias + 0x3b7d90u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 4096u);
  }
}

static int mango_is_ofdp_unity(const MangoLoadedLibrary* lib) {
  const char* base;
  if (lib == NULL || lib->path[0] == '\0') {
    return 0;
  }
  base = strrchr(lib->path, '/');
  base = base ? base + 1 : lib->path;
  return strcmp(base, "libunity.so") == 0;
}

static void mango_patch_ofdp_unity(MangoLoadedLibrary* lib) {
  uint32_t alloc_fn, alloc2_fn, realloc_fn, hdr, sen;
  if (!mango_is_ofdp_unity(lib)) {
    return;
  }
  alloc_fn = lib->load_bias + 0x102b78u;
  if (mango_guest_range_ok(lib, alloc_fn, MANGO_JNI_THUNK_SIZE) &&
      mango_load_u32_guest(lib->guest_mem, alloc_fn) == 0xe92d4bf0u) {
    /* r0 is the byte size at every call site we inspected; route to guest malloc. */
    mango_write_jni_thunk(lib->guest_mem, alloc_fn, MANGO_LIBC_SVC_BASE + MANGO_LIBC_MALLOC);
  }
  /* Sibling typed alloc VA 0x102c48: r0=bytes at OFDP sites (labels in r1+). */
  alloc2_fn = lib->load_bias + 0x102c48u;
  if (mango_guest_range_ok(lib, alloc2_fn, MANGO_JNI_THUNK_SIZE) &&
      mango_load_u32_guest(lib->guest_mem, alloc2_fn) == 0xe92d4bf0u) {
    mango_write_jni_thunk(lib->guest_mem, alloc2_fn, MANGO_LIBC_SVC_BASE + MANGO_LIBC_MALLOC);
  }
  /* Sibling realloc VA 0x103a64: r0=old, r1=bytes (vector grow when capacity>=0). */
  realloc_fn = lib->load_bias + 0x103a64u;
  if (mango_guest_range_ok(lib, realloc_fn, MANGO_JNI_THUNK_SIZE) &&
      mango_load_u32_guest(lib->guest_mem, realloc_fn) == 0xe92d4bf0u) {
    mango_write_jni_thunk(lib->guest_mem, realloc_fn, MANGO_LIBC_SVC_BASE + MANGO_LIBC_REALLOC);
  }

  hdr = lib->load_bias + 0x12d3d80u;
  if (!mango_guest_range_ok(lib, hdr, 0x40u)) {
    return;
  }
  /* Empty intrusive tree: head -> embedded sentinel; sentinel+0xc == sentinel+4. */
  sen = hdr + 0x2cu;
  mango_store_u32_guest(lib->guest_mem, hdr + 0x20u, sen);
  mango_store_u32_guest(lib->guest_mem, sen + 0x8u, 0u);
  mango_store_u32_guest(lib->guest_mem, sen + 0xcu, sen + 0x4u);
}

static void mango_seed_ofdp_memory_manager(MangoLoadedLibrary* lib) {
  uint32_t mm, sen;
  if (!mango_is_ofdp_unity(lib)) {
    return;
  }
  mm = lib->load_bias + 0x12c1630u;
  if (!mango_guest_range_ok(lib, mm, 0x50u)) {
    return;
  }
  /* Match list1 empty shape (head == sentinel == &embedded). Ctors leave list2
   * at +0x30/+0x34 as 0, which Contains() treats as a node at address 0. */
  if (mango_load_u32_guest(lib->guest_mem, mm + 0x34u) == 0u) {
    sen = mm + 0x30u;
    mango_store_u32_guest(lib->guest_mem, mm + 0x30u, sen);
    mango_store_u32_guest(lib->guest_mem, mm + 0x34u, sen);
  }
}

static void mango_seed_ofdp_hash_tree(MangoLoadedLibrary* lib) {
  uint32_t obj, tree, sen, alloc_site, dtor;
  if (!mango_is_ofdp_unity(lib)) {
    return;
  }
  obj = lib->load_bias + 0x12d7fa0u;
  /* Carve empty tree from unused BSS just past the object (obj ends ~+0x40). */
  tree = obj + 0x40u;
  if (!mango_guest_range_ok(lib, obj, 0x58u)) {
    return;
  }
  /* Pre-shape empty tree; ctor will re-init the same bytes after we redirect its
   * allocator to return this address. */
  sen = tree + 4u;
  mango_store_u32_guest(lib->guest_mem, tree + 0x8u, 0u);
  mango_store_u32_guest(lib->guest_mem, tree + 0xcu, sen);
  mango_store_u32_guest(lib->guest_mem, tree + 0x10u, sen);
  mango_store_u32_guest(lib->guest_mem, tree + 0x14u, 0u);
  mango_store_u32_guest(lib->guest_mem, obj + 4u, tree);

  /* Ctor VA 0x87fa84 calls Unity alloc at 0x4c0c48 (often NULL under the shim).
   * Replace mov r0,#0x18; mov r1,#6; mov r2,#0x10; bl alloc with
   * movw/movt/add that set r0 = r4 + 0x168a14 (BSS tree relative to the same
   * base the ctor already computed into r4). Method pointers still install. */
  alloc_site = lib->load_bias + 0x4c1aa4u;
  if (mango_guest_range_ok(lib, alloc_site, 16u) &&
      mango_load_u32_guest(lib->guest_mem, alloc_site) == 0xe3a00018u &&
      mango_load_u32_guest(lib->guest_mem, alloc_site + 4u) == 0xe3a01006u) {
    mango_store_u32_guest(lib->guest_mem, alloc_site + 0u, 0xe3080a14u); /* movw r0, #0x8a14 */
    mango_store_u32_guest(lib->guest_mem, alloc_site + 4u, 0xe3400016u); /* movt r0, #0x16 */
    mango_store_u32_guest(lib->guest_mem, alloc_site + 8u, 0xe0840000u); /* add r0, r4, r0 */
    mango_store_u32_guest(lib->guest_mem, alloc_site + 12u, 0xe1a00000u); /* nop */
  }

  /* Reset VA 0x87fb30 frees/clears obj+4 — disable it. */
  dtor = lib->load_bias + 0x4c1b30u;
  if (mango_guest_range_ok(lib, dtor, 4u) &&
      mango_load_u32_guest(lib->guest_mem, dtor) == 0xe92d4830u) {
    mango_store_u32_guest(lib->guest_mem, dtor, 0xe12fff1eu); /* bx lr */
  }
}

static uint32_t mango_arm_b(uint32_t from, uint32_t to, uint32_t cond) {
  int32_t imm24 = (int32_t)(to - from - 8u) / 4;
  return (cond << 28) | 0x0a000000u | ((uint32_t)imm24 & 0x00ffffffu);
}

static void mango_patch_ofdp_label_lookup_null(MangoLoadedLibrary* lib) {
  uint32_t site, cont, tramp;
  if (!mango_is_ofdp_unity(lib)) {
    return;
  }
  /* VA 0x103f54: ldr r1,[r0]; ldr r2,[r1,#0x1c]; mov r1,r5; blx r2 */
  site = lib->load_bias + 0x103f54u;
  cont = lib->load_bias + 0x103f70u; /* mov r0, r8 — after cmp/movlo */
  /* Unused BSS just past the Hash128 tree carve (obj 0x12d7fa0 + 0x58). */
  tramp = lib->load_bias + 0x12d8020u;
  if (!mango_guest_range_ok(lib, site, 16u) || !mango_guest_range_ok(lib, tramp, 40u)) {
    return;
  }
  if (mango_load_u32_guest(lib->guest_mem, site) != 0xe5901000u ||
      mango_load_u32_guest(lib->guest_mem, site + 4u) != 0xe591201cu) {
    return;
  }
  /* trampoline: null-check r0 then same vtable call + min into r4; else leave r4 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x00u, 0xe3500000u); /* cmp r0, #0 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x04u, mango_arm_b(tramp + 0x04u, cont, 0x0u)); /* beq cont */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x08u, 0xe5901000u); /* ldr r1, [r0] */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x0cu, 0xe591201cu); /* ldr r2, [r1, #0x1c] */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x10u, 0xe1a01005u); /* mov r1, r5 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x14u, 0xe12fff32u); /* blx r2 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x18u, 0xe1500004u); /* cmp r0, r4 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x1cu, 0x31a04000u); /* movlo r4, r0 */
  mango_store_u32_guest(lib->guest_mem, tramp + 0x20u, mango_arm_b(tramp + 0x20u, cont, 0xeu)); /* b cont */
  mango_store_u32_guest(lib->guest_mem, site, mango_arm_b(site, tramp, 0xeu)); /* b tramp */
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
  mango_patch_ofdp_unity(lib);
  mango_patch_meritous_img_load(lib);
  mango_patch_meritous_skip_plasma(lib);
  mango_seed_ofdp_mono_gc(lib);
  mango_run_constructors(lib);
  mango_seed_ofdp_mono_gc(lib);
  mango_seed_ofdp_memory_manager(lib);
  mango_seed_ofdp_hash_tree(lib);
  mango_patch_ofdp_label_lookup_null(lib);
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
