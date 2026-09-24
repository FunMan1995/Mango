/* Must come before any system header, see mango/native_bridge.h for why. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
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
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
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
#define MANGO_JNI_CALL_FLOAT_METHOD 55
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
#define MANGO_JNI_GET_ARRAY_LENGTH 171
#define MANGO_JNI_NEW_INT_ARRAY 179
#define MANGO_JNI_GET_INT_ARRAY_ELEMENTS 187
#define MANGO_JNI_RELEASE_INT_ARRAY_ELEMENTS 195
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
#define MANGO_LIBC_SDL_SET_VIDEO_MODE 141
#define MANGO_LIBC_SDL_SET_COLOR_KEY 142
#define MANGO_LIBC_DRAW_TEXT 143
#define MANGO_LIBC_GETENV 144
#define MANGO_LIBC_STAT 145
#define MANGO_LIBC_LSTAT 146
#define MANGO_LIBC_ACCESS 147
#define MANGO_LIBC_AASSET_MGR_FROM_JAVA 148
#define MANGO_LIBC_AASSET_MGR_OPEN 149
#define MANGO_LIBC_AASSET_MGR_OPENDIR 150
#define MANGO_LIBC_AASSET_DIR_GET_NEXT 151
#define MANGO_LIBC_AASSET_DIR_CLOSE 152
#define MANGO_LIBC_AASSET_GET_LENGTH 153
#define MANGO_LIBC_AASSET_READ 154
#define MANGO_LIBC_AASSET_CLOSE 155
#define MANGO_LIBC_AASSET_OPEN_FD 156
#define MANGO_LIBC_SL_CREATE_ENGINE 157
#define MANGO_LIBC_COUNT 158
#define MANGO_TSD_KEYS 16

/* Soft OpenSLES vtable methods (heap thunks; not PLT-imported by name). */
#define MANGO_SL_SVC_BASE 0x3000u
#define MANGO_SL_GET_INTERFACE 0
#define MANGO_SL_CREATE_OUTPUT_MIX 1
#define MANGO_SL_CREATE_AUDIO_PLAYER 2
#define MANGO_SL_COUNT 3

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
  uint32_t ctype_addr;       /* pointer cell → bionic _ctype_ table */
  uint32_t tolower_tab_addr; /* pointer cell → _tolower_tab_ */
  uint32_t toupper_tab_addr; /* pointer cell → _toupper_tab_ */
  uint32_t environ_addr;     /* pointer cell → char** environ (NULL-terminated) */
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
    "SDL_SetVideoMode",
    "SDL_SetColorKey",
    "draw_text",
    "getenv",
    "stat",
    "lstat",
    "access",
    "AAssetManager_fromJava",
    "AAssetManager_open",
    "AAssetManager_openDir",
    "AAssetDir_getNextFileName",
    "AAssetDir_close",
    "AAsset_getLength",
    "AAsset_read",
    "AAsset_close",
    "AAsset_openFileDescriptor",
    "slCreateEngine",
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
static uint32_t g_fb_surf;
static MangoLoadedLibrary* g_fb_lib;
static uint64_t g_blit_count;
static uint64_t g_blit_bytes;
static uint64_t g_fill_count;
static int g_frame_dumped;
static uint64_t g_ckey_blit_count;
static uint64_t g_ckey_pixels_skipped;
static uint64_t g_lrand48_state = 0x1234abcd330eull; /* POSIX drand48 seed */
/* Soft-float host-stub call histogram (Meritous mapgen / title plasma). */
static uint64_t g_aeabi_calls[MANGO_LIBC_COUNT];
static uint64_t g_aeabi_calls_total;
static uint64_t g_pc_hist_steps;
enum { MANGO_PC_HIST_BUCKETS = 4096 };
static uint32_t g_pc_hist[MANGO_PC_HIST_BUCKETS];
static int g_meritous_progress_armed;
static uint32_t g_meritous_bias;
static MangoLoadedLibrary* g_meritous_lib;
static uint32_t g_meritous_seen_mask; /* bit0 DungeonPlay, bit1 Generate, bit2 RandomGenerateMap, bit3 DestroyDungeon/mapIO, bit4 title→exit rewritten, bit5 title→exit armed */
static uint8_t g_meritous_font[16384]; /* 256 glyphs × 64B rearrange; was 8192 and clobbered progress statics */
static int g_meritous_font_ready;
static uint32_t g_meritous_draw_text_calls;

/* Soft OpenSLES (Pacman Q2 research/49): guest objects + IID cells so
 * slCreateEngine writes a live SLObjectItf instead of leaving engineObj=0. */
static int g_sl_ready;
static uint32_t g_sl_success_stub;   /* mov r0,#0; bx lr */
static uint32_t g_sl_destroy_stub;   /* bx lr (void Destroy) */
static uint32_t g_sl_obj_vtable;
static uint32_t g_sl_engine_vtable;
static uint32_t g_sl_play_vtable;
static uint32_t g_sl_seek_vtable;
static uint32_t g_sl_bq_vtable;
static uint32_t g_sl_volume_vtable;
static uint32_t g_sl_effect_vtable;
static uint32_t g_sl_iid_engine;       /* UUID struct addr (GetInterface arg) */
static uint32_t g_sl_iid_play;
static uint32_t g_sl_iid_seek;
static uint32_t g_sl_iid_volume;
static uint32_t g_sl_iid_bufferqueue;
static uint32_t g_sl_iid_effectsend;
static uint32_t g_sl_iid_engine_cell;  /* import: pointer cell → UUID */
static uint32_t g_sl_iid_play_cell;
static uint32_t g_sl_iid_seek_cell;
static uint32_t g_sl_iid_volume_cell;
static uint32_t g_sl_iid_bufferqueue_cell;
static uint32_t g_sl_iid_effectsend_cell;

static void mango_store_u32_guest(uint8_t* mem, uint32_t addr, uint32_t v);
static uint32_t mango_load_u32_guest(const uint8_t* mem, uint32_t addr);
static int mango_guest_range_ok(const MangoLoadedLibrary* lib, uint32_t addr, uint32_t n);
static uint32_t mango_handle_intern(void* p);
static uint32_t mango_guest_strdup(MangoLoadedLibrary* lib, const char* s);
static void* mango_handle_lookup(uint32_t id);
static const char* mango_fake_jstring_chars(MangoLoadedLibrary* lib, void* js);
static int mango_host_resolve_path(MangoLoadedLibrary* lib, const char* path, char* out,
                                   size_t outsz);
#ifndef ANDROID
static int mango_png_decode(const char* host_path, uint8_t** out_pixels, uint32_t* out_w,
                            uint32_t* out_h, uint32_t* out_bpp);
#endif

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
  /* Ranges from nm -D on libapplication.so (Thumb).
   * Title→exit must NOT depend solely on flaky DungeonPlay PC/LR samples
   * (research/41). Track mapgen (Generate / RandomGenerateMap); arm bit32 on
   * DestroyDungeon after any progress, title-head re-entry, or frame dump
   * (see mango_dump_framebuffer). Do not rewrite at first mapgen sample —
   * that races the still-running New Game and exits prematurely. */
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
    /* DestroyDungeon after any dungeon/mapgen progress ⇒ arm title→exit.
     * Previously required bit0 DungeonPlay only — that sample is flaky. */
    if ((g_meritous_seen_mask & 7u) && !(g_meritous_seen_mask & 32u)) {
      g_meritous_seen_mask |= 32u;
      fprintf(stderr, "mango: Meritous post-dungeon DestroyDungeon\n");
    }
  }
  /* Re-enter title menu after first dungeon/mapgen progress → arm exit. */
  if (va == 0x1ccd8u && (g_meritous_seen_mask & 7u) && !(g_meritous_seen_mask & 32u)) {
    g_meritous_seen_mask |= 32u;
  }
  /* Once armed, rewrite title head to exit (first visit already forced New Game). */
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
        MangoLoadedLibrary* tlib = g_meritous_lib ? g_meritous_lib : g_libs[0];
        if (thunk && tlib && mango_guest_range_ok(tlib, th, 12u)) {
          /* movs r0,#0; ldr r1,[pc,#4]; bx r1; nop; .word thunk */
          tlib->guest_mem[th + 0u] = 0x00u;
          tlib->guest_mem[th + 1u] = 0x20u; /* movs r0,#0 */
          tlib->guest_mem[th + 2u] = 0x01u;
          tlib->guest_mem[th + 3u] = 0x49u; /* ldr r1,[pc,#4] */
          tlib->guest_mem[th + 4u] = 0x08u;
          tlib->guest_mem[th + 5u] = 0x47u; /* bx r1 */
          tlib->guest_mem[th + 6u] = 0x00u;
          tlib->guest_mem[th + 7u] = 0xbfu; /* nop */
          mango_store_u32_guest(tlib->guest_mem, th + 8u, thunk);
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

/* Android AAsset* backed by $MANGO_ASSET_ROOT (Pacman Q0 research/47). Guest
 * handles are small table ids; getNextFileName strings live in guest heap. */
#define MANGO_AASSET_MAX 64
#define MANGO_AASSET_DIR_MAX 16
#define MANGO_AASSET_MGR_HANDLE 1u

typedef struct {
  int used;
  FILE* fp;
  long length;
  long pos;
  char host_path[768];
} MangoAAsset;

typedef struct {
  int used;
  DIR* dir;
  char host_dir[768];
  uint32_t guest_name;
} MangoAAssetDir;

static MangoAAsset g_aassets[MANGO_AASSET_MAX];
static MangoAAssetDir g_aasset_dirs[MANGO_AASSET_DIR_MAX];

static int mango_aasset_host_path(MangoLoadedLibrary* lib, const char* rel, char* out,
                                  size_t outsz) {
  const char* root;
  (void)lib;
  if (!rel || !out || outsz == 0) {
    return 0;
  }
  while (rel[0] == '.' && rel[1] == '/') {
    rel += 2;
  }
  if (rel[0] == '/' && access(rel, F_OK) == 0) {
    snprintf(out, outsz, "%s", rel);
    return 1;
  }
  root = getenv("MANGO_ASSET_ROOT");
  if (root && root[0]) {
    if (rel[0] == '\0') {
      snprintf(out, outsz, "%s", root);
    } else {
      snprintf(out, outsz, "%s/%s", root, rel);
    }
    if (access(out, F_OK) == 0) {
      return 1;
    }
  }
  return 0;
}

static uint32_t mango_aasset_alloc(void) {
  for (uint32_t i = 1; i < MANGO_AASSET_MAX; i++) {
    if (!g_aassets[i].used) {
      memset(&g_aassets[i], 0, sizeof(g_aassets[i]));
      g_aassets[i].used = 1;
      return i;
    }
  }
  return 0;
}

static MangoAAsset* mango_aasset_get(uint32_t id) {
  if (id == 0 || id >= MANGO_AASSET_MAX || !g_aassets[id].used) {
    return NULL;
  }
  return &g_aassets[id];
}

static void mango_aasset_free(uint32_t id) {
  MangoAAsset* a = mango_aasset_get(id);
  if (!a) {
    return;
  }
  if (a->fp) {
    fclose(a->fp);
    a->fp = NULL;
  }
  a->used = 0;
}

static uint32_t mango_aasset_dir_alloc(void) {
  for (uint32_t i = 1; i < MANGO_AASSET_DIR_MAX; i++) {
    if (!g_aasset_dirs[i].used) {
      memset(&g_aasset_dirs[i], 0, sizeof(g_aasset_dirs[i]));
      g_aasset_dirs[i].used = 1;
      return i;
    }
  }
  return 0;
}

static MangoAAssetDir* mango_aasset_dir_get(uint32_t id) {
  if (id == 0 || id >= MANGO_AASSET_DIR_MAX || !g_aasset_dirs[id].used) {
    return NULL;
  }
  return &g_aasset_dirs[id];
}

static void mango_aasset_dir_free(uint32_t id) {
  MangoAAssetDir* d = mango_aasset_dir_get(id);
  if (!d) {
    return;
  }
  if (d->dir) {
    closedir(d->dir);
    d->dir = NULL;
  }
  d->guest_name = 0;
  d->used = 0;
}

/* Pacman Q1 (research/48): host PngManager + Bitmap + jintArray via JNI.
 * Art::loadPng calls open/getWidth/getHeight/getPixels/close on the
 * PngManager jobject passed to PacmanLib_init; soft Call* left levels[]
 * poisoned with float 0.25 bits (0x3e800000). */
#define MANGO_BITMAP_MAX 64
#define MANGO_JINTARRAY_MAX 64
#define MANGO_JNIMETHOD_MAX 128

typedef enum {
  MANGO_MID_UNKNOWN = 0,
  MANGO_MID_PNG_OPEN,
  MANGO_MID_PNG_GET_WIDTH,
  MANGO_MID_PNG_GET_HEIGHT,
  MANGO_MID_PNG_GET_PIXELS,
  MANGO_MID_PNG_CLOSE,
  /* Pacman Q3 StoreManager prefs (research/50) */
  MANGO_MID_STORE_LOAD_BOOL,
  MANGO_MID_STORE_SAVE_BOOL,
  MANGO_MID_STORE_LOAD_INT,
  MANGO_MID_STORE_SAVE_INT,
  MANGO_MID_STORE_LOAD_FLOAT,
  MANGO_MID_STORE_SAVE_FLOAT,
  MANGO_MID_STORE_LOAD_STRING,
  MANGO_MID_STORE_SAVE_STRING
} MangoMidKind;

typedef struct {
  int used;
  MangoMidKind kind;
  char name[64];
  char sig[96];
} MangoJniMethod;

typedef struct {
  int used;
  int width;
  int height;
  uint32_t* argb; /* host AARRGGBB, length width*height */
} MangoBitmap;

typedef struct {
  int used;
  int length;
  uint32_t guest_elems; /* guest jint[] backing store */
} MangoJIntArray;

static MangoJniMethod g_jni_methods[MANGO_JNIMETHOD_MAX];
static MangoBitmap g_bitmaps[MANGO_BITMAP_MAX];
static MangoJIntArray g_jintarrays[MANGO_JINTARRAY_MAX];
static MangoMidKind mango_classify_png_mid(const char* name, const char* sig) {
  if (!name) {
    return MANGO_MID_UNKNOWN;
  }
  if (strcmp(name, "open") == 0 && sig && strstr(sig, "Landroid/graphics/Bitmap;") != NULL &&
      strstr(sig, "String") != NULL) {
    return MANGO_MID_PNG_OPEN;
  }
  if (strcmp(name, "getWidth") == 0 && sig && strstr(sig, "Bitmap") != NULL) {
    return MANGO_MID_PNG_GET_WIDTH;
  }
  if (strcmp(name, "getHeight") == 0 && sig && strstr(sig, "Bitmap") != NULL) {
    return MANGO_MID_PNG_GET_HEIGHT;
  }
  if (strcmp(name, "getPixels") == 0 && sig && strstr(sig, "[I") != NULL) {
    return MANGO_MID_PNG_GET_PIXELS;
  }
  if (strcmp(name, "close") == 0 && sig && strstr(sig, "Bitmap") != NULL &&
      strstr(sig, ")V") != NULL) {
    return MANGO_MID_PNG_CLOSE;
  }
  /* StoreManager — honour defValue when key missing (Engine_saved → false). */
  if (strcmp(name, "loadBoolean") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")Z") != NULL) {
    return MANGO_MID_STORE_LOAD_BOOL;
  }
  if (strcmp(name, "saveBoolean") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")V") != NULL) {
    return MANGO_MID_STORE_SAVE_BOOL;
  }
  if (strcmp(name, "loadInt") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")I") != NULL) {
    return MANGO_MID_STORE_LOAD_INT;
  }
  if (strcmp(name, "saveInt") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")V") != NULL) {
    return MANGO_MID_STORE_SAVE_INT;
  }
  if (strcmp(name, "loadFloat") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")F") != NULL) {
    return MANGO_MID_STORE_LOAD_FLOAT;
  }
  if (strcmp(name, "saveFloat") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")V") != NULL) {
    return MANGO_MID_STORE_SAVE_FLOAT;
  }
  if (strcmp(name, "loadString") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")Ljava/lang/String;") != NULL) {
    return MANGO_MID_STORE_LOAD_STRING;
  }
  if (strcmp(name, "saveString") == 0 && sig && strstr(sig, "String") != NULL &&
      strstr(sig, ")V") != NULL) {
    return MANGO_MID_STORE_SAVE_STRING;
  }
  return MANGO_MID_UNKNOWN;
}

static MangoJniMethod* mango_jni_method_alloc(const char* name, const char* sig) {
  for (int i = 1; i < MANGO_JNIMETHOD_MAX; i++) {
    if (!g_jni_methods[i].used) {
      MangoJniMethod* m = &g_jni_methods[i];
      memset(m, 0, sizeof(*m));
      m->used = 1;
      m->kind = mango_classify_png_mid(name, sig);
      if (name) {
        strncpy(m->name, name, sizeof(m->name) - 1u);
      }
      if (sig) {
        strncpy(m->sig, sig, sizeof(m->sig) - 1u);
      }
      return m;
    }
  }
  return NULL;
}

static MangoJniMethod* mango_jni_method_from_mid(uint32_t mid_handle) {
  void* p = mango_handle_lookup(mid_handle);
  if (!p) {
    return NULL;
  }
  uintptr_t u = (uintptr_t)p;
  if (u >= (uintptr_t)&g_jni_methods[0] &&
      u < (uintptr_t)&g_jni_methods[MANGO_JNIMETHOD_MAX]) {
    MangoJniMethod* m = (MangoJniMethod*)p;
    return m->used ? m : NULL;
  }
  /* Legacy soft GetMethodID double-intern: handle → name_addr|1. */
  return NULL;
}

static MangoBitmap* mango_bitmap_alloc(void) {
  for (int i = 1; i < MANGO_BITMAP_MAX; i++) {
    if (!g_bitmaps[i].used) {
      memset(&g_bitmaps[i], 0, sizeof(g_bitmaps[i]));
      g_bitmaps[i].used = 1;
      return &g_bitmaps[i];
    }
  }
  return NULL;
}

static MangoBitmap* mango_bitmap_from_handle(uint32_t h) {
  void* p = mango_handle_lookup(h);
  if (!p) {
    return NULL;
  }
  uintptr_t u = (uintptr_t)p;
  if (u >= (uintptr_t)&g_bitmaps[0] && u < (uintptr_t)&g_bitmaps[MANGO_BITMAP_MAX]) {
    MangoBitmap* b = (MangoBitmap*)p;
    return b->used ? b : NULL;
  }
  return NULL;
}

static void mango_bitmap_free(MangoBitmap* b) {
  if (!b || !b->used) {
    return;
  }
  free(b->argb);
  b->argb = NULL;
  b->width = 0;
  b->height = 0;
  b->used = 0;
}

static MangoJIntArray* mango_jintarray_alloc(void) {
  for (int i = 1; i < MANGO_JINTARRAY_MAX; i++) {
    if (!g_jintarrays[i].used) {
      memset(&g_jintarrays[i], 0, sizeof(g_jintarrays[i]));
      g_jintarrays[i].used = 1;
      return &g_jintarrays[i];
    }
  }
  return NULL;
}

static MangoJIntArray* mango_jintarray_from_handle(uint32_t h) {
  void* p = mango_handle_lookup(h);
  if (!p) {
    return NULL;
  }
  uintptr_t u = (uintptr_t)p;
  if (u >= (uintptr_t)&g_jintarrays[0] &&
      u < (uintptr_t)&g_jintarrays[MANGO_JINTARRAY_MAX]) {
    MangoJIntArray* a = (MangoJIntArray*)p;
    return a->used ? a : NULL;
  }
  return NULL;
}

static const char* mango_jstring_chars(MangoLoadedLibrary* lib, uint32_t handle) {
  void* js = mango_handle_lookup(handle);
  if (!js) {
    return NULL;
  }
  return mango_fake_jstring_chars(lib, js);
}

static uint32_t mango_png_open(MangoLoadedLibrary* lib, const char* rel) {
  char host_path[768];
  uint8_t* pix = NULL;
  uint32_t w = 0, h = 0, bpp = 0;
  MangoBitmap* bmp;
  size_t n, i;
  (void)lib;
  if (!rel || rel[0] == '\0') {
    fprintf(stderr, "mango: PngManager.open FAIL (null path)\n");
    return 0;
  }
  if (!mango_aasset_host_path(lib, rel, host_path, sizeof(host_path))) {
    /* Also try host resolve (textures/... under asset root). */
    if (!mango_host_resolve_path(lib, rel, host_path, sizeof(host_path))) {
      fprintf(stderr, "mango: PngManager.open FAIL %s\n", rel);
      return 0;
    }
  }
#ifndef ANDROID
  if (mango_png_decode(host_path, &pix, &w, &h, &bpp) != 0 || !pix || w == 0 || h == 0) {
    fprintf(stderr, "mango: PngManager.open decode FAIL %s\n", host_path);
    free(pix);
    return 0;
  }
#else
  (void)host_path;
  fprintf(stderr, "mango: PngManager.open unavailable on ANDROID build\n");
  return 0;
#endif
  bmp = mango_bitmap_alloc();
  if (!bmp) {
    free(pix);
    return 0;
  }
  n = (size_t)w * (size_t)h;
  bmp->argb = (uint32_t*)malloc(n * sizeof(uint32_t));
  if (!bmp->argb) {
    free(pix);
    mango_bitmap_free(bmp);
    return 0;
  }
  bmp->width = (int)w;
  bmp->height = (int)h;
  if (bpp == 4) {
    for (i = 0; i < n; i++) {
      uint8_t r = pix[i * 4u + 0u];
      uint8_t g = pix[i * 4u + 1u];
      uint8_t b = pix[i * 4u + 2u];
      uint8_t a = pix[i * 4u + 3u];
      bmp->argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  } else if (bpp == 1) {
    for (i = 0; i < n; i++) {
      uint8_t g = pix[i];
      bmp->argb[i] = 0xff000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
    }
  } else {
    free(pix);
    mango_bitmap_free(bmp);
    return 0;
  }
  free(pix);
  fprintf(stderr, "mango: PngManager.open ok %s %ux%u\n", rel, w, h);
  return mango_handle_intern(bmp);
}

static uint32_t mango_jni_call_png(MangoLoadedLibrary* lib, MangoCpu* cpu, MangoJniMethod* mid,
                                   int form) {
  /* form: 0=Call*Method, 1=Call*MethodV (r3=va_list), 2=Call*MethodA (r3=jvalue*).
   * Pacman Art::loadPng uses the *V wrappers at 0xc4a4/0xc4be/0xc4d8. */
  uint32_t arg0 = 0, arg1 = 0;
  if (form == 1) {
    uint32_t va = cpu->r[3];
    if (!mango_guest_range_ok(lib, va, 4u)) {
      fprintf(stderr, "mango: PngManager Call*V bad va_list=%x\n", va);
      cpu->r[0] = 0;
      return 1;
    }
    arg0 = mango_load_u32_guest(lib->guest_mem, va);
    if (mango_guest_range_ok(lib, va + 4u, 4u)) {
      arg1 = mango_load_u32_guest(lib->guest_mem, va + 4u);
    }
  } else if (form == 2) {
    uint32_t jv = cpu->r[3];
    if (!mango_guest_range_ok(lib, jv, 8u)) {
      cpu->r[0] = 0;
      return 1;
    }
    /* jvalue is 8-byte aligned union; .l/.i in low 4 bytes each slot. */
    arg0 = mango_load_u32_guest(lib->guest_mem, jv);
    arg1 = mango_load_u32_guest(lib->guest_mem, jv + 8u);
  } else {
    arg0 = cpu->r[3];
    if (mango_guest_range_ok(lib, cpu->r[MANGO_REG_SP], 4u)) {
      arg1 = mango_load_u32_guest(lib->guest_mem, cpu->r[MANGO_REG_SP]);
    }
  }
  switch (mid->kind) {
    case MANGO_MID_PNG_OPEN: {
      const char* path = mango_jstring_chars(lib, arg0);
      uint32_t h = mango_png_open(lib, path);
      cpu->r[0] = h;
      return 1;
    }
    case MANGO_MID_PNG_GET_WIDTH: {
      MangoBitmap* b = mango_bitmap_from_handle(arg0);
      cpu->r[0] = b ? (uint32_t)b->width : 0;
      return 1;
    }
    case MANGO_MID_PNG_GET_HEIGHT: {
      MangoBitmap* b = mango_bitmap_from_handle(arg0);
      cpu->r[0] = b ? (uint32_t)b->height : 0;
      return 1;
    }
    case MANGO_MID_PNG_GET_PIXELS: {
      MangoBitmap* b = mango_bitmap_from_handle(arg0);
      MangoJIntArray* a = mango_jintarray_from_handle(arg1);
      int n, i;
      if (!b || !a || !b->argb || !a->guest_elems) {
        fprintf(stderr, "mango: PngManager.getPixels FAIL bmp=%u arr=%u\n", arg0, arg1);
        cpu->r[0] = 0;
        return 1;
      }
      n = b->width * b->height;
      if (n > a->length) {
        n = a->length;
      }
      if (!mango_guest_range_ok(lib, a->guest_elems, (uint32_t)n * 4u)) {
        cpu->r[0] = 0;
        return 1;
      }
      for (i = 0; i < n; i++) {
        mango_store_u32_guest(lib->guest_mem, a->guest_elems + (uint32_t)i * 4u, b->argb[i]);
      }
      cpu->r[0] = 0;
      return 1;
    }
    case MANGO_MID_PNG_CLOSE: {
      MangoBitmap* b = mango_bitmap_from_handle(arg0);
      if (b) {
        mango_bitmap_free(b);
      }
      cpu->r[0] = 0;
      return 1;
    }
    default:
      return 0;
  }
}

/* Pacman Q3 (research/50): host StoreManager in-memory prefs.
 * Soft CallBooleanMethod returned 1 → loadBool("Engine_saved", false) truthy →
 * Engine::initLogic called load() with null currentMenu → ELF-magic vtable LDR.
 * Honour defValue when key missing so cold start takes STATE_AFTER_LOADING. */
#define MANGO_PREF_MAX 64
#define MANGO_PREF_KEY_MAX 64
#define MANGO_PREF_STR_MAX 256

typedef enum {
  MANGO_PREF_BOOL = 1,
  MANGO_PREF_INT,
  MANGO_PREF_FLOAT,
  MANGO_PREF_STRING
} MangoPrefType;

typedef struct {
  int used;
  MangoPrefType type;
  char key[MANGO_PREF_KEY_MAX];
  int i;       /* bool/int */
  float f;
  char* s;     /* heap string for PREF_STRING */
} MangoPref;

static MangoPref g_prefs[MANGO_PREF_MAX];

static MangoPref* mango_pref_find(const char* key) {
  if (!key) {
    return NULL;
  }
  for (int i = 0; i < MANGO_PREF_MAX; i++) {
    if (g_prefs[i].used && strcmp(g_prefs[i].key, key) == 0) {
      return &g_prefs[i];
    }
  }
  return NULL;
}

static MangoPref* mango_pref_alloc(const char* key) {
  MangoPref* p = mango_pref_find(key);
  if (p) {
    return p;
  }
  for (int i = 0; i < MANGO_PREF_MAX; i++) {
    if (!g_prefs[i].used) {
      p = &g_prefs[i];
      memset(p, 0, sizeof(*p));
      p->used = 1;
      strncpy(p->key, key, sizeof(p->key) - 1u);
      return p;
    }
  }
  return NULL;
}

static void mango_pref_set_bool(const char* key, int v) {
  MangoPref* p = mango_pref_alloc(key);
  if (!p) {
    return;
  }
  free(p->s);
  p->s = NULL;
  p->type = MANGO_PREF_BOOL;
  p->i = v ? 1 : 0;
}

static void mango_pref_set_int(const char* key, int v) {
  MangoPref* p = mango_pref_alloc(key);
  if (!p) {
    return;
  }
  free(p->s);
  p->s = NULL;
  p->type = MANGO_PREF_INT;
  p->i = v;
}

static void mango_pref_set_float(const char* key, float v) {
  MangoPref* p = mango_pref_alloc(key);
  if (!p) {
    return;
  }
  free(p->s);
  p->s = NULL;
  p->type = MANGO_PREF_FLOAT;
  p->f = v;
}

static void mango_pref_set_string(const char* key, const char* val) {
  MangoPref* p = mango_pref_alloc(key);
  char* dup;
  if (!p) {
    return;
  }
  free(p->s);
  p->s = NULL;
  p->type = MANGO_PREF_STRING;
  if (!val) {
    val = "";
  }
  dup = (char*)malloc(strlen(val) + 1u);
  if (!dup) {
    return;
  }
  memcpy(dup, val, strlen(val) + 1u);
  p->s = dup;
}

static int mango_jni_store_args(MangoLoadedLibrary* lib, MangoCpu* cpu, int form,
                                uint32_t* arg0, uint32_t* arg1, uint32_t* arg2) {
  /* form: 0=Call*Method, 1=Call*MethodV, 2=Call*MethodA. Store::* uses *V. */
  *arg0 = 0;
  *arg1 = 0;
  *arg2 = 0;
  if (form == 1) {
    uint32_t va = cpu->r[3];
    if (!mango_guest_range_ok(lib, va, 4u)) {
      return 0;
    }
    *arg0 = mango_load_u32_guest(lib->guest_mem, va);
    if (mango_guest_range_ok(lib, va + 4u, 4u)) {
      *arg1 = mango_load_u32_guest(lib->guest_mem, va + 4u);
    }
    if (mango_guest_range_ok(lib, va + 8u, 4u)) {
      *arg2 = mango_load_u32_guest(lib->guest_mem, va + 8u);
    }
    return 1;
  }
  if (form == 2) {
    uint32_t jv = cpu->r[3];
    if (!mango_guest_range_ok(lib, jv, 8u)) {
      return 0;
    }
    *arg0 = mango_load_u32_guest(lib->guest_mem, jv);
    *arg1 = mango_load_u32_guest(lib->guest_mem, jv + 8u);
    if (mango_guest_range_ok(lib, jv + 16u, 4u)) {
      *arg2 = mango_load_u32_guest(lib->guest_mem, jv + 16u);
    }
    return 1;
  }
  *arg0 = cpu->r[3];
  if (mango_guest_range_ok(lib, cpu->r[MANGO_REG_SP], 4u)) {
    *arg1 = mango_load_u32_guest(lib->guest_mem, cpu->r[MANGO_REG_SP]);
  }
  if (mango_guest_range_ok(lib, cpu->r[MANGO_REG_SP] + 4u, 4u)) {
    *arg2 = mango_load_u32_guest(lib->guest_mem, cpu->r[MANGO_REG_SP] + 4u);
  }
  return 1;
}

static uint32_t mango_jni_call_store(MangoLoadedLibrary* lib, MangoCpu* cpu, MangoJniMethod* mid,
                                     int form) {
  uint32_t arg0 = 0, arg1 = 0, arg2 = 0;
  const char* key;
  if (mid->kind < MANGO_MID_STORE_LOAD_BOOL || mid->kind > MANGO_MID_STORE_SAVE_STRING) {
    return 0;
  }
  if (!mango_jni_store_args(lib, cpu, form, &arg0, &arg1, &arg2)) {
    fprintf(stderr, "mango: StoreManager Call* bad args form=%d\n", form);
    cpu->r[0] = 0;
    return 1;
  }
  key = mango_jstring_chars(lib, arg0);
  if (!key) {
    key = "";
  }
  switch (mid->kind) {
    case MANGO_MID_STORE_LOAD_BOOL: {
      MangoPref* p = mango_pref_find(key);
      int def = arg1 ? 1 : 0;
      int v = p && p->type == MANGO_PREF_BOOL ? p->i : def;
      fprintf(stderr, "mango: StoreManager.loadBoolean %s -> %d (def=%d)\n", key, v, def);
      cpu->r[0] = (uint32_t)v;
      return 1;
    }
    case MANGO_MID_STORE_SAVE_BOOL: {
      mango_pref_set_bool(key, arg1 ? 1 : 0);
      fprintf(stderr, "mango: StoreManager.saveBoolean %s = %d\n", key, arg1 ? 1 : 0);
      cpu->r[0] = 0;
      return 1;
    }
    case MANGO_MID_STORE_LOAD_INT: {
      MangoPref* p = mango_pref_find(key);
      int def = (int)arg1;
      int v = p && p->type == MANGO_PREF_INT ? p->i : def;
      fprintf(stderr, "mango: StoreManager.loadInt %s -> %d (def=%d)\n", key, v, def);
      cpu->r[0] = (uint32_t)v;
      return 1;
    }
    case MANGO_MID_STORE_SAVE_INT: {
      mango_pref_set_int(key, (int)arg1);
      fprintf(stderr, "mango: StoreManager.saveInt %s = %d\n", key, (int)arg1);
      cpu->r[0] = 0;
      return 1;
    }
    case MANGO_MID_STORE_LOAD_FLOAT: {
      /* Varargs promote float→double; lo/hi in arg1/arg2 after key. */
      union {
        double d;
        uint32_t u[2];
      } du;
      union {
        float f;
        uint32_t u;
      } fu;
      MangoPref* p = mango_pref_find(key);
      du.u[0] = arg1;
      du.u[1] = arg2;
      fu.f = (float)du.d;
      if (p && p->type == MANGO_PREF_FLOAT) {
        fu.f = p->f;
      }
      fprintf(stderr, "mango: StoreManager.loadFloat %s -> %g\n", key, (double)fu.f);
      cpu->r[0] = fu.u;
      return 1;
    }
    case MANGO_MID_STORE_SAVE_FLOAT: {
      union {
        double d;
        uint32_t u[2];
      } du;
      du.u[0] = arg1;
      du.u[1] = arg2;
      mango_pref_set_float(key, (float)du.d);
      fprintf(stderr, "mango: StoreManager.saveFloat %s = %g\n", key, du.d);
      cpu->r[0] = 0;
      return 1;
    }
    case MANGO_MID_STORE_LOAD_STRING: {
      MangoPref* p = mango_pref_find(key);
      if (p && p->type == MANGO_PREF_STRING && p->s) {
        uint32_t ga = mango_guest_strdup(lib, p->s);
        cpu->r[0] = mango_handle_intern((void*)(uintptr_t)ga);
        fprintf(stderr, "mango: StoreManager.loadString %s -> (stored)\n", key);
      } else {
        /* Honour def jobject when unset. */
        cpu->r[0] = arg1;
        fprintf(stderr, "mango: StoreManager.loadString %s -> (def)\n", key);
      }
      return 1;
    }
    case MANGO_MID_STORE_SAVE_STRING: {
      const char* val = mango_jstring_chars(lib, arg1);
      mango_pref_set_string(key, val ? val : "");
      fprintf(stderr, "mango: StoreManager.saveString %s\n", key);
      cpu->r[0] = 0;
      return 1;
    }
    default:
      return 0;
  }
}

static uint32_t mango_jni_call_host(MangoLoadedLibrary* lib, MangoCpu* cpu, MangoJniMethod* mid,
                                    int form) {
  if (!mid || mid->kind == MANGO_MID_UNKNOWN) {
    return 0;
  }
  if (mango_jni_call_png(lib, cpu, mid, form)) {
    return 1;
  }
  return mango_jni_call_store(lib, cpu, mid, form);
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

/* Map guest path → host path for stat/lstat/access (fopen search order, but
 * existence via F_OK so write-only probes still resolve under asset root). */
static int mango_host_map_path(MangoLoadedLibrary* lib, const char* path, char* out,
                               size_t outsz) {
  const char* root;
  if (!path || !out || outsz == 0) {
    return 0;
  }
  if (access(path, F_OK) == 0) {
    snprintf(out, outsz, "%s", path);
    return 1;
  }
  if (lib && lib->path[0] && strrchr(lib->path, '/')) {
    const char* slash = strrchr(lib->path, '/');
    int dlen = (int)(slash - lib->path);
    snprintf(out, outsz, "%.*s/../gamedata/%s", dlen, lib->path, path);
    if (access(out, F_OK) == 0) {
      return 1;
    }
    snprintf(out, outsz, "%.*s/../%s", dlen, lib->path, path);
    if (access(out, F_OK) == 0) {
      return 1;
    }
  }
  root = getenv("MANGO_ASSET_ROOT");
  if (root && root[0]) {
    snprintf(out, outsz, "%s/%s", root, path);
    if (access(out, F_OK) == 0) {
      return 1;
    }
  }
  /* Fall back to as-is so host errno (ENOENT) is meaningful. */
  snprintf(out, outsz, "%s", path);
  return 1;
}

/* Pack host struct stat into bionic armeabi layout (96 bytes). */
static void mango_store_bionic_stat(uint8_t* mem, uint32_t addr, const struct stat* st) {
  memset(mem + addr, 0, 96);
  /* st_dev u64 @0 */
  {
    uint64_t v = (uint64_t)st->st_dev;
    memcpy(mem + addr + 0, &v, 8);
  }
  /* __st_ino u32 @12 */
  {
    uint32_t v = (uint32_t)st->st_ino;
    memcpy(mem + addr + 12, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_mode;
    memcpy(mem + addr + 16, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_nlink;
    memcpy(mem + addr + 20, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_uid;
    memcpy(mem + addr + 24, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_gid;
    memcpy(mem + addr + 28, &v, 4);
  }
  {
    uint64_t v = (uint64_t)st->st_rdev;
    memcpy(mem + addr + 32, &v, 8);
  }
  {
    int64_t v = (int64_t)st->st_size;
    memcpy(mem + addr + 44, &v, 8);
  }
  {
    uint32_t v = (uint32_t)st->st_blksize;
    memcpy(mem + addr + 52, &v, 4);
  }
  {
    uint64_t v = (uint64_t)st->st_blocks;
    memcpy(mem + addr + 56, &v, 8);
  }
  {
    uint32_t v = (uint32_t)st->st_atime;
    memcpy(mem + addr + 64, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_mtime;
    memcpy(mem + addr + 72, &v, 4);
  }
  {
    uint32_t v = (uint32_t)st->st_ctime;
    memcpy(mem + addr + 80, &v, 4);
  }
  {
    uint64_t v = (uint64_t)st->st_ino;
    memcpy(mem + addr + 88, &v, 8);
  }
}

static void mango_set_guest_errno(MangoLoadedLibrary* lib, int err) {
  mango_store_u32_guest(lib->guest_mem, lib->guest_errno_addr, (uint32_t)err);
}

#ifndef ANDROID
/* Decode PNG via host libpng. Meritous assets are 8-bit grayscale → out_bpp=1;
 * other PNGs expand to RGBA8888 → out_bpp=4. */
static int mango_png_decode(const char* host_path, uint8_t** out_pixels,
                            uint32_t* out_w, uint32_t* out_h, uint32_t* out_bpp) {
  FILE* fp;
  png_structp png = NULL;
  png_infop info = NULL;
  png_bytep* rows = NULL;
  uint8_t* pixels = NULL;
  png_uint_32 w = 0, h = 0;
  size_t rowbytes, nbytes;
  png_uint_32 y;
  int want_gray = 0;
  int bit_depth = 0, color_type = 0;
  if (!host_path || !out_pixels || !out_w || !out_h || !out_bpp) {
    return -1;
  }
  *out_pixels = NULL;
  *out_w = 0;
  *out_h = 0;
  *out_bpp = 0;
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
    free(pixels);
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
  bit_depth = png_get_bit_depth(png, info);
  color_type = png_get_color_type(png, info);
  if (w == 0 || h == 0 || w > 4096u || h > 4096u) {
    longjmp(png_jmpbuf(png), 1);
  }
  want_gray = (color_type == PNG_COLOR_TYPE_GRAY ||
               color_type == PNG_COLOR_TYPE_GRAY_ALPHA);
  if (want_gray) {
    if (color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
      png_set_strip_alpha(png);
    }
    if (bit_depth < 8) {
      png_set_expand_gray_1_2_4_to_8(png);
    }
    if (bit_depth == 16) {
      png_set_strip_16(png);
    }
  } else {
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_palette_to_rgb(png);
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
  if (want_gray) {
    if (rowbytes < (size_t)w) {
      longjmp(png_jmpbuf(png), 1);
    }
    nbytes = (size_t)w * (size_t)h;
    *out_bpp = 1;
  } else {
    if (rowbytes < (size_t)w * 4u) {
      longjmp(png_jmpbuf(png), 1);
    }
    nbytes = (size_t)w * (size_t)h * 4u;
    *out_bpp = 4;
  }
  pixels = (uint8_t*)malloc(nbytes);
  rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
  if (!pixels || !rows) {
    longjmp(png_jmpbuf(png), 1);
  }
  for (y = 0; y < h; y++) {
    rows[y] = pixels + (size_t)y * (want_gray ? (size_t)w : (size_t)w * 4u);
  }
  if (rowbytes == (want_gray ? (size_t)w : (size_t)w * 4u)) {
    png_read_image(png, rows);
  } else {
    png_bytep tmp = (png_bytep)malloc(rowbytes);
    size_t copy = want_gray ? (size_t)w : (size_t)w * 4u;
    if (!tmp) {
      longjmp(png_jmpbuf(png), 1);
    }
    for (y = 0; y < h; y++) {
      png_read_row(png, tmp, NULL);
      memcpy(rows[y], tmp, copy);
    }
    free(tmp);
  }
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  free(rows);
  *out_pixels = pixels;
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
static uint32_t mango_guest_sdl_surface_bpp(MangoLoadedLibrary* lib, uint32_t w, uint32_t h,
                                           uint32_t bpp);
static int mango_host_resolve_path(MangoLoadedLibrary* lib, const char* path,
                                   char* out, size_t outsz);
#ifndef ANDROID
static int mango_png_decode(const char* host_path, uint8_t** out_pixels,
                            uint32_t* out_w, uint32_t* out_h, uint32_t* out_bpp);
#endif
static int mango_sdl_soft_blit(MangoLoadedLibrary* lib, uint32_t src, uint32_t srcrect,
                               uint32_t dst, uint32_t dstrect);
static int mango_sdl_soft_fill(MangoLoadedLibrary* lib, uint32_t dst, uint32_t dstrect,
                               uint32_t color);
static void mango_dump_framebuffer_pgm(MangoLoadedLibrary* lib, uint32_t surf);
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


/* Bionic/BSD ctype class bits (Android <ctype.h>). Tcl indexes
 * _ctype_[ch+1] and masks _ISspace=0x08 — never bind OBJECT imports to stub. */
#define MANGO_CTYPE_U 0x01u
#define MANGO_CTYPE_L 0x02u
#define MANGO_CTYPE_N 0x04u
#define MANGO_CTYPE_S 0x08u
#define MANGO_CTYPE_P 0x10u
#define MANGO_CTYPE_C 0x20u
#define MANGO_CTYPE_X 0x40u
#define MANGO_CTYPE_B 0x80u

static uint8_t mango_ctype_class(int ch) {
  if (ch < 0 || ch > 255) {
    return 0;
  }
  if (ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r') {
    return (uint8_t)(MANGO_CTYPE_C | MANGO_CTYPE_S | (ch == '\t' ? MANGO_CTYPE_B : 0));
  }
  if (ch == ' ') {
    return (uint8_t)(MANGO_CTYPE_S | MANGO_CTYPE_B);
  }
  if (ch >= '0' && ch <= '9') {
    uint8_t x = (ch <= '9') ? MANGO_CTYPE_X : 0; /* digits are hex too for 0-9 */
    (void)x;
    return (uint8_t)(MANGO_CTYPE_N | MANGO_CTYPE_X);
  }
  if (ch >= 'A' && ch <= 'Z') {
    uint8_t bits = (uint8_t)MANGO_CTYPE_U;
    if ((ch >= 'A' && ch <= 'F')) {
      bits = (uint8_t)(bits | MANGO_CTYPE_X);
    }
    return bits;
  }
  if (ch >= 'a' && ch <= 'z') {
    uint8_t bits = (uint8_t)MANGO_CTYPE_L;
    if ((ch >= 'a' && ch <= 'f')) {
      bits = (uint8_t)(bits | MANGO_CTYPE_X);
    }
    return bits;
  }
  if (ch < 0x20 || ch == 0x7f) {
    return (uint8_t)MANGO_CTYPE_C;
  }
  /* printable punctuation / symbol */
  return (uint8_t)MANGO_CTYPE_P;
}

static void mango_init_ctype_tables(MangoLoadedLibrary* lib) {
  uint8_t* mem = lib->guest_mem;
  uint32_t base = lib->heap_base + lib->heap_used;
  uint32_t ctype_tab;
  uint32_t tolower_tab;
  uint32_t toupper_tab;
  uint32_t ctype_cell;
  uint32_t tolower_cell;
  uint32_t toupper_cell;
  int c;

  /* Classic 1+256 layout: index 0 = EOF slot; Tcl uses table[ch+1]. */
  ctype_tab = base;
  mem[ctype_tab + 0u] = 0;
  for (c = 0; c < 256; c++) {
    mem[ctype_tab + 1u + (uint32_t)c] = mango_ctype_class(c);
  }
  base = (ctype_tab + 257u + 3u) & ~3u;

  tolower_tab = base;
  mem[tolower_tab + 0u] = (uint8_t)0xff;
  mem[tolower_tab + 1u] = (uint8_t)0xff; /* -1 as int16 LE */
  for (c = 0; c < 256; c++) {
    int16_t v = (int16_t)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
    uint32_t off = tolower_tab + 2u + (uint32_t)c * 2u;
    mem[off] = (uint8_t)(v & 0xff);
    mem[off + 1u] = (uint8_t)((v >> 8) & 0xff);
  }
  base = (tolower_tab + 2u * 257u + 3u) & ~3u;

  toupper_tab = base;
  mem[toupper_tab + 0u] = (uint8_t)0xff;
  mem[toupper_tab + 1u] = (uint8_t)0xff;
  for (c = 0; c < 256; c++) {
    int16_t v = (int16_t)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
    uint32_t off = toupper_tab + 2u + (uint32_t)c * 2u;
    mem[off] = (uint8_t)(v & 0xff);
    mem[off + 1u] = (uint8_t)((v >> 8) & 0xff);
  }
  base = (toupper_tab + 2u * 257u + 3u) & ~3u;

  /* BSD/bionic expose _ctype_ as a POINTER object (char *). GLOB_DAT binds
   * GOT to that cell; guest does ldr/ldr to obtain the table base. Returning
   * the table address itself made the second ldr read table bytes as a VA
   * (0x20202000 from control-class 0x20 bytes). */
  ctype_cell = base;
  mango_store_u32_guest(mem, ctype_cell, ctype_tab);
  tolower_cell = ctype_cell + 4u;
  mango_store_u32_guest(mem, tolower_cell, tolower_tab);
  toupper_cell = tolower_cell + 4u;
  mango_store_u32_guest(mem, toupper_cell, toupper_tab);
  base = toupper_cell + 4u;

  lib->ctype_addr = ctype_cell;
  lib->tolower_tab_addr = tolower_cell;
  lib->toupper_tab_addr = toupper_cell;

  /* environ: char ** — empty list (single NULL). TclSetupEnv walks it. */
  {
    uint32_t env_array = base;
    mango_store_u32_guest(mem, env_array, 0); /* NULL terminator */
    base = env_array + 4u;
    lib->environ_addr = base;
    mango_store_u32_guest(mem, lib->environ_addr, env_array);
    base = lib->environ_addr + 4u;
  }

  lib->heap_used = base - lib->heap_base;

  /* Sanity: space has _S bit; colon does not (Tcl list path separator walk). */
  if ((mem[ctype_tab + 1u + (uint32_t)' '] & MANGO_CTYPE_S) == 0 ||
      (mem[ctype_tab + 1u + (uint32_t)':'] & MANGO_CTYPE_S) != 0) {
    fprintf(stderr, "mango: ctype table sanity failed space=%u colon=%u\n",
            (unsigned)mem[ctype_tab + 1u + (uint32_t)' '],
            (unsigned)mem[ctype_tab + 1u + (uint32_t)':']);
  }
}

/* Pacman Q2: minimal soft OpenSLES. SLObjectItf is double-indirect
 * (pointer to pointer-to-vtable). Vtable slots that must write out-params
 * use SVC thunks; everything else is SUCCESS / void no-ops. */
static uint32_t mango_sl_fill_vtable(MangoLoadedLibrary* lib, uint32_t nslots,
                                     uint32_t getiface_slot, uint32_t create_player_slot,
                                     uint32_t create_mix_slot) {
  uint32_t vt = mango_guest_alloc(lib, nslots * 4u);
  uint32_t i;
  uint32_t gi_thunk = 0;
  uint32_t cp_thunk = 0;
  uint32_t cm_thunk = 0;
  if (vt == 0) {
    return 0;
  }
  if (getiface_slot < nslots) {
    gi_thunk = mango_guest_alloc(lib, MANGO_JNI_THUNK_SIZE);
    if (gi_thunk) {
      mango_write_jni_thunk(lib->guest_mem, gi_thunk,
                            MANGO_SL_SVC_BASE + MANGO_SL_GET_INTERFACE);
    }
  }
  if (create_player_slot < nslots) {
    cp_thunk = mango_guest_alloc(lib, MANGO_JNI_THUNK_SIZE);
    if (cp_thunk) {
      mango_write_jni_thunk(lib->guest_mem, cp_thunk,
                            MANGO_SL_SVC_BASE + MANGO_SL_CREATE_AUDIO_PLAYER);
    }
  }
  if (create_mix_slot < nslots) {
    cm_thunk = mango_guest_alloc(lib, MANGO_JNI_THUNK_SIZE);
    if (cm_thunk) {
      mango_write_jni_thunk(lib->guest_mem, cm_thunk,
                            MANGO_SL_SVC_BASE + MANGO_SL_CREATE_OUTPUT_MIX);
    }
  }
  for (i = 0; i < nslots; i++) {
    uint32_t fn = g_sl_success_stub;
    /* Object::Destroy is void @ slot 6; engine/itfs use SUCCESS there. */
    if (i == 6u && g_sl_destroy_stub && getiface_slot == 3u) {
      fn = g_sl_destroy_stub;
    }
    if (i == getiface_slot && gi_thunk) {
      fn = gi_thunk;
    }
    if (i == create_player_slot && cp_thunk) {
      fn = cp_thunk;
    }
    if (i == create_mix_slot && cm_thunk) {
      fn = cm_thunk;
    }
    mango_store_u32_guest(lib->guest_mem, vt + i * 4u, fn);
  }
  return vt;
}

static uint32_t mango_sl_make_itf(MangoLoadedLibrary* lib, uint32_t vtable) {
  uint32_t cell = mango_guest_alloc(lib, 4u);
  if (cell == 0 || vtable == 0) {
    return 0;
  }
  mango_store_u32_guest(lib->guest_mem, cell, vtable);
  return cell; /* SLObjectItf / SL*Itf value */
}

static uint32_t mango_sl_make_iid_cell(MangoLoadedLibrary* lib, uint32_t* out_uuid) {
  /* Distinct 16-byte UUID + pointer cell. Identity compare is enough for soft
   * GetInterface; content unused. */
  uint32_t uuid = mango_guest_alloc(lib, 16u);
  uint32_t cell = mango_guest_alloc(lib, 4u);
  if (uuid == 0 || cell == 0) {
    return 0;
  }
  /* stamp a unique tag so UUIDs differ if anyone memcmp's */
  mango_store_u32_guest(lib->guest_mem, uuid, uuid);
  mango_store_u32_guest(lib->guest_mem, cell, uuid);
  if (out_uuid) {
    *out_uuid = uuid;
  }
  return cell;
}

static void mango_init_opensles(MangoLoadedLibrary* lib) {
  uint8_t* mem;
  if (g_sl_ready || lib == NULL || lib->guest_mem == NULL) {
    return;
  }
  mem = lib->guest_mem;

  g_sl_success_stub = mango_guest_alloc(lib, 8u);
  if (g_sl_success_stub == 0) {
    return;
  }
  mango_store_u32_guest(mem, g_sl_success_stub, 0xE3A00000u); /* mov r0, #0 */
  mango_store_u32_guest(mem, g_sl_success_stub + 4u, 0xE12FFF1Eu); /* bx lr */

  g_sl_destroy_stub = mango_guest_alloc(lib, 8u);
  if (g_sl_destroy_stub == 0) {
    return;
  }
  mango_store_u32_guest(mem, g_sl_destroy_stub, 0xE12FFF1Eu); /* bx lr */
  mango_store_u32_guest(mem, g_sl_destroy_stub + 4u, 0xE1A00000u); /* nop */

  /* Object vtable: Realize@0, GetInterface@3 (0xc), Destroy@6 (0x18). */
  g_sl_obj_vtable = mango_sl_fill_vtable(lib, 10u, 3u, 0xffffffffu, 0xffffffffu);
  /* Engine: CreateAudioPlayer@2 (8), CreateOutputMix@7 (0x1c). */
  g_sl_engine_vtable = mango_sl_fill_vtable(lib, 16u, 0xffffffffu, 2u, 7u);
  /* Play / Seek / BufferQueue / Volume / EffectSend — SUCCESS no-ops. */
  g_sl_play_vtable = mango_sl_fill_vtable(lib, 12u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
  g_sl_seek_vtable = mango_sl_fill_vtable(lib, 4u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
  g_sl_bq_vtable = mango_sl_fill_vtable(lib, 8u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
  g_sl_volume_vtable = mango_sl_fill_vtable(lib, 8u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
  g_sl_effect_vtable = mango_sl_fill_vtable(lib, 8u, 0xffffffffu, 0xffffffffu, 0xffffffffu);

  g_sl_iid_engine_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_engine);
  g_sl_iid_play_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_play);
  g_sl_iid_seek_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_seek);
  g_sl_iid_volume_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_volume);
  g_sl_iid_bufferqueue_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_bufferqueue);
  g_sl_iid_effectsend_cell = mango_sl_make_iid_cell(lib, &g_sl_iid_effectsend);

  if (g_sl_obj_vtable && g_sl_engine_vtable && g_sl_iid_engine_cell) {
    g_sl_ready = 1;
    fprintf(stderr, "mango: soft OpenSLES ready obj_vt=%x engine_vt=%x\n",
            g_sl_obj_vtable, g_sl_engine_vtable);
  }
}

static void mango_sl_svc(MangoLoadedLibrary* lib, MangoCpu* cpu, uint32_t fn) {
  uint32_t r0 = cpu->r[0];
  uint32_t r1 = cpu->r[1];
  uint32_t r2 = cpu->r[2];
  (void)r0;
  switch (fn) {
    case MANGO_SL_GET_INTERFACE: {
      /* GetInterface(self, iid, pInterface) — iid is UUID addr. */
      uint32_t vt = g_sl_play_vtable;
      uint32_t itf;
      if (r1 == g_sl_iid_engine) {
        vt = g_sl_engine_vtable;
      } else if (r1 == g_sl_iid_play) {
        vt = g_sl_play_vtable;
      } else if (r1 == g_sl_iid_seek) {
        vt = g_sl_seek_vtable;
      } else if (r1 == g_sl_iid_volume) {
        vt = g_sl_volume_vtable;
      } else if (r1 == g_sl_iid_bufferqueue) {
        vt = g_sl_bq_vtable;
      } else if (r1 == g_sl_iid_effectsend) {
        vt = g_sl_effect_vtable;
      }
      itf = mango_sl_make_itf(lib, vt);
      if (r2 != 0 && mango_guest_range_ok(lib, r2, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r2, itf);
      }
      cpu->r[0] = 0; /* SL_RESULT_SUCCESS */
      break;
    }
    case MANGO_SL_CREATE_OUTPUT_MIX:
    case MANGO_SL_CREATE_AUDIO_PLAYER: {
      /* Create*(self, pObject, ...) — write new soft SLObjectItf to *pObject. */
      uint32_t obj = mango_sl_make_itf(lib, g_sl_obj_vtable);
      if (r1 != 0 && mango_guest_range_ok(lib, r1, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r1, obj);
      }
      cpu->r[0] = 0;
      break;
    }
    default:
      cpu->r[0] = 0;
      break;
  }
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
    lib->ctype_addr = first->ctype_addr;
    lib->tolower_tab_addr = first->tolower_tab_addr;
    lib->toupper_tab_addr = first->toupper_tab_addr;
    lib->environ_addr = first->environ_addr;
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
  mango_init_ctype_tables(lib); /* bump heap_used for _ctype_ / case tabs */
  mango_init_opensles(lib); /* Pacman Q2 soft OpenSLES IIDs + vtables */
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
  if (strcmp(name, "_ctype_") == 0) {
    /* Pointer cell → table; never stub_addr (research/43). */
    return lib->ctype_addr;
  }
  if (strcmp(name, "_tolower_tab_") == 0) {
    return lib->tolower_tab_addr;
  }
  if (strcmp(name, "_toupper_tab_") == 0) {
    return lib->toupper_tab_addr;
  }
  if (strcmp(name, "environ") == 0) {
    return lib->environ_addr;
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
  } else if (strcmp(name, "_Znwj") == 0 || strcmp(name, "_Znaj") == 0) {
    /* Pacman Q1: Art::loadPng / argb2rgba use operator new / new[]. Stub
     * returned NULL and argb2rgba wrote pixels at 0 → clobbered .text. */
    name = "malloc";
  } else if (strcmp(name, "_ZdlPv") == 0 || strcmp(name, "_ZdaPv") == 0) {
    name = "free";
  } else if (strncmp(name, "SL_IID_", 7) == 0) {
    /* Pacman Q2: OpenSLES IID objects — pointer cells, never stub_addr. */
    mango_init_opensles(lib);
    if (strcmp(name, "SL_IID_ENGINE") == 0) {
      return g_sl_iid_engine_cell;
    }
    if (strcmp(name, "SL_IID_PLAY") == 0) {
      return g_sl_iid_play_cell;
    }
    if (strcmp(name, "SL_IID_SEEK") == 0) {
      return g_sl_iid_seek_cell;
    }
    if (strcmp(name, "SL_IID_VOLUME") == 0) {
      return g_sl_iid_volume_cell;
    }
    if (strcmp(name, "SL_IID_BUFFERQUEUE") == 0) {
      return g_sl_iid_bufferqueue_cell;
    }
    if (strcmp(name, "SL_IID_EFFECTSEND") == 0) {
      return g_sl_iid_effectsend_cell;
    }
    return lib->stub_addr;
  } else if (strncmp(name, "AAsset", 6) == 0) {
    /* Pacman Q0: real AAsset* host I/O via kLibcNames + MANGO_ASSET_ROOT.
     * Do not soft-map to eglGetDisplay (getNextFileName must eventually NULL). */
  } else if (strncmp(name, "egl", 3) == 0 || strncmp(name, "ANative", 7) == 0 ||
             strncmp(name, "ALooper", 7) == 0) {
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

static void mango_meritous_host_draw_text(MangoLoadedLibrary* lib, uint32_t x0, uint32_t y0,
                                          uint32_t str_addr, uint32_t color);
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
      /* Meritous libsdl_image IMG_Load (basename-gated thunk). Grayscale PNGs
       * become 8bpp surfaces (Meritous screen/fog); others RGBA8888. */
      const char* path = mango_guest_cstr(lib, r0);
      uint32_t surf = 0;
      char host_path[768];
      static int s_img;
#ifndef ANDROID
      uint8_t* pix = NULL;
      uint32_t pw = 0, ph = 0, pbpp = 0;
#endif
      if (path && mango_host_resolve_path(lib, path, host_path, sizeof(host_path))) {
#ifndef ANDROID
        if (mango_png_decode(host_path, &pix, &pw, &ph, &pbpp) == 0 && pix) {
          uint32_t bpp = (pbpp == 1u) ? 8u : 32u;
          surf = mango_guest_sdl_surface_bpp(lib, pw, ph, bpp);
          if (surf) {
            uint32_t pixels = mango_load_u32_guest(lib->guest_mem, surf + 20u);
            uint32_t nbytes = pw * ph * (pbpp == 1u ? 1u : 4u);
            uint32_t nonzero = 0;
            uint32_t i;
            if (pixels && mango_guest_range_ok(lib, pixels, nbytes)) {
              memcpy(lib->guest_mem + pixels, pix, nbytes);
              for (i = 0; i < nbytes; i++) {
                if (pix[i] != 0) {
                  nonzero++;
                }
              }
            }
            if (s_img < 40) {
              fprintf(stderr,
                      "mango: IMG_Load '%s' -> %ux%u %ubpp (%u nonzero bytes)\n",
                      path, (unsigned)pw, (unsigned)ph, (unsigned)bpp,
                      (unsigned)nonzero);
              s_img++;
            }
          }
          free(pix);
          pix = NULL;
        }
#endif
      }
      if (!surf) {
        if (s_img < 40) {
          fprintf(stderr, "mango: IMG_Load '%s' (blank fallback)\n",
                  path ? path : "(null)");
          s_img++;
        }
        surf = mango_guest_sdl_surface_bpp(lib, 256u, 256u, 8u);
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
      /* Host soft blit (same BytesPerPixel). DrawLevel ~350 tile copies. */
      uint32_t dstrect = cpu->r[3];
      cpu->r[0] = (uint32_t)mango_sdl_soft_blit(lib, r0, r1, r2, dstrect);
      break;
    }
    case MANGO_LIBC_SDL_UPDATE_RECT: {
      /* Android Meritous VideoUpdate calls UpdateRect (not Flip). Dump once
       * after DrawPlayer has colorkey-blitted onto the framebuffer. */
      static int s_ur;
      uint32_t surf = r0 ? r0 : g_fb_surf;
      if (s_ur < 8) {
        fprintf(stderr,
                "mango: SDL_UpdateRect surf=0x%x ckey_blits=%llu blits=%llu\n",
                (unsigned)surf, (unsigned long long)g_ckey_blit_count,
                (unsigned long long)g_blit_count);
        s_ur++;
      }
      if (!g_frame_dumped && surf &&
          (g_ckey_blit_count >= 1ull || g_blit_count >= 350ull)) {
        mango_dump_framebuffer_pgm(g_fb_lib ? g_fb_lib : lib, surf);
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SDL_SET_PALETTE:
    case MANGO_LIBC_SDL_SET_COLORS: {
      cpu->r[0] = 1; /* success */
      break;
    }
    case MANGO_LIBC_DRAW_TEXT: {
      uint32_t color = cpu->r[3];
      mango_meritous_host_draw_text(lib, r0, r1, r2, color);
      if (g_meritous_draw_text_calls == 1u) {
        const char* s = mango_guest_cstr(lib, r2);
        fprintf(stderr, "mango: draw_text host first '%s' @(%u,%u)\n",
                s ? s : "", (unsigned)r0, (unsigned)r1);
      }
      break;
    }
    case MANGO_LIBC_SDL_SET_COLOR_KEY: {
      /* SDL_SetColorKey(surface, flag, key) — Meritous Android sprites use SRCCOLORKEY (often key=255). */
      uint32_t surf = r0, flag = r1, key = r2;
      uint32_t fmt, flags;
      static int s_ck;
      if (!surf || !mango_guest_range_ok(lib, surf, 60u)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      fmt = mango_load_u32_guest(lib->guest_mem, surf + 4u);
      flags = mango_load_u32_guest(lib->guest_mem, surf + 0u);
      if (!fmt || !mango_guest_range_ok(lib, fmt, 40u)) {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      if (flag & 0x00001000u) { /* SDL_SRCCOLORKEY */
        mango_store_u32_guest(lib->guest_mem, surf + 0u, flags | 0x00001000u);
        mango_store_u32_guest(lib->guest_mem, fmt + 32u, key);
      } else {
        mango_store_u32_guest(lib->guest_mem, surf + 0u, flags & ~0x00001000u);
      }
      if (s_ck < 24) {
        fprintf(stderr,
                "mango: SDL_SetColorKey surf=0x%x flag=0x%x key=%u\n",
                (unsigned)surf, (unsigned)flag, (unsigned)key);
        s_ck++;
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_SDL_FILL_RECT: {
      cpu->r[0] = (uint32_t)mango_sdl_soft_fill(lib, r0, r1, r2);
      break;
    }
    case MANGO_LIBC_SDL_SET_VIDEO_MODE: {
      uint32_t w = r0, h = r1, bpp = r2;
      uint32_t surf;
      if (bpp != 8u && bpp != 32u) {
        bpp = 8u;
      }
      surf = mango_guest_sdl_surface_bpp(lib, w, h, bpp);
      if (surf) {
        g_fb_surf = surf;
        g_fb_lib = lib;
        fprintf(stderr, "mango: SDL_SetVideoMode %ux%u %ubpp -> surf=0x%x\n",
                (unsigned)w, (unsigned)h, (unsigned)bpp, (unsigned)surf);
      }
      cpu->r[0] = surf;
      break;
    }
    case MANGO_LIBC_SDL_FLIP: {
      if (r0) {
        mango_dump_framebuffer_pgm(lib, r0);
      } else if (g_fb_surf) {
        mango_dump_framebuffer_pgm(g_fb_lib ? g_fb_lib : lib, g_fb_surf);
      }
      cpu->r[0] = 0;
      break;
    }
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
    case MANGO_LIBC_GETENV: {
      const char* key = mango_guest_cstr(lib, r0);
      const char* val = (key && key[0]) ? getenv(key) : NULL;
      cpu->r[0] = val ? mango_guest_strdup(lib, val) : 0;
      break;
    }
    case MANGO_LIBC_STAT:
    case MANGO_LIBC_LSTAT: {
      const char* path = mango_guest_cstr(lib, r0);
      char host[768];
      struct stat st;
      int rc;
      if (!path || !mango_guest_range_ok(lib, r1, 96u)) {
        mango_set_guest_errno(lib, EFAULT);
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      if (!mango_host_map_path(lib, path, host, sizeof(host))) {
        mango_set_guest_errno(lib, ENOENT);
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      rc = (fn == MANGO_LIBC_LSTAT) ? lstat(host, &st) : stat(host, &st);
      if (rc != 0) {
        mango_set_guest_errno(lib, errno);
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      mango_store_bionic_stat(lib->guest_mem, r1, &st);
      cpu->r[0] = 0;
      break;
    }
    case MANGO_LIBC_ACCESS: {
      const char* path = mango_guest_cstr(lib, r0);
      char host[768];
      int rc;
      if (!path) {
        mango_set_guest_errno(lib, EFAULT);
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      if (!mango_host_map_path(lib, path, host, sizeof(host))) {
        mango_set_guest_errno(lib, ENOENT);
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      rc = access(host, (int)r1);
      if (rc != 0) {
        mango_set_guest_errno(lib, errno);
      }
      cpu->r[0] = (uint32_t)rc;
      break;
    }
    case MANGO_LIBC_AASSET_MGR_FROM_JAVA:
      /* Opaque manager; open/openDir ignore contents. */
      cpu->r[0] = MANGO_AASSET_MGR_HANDLE;
      break;
    case MANGO_LIBC_AASSET_MGR_OPEN: {
      /* AAssetManager_open(mgr, filename, mode) */
      const char* rel = mango_guest_cstr(lib, r1);
      char host[768];
      FILE* fp;
      long len;
      uint32_t id;
      (void)r0;
      (void)r2;
      if (!rel || !mango_aasset_host_path(lib, rel, host, sizeof(host))) {
        static int s_fail;
        if (s_fail < 12) {
          fprintf(stderr, "mango: AAssetManager_open FAIL %s\n", rel ? rel : "(null)");
          s_fail++;
        }
        cpu->r[0] = 0;
        break;
      }
      fp = fopen(host, "rb");
      if (!fp) {
        cpu->r[0] = 0;
        break;
      }
      if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        cpu->r[0] = 0;
        break;
      }
      len = ftell(fp);
      if (len < 0) {
        fclose(fp);
        cpu->r[0] = 0;
        break;
      }
      rewind(fp);
      id = mango_aasset_alloc();
      if (id == 0) {
        fclose(fp);
        cpu->r[0] = 0;
        break;
      }
      g_aassets[id].fp = fp;
      g_aassets[id].length = len;
      g_aassets[id].pos = 0;
      snprintf(g_aassets[id].host_path, sizeof(g_aassets[id].host_path), "%s", host);
      {
        static int s_ok;
        if (s_ok < 16) {
          fprintf(stderr, "mango: AAssetManager_open ok %s len=%ld id=%u\n", rel, len, id);
          s_ok++;
        }
      }
      cpu->r[0] = id;
      break;
    }
    case MANGO_LIBC_AASSET_MGR_OPENDIR: {
      /* AAssetManager_openDir(mgr, dirName) — dirName may be "". */
      const char* rel = mango_guest_cstr(lib, r1);
      char host[768];
      DIR* dir;
      uint32_t id;
      (void)r0;
      if (rel == NULL) {
        rel = "";
      }
      if (!mango_aasset_host_path(lib, rel, host, sizeof(host))) {
        static int s_fail;
        if (s_fail < 8) {
          fprintf(stderr, "mango: AAssetManager_openDir FAIL %s\n", rel[0] ? rel : "(root)");
          s_fail++;
        }
        cpu->r[0] = 0;
        break;
      }
      dir = opendir(host);
      if (!dir) {
        cpu->r[0] = 0;
        break;
      }
      id = mango_aasset_dir_alloc();
      if (id == 0) {
        closedir(dir);
        cpu->r[0] = 0;
        break;
      }
      g_aasset_dirs[id].dir = dir;
      snprintf(g_aasset_dirs[id].host_dir, sizeof(g_aasset_dirs[id].host_dir), "%s", host);
      {
        static int s_ok;
        if (s_ok < 8) {
          fprintf(stderr, "mango: AAssetManager_openDir ok %s id=%u\n", rel[0] ? rel : "(root)", id);
          s_ok++;
        }
      }
      cpu->r[0] = id;
      break;
    }
    case MANGO_LIBC_AASSET_DIR_GET_NEXT: {
      /* AAssetDir_getNextFileName — return guest cstr or NULL at EOF. */
      MangoAAssetDir* d = mango_aasset_dir_get(r0);
      struct dirent* ent;
      if (!d || !d->dir) {
        cpu->r[0] = 0;
        break;
      }
      for (;;) {
        ent = readdir(d->dir);
        if (!ent) {
          cpu->r[0] = 0;
          break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
          continue;
        }
        d->guest_name = mango_guest_strdup(lib, ent->d_name);
        cpu->r[0] = d->guest_name;
        break;
      }
      break;
    }
    case MANGO_LIBC_AASSET_DIR_CLOSE:
      mango_aasset_dir_free(r0);
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_AASSET_GET_LENGTH: {
      MangoAAsset* a = mango_aasset_get(r0);
      cpu->r[0] = a ? (uint32_t)a->length : 0;
      break;
    }
    case MANGO_LIBC_AASSET_READ: {
      /* AAsset_read(asset, buf, count) */
      MangoAAsset* a = mango_aasset_get(r0);
      size_t want = (size_t)r2;
      size_t n;
      if (!a || !a->fp || want == 0 || !mango_guest_range_ok(lib, r1, (uint32_t)want)) {
        cpu->r[0] = 0;
        break;
      }
      if (fseek(a->fp, a->pos, SEEK_SET) != 0) {
        cpu->r[0] = 0;
        break;
      }
      n = fread(lib->guest_mem + r1, 1, want, a->fp);
      a->pos += (long)n;
      cpu->r[0] = (uint32_t)n;
      break;
    }
    case MANGO_LIBC_AASSET_CLOSE:
      mango_aasset_free(r0);
      cpu->r[0] = 0;
      break;
    case MANGO_LIBC_AASSET_OPEN_FD: {
      /* AAsset_openFileDescriptor(asset, off_t* outStart, off_t* outLength) */
      MangoAAsset* a = mango_aasset_get(r0);
      int fd;
      if (!a || a->host_path[0] == '\0') {
        cpu->r[0] = (uint32_t)-1;
        break;
      }
      if (r1 && mango_guest_range_ok(lib, r1, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r1, 0);
      }
      if (r2 && mango_guest_range_ok(lib, r2, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r2, (uint32_t)a->length);
      }
      fd = open(a->host_path, O_RDONLY);
      cpu->r[0] = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
      break;
    }
    case MANGO_LIBC_SL_CREATE_ENGINE: {
      /* slCreateEngine(pEngine, numOptions, pOptions, numIfaces, pIds, pReq)
       * Must write non-null SLObjectItf into *pEngine on SUCCESS (research/49). */
      uint32_t obj;
      mango_init_opensles(lib);
      obj = mango_sl_make_itf(lib, g_sl_obj_vtable);
      if (r0 != 0 && mango_guest_range_ok(lib, r0, 4u)) {
        mango_store_u32_guest(lib->guest_mem, r0, obj);
      }
      cpu->r[0] = 0; /* SL_RESULT_SUCCESS */
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
        const char* sig = mango_guest_cstr(lib, cpu->r[3]);
        MangoJniMethod* m = mango_jni_method_alloc(name, sig);
        if (m) {
          mid = (jmethodID)m;
          if (m->kind != MANGO_MID_UNKNOWN) {
            fprintf(stderr, "mango: GetMethodID host %s %s kind=%d\n", name,
                    sig ? sig : "?", (int)m->kind);
          }
        } else {
          mid = (jmethodID)(uintptr_t)mango_handle_intern((void*)(uintptr_t)(cpu->r[2] | 1u));
        }
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
    case MANGO_JNI_GET_STATIC_OBJECT_FIELD: {
      MangoJniMethod* mid = NULL;
      if (fn == MANGO_JNI_CALL_OBJECT_METHOD || fn == MANGO_JNI_CALL_OBJECT_METHOD + 1 ||
          fn == MANGO_JNI_CALL_OBJECT_METHOD + 2) {
        mid = mango_jni_method_from_mid(cpu->r[2]);
        if (mid && mid->kind != MANGO_MID_UNKNOWN &&
            mango_jni_call_host(lib, cpu, mid, (int)(fn - MANGO_JNI_CALL_OBJECT_METHOD))) {
          break;
        }
      }
      cpu->r[0] = mango_dummy_jobject(lib);
      break;
    }
    case MANGO_JNI_CALL_BOOLEAN_METHOD:
    case MANGO_JNI_CALL_BOOLEAN_METHOD + 1:
    case MANGO_JNI_CALL_BOOLEAN_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_BOOLEAN_METHOD + 2:
    case MANGO_JNI_GET_BOOLEAN_FIELD: {
      MangoJniMethod* mid = NULL;
      if (fn == MANGO_JNI_CALL_BOOLEAN_METHOD || fn == MANGO_JNI_CALL_BOOLEAN_METHOD + 1 ||
          fn == MANGO_JNI_CALL_BOOLEAN_METHOD + 2) {
        mid = mango_jni_method_from_mid(cpu->r[2]);
        if (mid && mid->kind != MANGO_MID_UNKNOWN &&
            mango_jni_call_host(lib, cpu, mid, (int)(fn - MANGO_JNI_CALL_BOOLEAN_METHOD))) {
          break;
        }
      }
      cpu->r[0] = 1;
      break;
    }
    case MANGO_JNI_CALL_INT_METHOD:
    case MANGO_JNI_CALL_INT_METHOD + 1:
    case MANGO_JNI_CALL_INT_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_INT_METHOD:
    case MANGO_JNI_CALL_STATIC_INT_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_INT_METHOD + 2:
    case MANGO_JNI_GET_INT_FIELD:
    case MANGO_JNI_GET_STATIC_INT_FIELD: {
      MangoJniMethod* mid = NULL;
      if (fn == MANGO_JNI_CALL_INT_METHOD || fn == MANGO_JNI_CALL_INT_METHOD + 1 ||
          fn == MANGO_JNI_CALL_INT_METHOD + 2) {
        mid = mango_jni_method_from_mid(cpu->r[2]);
        if (mid && mid->kind != MANGO_MID_UNKNOWN &&
            mango_jni_call_host(lib, cpu, mid, (int)(fn - MANGO_JNI_CALL_INT_METHOD))) {
          break;
        }
      }
      cpu->r[0] = 1;
      break;
    }
    case MANGO_JNI_CALL_FLOAT_METHOD:
    case MANGO_JNI_CALL_FLOAT_METHOD + 1:
    case MANGO_JNI_CALL_FLOAT_METHOD + 2: {
      MangoJniMethod* mid = mango_jni_method_from_mid(cpu->r[2]);
      if (mid && mid->kind != MANGO_MID_UNKNOWN &&
          mango_jni_call_host(lib, cpu, mid, (int)(fn - MANGO_JNI_CALL_FLOAT_METHOD))) {
        break;
      }
      cpu->r[0] = 0;
      break;
    }
    case MANGO_JNI_CALL_VOID_METHOD:
    case MANGO_JNI_CALL_VOID_METHOD + 1:
    case MANGO_JNI_CALL_VOID_METHOD + 2:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD + 1:
    case MANGO_JNI_CALL_STATIC_VOID_METHOD + 2:
    case MANGO_JNI_SET_OBJECT_FIELD:
    case MANGO_JNI_SET_INT_FIELD: {
      MangoJniMethod* mid = NULL;
      if (fn == MANGO_JNI_CALL_VOID_METHOD || fn == MANGO_JNI_CALL_VOID_METHOD + 1 ||
          fn == MANGO_JNI_CALL_VOID_METHOD + 2) {
        mid = mango_jni_method_from_mid(cpu->r[2]);
        if (mid && mid->kind != MANGO_MID_UNKNOWN &&
            mango_jni_call_host(lib, cpu, mid, (int)(fn - MANGO_JNI_CALL_VOID_METHOD))) {
          break;
        }
      }
      cpu->r[0] = 0;
      break;
    }
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
    case MANGO_JNI_GET_ARRAY_LENGTH: {
      MangoJIntArray* a = mango_jintarray_from_handle(cpu->r[1]);
      cpu->r[0] = a ? (uint32_t)a->length : 0;
      break;
    }
    case MANGO_JNI_NEW_INT_ARRAY: {
      int n = (int)cpu->r[1];
      MangoJIntArray* a;
      uint32_t ga;
      if (n < 0) {
        n = 0;
      }
      if (n > 16 * 1024 * 1024) {
        cpu->r[0] = 0;
        break;
      }
      a = mango_jintarray_alloc();
      if (!a) {
        cpu->r[0] = 0;
        break;
      }
      ga = n > 0 ? mango_guest_alloc(lib, (uint32_t)n * 4u) : 0;
      if (n > 0 && ga == 0) {
        a->used = 0;
        cpu->r[0] = 0;
        break;
      }
      a->length = n;
      a->guest_elems = ga;
      cpu->r[0] = mango_handle_intern(a);
      break;
    }
    case MANGO_JNI_GET_INT_ARRAY_ELEMENTS: {
      MangoJIntArray* a = mango_jintarray_from_handle(cpu->r[1]);
      if (!a) {
        cpu->r[0] = 0;
        break;
      }
      /* isCopy out-param: report not-a-copy so Release may no-op free. */
      if (cpu->r[2] && mango_guest_range_ok(lib, cpu->r[2], 1u)) {
        lib->guest_mem[cpu->r[2]] = 0;
      }
      cpu->r[0] = a->guest_elems; /* raw guest jint* for Texture.pixels */
      break;
    }
    case MANGO_JNI_RELEASE_INT_ARRAY_ELEMENTS:
      /* Guest keeps guest_elems pointer in Texture; mode ignored. */
      cpu->r[0] = 0;
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
    /* ART shorty: one char per type (Z/B/S/C/I/J/F/D/V/L/[). Do NOT use
     * mango_consume_jni_type here — that treats L as JNI Lfully/Name; and
     * collapses VIILLL → IIL, dropping Asset/Store jobjects (Pacman Q3). */
    const char* p = shorty;
    if (*p == '\0') {
      return -1;
    }
    *ret = *p++;
    while (*p) {
      if (n + 1 >= args_max) {
        return -1;
      }
      args[n++] = *p++;
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
            (imm >= MANGO_LIBC_SVC_BASE && imm < MANGO_LIBC_SVC_BASE + MANGO_LIBC_COUNT) ||
            (imm >= MANGO_SL_SVC_BASE && imm < MANGO_SL_SVC_BASE + MANGO_SL_COUNT)) {
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
    } else if (nr >= MANGO_SL_SVC_BASE && nr < MANGO_SL_SVC_BASE + MANGO_SL_COUNT) {
      mango_sl_svc(lib, cpu, nr - MANGO_SL_SVC_BASE);
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


/* SDL 1.2 software surface. bpp 8 (Meritous screen/tiles) or 32 (RGBA). */
static uint32_t mango_guest_sdl_surface_bpp(MangoLoadedLibrary* lib, uint32_t w, uint32_t h,
                                           uint32_t bpp) {
  uint32_t fmt, surf, pixels, pitch, nbytes, bpp_bytes;
  uint32_t pal = 0, colors = 0;
  if (w == 0 || h == 0 || w > 4096u || h > 4096u) {
    return 0;
  }
  if (bpp != 8u && bpp != 32u) {
    return 0;
  }
  bpp_bytes = bpp / 8u;
  pitch = w * bpp_bytes;
  nbytes = pitch * h;
  fmt = mango_guest_alloc(lib, 64u);
  surf = mango_guest_alloc(lib, 64u);
  pixels = mango_guest_alloc(lib, nbytes);
  if (!fmt || !surf || !pixels) {
    return 0;
  }
  if (bpp == 8u) {
    pal = mango_guest_alloc(lib, 16u);
    colors = mango_guest_alloc(lib, 256u * 4u);
    if (!pal || !colors) {
      return 0;
    }
    {
      uint32_t i;
      for (i = 0; i < 256u; i++) {
        lib->guest_mem[colors + i * 4u + 0u] = (uint8_t)i;
        lib->guest_mem[colors + i * 4u + 1u] = (uint8_t)i;
        lib->guest_mem[colors + i * 4u + 2u] = (uint8_t)i;
        lib->guest_mem[colors + i * 4u + 3u] = 0;
      }
    }
    mango_store_u32_guest(lib->guest_mem, pal + 0u, 256u);
    mango_store_u32_guest(lib->guest_mem, pal + 4u, colors);
    mango_store_u32_guest(lib->guest_mem, fmt + 0u, pal);
    lib->guest_mem[fmt + 4u] = 8;
    lib->guest_mem[fmt + 5u] = 1;
    mango_store_u32_guest(lib->guest_mem, fmt + 16u, 0u);
    mango_store_u32_guest(lib->guest_mem, fmt + 20u, 0u);
    mango_store_u32_guest(lib->guest_mem, fmt + 24u, 0u);
    mango_store_u32_guest(lib->guest_mem, fmt + 28u, 0u);
  } else {
    mango_store_u32_guest(lib->guest_mem, fmt + 0u, 0u);
    lib->guest_mem[fmt + 4u] = 32;
    lib->guest_mem[fmt + 5u] = 4;
    mango_store_u32_guest(lib->guest_mem, fmt + 16u, 0x000000ffu);
    mango_store_u32_guest(lib->guest_mem, fmt + 20u, 0x0000ff00u);
    mango_store_u32_guest(lib->guest_mem, fmt + 24u, 0x00ff0000u);
    mango_store_u32_guest(lib->guest_mem, fmt + 28u, 0xff000000u);
  }
  mango_store_u32_guest(lib->guest_mem, surf + 0u, 0u);
  mango_store_u32_guest(lib->guest_mem, surf + 4u, fmt);
  mango_store_u32_guest(lib->guest_mem, surf + 8u, w);
  mango_store_u32_guest(lib->guest_mem, surf + 12u, h);
  lib->guest_mem[surf + 16u] = (uint8_t)(pitch & 0xffu);
  lib->guest_mem[surf + 17u] = (uint8_t)((pitch >> 8) & 0xffu);
  mango_store_u32_guest(lib->guest_mem, surf + 20u, pixels);
  lib->guest_mem[surf + 32u] = 0;
  lib->guest_mem[surf + 33u] = 0;
  lib->guest_mem[surf + 34u] = 0;
  lib->guest_mem[surf + 35u] = 0;
  lib->guest_mem[surf + 36u] = (uint8_t)(w & 0xffu);
  lib->guest_mem[surf + 37u] = (uint8_t)((w >> 8) & 0xffu);
  lib->guest_mem[surf + 38u] = (uint8_t)(h & 0xffu);
  lib->guest_mem[surf + 39u] = (uint8_t)((h >> 8) & 0xffu);
  mango_store_u32_guest(lib->guest_mem, surf + 56u, 1u);
  return surf;
}

static uint32_t mango_guest_sdl_surface(MangoLoadedLibrary* lib, uint32_t w, uint32_t h) {
  return mango_guest_sdl_surface_bpp(lib, w, h, 32u);
}

static int16_t mango_load_i16_guest(const uint8_t* mem, uint32_t addr) {
  return (int16_t)(uint16_t)((uint16_t)mem[addr] | ((uint16_t)mem[addr + 1u] << 8));
}

static void mango_store_i16_guest(uint8_t* mem, uint32_t addr, int16_t v) {
  uint16_t u = (uint16_t)v;
  mem[addr] = (uint8_t)(u & 0xffu);
  mem[addr + 1u] = (uint8_t)((u >> 8) & 0xffu);
}

static int mango_sdl_read_rect(const uint8_t* mem, uint32_t addr, int* x, int* y, int* w,
                               int* h) {
  if (addr == 0) {
    return 0;
  }
  *x = mango_load_i16_guest(mem, addr);
  *y = mango_load_i16_guest(mem, addr + 2u);
  *w = (int)(uint16_t)((uint16_t)mem[addr + 4u] | ((uint16_t)mem[addr + 5u] << 8));
  *h = (int)(uint16_t)((uint16_t)mem[addr + 6u] | ((uint16_t)mem[addr + 7u] << 8));
  return 1;
}

static int mango_sdl_soft_blit(MangoLoadedLibrary* lib, uint32_t src, uint32_t srcrect,
                               uint32_t dst, uint32_t dstrect) {
  uint32_t sfmt, dfmt, spix, dpix;
  uint32_t sw, sh, dw, dh, spitch, dpitch;
  uint8_t sbpp, dbpp;
  int sx = 0, sy = 0, swid, shgt;
  int dx = 0, dy = 0, dwid, dhgt;
  int row;
  if (!lib || !src || !dst) {
    return -1;
  }
  if (!mango_guest_range_ok(lib, src, 60u) || !mango_guest_range_ok(lib, dst, 60u)) {
    return -1;
  }
  sfmt = mango_load_u32_guest(lib->guest_mem, src + 4u);
  dfmt = mango_load_u32_guest(lib->guest_mem, dst + 4u);
  sw = mango_load_u32_guest(lib->guest_mem, src + 8u);
  sh = mango_load_u32_guest(lib->guest_mem, src + 12u);
  dw = mango_load_u32_guest(lib->guest_mem, dst + 8u);
  dh = mango_load_u32_guest(lib->guest_mem, dst + 12u);
  spitch = (uint32_t)lib->guest_mem[src + 16u] | ((uint32_t)lib->guest_mem[src + 17u] << 8);
  dpitch = (uint32_t)lib->guest_mem[dst + 16u] | ((uint32_t)lib->guest_mem[dst + 17u] << 8);
  spix = mango_load_u32_guest(lib->guest_mem, src + 20u);
  dpix = mango_load_u32_guest(lib->guest_mem, dst + 20u);
  if (!sfmt || !dfmt || !spix || !dpix || !mango_guest_range_ok(lib, sfmt, 8u) ||
      !mango_guest_range_ok(lib, dfmt, 8u)) {
    return -1;
  }
  sbpp = lib->guest_mem[sfmt + 5u];
  dbpp = lib->guest_mem[dfmt + 5u];
  if (sbpp == 0 || dbpp == 0 || sbpp != dbpp) {
    return -1;
  }
  swid = (int)sw;
  shgt = (int)sh;
  if (!mango_sdl_read_rect(lib->guest_mem, srcrect, &sx, &sy, &swid, &shgt)) {
    sx = sy = 0;
    swid = (int)sw;
    shgt = (int)sh;
  }
  dwid = swid;
  dhgt = shgt;
  if (!mango_sdl_read_rect(lib->guest_mem, dstrect, &dx, &dy, &dwid, &dhgt)) {
    dx = dy = 0;
  }
  (void)dwid;
  (void)dhgt;
  if (sx < 0) { dx -= sx; swid += sx; sx = 0; }
  if (sy < 0) { dy -= sy; shgt += sy; sy = 0; }
  if (sx + swid > (int)sw) { swid = (int)sw - sx; }
  if (sy + shgt > (int)sh) { shgt = (int)sh - sy; }
  if (dx < 0) { sx -= dx; swid += dx; dx = 0; }
  if (dy < 0) { sy -= dy; shgt += dy; dy = 0; }
  if (dx + swid > (int)dw) { swid = (int)dw - dx; }
  if (dy + shgt > (int)dh) { shgt = (int)dh - dy; }
  if (swid <= 0 || shgt <= 0) {
    return 0;
  }
  if (!mango_guest_range_ok(lib, spix, spitch * sh) ||
      !mango_guest_range_ok(lib, dpix, dpitch * dh)) {
    return -1;
  }
  {
    uint32_t sflags = mango_load_u32_guest(lib->guest_mem, src + 0u);
    int use_ckey = (sflags & 0x00001000u) != 0;
    uint32_t ckey = 0;
    if (use_ckey && mango_guest_range_ok(lib, sfmt, 40u)) {
      ckey = mango_load_u32_guest(lib->guest_mem, sfmt + 32u);
    } else {
      use_ckey = 0;
    }
    if (!use_ckey) {
      for (row = 0; row < shgt; row++) {
        uint8_t* srow =
            lib->guest_mem + spix + (uint32_t)(sy + row) * spitch + (uint32_t)sx * sbpp;
        uint8_t* drow =
            lib->guest_mem + dpix + (uint32_t)(dy + row) * dpitch + (uint32_t)dx * dbpp;
        memcpy(drow, srow, (size_t)swid * (size_t)sbpp);
      }
    } else if (sbpp == 1) {
      uint8_t key8 = (uint8_t)(ckey & 0xffu);
      int col;
      uint64_t skipped = 0;
      for (row = 0; row < shgt; row++) {
        uint8_t* srow =
            lib->guest_mem + spix + (uint32_t)(sy + row) * spitch + (uint32_t)sx;
        uint8_t* drow =
            lib->guest_mem + dpix + (uint32_t)(dy + row) * dpitch + (uint32_t)dx;
        for (col = 0; col < swid; col++) {
          if (srow[col] == key8) {
            skipped++;
          } else {
            drow[col] = srow[col];
          }
        }
      }
      g_ckey_blit_count++;
      g_ckey_pixels_skipped += skipped;
    } else if (sbpp == 4) {
      int col;
      uint64_t skipped = 0;
      for (row = 0; row < shgt; row++) {
        uint32_t* srow = (uint32_t*)(lib->guest_mem + spix +
                                     (uint32_t)(sy + row) * spitch + (uint32_t)sx * 4u);
        uint32_t* drow = (uint32_t*)(lib->guest_mem + dpix +
                                     (uint32_t)(dy + row) * dpitch + (uint32_t)dx * 4u);
        for (col = 0; col < swid; col++) {
          if (srow[col] == ckey) {
            skipped++;
          } else {
            drow[col] = srow[col];
          }
        }
      }
      g_ckey_blit_count++;
      g_ckey_pixels_skipped += skipped;
    } else {
      return -1;
    }
  }
  g_blit_count++;
  g_blit_bytes += (uint64_t)swid * (uint64_t)shgt * (uint64_t)sbpp;
  if (dst == g_fb_surf || (dw >= 640u && dh >= 480u)) {
    g_fb_surf = dst;
    g_fb_lib = lib;
  }
  /* Frame dump deferred to SDL_Flip (after DrawPlayer/entities). */
  if (dstrect) {
    mango_store_i16_guest(lib->guest_mem, dstrect, (int16_t)dx);
    mango_store_i16_guest(lib->guest_mem, dstrect + 2u, (int16_t)dy);
    lib->guest_mem[dstrect + 4u] = (uint8_t)(swid & 0xff);
    lib->guest_mem[dstrect + 5u] = (uint8_t)((swid >> 8) & 0xff);
    lib->guest_mem[dstrect + 6u] = (uint8_t)(shgt & 0xff);
    lib->guest_mem[dstrect + 7u] = (uint8_t)((shgt >> 8) & 0xff);
  }
  return 0;
}

static int mango_sdl_soft_fill(MangoLoadedLibrary* lib, uint32_t dst, uint32_t dstrect,
                               uint32_t color) {
  uint32_t fmt, pix, w, h, pitch;
  uint8_t bpp;
  int x = 0, y = 0, rw, rh;
  int row;
  if (!lib || !dst || !mango_guest_range_ok(lib, dst, 60u)) {
    return -1;
  }
  fmt = mango_load_u32_guest(lib->guest_mem, dst + 4u);
  w = mango_load_u32_guest(lib->guest_mem, dst + 8u);
  h = mango_load_u32_guest(lib->guest_mem, dst + 12u);
  pitch = (uint32_t)lib->guest_mem[dst + 16u] | ((uint32_t)lib->guest_mem[dst + 17u] << 8);
  pix = mango_load_u32_guest(lib->guest_mem, dst + 20u);
  if (!fmt || !pix || !mango_guest_range_ok(lib, fmt, 8u)) {
    return -1;
  }
  bpp = lib->guest_mem[fmt + 5u];
  if (bpp == 0) {
    return -1;
  }
  rw = (int)w;
  rh = (int)h;
  if (!mango_sdl_read_rect(lib->guest_mem, dstrect, &x, &y, &rw, &rh)) {
    x = y = 0;
    rw = (int)w;
    rh = (int)h;
  }
  if (x < 0) { rw += x; x = 0; }
  if (y < 0) { rh += y; y = 0; }
  if (x + rw > (int)w) { rw = (int)w - x; }
  if (y + rh > (int)h) { rh = (int)h - y; }
  if (rw <= 0 || rh <= 0) {
    return 0;
  }
  if (!mango_guest_range_ok(lib, pix, pitch * h)) {
    return -1;
  }
  if (bpp == 1) {
    uint8_t c = (uint8_t)(color & 0xffu);
    for (row = 0; row < rh; row++) {
      memset(lib->guest_mem + pix + (uint32_t)(y + row) * pitch + (uint32_t)x, c,
             (size_t)rw);
    }
  } else if (bpp == 4) {
    for (row = 0; row < rh; row++) {
      uint32_t* rowp =
          (uint32_t*)(lib->guest_mem + pix + (uint32_t)(y + row) * pitch + (uint32_t)x * 4u);
      int col;
      for (col = 0; col < rw; col++) {
        rowp[col] = color;
      }
    }
  } else {
    return -1;
  }
  g_fill_count++;
  if (dst == g_fb_surf || (w >= 640u && h >= 480u)) {
    g_fb_surf = dst;
    g_fb_lib = lib;
  }
  return 0;
}


static void mango_meritous_host_draw_text(MangoLoadedLibrary* lib, uint32_t x0, uint32_t y0,
                                          uint32_t str_addr, uint32_t color) {
  static uint8_t raw[8192];
  uint32_t surf = g_fb_surf;
  uint32_t fmt, pix, w, h, pitch;
  uint8_t bpp, ink;
  const char* s;
  int x, y, start_x;
  if (!lib || !str_addr || !surf) {
    return;
  }
  if (!mango_guest_range_ok(lib, surf, 60u)) {
    return;
  }
  if (!g_meritous_font_ready) {
    char host_path[768];
    FILE* fp;
    size_t n;
    unsigned gi, b;
    if (!mango_host_resolve_path(lib, "dat/d/font.dat", host_path, sizeof(host_path))) {
      return;
    }
    fp = fopen(host_path, "rb");
    if (!fp) {
      return;
    }
    n = fread(raw, 1, sizeof(raw), fp);
    fclose(fp);
    if (n < 8192u) {
      return;
    }
    memset(g_meritous_font, 0, sizeof(g_meritous_font));
    for (gi = 0; gi < 256u; gi++) {
      const uint8_t* src = raw + gi * 32u;
      uint8_t* dst = g_meritous_font + gi * 64u;
      for (b = 0; b < 32u; b++) {
        dst[(b % 4u) * 8u + (b / 4u)] = src[b];
      }
    }
    g_meritous_font_ready = 1;
    fprintf(stderr, "mango: Meritous host font.dat loaded for draw_text\n");
  }
  fmt = mango_load_u32_guest(lib->guest_mem, surf + 4u);
  w = mango_load_u32_guest(lib->guest_mem, surf + 8u);
  h = mango_load_u32_guest(lib->guest_mem, surf + 12u);
  pitch = (uint32_t)lib->guest_mem[surf + 16u] | ((uint32_t)lib->guest_mem[surf + 17u] << 8);
  pix = mango_load_u32_guest(lib->guest_mem, surf + 20u);
  if (!fmt || !pix || !mango_guest_range_ok(lib, fmt, 8u)) {
    return;
  }
  bpp = lib->guest_mem[fmt + 5u];
  if (bpp != 1u || w == 0u || h == 0u || pitch < w ||
      !mango_guest_range_ok(lib, pix, pitch * h)) {
    return;
  }
  s = mango_guest_cstr(lib, str_addr);
  if (!s) {
    return;
  }
  ink = (uint8_t)(color & 0xffu);
  if (ink == 0) {
    ink = 0xffu;
  }
  x = (int)x0;
  y = (int)y0;
  start_x = x;
  for (; *s; s++) {
    unsigned char ch = (unsigned char)*s;
    int row, col;
    const uint8_t* glyph;
    if (ch == '\n') {
      y += 10;
      x = start_x;
      continue;
    }
    if (x >= 0 && y >= 0 && (uint32_t)(x + 8) <= w && (uint32_t)(y + 8) <= h) {
      glyph = g_meritous_font + ((unsigned)ch) * 64u;
      for (row = 0; row < 8; row++) {
        uint8_t* dst = lib->guest_mem + pix + (uint32_t)(y + row) * pitch + (uint32_t)x;
        for (col = 0; col < 8; col++) {
          uint8_t a = glyph[(unsigned)col * 8u + (unsigned)row];
          if (a != 0xffu && a >= 0x40u) {
            dst[col] = ink;
          }
        }
      }
    }
    x += 8;
  }
  g_meritous_draw_text_calls++;
}


/* Midpoint ring on 8bpp guest fb (Meritous screen). */
static void mango_meritous_soft_circle(MangoLoadedLibrary* lib, uint32_t pix, uint32_t pitch,
                                       uint32_t w, uint32_t h, int cx, int cy, int radius,
                                       uint8_t ink) {
  int x = radius;
  int y = 0;
  int err = 1 - x;
  if (!lib || radius <= 0) {
    return;
  }
  while (x >= y) {
    const int pts[8][2] = {{cx + x, cy + y}, {cx + y, cy + x}, {cx - y, cy + x}, {cx - x, cy + y},
                           {cx - x, cy - y}, {cx - y, cy - x}, {cx + y, cy - x}, {cx + x, cy - y}};
    for (int i = 0; i < 8; i++) {
      int px = pts[i][0];
      int py = pts[i][1];
      if (px >= 0 && py >= 0 && (uint32_t)px < w && (uint32_t)py < h) {
        lib->guest_mem[pix + (uint32_t)py * pitch + (uint32_t)px] = ink;
      }
    }
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

/* Crude degree-step arc on 8bpp fb (proof paint; not guest Arc). */
static void mango_meritous_soft_arc(MangoLoadedLibrary* lib, uint32_t pix, uint32_t pitch,
                                    uint32_t w, uint32_t h, int cx, int cy, int radius,
                                    int deg0, int deg1, uint8_t ink) {
  if (!lib || radius <= 0 || deg1 <= deg0) {
    return;
  }
  for (int d = deg0; d <= deg1; d++) {
    double rad = (double)d * 3.14159265358979323846 / 180.0;
    int px = cx + (int)(radius * cos(rad) + 0.5);
    int py = cy + (int)(radius * sin(rad) + 0.5);
    if (px >= 0 && py >= 0 && (uint32_t)px < w && (uint32_t)py < h) {
      lib->guest_mem[pix + (uint32_t)py * pitch + (uint32_t)px] = ink;
    }
    if (px + 1 >= 0 && (uint32_t)(px + 1) < w && py >= 0 && (uint32_t)py < h) {
      lib->guest_mem[pix + (uint32_t)py * pitch + (uint32_t)(px + 1)] = ink;
    }
    if (py + 1 >= 0 && (uint32_t)(py + 1) < h && px >= 0 && (uint32_t)px < w) {
      lib->guest_mem[pix + (uint32_t)(py + 1) * pitch + (uint32_t)px] = ink;
    }
  }
}

static void mango_dump_framebuffer_pgm(MangoLoadedLibrary* lib, uint32_t surf) {
  uint32_t fmt, pix, w, h, pitch;
  uint8_t bpp;
  const char* out_path;
  FILE* fp;
  uint32_t y, i, nbytes, nonzero = 0;
  if (g_frame_dumped || !lib || !surf) {
    return;
  }
  if (!mango_guest_range_ok(lib, surf, 60u)) {
    return;
  }
  fmt = mango_load_u32_guest(lib->guest_mem, surf + 4u);
  w = mango_load_u32_guest(lib->guest_mem, surf + 8u);
  h = mango_load_u32_guest(lib->guest_mem, surf + 12u);
  pitch = (uint32_t)lib->guest_mem[surf + 16u] | ((uint32_t)lib->guest_mem[surf + 17u] << 8);
  pix = mango_load_u32_guest(lib->guest_mem, surf + 20u);
  if (!fmt || !pix || !mango_guest_range_ok(lib, fmt, 8u)) {
    return;
  }
  bpp = lib->guest_mem[fmt + 5u];
  if (bpp != 1 || w == 0 || h == 0) {
    fprintf(stderr, "mango: frame dump skip (bpp=%u %ux%u)\n", (unsigned)bpp, (unsigned)w,
            (unsigned)h);
    return;
  }
  if (!mango_guest_range_ok(lib, pix, pitch * h)) {
    return;
  }
  nbytes = w * h;
  for (i = 0; i < nbytes; i++) {
    uint32_t row = i / w;
    uint32_t col = i % w;
    if (lib->guest_mem[pix + row * pitch + col] != 0) {
      nonzero++;
    }
  }
  out_path = getenv("MANGO_FRAME_DUMP");
  if (!out_path || !out_path[0]) {
    out_path = "/workspace/mango/fixtures/meritous/frame-dungeon.pgm";
  }
  /* One-dungeon New Game leaves meter FX idle; force rings/arcs for stop-4k/4l proof.
   * Dump itself is one-shot (g_frame_dumped). */
  if (g_meritous_progress_armed) {
    int cx = (int)(w / 2u);
    int cy = (int)(h / 2u);
    /* 4k: Circle/Shield-style rings */
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 48, 220);
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 64, 180);
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 72, 160);
    /* 4l: CircleEx-style thick ring + Arc-style quarter sweeps (distinct inks) */
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 88, 240);
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 90, 240);
    mango_meritous_soft_circle(lib, pix, pitch, w, h, cx, cy, 92, 240);
    mango_meritous_soft_arc(lib, pix, pitch, w, h, cx, cy, 110, 10, 80, 130);
    mango_meritous_soft_arc(lib, pix, pitch, w, h, cx, cy, 120, 100, 170, 110);
    fprintf(stderr,
            "mango: Meritous forced one-shot CircleEx/Arc paint (meter idle in drive)\n");
    /* Recount nonzero after paint. */
    nonzero = 0;
    for (i = 0; i < nbytes; i++) {
      uint32_t row = i / w;
      uint32_t col = i % w;
      if (lib->guest_mem[pix + row * pitch + col] != 0) {
        nonzero++;
      }
    }
  }
  fp = fopen(out_path, "wb");
  if (!fp) {
    fprintf(stderr, "mango: frame dump fopen failed '%s'\n", out_path);
    return;
  }
  fprintf(fp, "P5\n%u %u\n255\n", (unsigned)w, (unsigned)h);
  for (y = 0; y < h; y++) {
    fwrite(lib->guest_mem + pix + y * pitch, 1, w, fp);
  }
  fclose(fp);
  g_frame_dumped = 1;
  fprintf(stderr,
          "mango: dumped framebuffer %ux%u pgm '%s' (%u nonzero, blits=%llu fills=%llu "
          "bytes=%llu ckey_blits=%llu ckey_skip=%llu)\n",
          (unsigned)w, (unsigned)h, out_path, (unsigned)nonzero,
          (unsigned long long)g_blit_count, (unsigned long long)g_fill_count,
          (unsigned long long)g_blit_bytes, (unsigned long long)g_ckey_blit_count,
          (unsigned long long)g_ckey_pixels_skipped);
  /* One-dungeon drive: after first framebuffer dump, arm title→exit so return
   * to title does not start a second NewLevel (bump-heap OOM / NULL fill). */
  if (g_meritous_progress_armed && (g_meritous_seen_mask & 7u) &&
      !(g_meritous_seen_mask & 32u)) {
    g_meritous_seen_mask |= 32u;
  }
  if (g_meritous_progress_armed && (g_meritous_seen_mask & 32u) &&
      !(g_meritous_seen_mask & 16u)) {
    mango_meritous_progress(g_meritous_bias + 0x1ccd8u);
  }
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
  /* Basename alone is insufficient: OpenTTD (pelya/F-Droid) also ships
   * libapplication.so. Require a Meritous-only export so Meritous patches
   * stay basename+symbol gated and OpenTTD stays ungated for those hooks. */
  if (strcmp(base, "libapplication.so") != 0) {
    return 0;
  }
  return mango_elf32_find_symbol(&lib->image, "InitEnemies") != 0 ||
         mango_elf32_find_symbol(&lib->image, "DrawCircuit") != 0;
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
  /* VideoUpdate left live: SDL_UpdateRect dumps the painted frame after DrawPlayer. */
  /* draw_text @ ELF VA 0x17f4c → host soft glyphs (font.dat); guest draw_char is slow/empty. */
  addr = lib->load_bias + 0x17f4cu;
  if (mango_guest_range_ok(lib, addr, 8u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 0x46c04778u); /* bx pc; nop */
    mango_write_jni_thunk(lib->guest_mem, addr + 4u, MANGO_LIBC_SVC_BASE + MANGO_LIBC_DRAW_TEXT);
    fprintf(stderr, "mango: Meritous draw_text -> host soft glyphs\n");
  }
  /* DrawLevel full-screen clear can race HUD; keep status band y<32. */
  addr = lib->load_bias + 0x1859cu;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x20u;
    lib->guest_mem[addr + 1u] = 0x21u; /* movs r1, #0x20 (y=32) */
  }
  addr = lib->load_bias + 0x18592u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0xe0u;
    lib->guest_mem[addr + 1u] = 0x23u; /* movs r3, #0xe0; <<1 => h=448 */
    fprintf(stderr, "mango: Meritous DrawLevel clear preserves HUD y<32\n");
  }

  /* Gameplay draw path: Circle/CircleEx/Shield/Circuit/Arc live; Artifacts+SetTonedPalette stubbed.
   * Meters idle on one-dungeon drive → forced host CircleEx/Arc paint at frame dump (stop 4l). */
  {
    /* DrawLevel + DrawPlayer + DrawEntities + Circle/CircleEx/Shield/Circuit/Arc live;
     * draw_text host-thunked. Keep Artifacts + SetTonedPalette stubbed (palette churn). */
    static const uint32_t kBx[] = {
        0x1a540u, /* DrawArtifacts */
        0x16c70u, /* SetTonedPalette */
    };
    for (unsigned i = 0; i < sizeof(kBx) / sizeof(kBx[0]); i++) {
      addr = lib->load_bias + kBx[i];
      if (mango_guest_range_ok(lib, addr, 2u)) {
        lib->guest_mem[addr + 0u] = 0x70u;
        lib->guest_mem[addr + 1u] = 0x47u; /* bx lr */
      }
    }
    fprintf(stderr,
            "mango: Meritous live Circle/CircleEx/Shield/Circuit/Arc; host draw_text; stub Artifacts/Toned\n");
  }

  /* Title loop HEAD: first visit -> New Game; after mapgen/dungeon progress the
   * progress hook rewrites this site to exit so SDL_main returns cleanly. */
  addr = lib->load_bias + 0x1ccd8u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x55u;
    lib->guest_mem[addr + 1u] = 0xe1u; /* b 0x1cf86 New Game */
    fprintf(stderr, "mango: Meritous title head -> force New Game\n");
  }

  /* DungeonPlay left intact — title head branches to New Game at 0x1cf86. */

  /* Generate() → 3000 rooms (vanilla). Dist gate >20. InitEnemies: full
   * walk →3000 with place give-up (skip cr_w/h<2, n_enemies=5,
   * IsSolid-fail→next room), type-5 50→2 at even VA — stop 4m density. */
  addr = lib->load_bias + 0x1e61cu;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 3000u);
    fprintf(stderr, "mango: Meritous Generate room target restored ->3000\n");
  }
  addr = lib->load_bias + 0x1e5c2u;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x14u;
    lib->guest_mem[addr + 1u] = 0x29u; /* cmp r1, #0x14 */
    fprintf(stderr, "mango: Meritous Generate dist gate >49 -> >20\n");
  }
  /* Skip empty/tiny rooms then force n_enemies=5. Even Thumb; 0x1343a..0x1344f.
   * Keep IsSolid give-up; do not denser/4 or NOP-fallthrough retries. */
  addr = lib->load_bias + 0x1343au;
  if (mango_guest_range_ok(lib, addr, 22u)) {
    static const uint8_t kSkipTiny[] = {
        0x02u, 0x3bu, /* subs r3, #2 */
        0xb7u, 0x1eu, /* subs r7, r6, #2 */
        0x02u, 0x2bu, /* cmp r3, #2 */
        0x70u, 0xdbu, /* blt 0x13524 (next room) */
        0x02u, 0x2fu, /* cmp r7, #2 */
        0x6eu, 0xdbu, /* blt 0x13524 */
        0x5fu, 0x43u, /* muls r7, r3, r7 */
        0x05u, 0x21u, /* movs r1, #5 (was #1; stop 4m density) */
        0x03u, 0x91u, /* str r1, [sp, #0xc] */
        0x00u, 0x00u, /* movs r0, r0 (nop; interp lacks Thumb2 bf00) */
        0x00u, 0x00u, /* movs r0, r0 */
    };
    for (unsigned i = 0; i < sizeof(kSkipTiny); i++) {
      lib->guest_mem[addr + i] = kSkipTiny[i];
    }
    fprintf(stderr, "mango: Meritous InitEnemies skip cr_w/h<2 + n_enemies=5\n");
  }
  /* IsSolid fail -> next room (was retry place forever on solid rooms). */
  {
    static const struct { uint32_t off; uint8_t b0, b1; } kGive[] = {
        {0x134c2u, 0x2fu, 0xd1u}, /* bne 0x13524 */
        {0x134e0u, 0x20u, 0xd1u},
        {0x134fcu, 0x12u, 0xd1u},
        {0x1350cu, 0x0au, 0xd1u},
    };
    for (unsigned i = 0; i < sizeof(kGive) / sizeof(kGive[0]); i++) {
      addr = lib->load_bias + kGive[i].off;
      if (mango_guest_range_ok(lib, addr, 2u)) {
        lib->guest_mem[addr + 0u] = kGive[i].b0;
        lib->guest_mem[addr + 1u] = kGive[i].b1;
      }
    }
    fprintf(stderr, "mango: Meritous InitEnemies IsSolid fail -> next room\n");
  }
  addr = lib->load_bias + 0x1345cu;
  if (mango_guest_range_ok(lib, addr, 2u)) {
    lib->guest_mem[addr + 0u] = 0x02u;
    lib->guest_mem[addr + 1u] = 0x23u; /* movs r3, #2 (was #50 type-5) */
    fprintf(stderr, "mango: Meritous InitEnemies type5 enemies 50->2\n");
  }
  addr = lib->load_bias + 0x135c0u;
  if (mango_guest_range_ok(lib, addr, 4u)) {
    mango_store_u32_guest(lib->guest_mem, addr, 3000u * 0x34u);
    fprintf(stderr, "mango: Meritous InitEnemies room loop ->3000 (of 3000)\n");
  }

  g_meritous_progress_armed = 1;
  g_meritous_bias = lib->load_bias;
  g_meritous_lib = lib;
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
