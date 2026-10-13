/* research/107: differential test of the OpenTTD PaletteAnimate skip-loop
 * host fast path against the interpreter executing the real guest bytes
 * (libapplication.so 1.6.0.56 off 0x1da276..0x1da287). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mango/interp.h"

int mango_openttd_pa_skip_fastpath_test(MangoCpu* cpu, MangoMemory* mem, uint32_t bias,
                                        uint32_t* next_pc);

#define CODE 0x1000u     /* stands for libapplication off 0x1da276 */
#define BIAS (CODE - 0x1da276u)
#define DATA 0x2000u
#define MEMSZ 0x10000u

static const uint8_t kLoop[] = {0x01, 0x38, 0x01, 0xf1, 0x04, 0x01, 0x6a, 0xd0, 0x34,
                                0xf8, 0x02, 0x2f, 0xd3, 0xb2, 0xe2, 0x2b, 0xf6, 0xd9};

static uint32_t rng = 12345u;
static uint32_t rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

int main(void) {
  static uint8_t a[MEMSZ], b[MEMSZ];
  int fails = 0;
  for (int t = 0; t < 2000; t++) {
    memset(a, 0, MEMSZ);
    memcpy(a + CODE, kLoop, sizeof kLoop);
    for (uint32_t i = DATA; i < DATA + 0x4000u; i++) {
      uint32_t r = rnd();
      a[i] = (uint8_t)((r & 7u) == 0u ? (0xe0u + (r >> 4) % 32u) : (r >> 4));
    }
    memcpy(b, a, MEMSZ);
    MangoCpu c1;
    memset(&c1, 0, sizeof c1);
    for (int i = 0; i < 13; i++) c1.r[i] = rnd();
    c1.r[0] = 1u + rnd() % 300u;
    c1.r[4] = DATA - 2u + 2u * (rnd() % 64u);
    c1.cpsr = MANGO_CPSR_T | (rnd() & 0xf0000000u);
    MangoCpu c2 = c1;
    /* Reference: interpreter until it leaves the loop body. */
    MangoMemory m1 = {a, MEMSZ};
    c1.r[15] = CODE;
    uint32_t done = BIAS + 0x1da354u, anim = BIAS + 0x1da288u;
    int steps = 0;
    while (c1.r[15] != done && c1.r[15] != anim && steps < 100000) {
      if (mango_interp_run(&c1, &m1, 0xfffffff0u, 1) == -1 &&
          c1.r[15] != done && c1.r[15] != anim) {
        /* -1 is returned on max_steps; keep stepping */
      }
      steps++;
    }
    MangoMemory m2 = {b, MEMSZ};
    uint32_t next = 0;
    if (mango_openttd_pa_skip_fastpath_test(&c2, &m2, BIAS, &next) != 0) {
      fprintf(stderr, "FAIL t=%d fastpath declined\n", t);
      return 1;
    }
    c2.r[15] = next;
    uint32_t fm = 0xf0000000u;
    if (memcmp(c1.r, c2.r, sizeof c1.r) != 0 || (c1.cpsr & fm) != (c2.cpsr & fm) ||
        memcmp(a, b, MEMSZ) != 0) {
      fprintf(stderr, "FAIL t=%d pc %x/%x r0 %x/%x r4 %x/%x cpsr %x/%x\n", t, c1.r[15],
              c2.r[15], c1.r[0], c2.r[0], c1.r[4], c2.r[4], c1.cpsr, c2.cpsr);
      if (++fails > 5) return 1;
    }
  }
  if (fails) return 1;
  printf("pa_fastpath_diff: 2000/2000 match\n");
  return 0;
}
