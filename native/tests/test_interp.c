#include <stdio.h>
#include <string.h>

#include "mango/cpu.h"
#include "mango/decoder.h"
#include "mango/interp.h"

/* Hand-encoded A32, cond=AL throughout. See native/README.md before
 * trusting hex values blindly, worth re-deriving by hand when you read
 * this; that's caught real bugs during development more than once. */

/* Copies words into mem little-endian, starting at byte 0, and zeroes
 * the rest. A whole test's address space in one buffer, code and data
 * together, same as the real thing. */
static void load_words(uint8_t* mem, uint32_t mem_size, const uint32_t* words, uint32_t n) {
  memset(mem, 0, mem_size);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t w = words[i];
    mem[i * 4 + 0] = (uint8_t)(w & 0xFF);
    mem[i * 4 + 1] = (uint8_t)((w >> 8) & 0xFF);
    mem[i * 4 + 2] = (uint8_t)((w >> 16) & 0xFF);
    mem[i * 4 + 3] = (uint8_t)((w >> 24) & 0xFF);
  }
}

/* Local to this test file; interp.c's own version is static, not exported. */
static uint32_t bytes_to_u32_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void u32_to_bytes_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void load_halfwords(uint8_t* mem, uint32_t mem_size, const uint16_t* hw, uint32_t n) {
  memset(mem, 0, mem_size);
  for (uint32_t i = 0; i < n; i++) {
    uint16_t h = hw[i];
    mem[i * 2u + 0] = (uint8_t)(h & 0xFF);
    mem[i * 2u + 1] = (uint8_t)((h >> 8) & 0xFF);
  }
}

static int test_mov_add_bx(void) {
  static const uint32_t kProgram[] = {
      0xE3A00002u, /* mov r0, #2 */
      0xE2800003u, /* add r0, r0, #3 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xDEADBEEFu;

  int rc = mango_interp_run(&cpu, &mem, 0xDEADBEEFu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(mov_add_bx): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 5) {
    fprintf(stderr, "FAIL(mov_add_bx): expected r0 == 5, got %u\n", cpu.r[0]);
    return 1;
  }
  if (cpu.r[MANGO_REG_PC] != 0xDEADBEEFu) {
    fprintf(stderr, "FAIL(mov_add_bx): expected pc == sentinel LR, got 0x%08x\n",
            cpu.r[MANGO_REG_PC]);
    return 1;
  }
  printf("ok: mov + add + bx (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_branch_and_flags(void) {
  /* mov r0, #10 ; b skip ; mov r0, #99 (skipped) ; skip: sub r0, r0, #4 ;
   * cmp r0, #6 ; bx lr */
  static const uint32_t kProgram[] = {
      0xE3A0000Au, /* mov r0, #10 */
      0xEA000000u, /* b skip (skip = this instr's addr + 8) */
      0xE3A00063u, /* mov r0, #99, must never execute */
      0xE2400004u, /* skip: sub r0, r0, #4 */
      0xE3500006u, /* cmp r0, #6 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xCAFEF00Du;

  int rc = mango_interp_run(&cpu, &mem, 0xCAFEF00Du, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(branch_and_flags): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 6) {
    fprintf(stderr,
            "FAIL(branch_and_flags): expected r0 == 6 (branch must have "
            "skipped mov r0,#99), got %u\n",
            cpu.r[0]);
    return 1;
  }
  if (!(cpu.cpsr & MANGO_CPSR_Z)) {
    fprintf(stderr, "FAIL(branch_and_flags): expected Z flag set after cmp r0,#6\n");
    return 1;
  }
  printf("ok: b + sub + cmp (r0 = %u, Z set)\n", cpu.r[0]);
  return 0;
}

static int test_load_store_roundtrip(void) {
  /* mov r0, #100 ; mov r1, #64 ; str r0, [r1] ; mov r0, #0 ;
   * ldr r0, [r1] ; bx lr
   * Program is 6 words (24 bytes); r1 points at byte 64, comfortably
   * past the program itself, so code and the word we store/load don't
   * overlap. */
  static const uint32_t kProgram[] = {
      0xE3A00064u, /* mov r0, #100 */
      0xE3A01040u, /* mov r1, #64 */
      0xE5810000u, /* str r0, [r1] */
      0xE3A00000u, /* mov r0, #0 */
      0xE5910000u, /* ldr r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x1234u;

  int rc = mango_interp_run(&cpu, &mem, 0x1234u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(load_store_roundtrip): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 100) {
    fprintf(stderr,
            "FAIL(load_store_roundtrip): expected r0 == 100 after store+load "
            "round trip, got %u\n",
            cpu.r[0]);
    return 1;
  }
  /* Byte-check the store actually landed where expected, not just that
   * the load happened to return the right value some other way. */
  uint32_t stored = bytes_to_u32_le(mem_buf + 64);
  if (stored != 100) {
    fprintf(stderr, "FAIL(load_store_roundtrip): expected mem[64..67] == 100, got %u\n", stored);
    return 1;
  }
  printf("ok: str + ldr round trip (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_pc_relative_add(void) {
  /* add r0, pc, #4 ; bx lr
   * At address 0, PC reads as 0 + 8 = 8 per real hardware semantics, so
   * r0 should end up 8 + 4 = 12. This is the same addressing real
   * compiled code uses for literal-pool constants. */
  static const uint32_t kProgram[] = {
      0xE28F0004u, /* add r0, pc, #4 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x5678u;

  int rc = mango_interp_run(&cpu, &mem, 0x5678u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(pc_relative_add): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 12) {
    fprintf(stderr, "FAIL(pc_relative_add): expected r0 == 12, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: pc-relative add (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_load_out_of_bounds_rejected(void) {
  /* mov r1, #200 (way past the 32-byte buffer) ; ldr r0, [r1] ; bx lr
   * Must fail cleanly, not read past the buffer. Real bug class this
   * guards against: an ASan build would catch a broken bounds check
   * here immediately, that's the point of running this under
   * -fsanitize=address in CI rather than just eyeballing the code. */
  static const uint32_t kProgram[] = {
      0xE3A010C8u, /* mov r1, #200 */
      0xE5910000u, /* ldr r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;

  int rc = mango_interp_run(&cpu, &mem, 0xFFFFFFFFu, 100);
  if (rc == 0) {
    fprintf(stderr, "FAIL(load_out_of_bounds_rejected): expected a failure, got success\n");
    return 1;
  }
  printf("ok: out-of-bounds ldr correctly rejected\n");
  return 0;
}

static int test_load_unaligned_ok(void) {
  /* ARMv7 (Android) allows unaligned LDR. Bytes at 1..4: 10 a0 e3 00. */
  static const uint32_t kProgram[] = {
      0xE3A01001u, /* mov r1, #1 */
      0xE5910000u, /* ldr r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xFFFFFFFFu;

  int rc = mango_interp_run(&cpu, &mem, 0xFFFFFFFFu, 100);
  if (rc != 0 || cpu.r[0] != 0x00E3A010u) {
    fprintf(stderr, "FAIL(load_unaligned_ok): rc=%d r0=0x%x, want 0x00e3a010\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: unaligned ldr loads little-endian word\n");
  return 0;
}

static int test_load_store_byte_roundtrip(void) {
  /* mov r0, #171 ; mov r1, #64 ; strb r0, [r1] ; mov r0, #0 ;
   * ldrb r0, [r1] ; bx lr
   * 171 (0xAB) has the high bit of the byte set: if LDRB ever
   * accidentally sign-extended instead of zero-extending, this would
   * come back as 0xFFFFFFAB instead of 0xAB and the test would catch
   * it. */
  static const uint32_t kProgram[] = {
      0xE3A000ABu, /* mov r0, #171 */
      0xE3A01040u, /* mov r1, #64 */
      0xE5C10000u, /* strb r0, [r1] */
      0xE3A00000u, /* mov r0, #0 */
      0xE5D10000u, /* ldrb r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x9999u;

  int rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(load_store_byte_roundtrip): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 171) {
    fprintf(stderr, "FAIL(load_store_byte_roundtrip): expected r0 == 171, got %u (0x%08x)\n",
            cpu.r[0], cpu.r[0]);
    return 1;
  }
  /* Byte-check the neighboring bytes were left alone (STRB must not
   * touch more than one byte). */
  if (mem_buf[65] != 0 || mem_buf[66] != 0 || mem_buf[67] != 0) {
    fprintf(stderr, "FAIL(load_store_byte_roundtrip): strb touched neighboring bytes\n");
    return 1;
  }
  printf("ok: strb + ldrb round trip, zero-extended (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_conditional_branch_taken(void) {
  /* mov r0, #5 ; cmp r0, #5 ; beq target ; mov r0, #99 (must be skipped) ;
   * target: mov r1, #1 ; bx lr
   * r0 == r0, so Z is set and the branch must be taken. */
  static const uint32_t kProgram[] = {
      0xE3A00005u, /* mov r0, #5 */
      0xE3500005u, /* cmp r0, #5 */
      0x0A000000u, /* beq target (target = this instr's addr + 8) */
      0xE3A00063u, /* mov r0, #99, must never execute */
      0xE3A01001u, /* target: mov r1, #1 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x1111u;

  int rc = mango_interp_run(&cpu, &mem, 0x1111u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(conditional_branch_taken): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 5) {
    fprintf(stderr,
            "FAIL(conditional_branch_taken): expected r0 == 5 (beq must have "
            "skipped mov r0,#99), got %u\n",
            cpu.r[0]);
    return 1;
  }
  if (cpu.r[1] != 1) {
    fprintf(stderr,
            "FAIL(conditional_branch_taken): expected r1 == 1 (target must "
            "have run), got %u\n",
            cpu.r[1]);
    return 1;
  }
  printf("ok: beq taken (r0 = %u, r1 = %u)\n", cpu.r[0], cpu.r[1]);
  return 0;
}

static int test_signed_vs_unsigned_condition_flags(void) {
  /* mov r0, #0 ; sub r0, r0, #1 (r0 = 0xFFFFFFFF, i.e. -1 signed) ;
   * cmp r0, #1 ; movlt r1, #1 (signed: -1 < 1, should execute) ;
   * movge r2, #1 (signed: NOT -1 >= 1, must not execute) ; bx lr
   *
   * The point of this test: 0xFFFFFFFF is simultaneously "less than 1"
   * as a signed number and "greater than 1" as an unsigned one. Getting
   * this right depends on C and V being computed correctly in CMP, not
   * just N and Z, this is exactly the kind of case that would silently
   * pass with N/Z alone but fail here. */
  static const uint32_t kProgram[] = {
      0xE3A00000u, /* mov r0, #0 */
      0xE2400001u, /* sub r0, r0, #1 */
      0xE3500001u, /* cmp r0, #1 */
      0xB3A01001u, /* movlt r1, #1 */
      0xA3A02001u, /* movge r2, #1 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x2222u;

  int rc = mango_interp_run(&cpu, &mem, 0x2222u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(signed_vs_unsigned_condition_flags): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xFFFFFFFFu) {
    fprintf(stderr, "FAIL(signed_vs_unsigned_condition_flags): expected r0 == -1, got %u\n",
            cpu.r[0]);
    return 1;
  }
  if (cpu.r[1] != 1) {
    fprintf(stderr,
            "FAIL(signed_vs_unsigned_condition_flags): expected r1 == 1 "
            "(movlt should have executed, -1 < 1), got %u\n",
            cpu.r[1]);
    return 1;
  }
  if (cpu.r[2] != 0) {
    fprintf(stderr,
            "FAIL(signed_vs_unsigned_condition_flags): expected r2 == 0 "
            "(movge should NOT have executed, -1 is not >= 1), got %u\n",
            cpu.r[2]);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_C) == 0) {
    fprintf(stderr,
            "FAIL(signed_vs_unsigned_condition_flags): expected C set "
            "(0xFFFFFFFF >= 1 unsigned)\n");
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_V) != 0) {
    fprintf(stderr,
            "FAIL(signed_vs_unsigned_condition_flags): expected V clear "
            "(-1 - 1 doesn't signed-overflow)\n");
    return 1;
  }
  printf("ok: signed vs unsigned flags (r1 = %u, r2 = %u)\n", cpu.r[1], cpu.r[2]);
  return 0;
}

static int test_shifted_operand2(void) {
  /* mov r0, #16 ; mov r1, r0, LSL #2 ; mov r2, r0, LSR #2 ; mov r0, #0 ;
   * sub r0, r0, #16 (r0 = -16 = 0xFFFFFFF0) ; mov r3, r0, ASR #2 ;
   * mov r4, r0, ROR #4 ; bx lr
   * ASR specifically needs a negative input to actually distinguish
   * itself from LSR (they're identical for positive values), that's why
   * r0 gets reused as -16 partway through instead of adding a 7th
   * register. */
  static const uint32_t kProgram[] = {
      0xE3A00010u, /* mov r0, #16 */
      0xE1A01100u, /* mov r1, r0, LSL #2 */
      0xE1A02120u, /* mov r2, r0, LSR #2 */
      0xE3A00000u, /* mov r0, #0 */
      0xE2400010u, /* sub r0, r0, #16 */
      0xE1A03140u, /* mov r3, r0, ASR #2 */
      0xE1A04260u, /* mov r4, r0, ROR #4 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 8);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x3333u;

  int rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(shifted_operand2): mango_interp_run returned %d\n", rc);
    return 1;
  }
  struct {
    uint32_t reg;
    uint32_t got;
    uint32_t want;
    const char* label;
  } checks[] = {
      {1, cpu.r[1], 64, "LSL #2 of 16"},
      {2, cpu.r[2], 4, "LSR #2 of 16"},
      {3, cpu.r[3], 0xFFFFFFFCu, "ASR #2 of -16"},
      {4, cpu.r[4], 0x0FFFFFFFu, "ROR #4 of -16"},
  };
  for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
    if (checks[i].got != checks[i].want) {
      fprintf(stderr, "FAIL(shifted_operand2): %s: expected 0x%08x, got 0x%08x\n", checks[i].label,
              checks[i].want, checks[i].got);
      return 1;
    }
  }
  printf("ok: shifted operand2, LSL/LSR/ASR/ROR all correct\n");
  return 0;
}

static int test_adds_signed_overflow_without_carry(void) {
  /* mov r0, #1 ; mov r0, r0, LSL #30 (r0 = 0x40000000) ;
   * adds r0, r0, r0 (r0 = 0x80000000) ; bx lr
   *
   * The point: 0x40000000 + 0x40000000 = 0x80000000 fits fine in 32
   * unsigned bits (no carry out, C should be 0), but as signed 32-bit
   * numbers it's a textbook overflow, two positives summing to
   * something that looks negative (V should be 1). Getting V right
   * without it just tracking C is exactly what this checks. */
  static const uint32_t kProgram[] = {
      0xE3A00001u, /* mov r0, #1 */
      0xE1A00F00u, /* mov r0, r0, LSL #30 */
      0xE0900000u, /* adds r0, r0, r0 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x4444u;

  int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(adds_signed_overflow_without_carry): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0x80000000u) {
    fprintf(stderr, "FAIL(adds_overflow): expected r0 == 0x80000000, got 0x%08x\n", cpu.r[0]);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_C) != 0) {
    fprintf(stderr, "FAIL(adds_overflow): expected C clear (no unsigned carry)\n");
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_V) == 0) {
    fprintf(stderr, "FAIL(adds_overflow): expected V set (signed overflow)\n");
    return 1;
  }
  printf("ok: adds signed overflow without carry (r0 = 0x%08x)\n", cpu.r[0]);
  return 0;
}

static int test_subs_borrow_no_overflow(void) {
  /* mov r0, #5 ; subs r0, r0, #10 (r0 = -5) ; bx lr
   * 5 - 10 needs a borrow (C should clear), but -5 is a completely
   * ordinary in-range signed result, not an overflow (V should stay
   * clear). Checks C and V are computed independently, not one implying
   * the other. */
  static const uint32_t kProgram[] = {
      0xE3A00005u, /* mov r0, #5 */
      0xE250000Au, /* subs r0, r0, #10 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x5555u;

  int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(subs_borrow_no_overflow): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xFFFFFFFBu) {
    fprintf(stderr, "FAIL(subs_borrow_no_overflow): expected r0 == -5, got 0x%08x\n", cpu.r[0]);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_C) != 0) {
    fprintf(stderr, "FAIL(subs_borrow_no_overflow): expected C clear (5 < 10, borrow occurred)\n");
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_V) != 0) {
    fprintf(stderr, "FAIL(subs_borrow_no_overflow): expected V clear (-5 doesn't overflow)\n");
    return 1;
  }
  printf("ok: subs borrow without overflow (r0 = 0x%08x)\n", cpu.r[0]);
  return 0;
}

static int test_bl_call_and_return(void) {
  /* bl sets LR then jumps; the callee's bx lr returns right after the bl. */
  static const uint32_t kProgram[] = {
      0xE3A00001u, /* mov r0, #1 */
      0xEB000001u, /* bl func */
      0xE3A00002u, /* mov r0, #2 */
      0xE12FFF15u, /* bx r5 */
      0xE3A0102Au, /* func: mov r1, #42 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[5] = 0x6666u;

  int rc = mango_interp_run(&cpu, &mem, 0x6666u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(bl_call_and_return): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[1] != 42) {
    fprintf(stderr, "FAIL(bl_call_and_return): expected r1 == 42, got %u\n", cpu.r[1]);
    return 1;
  }
  if (cpu.r[0] != 2) {
    fprintf(stderr, "FAIL(bl_call_and_return): expected r0 == 2, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: bl call and return (r0 = %u, r1 = %u)\n", cpu.r[0], cpu.r[1]);
  return 0;
}

static int test_full_alu_opcodes(void) {
  /* mov r0,#0xCC ; and r1,r0,#0xAA ; eor r2,r0,#0xF0 ; orr r3,r0,#0x0F ;
   * bic r4,r0,#0xF0 ; mvn r5,#0 ; rsb r6,r0,#0xFF ; bx lr
   * Exercises every opcode that wasn't already covered: AND, EOR, ORR,
   * BIC, MVN, RSB. */
  static const uint32_t kProgram[] = {
      0xE3A000CCu, /* mov r0, #0xCC */
      0xE20010AAu, /* and r1, r0, #0xAA */
      0xE22020F0u, /* eor r2, r0, #0xF0 */
      0xE380300Fu, /* orr r3, r0, #0x0F */
      0xE3C040F0u, /* bic r4, r0, #0xF0 */
      0xE3E05000u, /* mvn r5, #0 */
      0xE26060FFu, /* rsb r6, r0, #0xFF */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 8);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x7777u;

  int rc = mango_interp_run(&cpu, &mem, 0x7777u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(full_alu_opcodes): mango_interp_run returned %d\n", rc);
    return 1;
  }
  struct {
    uint32_t reg;
    uint32_t want;
    const char* name;
  } checks[] = {
      {cpu.r[1], 0x88, "r1 (and)"},        {cpu.r[2], 0x3C, "r2 (eor)"},
      {cpu.r[3], 0xCF, "r3 (orr)"},        {cpu.r[4], 0x0C, "r4 (bic)"},
      {cpu.r[5], 0xFFFFFFFFu, "r5 (mvn)"}, {cpu.r[6], 0x33, "r6 (rsb)"},
  };
  for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
    if (checks[i].reg != checks[i].want) {
      fprintf(stderr, "FAIL(full_alu_opcodes): expected %s == 0x%x, got 0x%x\n", checks[i].name,
              checks[i].want, checks[i].reg);
      return 1;
    }
  }
  printf("ok: and/eor/orr/bic/mvn/rsb all correct\n");
  return 0;
}

static int test_adc_carry_chain(void) {
  /* A 64-bit add spread across two 32-bit registers each, the actual
   * reason ADC exists: adds+adc must carry the low word's overflow into
   * the high word.
   * mvn r0,#0 (r0=0xFFFFFFFF, low(A)) ; mov r1,#1 (high(A)) ;
   * mov r2,#1 (low(B)) ; mov r3,#0 (high(B)) ;
   * adds r4,r0,r2 (0xFFFFFFFF+1 wraps to 0, C=1) ;
   * adc r5,r1,r3 (1+0+C = 2) ; bx lr */
  static const uint32_t kProgram[] = {
      0xE3E00000u, /* mvn r0, #0 */
      0xE3A01001u, /* mov r1, #1 */
      0xE3A02001u, /* mov r2, #1 */
      0xE3A03000u, /* mov r3, #0 */
      0xE0904002u, /* adds r4, r0, r2 */
      0xE0A15003u, /* adc r5, r1, r3 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 7);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x8888u;

  int rc = mango_interp_run(&cpu, &mem, 0x8888u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(adc_carry_chain): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 0 || cpu.r[5] != 2) {
    fprintf(stderr, "FAIL(adc_carry_chain): expected r4==0 r5==2, got r4=%u r5=%u\n", cpu.r[4],
            cpu.r[5]);
    return 1;
  }
  printf("ok: adds+adc carries a 64-bit add across two registers (r4=%u, r5=%u)\n", cpu.r[4],
         cpu.r[5]);
  return 0;
}

static int test_sbc_rsc(void) {
  /* mov r0,#5 ; subs r0,r0,#10 (r0 = -5, borrow, C=0) ;
   * sbc r1,r0,#2 (r1 = -5 - 2 - !C(1) = -8) ;
   * rsc r2,r0,#0 (r2 = 0 - (-5) - !C(1) = 4) ; bx lr */
  static const uint32_t kProgram[] = {
      0xE3A00005u, /* mov r0, #5 */
      0xE250000Au, /* subs r0, r0, #10 */
      0xE2C01002u, /* sbc r1, r0, #2 */
      0xE2E02000u, /* rsc r2, r0, #0 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x1234u;

  int rc = mango_interp_run(&cpu, &mem, 0x1234u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(sbc_rsc): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xFFFFFFFBu || cpu.r[1] != 0xFFFFFFF8u || cpu.r[2] != 4) {
    fprintf(stderr,
            "FAIL(sbc_rsc): expected r0=0xfffffffb r1=0xfffffff8 r2=4, got r0=0x%x r1=0x%x r2=%u\n",
            cpu.r[0], cpu.r[1], cpu.r[2]);
    return 1;
  }
  printf("ok: sbc/rsc borrow chain correct (r1=0x%x, r2=%u)\n", cpu.r[1], cpu.r[2]);
  return 0;
}

static int test_mul(void) {
  /* mov r0,#6 ; mov r1,#7 ; mul r2,r0,r1 ; bx lr */
  static const uint32_t kProgram[] = {
      0xE3A00006u, /* mov r0, #6 */
      0xE3A01007u, /* mov r1, #7 */
      0xE0020190u, /* mul r2, r0, r1 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x5555u;

  int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(mul): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[2] != 42) {
    fprintf(stderr, "FAIL(mul): expected r2 == 42, got %u\n", cpu.r[2]);
    return 1;
  }
  printf("ok: mul r2, r0, r1 (r2 = %u)\n", cpu.r[2]);
  return 0;
}

static int test_tst_teq_cmn_dont_write_rd(void) {
  /* mov r0,#0x0F ; mov r1,#0x99 (poisons r1 so a wrongly-written TST/TEQ/
   * CMN would be caught) ; tst r0,#0xFF ; teq r0,#0x0F ; cmn r0,#1 ; bx lr
   * All three only need to leave r1 untouched; their flag effects are
   * already covered indirectly by the conditional-branch tests. */
  static const uint32_t kProgram[] = {
      0xE3A0000Fu, /* mov r0, #0x0F */
      0xE3A01099u, /* mov r1, #0x99 */
      0xE31000FFu, /* tst r0, #0xFF, Rd field left 0 */
      0xE330000Fu, /* teq r0, #0x0F, Rd field left 0 */
      0xE3700001u, /* cmn r0, #1, Rd field left 0 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x4444u;

  int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(tst_teq_cmn_dont_write_rd): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0x0F || cpu.r[1] != 0x99) {
    fprintf(stderr,
            "FAIL(tst_teq_cmn_dont_write_rd): expected r0=0x0f r1=0x99 unchanged, got r0=0x%x "
            "r1=0x%x\n",
            cpu.r[0], cpu.r[1]);
    return 1;
  }
  printf("ok: tst/teq/cmn leave their registers alone, flags only\n");
  return 0;
}

static int test_svc_stops_and_can_resume(void) {
  /* mov r7,#1 ; svc #0 ; mov r0,#99 ; bx lr. mango_core has no syscalls of
   * its own (see native/README.md); it stops at the SVC and hands control
   * back, exactly the interface linux/'s and native_bridge_shim.c's
   * eventual syscall thunking would build on. */
  static const uint32_t kProgram[] = {
      0xE3A07001u, /* mov r7, #1 */
      0xEF000000u, /* svc #0 */
      0xE3A00063u, /* mov r0, #99 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x9999u;

  int rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 1) {
    fprintf(stderr, "FAIL(svc_stops_and_can_resume): first run returned %d, want 1\n", rc);
    return 1;
  }
  if (cpu.r[MANGO_REG_PC] != 4 || cpu.r[7] != 1) {
    fprintf(stderr, "FAIL(svc_stops_and_can_resume): pc=%u r7=%u, want pc=4 r7=1\n",
            cpu.r[MANGO_REG_PC], cpu.r[7]);
    return 1;
  }

  /* A caller would do its syscall here; this test just resumes past it. */
  cpu.r[MANGO_REG_PC] += 4;
  rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 0 || cpu.r[0] != 99) {
    fprintf(stderr,
            "FAIL(svc_stops_and_can_resume): resumed run gave rc=%d r0=%u, want rc=0 r0=99\n", rc,
            cpu.r[0]);
    return 1;
  }
  printf("ok: svc stops the interpreter with r7 intact, and resuming past it works\n");
  return 0;
}

/* Independent of mango_decode: cond=AL, bits 27-25=100. Cross-checks the
 * hand-written LDM/STM words, same "re-derive, don't trust hex blindly"
 * rule as the rest of this file. */
static uint32_t encode_ldm_stm(int p, int u, int w, int l, uint32_t rn, uint32_t reglist) {
  return 0xE0000000u | (1u << 27) | ((uint32_t)p << 24) | ((uint32_t)u << 23) |
         ((uint32_t)w << 21) | ((uint32_t)l << 20) | ((rn & 0xFu) << 16) | (reglist & 0xFFFFu);
}

static int test_push_pop_roundtrip(void) {
  /* Classic A32 prologue/epilogue: PUSH {r4, r5, lr} / POP {r4, r5, pc}.
   * Callee clobbers r4/r5, then the pop must restore them and return via
   * the saved LR loaded into PC, no BX. */
  uint32_t push = encode_ldm_stm(1, 0, 1, 0, MANGO_REG_SP, (1u << 4) | (1u << 5) | (1u << 14));
  uint32_t pop = encode_ldm_stm(0, 1, 1, 1, MANGO_REG_SP, (1u << 4) | (1u << 5) | (1u << 15));
  if (push != 0xE92D4030u || pop != 0xE8BD8030u) {
    fprintf(stderr, "FAIL(push_pop_roundtrip): encoder mismatch, push=0x%08x pop=0x%08x\n", push,
            pop);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE92D4030u, /* push {r4, r5, lr}  STMDB sp!, {r4, r5, lr} */
      0xE3A04001u, /* mov r4, #1 */
      0xE3A05002u, /* mov r5, #2 */
      0xE8BD8030u, /* pop {r4, r5, pc}   LDMIA sp!, {r4, r5, pc} */
  };

  uint8_t mem_buf[256];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[4] = 0xA1A1A1A1u;
  cpu.r[5] = 0xB2B2B2B2u;
  cpu.r[MANGO_REG_SP] = 128;
  cpu.r[MANGO_REG_LR] = 0xDEAD0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xDEAD0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(push_pop_roundtrip): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 0xA1A1A1A1u || cpu.r[5] != 0xB2B2B2B2u) {
    fprintf(stderr, "FAIL(push_pop_roundtrip): callee-saved regs not restored, r4=0x%x r5=0x%x\n",
            cpu.r[4], cpu.r[5]);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != 128) {
    fprintf(stderr, "FAIL(push_pop_roundtrip): expected sp == 128 after pop, got %u\n",
            cpu.r[MANGO_REG_SP]);
    return 1;
  }
  if (cpu.r[MANGO_REG_PC] != 0xDEAD0000u) {
    fprintf(stderr, "FAIL(push_pop_roundtrip): expected pc == saved lr, got 0x%08x\n",
            cpu.r[MANGO_REG_PC]);
    return 1;
  }
  printf("ok: push/pop round trip restores r4/r5 and returns via pc\n");
  return 0;
}


static int test_stmdb_sp_in_list(void) {
  /* OFDP stop word 0xe92d6000: STMDB sp!, {sp, lr}. ARMv7 stored SP is
   * implementation-defined with writeback; match common AAPCS/bionic:
   * store the original (pre-writeback) SP. Also allow LDM with SP in list. */
  uint32_t stmdb = encode_ldm_stm(1, 0, 1, 0, MANGO_REG_SP, (1u << MANGO_REG_SP) | (1u << MANGO_REG_LR));
  uint32_t ldmia = encode_ldm_stm(0, 1, 1, 1, MANGO_REG_SP, (1u << MANGO_REG_SP) | (1u << MANGO_REG_LR));
  if (stmdb != 0xE92D6000u) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): encoder mismatch, got 0x%08x\n", stmdb);
    return 1;
  }
  MangoInsn insn;
  if (mango_decode(stmdb, &insn) != 0 || insn.op != MANGO_OP_STM || insn.rn != MANGO_REG_SP ||
      insn.reglist != 0x6000u || !insn.w || !insn.p || insn.u) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): STMDB sp!,{sp,lr} rejected or mis-decoded\n");
    return 1;
  }
  if (mango_decode(ldmia, &insn) != 0 || insn.op != MANGO_OP_LDM) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): LDMIA sp!,{sp,lr} rejected\n");
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE92D6000u, /* stmdb sp!, {sp, lr} */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[256];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  const uint32_t orig_sp = 128u;
  const uint32_t orig_lr = 0xCAFE0000u;
  cpu.r[MANGO_REG_SP] = orig_sp;
  cpu.r[MANGO_REG_LR] = orig_lr;

  int rc = mango_interp_run(&cpu, &mem, orig_lr, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != orig_sp - 8u) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): expected sp=%u after writeback, got %u\n",
            orig_sp - 8u, cpu.r[MANGO_REG_SP]);
    return 1;
  }
  /* Lowest-numbered register at lowest address: SP at orig_sp-8, LR at orig_sp-4. */
  uint32_t stored_sp = bytes_to_u32_le(mem_buf + (orig_sp - 8u));
  uint32_t stored_lr = bytes_to_u32_le(mem_buf + (orig_sp - 4u));
  if (stored_sp != orig_sp) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): stored SP should be original 0x%x, got 0x%x\n", orig_sp,
            stored_sp);
    return 1;
  }
  if (stored_lr != orig_lr) {
    fprintf(stderr, "FAIL(stmdb_sp_in_list): stored LR mismatch, got 0x%x\n", stored_lr);
    return 1;
  }
  printf("ok: STMDB sp!,{sp,lr} stores original SP then writeback; LDM SP-in-list decodes\n");
  return 0;
}

static int test_stmia_ldmia_no_writeback(void) {
  /* STMIA/LDMIA without writeback: lowest register at lowest address, base
   * left alone. r1 points at byte 64, past the program. */
  uint32_t stmia = encode_ldm_stm(0, 1, 0, 0, 1, 0x000Du); /* {r0, r2, r3} */
  uint32_t ldmia = encode_ldm_stm(0, 1, 0, 1, 1, 0x000Du);
  if (stmia != 0xE881000Du || ldmia != 0xE891000Du) {
    fprintf(stderr, "FAIL(stmia_ldmia_no_writeback): encoder mismatch\n");
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A0000Au, /* mov r0, #10 */
      0xE3A02014u, /* mov r2, #20 */
      0xE3A0301Eu, /* mov r3, #30 */
      0xE3A01040u, /* mov r1, #64 */
      0xE881000Du, /* stmia r1, {r0, r2, r3} */
      0xE3A00000u, /* mov r0, #0 */
      0xE3A02000u, /* mov r2, #0 */
      0xE3A03000u, /* mov r3, #0 */
      0xE891000Du, /* ldmia r1, {r0, r2, r3} */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 10);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xABCDu;

  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(stmia_ldmia_no_writeback): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 10 || cpu.r[2] != 20 || cpu.r[3] != 30) {
    fprintf(stderr, "FAIL(stmia_ldmia_no_writeback): expected r0=10 r2=20 r3=30, got %u %u %u\n",
            cpu.r[0], cpu.r[2], cpu.r[3]);
    return 1;
  }
  if (cpu.r[1] != 64) {
    fprintf(stderr,
            "FAIL(stmia_ldmia_no_writeback): r1 should be unchanged without writeback, got %u\n",
            cpu.r[1]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + 64) != 10 || bytes_to_u32_le(mem_buf + 68) != 20 ||
      bytes_to_u32_le(mem_buf + 72) != 30) {
    fprintf(stderr,
            "FAIL(stmia_ldmia_no_writeback): memory order is not lowest-reg at lowest-addr\n");
    return 1;
  }
  printf("ok: stmia/ldmia without writeback, lowest register at lowest address\n");
  return 0;
}

static int test_stmib_and_writeback(void) {
  /* STMIB (P=1) must skip the word at the base; STMIA! must advance r1. */
  uint32_t stmib = encode_ldm_stm(1, 1, 0, 0, 1, 0x0005u); /* {r0, r2} */
  uint32_t stmia_wb = encode_ldm_stm(0, 1, 1, 0, 1, 0x0005u);
  if (stmib != 0xE9810005u || stmia_wb != 0xE8A10005u) {
    fprintf(stderr, "FAIL(stmib_and_writeback): encoder mismatch stmib=0x%08x stmia!=0x%08x\n",
            stmib, stmia_wb);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A00007u, /* mov r0, #7 */
      0xE3A02009u, /* mov r2, #9 */
      0xE3A01040u, /* mov r1, #64 */
      0xE9810005u, /* stmib r1, {r0, r2} */
      0xE8A10005u, /* stmia r1!, {r0, r2} */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x4321u;

  int rc = mango_interp_run(&cpu, &mem, 0x4321u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(stmib_and_writeback): mango_interp_run returned %d\n", rc);
    return 1;
  }
  /* STMIB r1=64: stores at 68 and 72, leaves byte 64 alone. Then STMIA!
   * stores at 64 and 68 and sets r1 to 72. So 64 and 68 are 7 and 9 from
   * the second store; 72 is still 9 from STMIB. */
  if (bytes_to_u32_le(mem_buf + 64) != 7 || bytes_to_u32_le(mem_buf + 68) != 9 ||
      bytes_to_u32_le(mem_buf + 72) != 9) {
    fprintf(stderr, "FAIL(stmib_and_writeback): mem[64,68,72]=%u,%u,%u want 7,9,9\n",
            bytes_to_u32_le(mem_buf + 64), bytes_to_u32_le(mem_buf + 68),
            bytes_to_u32_le(mem_buf + 72));
    return 1;
  }
  if (cpu.r[1] != 72) {
    fprintf(stderr, "FAIL(stmib_and_writeback): expected r1 == 72 after stmia!, got %u\n",
            cpu.r[1]);
    return 1;
  }
  printf("ok: stmib addressing and stmia writeback\n");
  return 0;
}

static int test_ldm_stm_rejected_shapes(void) {
  MangoInsn insn;
  uint32_t empty = encode_ldm_stm(0, 1, 0, 0, 0, 0);
  if (mango_decode(empty, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): empty reglist was decoded\n");
    return 1;
  }
  uint32_t s_bit = 0xE8C00001u; /* STMIA r0, {r0} with S=1 */
  if (mango_decode(s_bit, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): S-bit form was decoded\n");
    return 1;
  }
  uint32_t stm_pc = encode_ldm_stm(0, 1, 0, 0, 0, 1u << 15);
  if (mango_decode(stm_pc, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): stm of pc was decoded\n");
    return 1;
  }
  uint32_t wb_rn = encode_ldm_stm(0, 1, 1, 0, 0, 1u); /* STMIA r0!, {r0} */
  if (mango_decode(wb_rn, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): writeback with non-SP rn in list was decoded\n");
    return 1;
  }
  uint32_t pc_base = encode_ldm_stm(0, 1, 0, 0, MANGO_REG_PC, 1u);
  if (mango_decode(pc_base, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): pc as base was decoded\n");
    return 1;
  }
  printf("ok: empty/S-bit/stm-pc/wb-non-sp-rn-in-list/pc-base ldm/stm shapes rejected\n");
  return 0;
}

static int test_stm_out_of_bounds_rejected(void) {
  /* stmia r1, {r0, r2, r3} at address 24 in a 32-byte buffer: 24+12=36. */
  static const uint32_t kProgram[] = {
      0xE3A01018u, /* mov r1, #24 */
      0xE881000Du, /* stmia r1, {r0, r2, r3} */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;

  int rc = mango_interp_run(&cpu, &mem, 0xFFFFFFFFu, 100);
  if (rc == 0) {
    fprintf(stderr, "FAIL(stm_out_of_bounds_rejected): expected a failure, got success\n");
    return 1;
  }
  printf("ok: out-of-bounds stm correctly rejected\n");
  return 0;
}

static int test_s0_compares_rejected(void) {
  /* TST/TEQ/CMP/CMN with S=0 isn't those ops at all (MRS/MSR overlap);
   * decoding it as a flag-less compare would be silently wrong. */
  MangoInsn insn;
  uint32_t cmp_s0 = 0xE1400001u; /* looks like "cmp r0,r1" but S=0: not really CMP */
  if (mango_decode(cmp_s0, &insn) == 0) {
    fprintf(stderr,
            "FAIL(s0_compares_rejected): CMP-shaped word with S=0 was decoded, "
            "should be rejected\n");
    return 1;
  }
  printf("ok: S=0 tst/teq/cmp/cmn shapes correctly rejected\n");
  return 0;
}

static uint32_t encode_mla(uint32_t rd, uint32_t rm, uint32_t rs, uint32_t ra) {
  return 0xE0200090u | ((rd & 0xFu) << 16) | ((ra & 0xFu) << 12) | ((rs & 0xFu) << 8) | (rm & 0xFu);
}

/* Long multiply: UMULL/UMLAL/SMULL/SMLAL. signed=U bit22, acc=A bit21.
 * Encoding: RdHi RdLo Rm 1001 Rn with product Rn*Rm → RdLo:RdHi. */
static uint32_t encode_mull(int signed_mul, int acc, int s, uint32_t rdlo, uint32_t rdhi,
                            uint32_t rn, uint32_t rm) {
  return 0xE0000090u | (1u << 23) | ((uint32_t)signed_mul << 22) | ((uint32_t)acc << 21) |
         ((uint32_t)s << 20) | ((rdhi & 0xFu) << 16) | ((rdlo & 0xFu) << 12) | ((rm & 0xFu) << 8) |
         (rn & 0xFu);
}

static uint32_t encode_ldst(int i, int p, int u, int b, int w, int l, uint32_t rn, uint32_t rt,
                            uint32_t operand12) {
  return 0xE0000000u | (1u << 26) | ((uint32_t)i << 25) | ((uint32_t)p << 24) |
         ((uint32_t)u << 23) | ((uint32_t)b << 22) | ((uint32_t)w << 21) | ((uint32_t)l << 20) |
         ((rn & 0xFu) << 16) | ((rt & 0xFu) << 12) | (operand12 & 0xFFFu);
}

static int test_mla(void) {
  uint32_t mla = encode_mla(0, 0, 1, 3); /* mla r0, r0, r1, r3 */
  if (mla != 0xE0203190u) {
    fprintf(stderr, "FAIL(mla): encoder mismatch, got 0x%08x\n", mla);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A00006u, /* mov r0, #6 */
      0xE3A01007u, /* mov r1, #7 */
      0xE3A0300Au, /* mov r3, #10 */
      0xE0203190u, /* mla r0, r0, r1, r3 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x4242u;

  int rc = mango_interp_run(&cpu, &mem, 0x4242u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(mla): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 52) {
    fprintf(stderr, "FAIL(mla): expected r0 == 52 (6*7+10), got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: mla r0, r0, r1, r3 (r0 = %u)\n", cpu.r[0]);
  return 0;
}


static int test_mull_long(void) {
  /* OFDP nativeRender stop 0xe0810096: umull r0, r1, r6, r0 (div10 magic). */
  uint32_t umull_ofdp = encode_mull(0, 0, 0, 0, 1, 6, 0);
  if (umull_ofdp != 0xE0810096u) {
    fprintf(stderr, "FAIL(mull): OFDP UMULL encoder got 0x%08x\n", umull_ofdp);
    return 1;
  }
  uint32_t umull = encode_mull(0, 0, 0, 2, 3, 4, 5); /* umull r2, r3, r4, r5 */
  uint32_t umlal = encode_mull(0, 1, 0, 2, 3, 4, 5);
  uint32_t smull = encode_mull(1, 0, 0, 2, 3, 4, 5);
  uint32_t smlal = encode_mull(1, 1, 0, 2, 3, 4, 5);
  if (umull != 0xE0832594u || umlal != 0xE0A32594u || smull != 0xE0C32594u ||
      smlal != 0xE0E32594u) {
    fprintf(stderr, "FAIL(mull): encoder mismatch umull=0x%08x umlal=0x%08x smull=0x%08x smlal=0x%08x\n",
            umull, umlal, smull, smlal);
    return 1;
  }

  /* umull r2,r3,r4,r5 with r4=0x10000 r5=0x10000 → 0x1_0000_0000 */
  {
    static const uint32_t kProg[] = {
        0xE3A04801u, /* mov r4, #0x10000 */
        0xE3A05801u, /* mov r5, #0x10000 */
        0xE0832594u, /* umull r2, r3, r4, r5 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 4);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[MANGO_REG_LR] = 0x1111u;
    int rc = mango_interp_run(&cpu, &mem, 0x1111u, 100);
    if (rc != 0 || cpu.r[2] != 0u || cpu.r[3] != 1u) {
      fprintf(stderr, "FAIL(umull): rc=%d r2=0x%x r3=0x%x\n", rc, cpu.r[2], cpu.r[3]);
      return 1;
    }
  }

  /* OFDP-shaped: umull r0,r1,r6,r0 with magic 0xcccccccd */
  {
    static const uint32_t kProg[] = {
        0xE0810096u, /* umull r0, r1, r6, r0 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[0] = 0xCCCCCCCDu;
    cpu.r[6] = 0x66EFDB57u;
    cpu.r[MANGO_REG_LR] = 0x2222u;
    int rc = mango_interp_run(&cpu, &mem, 0x2222u, 100);
    uint64_t want = (uint64_t)0x66EFDB57u * (uint64_t)0xCCCCCCCDu;
    if (rc != 0 || cpu.r[0] != (uint32_t)want || cpu.r[1] != (uint32_t)(want >> 32)) {
      fprintf(stderr, "FAIL(umull_ofdp): rc=%d r0=0x%x r1=0x%x want lo=0x%x hi=0x%x\n", rc,
              cpu.r[0], cpu.r[1], (uint32_t)want, (uint32_t)(want >> 32));
      return 1;
    }
  }

  /* umlal r2,r3,r4,r5: accumulate prior 1 into hi */
  {
    static const uint32_t kProg[] = {
        0xE0A32594u, /* umlal r2, r3, r4, r5 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[2] = 1u;
    cpu.r[3] = 1u;
    cpu.r[4] = 0x10000u;
    cpu.r[5] = 0x10000u;
    cpu.r[MANGO_REG_LR] = 0x3333u;
    int rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
    /* 0x1_0000_0000 + 0x1_0000_0001 = 0x2_0000_0001 */
    if (rc != 0 || cpu.r[2] != 1u || cpu.r[3] != 2u) {
      fprintf(stderr, "FAIL(umlal): rc=%d r2=0x%x r3=0x%x\n", rc, cpu.r[2], cpu.r[3]);
      return 1;
    }
  }

  /* smull r2,r3,r4,r5: (-2)*(-3)=6 */
  {
    static const uint32_t kProg[] = {
        0xE0C32594u, /* smull r2, r3, r4, r5 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = (uint32_t)-2;
    cpu.r[5] = (uint32_t)-3;
    cpu.r[MANGO_REG_LR] = 0x4444u;
    int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
    if (rc != 0 || cpu.r[2] != 6u || cpu.r[3] != 0u) {
      fprintf(stderr, "FAIL(smull): rc=%d r2=%u r3=%u\n", rc, cpu.r[2], cpu.r[3]);
      return 1;
    }
  }

  /* smull negative product: (-1)*2 = -2 → lo=0xfffffffe hi=0xffffffff */
  {
    static const uint32_t kProg[] = {
        0xE0C32594u,
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = (uint32_t)-1;
    cpu.r[5] = 2u;
    cpu.r[MANGO_REG_LR] = 0x5555u;
    int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
    if (rc != 0 || cpu.r[2] != 0xFFFFFFFEu || cpu.r[3] != 0xFFFFFFFFu) {
      fprintf(stderr, "FAIL(smull_neg): rc=%d r2=0x%x r3=0x%x\n", rc, cpu.r[2], cpu.r[3]);
      return 1;
    }
  }

  /* smlal r2,r3,r4,r5: (-1)*2 + (-1) = -3 */
  {
    static const uint32_t kProg[] = {
        0xE0E32594u,
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[2] = 0xFFFFFFFFu; /* acc = -1 */
    cpu.r[3] = 0xFFFFFFFFu;
    cpu.r[4] = (uint32_t)-1;
    cpu.r[5] = 2u;
    cpu.r[MANGO_REG_LR] = 0x6666u;
    int rc = mango_interp_run(&cpu, &mem, 0x6666u, 100);
    if (rc != 0 || cpu.r[2] != 0xFFFFFFFDu || cpu.r[3] != 0xFFFFFFFFu) {
      fprintf(stderr, "FAIL(smlal): rc=%d r2=0x%x r3=0x%x\n", rc, cpu.r[2], cpu.r[3]);
      return 1;
    }
  }

  printf("ok: UMULL/UMLAL/SMULL/SMLAL (incl. OFDP 0xe0810096 div10)\n");
  return 0;
}


/* SMLAxy / SMULxy: signed halfword multiply (+accumulate). N=bit5 Rn half,
 * M=bit6 Rm half. Encoding A8.8.207 / A8.8.220. */
static uint32_t encode_smlaxy(int n, int m, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
  return 0xE1000000u | ((rd & 0xFu) << 16) | ((ra & 0xFu) << 12) | ((rm & 0xFu) << 8) |
         (1u << 7) | ((uint32_t)m << 6) | ((uint32_t)n << 5) | (rn & 0xFu);
}

static uint32_t encode_smulxy(int n, int m, uint32_t rd, uint32_t rn, uint32_t rm) {
  return 0xE1600000u | ((rd & 0xFu) << 16) | ((rm & 0xFu) << 8) | (1u << 7) |
         ((uint32_t)m << 6) | ((uint32_t)n << 5) | (rn & 0xFu);
}

static int test_smlaxy_smulxy(void) {
  /* OFDP nativeRender stop 0xe10cc687: smlabb r12, r7, r6, r12 */
  uint32_t ofdp = encode_smlaxy(0, 0, 12, 7, 6, 12);
  if (ofdp != 0xE10CC687u) {
    fprintf(stderr, "FAIL(smlaxy): OFDP SMLABB encoder got 0x%08x\n", ofdp);
    return 1;
  }
  uint32_t smlabb = encode_smlaxy(0, 0, 0, 1, 2, 3);
  uint32_t smlabt = encode_smlaxy(0, 1, 0, 1, 2, 3);
  uint32_t smlatb = encode_smlaxy(1, 0, 0, 1, 2, 3);
  uint32_t smlatt = encode_smlaxy(1, 1, 0, 1, 2, 3);
  uint32_t smulbb = encode_smulxy(0, 0, 0, 1, 2);
  uint32_t smulbt = encode_smulxy(0, 1, 0, 1, 2);
  uint32_t smultb = encode_smulxy(1, 0, 0, 1, 2);
  uint32_t smultt = encode_smulxy(1, 1, 0, 1, 2);
  if (smlabb != 0xE1003281u || smlabt != 0xE10032C1u || smlatb != 0xE10032A1u ||
      smlatt != 0xE10032E1u || smulbb != 0xE1600281u || smulbt != 0xE16002C1u ||
      smultb != 0xE16002A1u || smultt != 0xE16002E1u) {
    fprintf(stderr,
            "FAIL(smlaxy): encoder mismatch smlabb=0x%08x smlabt=0x%08x smlatb=0x%08x "
            "smlatt=0x%08x smulbb=0x%08x smulbt=0x%08x smultb=0x%08x smultt=0x%08x\n",
            smlabb, smlabt, smlatb, smlatt, smulbb, smulbt, smultb, smultt);
    return 1;
  }

  /* Decode OFDP word */
  {
    MangoInsn insn;
    memset(&insn, 0, sizeof(insn));
    if (mango_decode(0xE10CC687u, &insn) != 0 || insn.op != MANGO_OP_SMLA || insn.rd != 12 ||
        insn.rn != 12 || insn.rm != 7 || insn.rs != 6 || insn.b != 0 || insn.u != 0) {
      fprintf(stderr, "FAIL(smlaxy): decode OFDP op=%d rd=%u rn=%u rm=%u rs=%u b=%d u=%d\n",
              (int)insn.op, insn.rd, insn.rn, insn.rm, insn.rs, insn.b, insn.u);
      return 1;
    }
  }

  /* smlabb r0, r1, r2, r3: bottom*bottom + acc. r1=0x0005, r2=0x0007, r3=10 → 45 */
  {
    static const uint32_t kProg[] = {
        0xE1003281u, /* smlabb r0, r1, r2, r3 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 5u;
    cpu.r[2] = 7u;
    cpu.r[3] = 10u;
    cpu.r[MANGO_REG_LR] = 0x1111u;
    int rc = mango_interp_run(&cpu, &mem, 0x1111u, 100);
    if (rc != 0 || cpu.r[0] != 45u) {
      fprintf(stderr, "FAIL(smlabb): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }

  /* OFDP-shaped: smlabb r12, r7, r6, r12 with r7=4 r6=3 r12=19 → 12+19=31 */
  {
    static const uint32_t kProg[] = {
        0xE10CC687u,
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[6] = 3u;
    cpu.r[7] = 4u;
    cpu.r[12] = 19u;
    cpu.r[MANGO_REG_LR] = 0x2222u;
    int rc = mango_interp_run(&cpu, &mem, 0x2222u, 100);
    if (rc != 0 || cpu.r[12] != 31u) {
      fprintf(stderr, "FAIL(smlabb_ofdp): rc=%d r12=%u\n", rc, cpu.r[12]);
      return 1;
    }
  }

  /* smlabt: bottom(Rn)*top(Rm). r1=0x0003, r2=0x00050000 → 3*5=15 + 1 = 16 */
  {
    static const uint32_t kProg[] = {
        0xE10032C1u, /* smlabt r0, r1, r2, r3 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 3u;
    cpu.r[2] = 0x00050000u;
    cpu.r[3] = 1u;
    cpu.r[MANGO_REG_LR] = 0x3333u;
    int rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
    if (rc != 0 || cpu.r[0] != 16u) {
      fprintf(stderr, "FAIL(smlabt): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }

  /* smlatb: top(Rn)*bottom(Rm). r1=0xfffe0000 (-2), r2=4 → -8 + 10 = 2 */
  {
    static const uint32_t kProg[] = {
        0xE10032A1u,
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0xFFFE0000u;
    cpu.r[2] = 4u;
    cpu.r[3] = 10u;
    cpu.r[MANGO_REG_LR] = 0x4444u;
    int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
    if (rc != 0 || cpu.r[0] != 2u) {
      fprintf(stderr, "FAIL(smlatb): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }

  /* smlatt: top*top. r1=0x00070000, r2=0x00060000 → 42 + 0 */
  {
    static const uint32_t kProg[] = {
        0xE10032E1u,
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x00070000u;
    cpu.r[2] = 0x00060000u;
    cpu.r[3] = 0u;
    cpu.r[MANGO_REG_LR] = 0x5555u;
    int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
    if (rc != 0 || cpu.r[0] != 42u) {
      fprintf(stderr, "FAIL(smlatt): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }

  /* smulbb / smulbt / smultb / smultt (no accumulate) */
  {
    static const uint32_t kProg[] = {
        0xE1600281u, /* smulbb r0, r1, r2 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0xFFFFu; /* -1 bottom */
    cpu.r[2] = 5u;
    cpu.r[MANGO_REG_LR] = 0x6666u;
    int rc = mango_interp_run(&cpu, &mem, 0x6666u, 100);
    if (rc != 0 || cpu.r[0] != (uint32_t)-5) {
      fprintf(stderr, "FAIL(smulbb): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }
  {
    static const uint32_t kProg[] = {
        0xE16002C1u, /* smulbt r0, r1, r2 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 2u;
    cpu.r[2] = 0x00030000u;
    cpu.r[MANGO_REG_LR] = 0x7777u;
    int rc = mango_interp_run(&cpu, &mem, 0x7777u, 100);
    if (rc != 0 || cpu.r[0] != 6u) {
      fprintf(stderr, "FAIL(smulbt): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }
  {
    static const uint32_t kProg[] = {
        0xE16002A1u, /* smultb r0, r1, r2 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x00040000u;
    cpu.r[2] = 5u;
    cpu.r[MANGO_REG_LR] = 0x8888u;
    int rc = mango_interp_run(&cpu, &mem, 0x8888u, 100);
    if (rc != 0 || cpu.r[0] != 20u) {
      fprintf(stderr, "FAIL(smultb): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }
  {
    static const uint32_t kProg[] = {
        0xE16002E1u, /* smultt r0, r1, r2 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0xFFFE0000u; /* -2 top */
    cpu.r[2] = 0x00030000u;
    cpu.r[MANGO_REG_LR] = 0x9999u;
    int rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
    if (rc != 0 || cpu.r[0] != (uint32_t)-6) {
      fprintf(stderr, "FAIL(smultt): rc=%d r0=%u\n", rc, cpu.r[0]);
      return 1;
    }
  }

  /* SMLA Q flag: 0x7fffffff + 1 overflows sticky Q */
  {
    static const uint32_t kProg[] = {
        0xE1003281u, /* smlabb r0, r1, r2, r3 */
        0xE12FFF1Eu,
    };
    uint8_t mem_buf[32];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 1u;
    cpu.r[2] = 1u;
    cpu.r[3] = 0x7FFFFFFFu;
    cpu.r[MANGO_REG_LR] = 0xAAAAu;
    int rc = mango_interp_run(&cpu, &mem, 0xAAAAu, 100);
    if (rc != 0 || cpu.r[0] != 0x80000000u || (cpu.cpsr & MANGO_CPSR_Q) == 0) {
      fprintf(stderr, "FAIL(smlabb_q): rc=%d r0=0x%x cpsr=0x%x\n", rc, cpu.r[0], cpu.cpsr);
      return 1;
    }
  }

  printf("ok: SMLAxy/SMULxy (incl. OFDP 0xe10cc687 SMLABB)\n");
  return 0;
}

static int test_ldr_str_writeback_and_postindex(void) {
  uint32_t str_pre = encode_ldst(0, 1, 1, 0, 1, 0, 1, 0, 4);  /* str r0, [r1, #4]! */
  uint32_t str_post = encode_ldst(0, 0, 1, 0, 0, 0, 1, 3, 4); /* str r3, [r1], #4 */
  uint32_t ldr_neg = encode_ldst(0, 1, 0, 0, 0, 1, 1, 4, 4);  /* ldr r4, [r1, #-4] */
  if (str_pre != 0xE5A10004u || str_post != 0xE4813004u || ldr_neg != 0xE5114004u) {
    fprintf(stderr, "FAIL(ldr_str_writeback): encoder mismatch\n");
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A00064u, /* mov r0, #100 */
      0xE3A01040u, /* mov r1, #64 */
      0xE5A10004u, /* str r0, [r1, #4]! */
      0xE3A03009u, /* mov r3, #9 */
      0xE4813004u, /* str r3, [r1], #4 */
      0xE5114004u, /* ldr r4, [r1, #-4] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 7);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x5151u;

  int rc = mango_interp_run(&cpu, &mem, 0x5151u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(ldr_str_writeback): mango_interp_run returned %d\n", rc);
    return 1;
  }
  /* str [r1,#4]! with r1=64: mem[68]=100, r1=68. Then str [r1],#4 with r3=9:
   * mem[68]=9, r1=72. Then ldr r4, [r1, #-4]: r4=mem[68]=9, r1 stays 72. */
  if (cpu.r[1] != 72 || cpu.r[4] != 9 || bytes_to_u32_le(mem_buf + 68) != 9) {
    fprintf(stderr, "FAIL(ldr_str_writeback): r1=%u r4=%u mem[68]=%u\n", cpu.r[1], cpu.r[4],
            bytes_to_u32_le(mem_buf + 68));
    return 1;
  }
  printf("ok: ldr/str pre-index writeback and post-index\n");
  return 0;
}

static int test_ldr_register_offset(void) {
  uint32_t str_reg = encode_ldst(1, 1, 1, 0, 0, 0, 1, 0, 2);     /* str r0, [r1, r2] */
  uint32_t ldr_lsl = encode_ldst(1, 1, 1, 0, 0, 1, 1, 4, 0x83u); /* ldr r4, [r1, r3, LSL #1] */
  if (str_reg != 0xE7810002u || ldr_lsl != 0xE7914083u) {
    fprintf(stderr, "FAIL(ldr_register_offset): encoder mismatch str=0x%08x ldr=0x%08x\n", str_reg,
            ldr_lsl);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A00037u, /* mov r0, #55 */
      0xE3A01040u, /* mov r1, #64 */
      0xE3A02008u, /* mov r2, #8 */
      0xE7810002u, /* str r0, [r1, r2] */
      0xE3A03004u, /* mov r3, #4 */
      0xE7914083u, /* ldr r4, [r1, r3, LSL #1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 7);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x6161u;

  int rc = mango_interp_run(&cpu, &mem, 0x6161u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(ldr_register_offset): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 55 || bytes_to_u32_le(mem_buf + 72) != 55 || cpu.r[1] != 64) {
    fprintf(stderr, "FAIL(ldr_register_offset): r4=%u mem[72]=%u r1=%u\n", cpu.r[4],
            bytes_to_u32_le(mem_buf + 72), cpu.r[1]);
    return 1;
  }
  printf("ok: ldr/str register offset, including LSL #1\n");
  return 0;
}

static int test_register_specified_shift(void) {
  /* mov r1, r0, LSL r2 / mov r3, r1, LSR r2 */
  static const uint32_t kProgram[] = {
      0xE3A00003u, /* mov r0, #3 */
      0xE3A02004u, /* mov r2, #4 */
      0xE1A01210u, /* mov r1, r0, LSL r2 */
      0xE1A03231u, /* mov r3, r1, LSR r2 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x7171u;

  int rc = mango_interp_run(&cpu, &mem, 0x7171u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(register_specified_shift): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[1] != 48 || cpu.r[3] != 3) {
    fprintf(stderr, "FAIL(register_specified_shift): expected r1=48 r3=3, got r1=%u r3=%u\n",
            cpu.r[1], cpu.r[3]);
    return 1;
  }
  printf("ok: register-specified shift LSL/LSR Rs\n");
  return 0;
}

static int test_shifter_carry_and_rrx(void) {
  static const uint32_t kProgram[] = {
      0xE3A00001u, /* mov r0, #1 */
      0xE1A00F80u, /* mov r0, r0, LSL #31 */
      0xE1B01080u, /* movs r1, r0, LSL #1 */
      0xE1B03020u, /* movs r3, r0, LSR #32 */
      0xE1B02060u, /* movs r2, r0, RRX */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0x8181u;

  int rc = mango_interp_run(&cpu, &mem, 0x8181u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(shifter_carry_and_rrx): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[1] != 0 || cpu.r[3] != 0) {
    fprintf(stderr, "FAIL(shifter_carry_and_rrx): expected r1=0 r3=0, got r1=%u r3=%u\n", cpu.r[1],
            cpu.r[3]);
    return 1;
  }
  if (cpu.r[2] != 0xC0000000u) {
    fprintf(stderr, "FAIL(shifter_carry_and_rrx): expected r2=0xc0000000 after RRX, got 0x%08x\n",
            cpu.r[2]);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_C) != 0) {
    fprintf(stderr, "FAIL(shifter_carry_and_rrx): RRX of even r0 should clear C\n");
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_Z) != 0 || (cpu.cpsr & MANGO_CPSR_N) == 0) {
    fprintf(stderr, "FAIL(shifter_carry_and_rrx): expected N set and Z clear after movs rrx\n");
    return 1;
  }
  printf("ok: shifter carry-out, LSR #32, and RRX\n");
  return 0;
}

static int test_ldst_rejected_shapes(void) {
  MangoInsn insn;
  uint32_t ldrt = encode_ldst(0, 0, 1, 0, 1, 1, 1, 0, 0); /* ldrt r0, [r1] */
  if (mango_decode(ldrt, &insn) == 0) {
    fprintf(stderr, "FAIL(ldst_rejected_shapes): ldrt was decoded\n");
    return 1;
  }
  uint32_t wb_pc = encode_ldst(0, 1, 1, 0, 1, 1, MANGO_REG_PC, 0, 4); /* ldr r0, [pc, #4]! */
  if (mango_decode(wb_pc, &insn) == 0) {
    fprintf(stderr, "FAIL(ldst_rejected_shapes): writeback into pc was decoded\n");
    return 1;
  }
  uint32_t ldr_same = encode_ldst(0, 1, 1, 0, 1, 1, 1, 1, 4); /* ldr r1, [r1, #4]! */
  if (mango_decode(ldr_same, &insn) == 0) {
    fprintf(stderr, "FAIL(ldst_rejected_shapes): ldr writeback into same dest was decoded\n");
    return 1;
  }
  uint32_t shift_pc = 0xE1A00F11u; /* mov r0, r1, LSL pc */
  if (mango_decode(shift_pc, &insn) == 0) {
    fprintf(stderr, "FAIL(ldst_rejected_shapes): LSL pc shift amount was decoded\n");
    return 1;
  }
  printf("ok: ldrt / writeback-pc / ldr-same-dest / LSL-pc shapes rejected\n");
  return 0;
}

/* Extra load/store: bits 27-25=000, bit7=bit4=1. i is bit 22 (1=imm). */
static uint32_t encode_extra_ldst(int p, int u, int i, int w, int l, uint32_t rn, uint32_t rt,
                                  int s, int h, uint32_t imm8_or_rm) {
  uint32_t hi = i ? ((imm8_or_rm >> 4) & 0xFu) : 0u;
  uint32_t lo = imm8_or_rm & 0xFu;
  return 0xE0000000u | ((uint32_t)p << 24) | ((uint32_t)u << 23) | ((uint32_t)i << 22) |
         ((uint32_t)w << 21) | ((uint32_t)l << 20) | ((rn & 0xFu) << 16) | ((rt & 0xFu) << 12) |
         (hi << 8) | (1u << 7) | ((uint32_t)s << 6) | ((uint32_t)h << 5) | (1u << 4) | lo;
}

static int test_ldrh_strh_roundtrip(void) {
  uint32_t strh = encode_extra_ldst(1, 1, 1, 0, 0, 1, 0, 0, 1, 4); /* strh r0, [r1, #4] */
  uint32_t ldrh = encode_extra_ldst(1, 1, 1, 0, 1, 1, 2, 0, 1, 4); /* ldrh r2, [r1, #4] */
  if (strh != 0xE1C100B4u || ldrh != 0xE1D120B4u) {
    fprintf(stderr, "FAIL(ldrh_strh_roundtrip): encoder mismatch strh=0x%08x ldrh=0x%08x\n", strh,
            ldrh);
    return 1;
  }

  /* Neighbor bytes must stay 0, proving STRH writes two bytes not four. */
  static const uint32_t kProg[] = {
      0xE3A000ABu, /* mov r0, #0xAB */
      0xE3A01040u, /* mov r1, #64 */
      0xE1C100B4u, /* strh r0, [r1, #4] */
      0xE3A02000u, /* mov r2, #0 */
      0xE1D120B4u, /* ldrh r2, [r1, #4] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProg, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xA1A1u;

  int rc = mango_interp_run(&cpu, &mem, 0xA1A1u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(ldrh_strh_roundtrip): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[2] != 0xABu) {
    fprintf(stderr, "FAIL(ldrh_strh_roundtrip): expected r2==0xab, got 0x%x\n", cpu.r[2]);
    return 1;
  }
  if (mem_buf[68] != 0xAB || mem_buf[69] != 0 || mem_buf[70] != 0 || mem_buf[71] != 0) {
    fprintf(stderr, "FAIL(ldrh_strh_roundtrip): strh wrote more than two bytes\n");
    return 1;
  }
  printf("ok: strh + ldrh round trip, zero-extended (r2 = %u)\n", cpu.r[2]);
  return 0;
}

static int test_ldrsb_ldrsh_sign_extend(void) {
  uint32_t ldrsb = encode_extra_ldst(1, 1, 1, 0, 1, 1, 0, 1, 0, 0); /* ldrsb r0, [r1] */
  uint32_t ldrsh = encode_extra_ldst(1, 1, 1, 0, 1, 1, 2, 1, 1, 2); /* ldrsh r2, [r1, #2] */
  if (ldrsb != 0xE1D100D0u || ldrsh != 0xE1D120F2u) {
    fprintf(stderr, "FAIL(ldrsb_ldrsh_sign_extend): encoder mismatch\n");
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A01040u, /* mov r1, #64 */
      0xE1D100D0u, /* ldrsb r0, [r1] */
      0xE1D120F2u, /* ldrsh r2, [r1, #2] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  mem_buf[64] = 0xAB; /* signed byte -85 */
  mem_buf[66] = 0x00;
  mem_buf[67] = 0x80; /* halfword 0x8000, little-endian */
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xB2B2u;

  int rc = mango_interp_run(&cpu, &mem, 0xB2B2u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(ldrsb_ldrsh_sign_extend): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xFFFFFFABu) {
    fprintf(stderr, "FAIL(ldrsb_ldrsh_sign_extend): expected r0==0xffffffab, got 0x%08x\n",
            cpu.r[0]);
    return 1;
  }
  if (cpu.r[2] != 0xFFFF8000u) {
    fprintf(stderr, "FAIL(ldrsb_ldrsh_sign_extend): expected r2==0xffff8000, got 0x%08x\n",
            cpu.r[2]);
    return 1;
  }
  printf("ok: ldrsb/ldrsh sign-extend (r0=0x%08x, r2=0x%08x)\n", cpu.r[0], cpu.r[2]);
  return 0;
}

static int test_extra_ldst_writeback_and_reg_offset(void) {
  uint32_t strh_wb = encode_extra_ldst(1, 1, 1, 1, 0, 1, 0, 0, 1, 4);  /* strh r0, [r1, #4]! */
  uint32_t ldrh_reg = encode_extra_ldst(1, 1, 0, 0, 1, 1, 2, 0, 1, 2); /* ldrh r2, [r1, r2] */
  if (strh_wb != 0xE1E100B4u || ldrh_reg != 0xE19120B2u) {
    fprintf(stderr, "FAIL(extra_ldst_writeback): encoder mismatch wb=0x%08x reg=0x%08x\n", strh_wb,
            ldrh_reg);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A000AAu, /* mov r0, #0xAA */
      0xE3A01040u, /* mov r1, #64 */
      0xE1E100B4u, /* strh r0, [r1, #4]! */
      0xE3A02000u, /* mov r2, #0  — then we need r2=0 as offset from new r1? */
      0xE19120B2u, /* ldrh r2, [r1, r2]  r1=68, r2=0 => load mem[68] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xC3C3u;

  int rc = mango_interp_run(&cpu, &mem, 0xC3C3u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(extra_ldst_writeback): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[1] != 68 || cpu.r[2] != 0xAAu) {
    fprintf(stderr, "FAIL(extra_ldst_writeback): expected r1=68 r2=0xaa, got r1=%u r2=0x%x\n",
            cpu.r[1], cpu.r[2]);
    return 1;
  }
  printf("ok: strh writeback and ldrh register offset\n");
  return 0;
}

static int test_ldrh_unaligned_ok(void) {
  static const uint32_t kProgram[] = {
      0xE3A01041u, /* mov r1, #65 */
      0xE1D100B0u, /* ldrh r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  mem_buf[65] = 0x34;
  mem_buf[66] = 0x12;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xFFFFFFFFu;

  int rc = mango_interp_run(&cpu, &mem, 0xFFFFFFFFu, 100);
  if (rc != 0 || cpu.r[0] != 0x1234u) {
    fprintf(stderr, "FAIL(ldrh_unaligned_ok): rc=%d r0=0x%x, want 0x1234\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: unaligned ldrh loads little-endian halfword\n");
  return 0;
}

static int test_extra_ldst_rejected_shapes(void) {
  MangoInsn insn;
  uint32_t odd = encode_extra_ldst(1, 1, 1, 0, 0, 1, 1, 1, 0, 0); /* ldrd r1, [r1] odd Rt */
  if (mango_decode(odd, &insn) == 0) {
    fprintf(stderr, "FAIL(extra_ldst_rejected_shapes): ldrd with odd rt was decoded\n");
    return 1;
  }
  uint32_t wb_pc = encode_extra_ldst(1, 1, 1, 1, 1, MANGO_REG_PC, 0, 0, 1, 4);
  if (mango_decode(wb_pc, &insn) == 0) {
    fprintf(stderr, "FAIL(extra_ldst_rejected_shapes): ldrh writeback into pc was decoded\n");
    return 1;
  }
  printf("ok: ldrd-odd-rt / ldrh-writeback-pc shapes rejected\n");
  return 0;
}

static int test_ldrd_strd_roundtrip(void) {
  uint32_t strd = encode_extra_ldst(1, 1, 1, 0, 0, 2, 0, 1, 1, 8); /* strd r0, r1, [r2, #8] */
  uint32_t ldrd = encode_extra_ldst(1, 1, 1, 0, 0, 2, 4, 1, 0, 8); /* ldrd r4, r5, [r2, #8] */
  if (strd != 0xE1C200F8u || ldrd != 0xE1C240D8u) {
    fprintf(stderr, "FAIL(ldrd_strd_roundtrip): encoder mismatch strd=0x%08x ldrd=0x%08x\n", strd,
            ldrd);
    return 1;
  }

  static const uint32_t kProgram[] = {
      0xE3A00011u, /* mov r0, #0x11 */
      0xE3A01022u, /* mov r1, #0x22 */
      0xE3A02040u, /* mov r2, #64 */
      0xE1C200F8u, /* strd r0, r1, [r2, #8] */
      0xE1C240D8u, /* ldrd r4, r5, [r2, #8] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xE5E5u;

  int rc = mango_interp_run(&cpu, &mem, 0xE5E5u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(ldrd_strd_roundtrip): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 0x11 || cpu.r[5] != 0x22) {
    fprintf(stderr, "FAIL(ldrd_strd_roundtrip): expected r4=0x11 r5=0x22, got r4=0x%x r5=0x%x\n",
            cpu.r[4], cpu.r[5]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + 72) != 0x11 || bytes_to_u32_le(mem_buf + 76) != 0x22) {
    fprintf(stderr, "FAIL(ldrd_strd_roundtrip): memory pair is not r0 then r1\n");
    return 1;
  }
  printf("ok: strd + ldrd round trip (r4=0x%x, r5=0x%x)\n", cpu.r[4], cpu.r[5]);
  return 0;
}

static int test_thumb_ldmia_wb(void) {
  /* Meritous SDL_main hit ldm r6!,{r0} (0xce01) with r6=0x17 because argv
   * was a JNI handle id — not an LDM execute bug. Prove Thumb LDMIA works
   * with an aligned base, and that an unaligned base still fails (ARM). */
  MangoInsn insn;
  if (mango_decode_t16(0xce01u, &insn) != 0 || insn.op != MANGO_OP_LDM || insn.rn != 6u ||
      insn.reglist != 0x1u || !insn.w || !insn.u || insn.p) {
    fprintf(stderr, "FAIL(thumb_ldmia_wb): 0xce01 did not decode as ldm r6!,{r0}\n");
    return 1;
  }

  static const uint16_t kProg[] = {
      0xce01u, /* ldm r6!, {r0} */
      0x4770u, /* bx lr */
  };
  uint8_t mem_buf[64];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);
  /* word at 16: payload 0xA1B2C3D4 */
  mem_buf[16] = 0xD4;
  mem_buf[17] = 0xC3;
  mem_buf[18] = 0xB2;
  mem_buf[19] = 0xA1;

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[6] = 16u;
  cpu.r[MANGO_REG_LR] = 0xCAFE0000u;
  int rc = mango_interp_run(&cpu, &mem, 0xCAFE0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_ldmia_wb): aligned ldm returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xA1B2C3D4u || cpu.r[6] != 20u) {
    fprintf(stderr, "FAIL(thumb_ldmia_wb): r0=0x%x r6=%u want r0=0xa1b2c3d4 r6=20\n", cpu.r[0],
            cpu.r[6]);
    return 1;
  }

  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[6] = 17u; /* Meritous-shaped unaligned base */
  cpu.r[MANGO_REG_LR] = 0xCAFE0000u;
  cpu.r[MANGO_REG_PC] = 0;
  rc = mango_interp_run(&cpu, &mem, 0xCAFE0000u, 100);
  if (rc == 0) {
    fprintf(stderr, "FAIL(thumb_ldmia_wb): unaligned ldm r6=17 should stop\n");
    return 1;
  }
  printf("ok: Thumb ldm r6!,{r0} aligned loads+wb; unaligned base stops\n");
  return 0;
}

static int test_thumb_mov_add_bx(void) {
  static const uint16_t kProg[] = {
      0x2002u, /* mov r0, #2 */
      0x3003u, /* add r0, #3 */
      0x4770u, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[MANGO_REG_LR] = 0xCAFE0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xCAFE0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_mov_add_bx): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 5) {
    fprintf(stderr, "FAIL(thumb_mov_add_bx): expected r0 == 5, got %u\n", cpu.r[0]);
    return 1;
  }
  if (cpu.cpsr & MANGO_CPSR_T) {
    fprintf(stderr, "FAIL(thumb_mov_add_bx): bx to even lr should have left Thumb state\n");
    return 1;
  }
  printf("ok: thumb mov + add + bx (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_push_pop(void) {
  static const uint16_t kProg[] = {
      0xB510u, /* push {r4, lr} */
      0x2401u, /* mov r4, #1 */
      0xBD10u, /* pop {r4, pc} */
  };

  uint8_t mem_buf[256];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[4] = 0xA1A1A1A1u;
  cpu.r[MANGO_REG_SP] = 128;
  cpu.r[MANGO_REG_LR] = 0xDEAD0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xDEAD0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_push_pop): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 0xA1A1A1A1u || cpu.r[MANGO_REG_SP] != 128) {
    fprintf(stderr, "FAIL(thumb_push_pop): r4=0x%x sp=%u\n", cpu.r[4], cpu.r[MANGO_REG_SP]);
    return 1;
  }
  printf("ok: thumb push/pop restores r4 and returns via pc\n");
  return 0;
}

static int test_arm_bx_into_thumb(void) {
  /* ARM at 0: bx r1. r1 = 8|1, Thumb at 8: mov r0, #7; bx lr */
  static const uint32_t kArm[] = {
      0xE12FFF11u, /* bx r1 */
  };
  static const uint16_t kThumb[] = {
      0x2007u, /* mov r0, #7 */
      0x4770u, /* bx lr */
  };

  uint8_t mem_buf[64];
  memset(mem_buf, 0, sizeof(mem_buf));
  mem_buf[0] = 0x11;
  mem_buf[1] = 0xFF;
  mem_buf[2] = 0x2F;
  mem_buf[3] = 0xE1;
  mem_buf[8] = 0x07;
  mem_buf[9] = 0x20;
  mem_buf[10] = 0x70;
  mem_buf[11] = 0x47;
  (void)kArm;
  (void)kThumb;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[1] = 8u | 1u;
  cpu.r[MANGO_REG_LR] = 0xF00Fu;

  int rc = mango_interp_run(&cpu, &mem, 0xF00Fu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(arm_bx_into_thumb): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 7) {
    fprintf(stderr, "FAIL(arm_bx_into_thumb): expected r0 == 7, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: ARM bx into Thumb and back (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_bl(void) {
  /* BL at 0 to 8 (imm=4). Return lands at 4. Callee sets r0=42. */
  static const uint16_t kProg[] = {
      0xF000u, 0xF802u, /* bl .+8 */
      0x2101u,          /* mov r1, #1 */
      0x4710u,          /* bx r2 */
      0x202Au,          /* mov r0, #42 */
      0x4770u,          /* bx lr */
  };

  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[2] = 0xBEEF0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xBEEF0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_bl): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 42 || cpu.r[1] != 1) {
    fprintf(stderr, "FAIL(thumb_bl): expected r0=42 r1=1, got r0=%u r1=%u\n", cpu.r[0], cpu.r[1]);
    return 1;
  }
  printf("ok: thumb BL call and return (r0 = %u, r1 = %u)\n", cpu.r[0], cpu.r[1]);
  return 0;
}

static int test_thumb_movw_movt(void) {
  static const uint16_t kProg[] = {
      0xF241u, 0x2034u, /* movw r0, #0x1234 */
      0xF6CAu, 0x30CDu, /* movt r0, #0xabcd */
      0x4770u,          /* bx lr */
  };

  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[MANGO_REG_LR] = 0xCAFE0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xCAFE0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_movw_movt): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xABCD1234u) {
    fprintf(stderr, "FAIL(thumb_movw_movt): expected r0==0xabcd1234, got 0x%08x\n", cpu.r[0]);
    return 1;
  }
  printf("ok: thumb MOVW/MOVT (r0 = 0x%08x)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_it_eq_taken(void) {
  static const uint16_t kProg[] = {
      0x4280u, /* cmp r0, r0 */
      0xBF08u, /* it eq */
      0x3001u, /* add r0, #1 */
      0x4770u, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[0] = 7;
  cpu.r[MANGO_REG_LR] = 0x11110000u;

  int rc = mango_interp_run(&cpu, &mem, 0x11110000u, 100);
  if (rc != 0 || cpu.r[0] != 8) {
    fprintf(stderr, "FAIL(thumb_it_eq_taken): rc=%d r0=%u, want r0=8\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: thumb IT EQ taken (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_it_eq_skipped(void) {
  static const uint16_t kProg[] = {
      0x2801u, /* cmp r0, #1 */
      0xBF08u, /* it eq */
      0x3001u, /* add r0, #1 */
      0x4770u, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[0] = 7;
  cpu.r[MANGO_REG_LR] = 0x22220000u;

  int rc = mango_interp_run(&cpu, &mem, 0x22220000u, 100);
  if (rc != 0 || cpu.r[0] != 7) {
    fprintf(stderr, "FAIL(thumb_it_eq_skipped): rc=%d r0=%u, want r0=7\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: thumb IT EQ skipped (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_b_w(void) {
  static const uint16_t kProg[] = {
      0xF000u, 0xB802u, /* b.w .+8 */
      0x2001u,          /* mov r0, #1, must skip */
      0x4770u,          /* bx lr */
      0x2009u,          /* mov r0, #9 */
      0x4770u,          /* bx lr */
  };

  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[MANGO_REG_LR] = 0x33330000u;

  int rc = mango_interp_run(&cpu, &mem, 0x33330000u, 100);
  if (rc != 0 || cpu.r[0] != 9) {
    fprintf(stderr, "FAIL(thumb_b_w): rc=%d r0=%u, want r0=9\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: thumb B.W (r0 = %u)\n", cpu.r[0]);
  return 0;
}

static int test_swp_and_swpb(void) {
  static const uint32_t kProgram[] = {
      0xE3A00011u, /* mov r0, #0x11  (will be overwritten by swp) */
      0xE3A01022u, /* mov r1, #0x22 */
      0xE3A02040u, /* mov r2, #64 */
      0xE1020091u, /* swp r0, r1, [r2] */
      0xE3A03033u, /* mov r3, #0x33 */
      0xE1423091u, /* swpb r3, r1, [r2] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 7);
  mem_buf[64] = 0x44;
  mem_buf[65] = 0x55;
  mem_buf[66] = 0x66;
  mem_buf[67] = 0x77; /* word 0x77665544 */
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xD4D4u;

  int rc = mango_interp_run(&cpu, &mem, 0xD4D4u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(swp_and_swpb): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0x77665544u) {
    fprintf(stderr, "FAIL(swp_and_swpb): expected r0==0x77665544 after swp, got 0x%08x\n",
            cpu.r[0]);
    return 1;
  }
  /* After swp, mem is 0x00000022 (r1). Then swpb stores r1's low byte 0x22
   * again and r3 gets the low byte that was there, 0x22. */
  if (cpu.r[3] != 0x22u) {
    fprintf(stderr, "FAIL(swp_and_swpb): expected r3==0x22 after swpb, got 0x%x\n", cpu.r[3]);
    return 1;
  }
  if (mem_buf[64] != 0x22 || mem_buf[65] != 0 || mem_buf[66] != 0 || mem_buf[67] != 0) {
    fprintf(stderr, "FAIL(swp_and_swpb): memory after swp/swpb is %02x %02x %02x %02x\n",
            mem_buf[64], mem_buf[65], mem_buf[66], mem_buf[67]);
    return 1;
  }
  printf("ok: swp and swpb (r0=0x%08x, r3=0x%x)\n", cpu.r[0], cpu.r[3]);
  return 0;
}

static int test_arm_movw_movt(void) {
  /* JNI_VERSION_1_6 is the real Unity JNI_OnLoad return: movw/movt r0. */
  static const uint32_t kProgram[] = {
      0xE3000006u, /* movw r0, #6 */
      0xE3400001u, /* movt r0, #1 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[0] = 0xFFFFFFFFu;
  cpu.r[MANGO_REG_LR] = 0xCAFE0000u;

  int rc = mango_interp_run(&cpu, &mem, 0xCAFE0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(arm_movw_movt): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0x00010006u) {
    fprintf(stderr, "FAIL(arm_movw_movt): expected r0==0x00010006, got 0x%08x\n", cpu.r[0]);
    return 1;
  }
  printf("ok: A32 MOVW/MOVT builds JNI_VERSION_1_6 (r0=0x%08x)\n", cpu.r[0]);
  return 0;
}

static int test_arm_blx_reg(void) {
  /* blx r1 to a callee at 8; return lands on bx r2 (the sentinel). */
  static const uint32_t kProgram[] = {
      0xE12FFF31u, /* blx r1 */
      0xE12FFF12u, /* bx r2 */
      0xE3A00007u, /* mov r0, #7 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[1] = 8u;
  cpu.r[2] = 0xBEEF0000u;
  cpu.r[MANGO_REG_LR] = 0xBAD0u;

  int rc = mango_interp_run(&cpu, &mem, 0xBEEF0000u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(arm_blx_reg): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 7) {
    fprintf(stderr, "FAIL(arm_blx_reg): expected r0==7 after blx r1, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: A32 BLX Rm calls through a register (r0=%u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb_blx_reg(void) {
  /* Thumb: blx r1 to ARM callee at 8; return lands on bx r2 (the sentinel). */
  static const uint16_t kThumb[] = {
      0x4788u, /* blx r1 */
      0x4710u, /* bx r2 */
  };
  static const uint32_t kArm[] = {
      0xE3A0002Au, /* mov r0, #42 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, 8, kThumb, 2);
  mem_buf[8] = (uint8_t)(kArm[0] & 0xFFu);
  mem_buf[9] = (uint8_t)((kArm[0] >> 8) & 0xFFu);
  mem_buf[10] = (uint8_t)((kArm[0] >> 16) & 0xFFu);
  mem_buf[11] = (uint8_t)((kArm[0] >> 24) & 0xFFu);
  mem_buf[12] = (uint8_t)(kArm[1] & 0xFFu);
  mem_buf[13] = (uint8_t)((kArm[1] >> 8) & 0xFFu);
  mem_buf[14] = (uint8_t)((kArm[1] >> 16) & 0xFFu);
  mem_buf[15] = (uint8_t)((kArm[1] >> 24) & 0xFFu);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[1] = 8u;
  cpu.r[2] = 0xF00Du;
  cpu.r[MANGO_REG_LR] = 0xBAD0u;

  int rc = mango_interp_run(&cpu, &mem, 0xF00Du, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_blx_reg): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 42) {
    fprintf(stderr, "FAIL(thumb_blx_reg): expected r0==42, got %u\n", cpu.r[0]);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_T) == 0) {
    fprintf(stderr, "FAIL(thumb_blx_reg): return from ARM callee should restore Thumb\n");
    return 1;
  }
  printf("ok: Thumb BLX Rm into ARM and back (r0=%u)\n", cpu.r[0]);
  return 0;
}

static int test_thumb32_ldr_str_imm(void) {
  /* STR.W r2, [r1, #8]; LDR.W r0, [r1, #8]; bx lr */
  static const uint16_t kProg[] = {
      0xF8C1u, 0x2008u, /* str.w r2, [r1, #8] */
      0xF8D1u, 0x0008u, /* ldr.w r0, [r1, #8] */
      0x4770u,          /* bx lr */
  };

  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[1] = 16u;
  cpu.r[2] = 0xAABBCCDDu;
  cpu.r[MANGO_REG_LR] = 0xD00Du;

  int rc = mango_interp_run(&cpu, &mem, 0xD00Du, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb32_ldr_str_imm): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 0xAABBCCDDu) {
    fprintf(stderr, "FAIL(thumb32_ldr_str_imm): expected r0==0xAABBCCDD, got 0x%08x\n", cpu.r[0]);
    return 1;
  }
  printf("ok: T32 STR.W/LDR.W imm12 roundtrip (r0=0x%08x)\n", cpu.r[0]);
  return 0;
}

static int test_thumb32_addw(void) {
  /* addw r0, r0, #8; bx lr */
  static const uint16_t kProg[] = {
      0xF200u, 0x0008u, /* addw r0, r0, #8 */
      0x4770u,          /* bx lr */
  };

  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[0] = 10;
  cpu.r[MANGO_REG_LR] = 0xABCDu;

  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb32_addw): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 18) {
    fprintf(stderr, "FAIL(thumb32_addw): expected r0==18, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: T32 ADDW r0, r0, #8 (r0=%u)\n", cpu.r[0]);
  return 0;
}


static int test_t32_push_w_stmdb_sp(void) {
  /* Q-OTTD-0a: push.w {r3-r11,lr} = e92d 4ff8. Stop at PC=4 after the T32. */
  static const uint16_t kProg[] = {
      0xE92Du, 0x4FF8u, /* stmdb sp!, {r3-r11,lr} */
      0x4770u,          /* bx lr (not executed) */
  };
  uint8_t mem_buf[0x200];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  memset(mem_buf + 16, 0xA5, sizeof(mem_buf) - 16);

  MangoInsn di;
  if (mango_decode_t32(0xE92Du, 0x4FF8u, &di) != 0 || di.op != MANGO_OP_STM ||
      di.rn != MANGO_REG_SP || di.p != 1 || di.u != 0 || di.w != 1 || di.reglist != 0x4FF8u) {
    fprintf(stderr, "FAIL(t32_push_w): decode op=%d rn=%u p=%d u=%d w=%d list=0x%x\n", di.op,
            di.rn, di.p, di.u, di.w, di.reglist);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t pre_sp = 0x100u;
  cpu.r[MANGO_REG_SP] = pre_sp;
  cpu.r[3] = 0x30033333u;
  cpu.r[4] = 0x40044444u;
  cpu.r[5] = 0x50055555u;
  cpu.r[6] = 0x60066666u;
  cpu.r[7] = 0x70077777u;
  cpu.r[8] = 0x80088888u;
  cpu.r[9] = 0x90099999u;
  cpu.r[10] = 0xA00AAAAAu;
  cpu.r[11] = 0xB00BBBBBu;
  cpu.r[MANGO_REG_LR] = 0xE00EEEEEu;

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 4u, 10);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_push_w): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp - 40u) {
    fprintf(stderr, "FAIL(t32_push_w): sp=0x%x want 0x%x\n", cpu.r[MANGO_REG_SP], pre_sp - 40u);
    return 1;
  }
  static const uint32_t kWant[] = {0x30033333u, 0x40044444u, 0x50055555u, 0x60066666u,
                                   0x70077777u, 0x80088888u, 0x90099999u, 0xA00AAAAAu,
                                   0xB00BBBBBu, 0xE00EEEEEu};
  uint32_t sp = cpu.r[MANGO_REG_SP];
  for (int i = 0; i < 10; i++) {
    uint32_t got = bytes_to_u32_le(mem_buf + sp + (uint32_t)i * 4u);
    if (got != kWant[i]) {
      fprintf(stderr, "FAIL(t32_push_w): mem[+0x%x]=0x%x want 0x%x\n", i * 4, got, kWant[i]);
      return 1;
    }
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_push_w): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 PUSH.W {r3-r11,lr} (Q-OTTD-0a)\n");
  return 0;
}

static int test_t32_mul_ra15(void) {
  /* Q-OTTD-0b: mul.w r1, r1, r4 = fb01 f104 */
  MangoInsn di;
  if (mango_decode_t32(0xFB01u, 0xF104u, &di) != 0 || di.op != MANGO_OP_MUL || di.rd != 1 ||
      di.rm != 1 || di.rs != 4 || di.sets_flags != 0) {
    fprintf(stderr, "FAIL(t32_mul): decode op=%d rd=%u rm=%u rs=%u s=%d\n", di.op, di.rd, di.rm,
            di.rs, di.sets_flags);
    return 1;
  }

  struct {
    uint32_t r1, r4, want;
  } cases[] = {
      {7u, 9u, 0x3fu},
      {0x12345678u, 0x10u, 0x23456780u},
      {0xffffffffu, 0xffffffffu, 0x1u},
  };
  for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    static const uint16_t kProg[] = {0xFB01u, 0xF104u, 0x4770u};
    uint8_t mem_buf[32];
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[1] = cases[c].r1;
    cpu.r[4] = cases[c].r4;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
    if (rc != 0 || cpu.r[1] != cases[c].want || cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t32_mul#%u): rc=%d r1=0x%x want 0x%x cpsr 0x%x->0x%x\n", c, rc,
              cpu.r[1], cases[c].want, cpsr_before, cpu.cpsr);
      return 1;
    }
  }
  printf("ok: T32 MUL Rd,Rn,Rm Ra=15 flags off (Q-OTTD-0b)\n");
  return 0;
}

static int test_t32_pop_w_ldmia_sp(void) {
  /* Q-OTTD-0c: pop.w {r3-r11,lr} = e8bd 4ff8. Stop at PC=4 after the T32. */
  static const uint16_t kProg[] = {
      0xE8BDu, 0x4FF8u, /* ldmia sp!, {r3-r11,lr} */
      0x4770u,          /* bx lr (not executed) */
  };
  uint8_t mem_buf[0x200];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  memset(mem_buf + 16, 0xA5, sizeof(mem_buf) - 16);

  MangoInsn di;
  if (mango_decode_t32(0xE8BDu, 0x4FF8u, &di) != 0 || di.op != MANGO_OP_LDM ||
      di.rn != MANGO_REG_SP || di.p != 0 || di.u != 1 || di.w != 1 || di.reglist != 0x4FF8u) {
    fprintf(stderr, "FAIL(t32_pop_w): decode op=%d rn=%u p=%d u=%d w=%d list=0x%x\n", di.op,
            di.rn, di.p, di.u, di.w, di.reglist);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t pre_sp = 0x40u;
  cpu.r[MANGO_REG_SP] = pre_sp;
  static const uint32_t kSeed[] = {0x30033333u, 0x40044444u, 0x50055555u, 0x60066666u,
                                   0x70077777u, 0x80088888u, 0x90099999u, 0xA00AAAAAu,
                                   0xB00BBBBBu, 0xE00EEEEEu};
  for (int i = 0; i < 10; i++) {
    u32_to_bytes_le(mem_buf + pre_sp + (uint32_t)i * 4u, kSeed[i]);
  }
  /* GPRs zeroed via memset */

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 4u, 10);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_pop_w): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp + 40u) {
    fprintf(stderr, "FAIL(t32_pop_w): sp=0x%x want 0x%x\n", cpu.r[MANGO_REG_SP], pre_sp + 40u);
    return 1;
  }
  if (cpu.r[3] != 0x30033333u || cpu.r[4] != 0x40044444u || cpu.r[5] != 0x50055555u ||
      cpu.r[6] != 0x60066666u || cpu.r[7] != 0x70077777u || cpu.r[8] != 0x80088888u ||
      cpu.r[9] != 0x90099999u || cpu.r[10] != 0xA00AAAAAu || cpu.r[11] != 0xB00BBBBBu ||
      cpu.r[MANGO_REG_LR] != 0xE00EEEEEu) {
    fprintf(stderr,
            "FAIL(t32_pop_w): regs r3=0x%x r4=0x%x r5=0x%x r6=0x%x r7=0x%x r8=0x%x r9=0x%x "
            "r10=0x%x r11=0x%x lr=0x%x\n",
            cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.r[8], cpu.r[9], cpu.r[10],
            cpu.r[11], cpu.r[MANGO_REG_LR]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_pop_w): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 POP.W {r3-r11,lr} (Q-OTTD-0c)\n");
  return 0;
}

static int test_t32_pop_w_ldmia_sp_sib(void) {
  /* Q-OTTD-0c sibling: pop.w {r4,lr} = e8bd 4010 */
  static const uint16_t kProg[] = {
      0xE8BDu, 0x4010u,
      0x4770u,
  };
  uint8_t mem_buf[0x100];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
  memset(mem_buf + 16, 0xA5, sizeof(mem_buf) - 16);

  MangoInsn di;
  if (mango_decode_t32(0xE8BDu, 0x4010u, &di) != 0 || di.op != MANGO_OP_LDM ||
      di.rn != MANGO_REG_SP || di.p != 0 || di.u != 1 || di.w != 1 || di.reglist != 0x4010u) {
    fprintf(stderr, "FAIL(t32_pop_w_sib): decode op=%d rn=%u p=%d u=%d w=%d list=0x%x\n", di.op,
            di.rn, di.p, di.u, di.w, di.reglist);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t pre_sp = 0x40u;
  cpu.r[MANGO_REG_SP] = pre_sp;
  u32_to_bytes_le(mem_buf + pre_sp, 0x40044444u);
  u32_to_bytes_le(mem_buf + pre_sp + 4u, 0xE00EEEEEu);

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 4u, 10);
  if (rc != 0 || cpu.r[MANGO_REG_SP] != pre_sp + 8u || cpu.r[4] != 0x40044444u ||
      cpu.r[MANGO_REG_LR] != 0xE00EEEEEu || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_pop_w_sib): rc=%d sp=0x%x r4=0x%x lr=0x%x cpsr 0x%x->0x%x\n", rc,
            cpu.r[MANGO_REG_SP], cpu.r[4], cpu.r[MANGO_REG_LR], cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 POP.W {r4,lr} sibling (Q-OTTD-0c)\n");
  return 0;
}

static int test_t32_pop_w_ldmia_sp_reject(void) {
  /* Empty reglist must fail. P=1 PC-in-list is Q-OTTD-0i-pop (allowed). */
  MangoInsn di;
  if (mango_decode_t32(0xE8BDu, 0x0000u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_pop_w_reject): empty list decoded as op=%d\n", di.op);
    return 1;
  }
  printf("ok: T32 POP.W reject empty list (Q-OTTD-0c)\n");
  return 0;
}


static int test_t32_mov_w_modimm_0(void) {
  /* Q-OTTD-0d: mov.w r8,#0 = f04f 0800. S=0 leaves NZCV. */
  static const uint16_t kProg[] = {0xF04Fu, 0x0800u, 0x4770u};
  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF04Fu, 0x0800u, &di) != 0 || di.op != MANGO_OP_MOV || di.rd != 8 ||
      di.is_imm != 1 || di.sets_flags != 0 || di.imm != 0u) {
    fprintf(stderr, "FAIL(t32_mov_w_0): decode op=%d rd=%u imm=0x%x s=%d is_imm=%d\n", di.op,
            di.rd, di.imm, di.sets_flags, di.is_imm);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[8] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[8] != 0u || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_mov_w_0): rc=%d r8=0x%x cpsr 0x%x->0x%x\n", rc, cpu.r[8],
            cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 MOV.W r8,#0 modified-imm (Q-OTTD-0d)\n");
  return 0;
}

static int test_t32_mov_w_modimm_25(void) {
  /* Q-OTTD-0d sibling: mov.w r8,#0x25 = f04f 0825 (covers many f04f drive hits). */
  static const uint16_t kProg[] = {0xF04Fu, 0x0825u, 0x4770u};
  uint8_t mem_buf[32];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF04Fu, 0x0825u, &di) != 0 || di.op != MANGO_OP_MOV || di.rd != 8 ||
      di.is_imm != 1 || di.sets_flags != 0 || di.imm != 0x25u) {
    fprintf(stderr, "FAIL(t32_mov_w_25): decode op=%d rd=%u imm=0x%x s=%d\n", di.op, di.rd, di.imm,
            di.sets_flags);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[8] = 0u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[8] != 0x25u || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_mov_w_25): rc=%d r8=0x%x want 0x25 cpsr 0x%x->0x%x\n", rc, cpu.r[8],
            cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 MOV.W r8,#0x25 modified-imm (Q-OTTD-0d)\n");
  return 0;
}

static int test_t32_sub_w_modimm_sp(void) {
  /* Q-OTTD-0d: sub.w sp,sp,#0x2200 = f5ad 5d08.
   * ThumbExpandImm(0xD08)=0x2200 — NOT plain SUBW #0xD08 (that is f6ad 5d08). */
  static const uint16_t kProg[] = {0xF5ADu, 0x5D08u, 0x4770u};
  uint8_t mem_buf[0x4000];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF5ADu, 0x5D08u, &di) != 0 || di.op != MANGO_OP_SUB ||
      di.rd != MANGO_REG_SP || di.rn != MANGO_REG_SP || di.is_imm != 1 || di.sets_flags != 0 ||
      di.imm != 0x2200u) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): decode op=%d rd=%u rn=%u imm=0x%x (want 0x2200) s=%d\n",
            di.op, di.rd, di.rn, di.imm, di.sets_flags);
    return 1;
  }
  /* Guard against the #0xD08 conflation trap. */
  if (di.imm == 0xD08u) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): treated as plain #0xD08 — must be ThumbExpandImm→0x2200\n");
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t pre_sp = 0x3000u;
  cpu.r[MANGO_REG_SP] = pre_sp;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp - 0x2200u) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): sp=0x%x want 0x%x (delta -0x2200, not -0xD08)\n",
            cpu.r[MANGO_REG_SP], pre_sp - 0x2200u);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] == pre_sp - 0xD08u) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): SP delta was -0xD08 — wrong encoding path\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_sub_w_sp): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 SUB.W sp,sp,#0x2200 modified-imm (Q-OTTD-0d)\n");
  return 0;
}

static int test_t32_mov_w_modimm_reject_s1(void) {
  /* S=1 MOVS.W form must stay uncover this bite (e.g. f05f …). */
  MangoInsn di;
  if (mango_decode_t32(0xF05Fu, 0x0800u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_mov_w_reject_s1): S=1 decoded as op=%d\n", di.op);
    return 1;
  }
  /* Existing plain SUBW f6ad 5d08 must remain SUB with imm=0xD08, not 0x2200. */
  if (mango_decode_t32(0xF6ADu, 0x5D08u, &di) != 0 || di.op != MANGO_OP_SUB ||
      di.imm != 0xD08u) {
    fprintf(stderr, "FAIL(t32_mov_w_reject_s1): SUBW f6ad5d08 op=%d imm=0x%x want 0xD08\n", di.op,
            di.imm);
    return 1;
  }
  printf("ok: T32 MOV.W reject S=1; SUBW #0xD08 untouched (Q-OTTD-0d)\n");
  return 0;
}


static int test_t32_add_w_modimm_sp(void) {
  /* Q-OTTD-0e: add.w r1,sp,#0x2200 = f50d 5108.
   * ThumbExpandImm(0xD08)=0x2200 — NOT plain ADDW #0xD08 (that is f60d 5108). */
  static const uint16_t kProg[] = {0xF50Du, 0x5108u, 0x4770u};
  uint8_t mem_buf[0x4000];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF50Du, 0x5108u, &di) != 0 || di.op != MANGO_OP_ADD || di.rd != 1 ||
      di.rn != MANGO_REG_SP || di.is_imm != 1 || di.sets_flags != 0 || di.imm != 0x2200u) {
    fprintf(stderr, "FAIL(t32_add_w_sp): decode op=%d rd=%u rn=%u imm=0x%x (want 0x2200) s=%d\n",
            di.op, di.rd, di.rn, di.imm, di.sets_flags);
    return 1;
  }
  if (di.imm == 0xD08u) {
    fprintf(stderr, "FAIL(t32_add_w_sp): treated as plain #0xD08 — must be ThumbExpandImm→0x2200\n");
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t pre_sp = 0x3000u;
  cpu.r[MANGO_REG_SP] = pre_sp;
  cpu.r[1] = 0u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_add_w_sp): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[1] != pre_sp + 0x2200u) {
    fprintf(stderr, "FAIL(t32_add_w_sp): r1=0x%x want 0x%x (sp+0x2200, not +0xD08)\n", cpu.r[1],
            pre_sp + 0x2200u);
    return 1;
  }
  if (cpu.r[1] == pre_sp + 0xD08u) {
    fprintf(stderr, "FAIL(t32_add_w_sp): R1 delta was +0xD08 — wrong encoding path\n");
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp) {
    fprintf(stderr, "FAIL(t32_add_w_sp): sp changed 0x%x -> 0x%x\n", pre_sp, cpu.r[MANGO_REG_SP]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_add_w_sp): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 ADD.W r1,sp,#0x2200 modified-imm (Q-OTTD-0e)\n");
  return 0;
}

static int test_t32_strd_imm_offset(void) {
  /* Q-OTTD-0e-strd: strd r8,r9,[r1,#8] = e9c1 8902. W=0 → Rn unchanged. */
  static const uint16_t kProg[] = {0xE9C1u, 0x8902u, 0x4770u};
  uint8_t mem_buf[128];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xE9C1u, 0x8902u, &di) != 0 || di.op != MANGO_OP_STRD || di.rd != 8 ||
      di.rn != 1 || di.is_imm != 1 || di.imm != 8u || di.p != 1 || di.u != 1 || di.w != 0) {
    fprintf(stderr,
            "FAIL(t32_strd): decode op=%d rd=%u rn=%u imm=%u p=%d u=%d w=%d (want STRD r8,[r1,#8] "
            "W=0)\n",
            di.op, di.rd, di.rn, di.imm, di.p, di.u, di.w);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  uint32_t base = 64u;
  cpu.r[1] = base;
  cpu.r[8] = 0x80088888u;
  cpu.r[9] = 0x90099999u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_strd): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[1] != base) {
    fprintf(stderr, "FAIL(t32_strd): r1 writeback 0x%x -> 0x%x (W=0 must leave Rn)\n", base,
            cpu.r[1]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 0) != 0u || bytes_to_u32_le(mem_buf + base + 4) != 0u) {
    fprintf(stderr, "FAIL(t32_strd): words at [r1+0]/[r1+4] should stay 0\n");
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 8) != 0x80088888u ||
      bytes_to_u32_le(mem_buf + base + 12) != 0x90099999u) {
    fprintf(stderr, "FAIL(t32_strd): mem[r1+8]=0x%x mem[r1+12]=0x%x want r8/r9\n",
            bytes_to_u32_le(mem_buf + base + 8), bytes_to_u32_le(mem_buf + base + 12));
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_strd): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 STRD r8,r9,[r1,#8] W=0 (Q-OTTD-0e-strd)\n");
  return 0;
}

static int test_t32_add_strd_reject(void) {
  /* Optional negatives: S=1 ADD uncover; W=1 STRD e9e1 uncover; ADDW untouched. */
  MangoInsn di;
  if (mango_decode_t32(0xF11Du, 0x5108u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_strd_reject): S=1 ADD decoded as op=%d\n", di.op);
    return 1;
  }
  if (mango_decode_t32(0xE9E1u, 0x8902u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_strd_reject): W=1 STRD e9e1 decoded as op=%d w=%d\n", di.op,
            di.w);
    return 1;
  }
  /* Existing plain ADDW f60d 5108 must remain ADD with imm=0xD08, not 0x2200. */
  if (mango_decode_t32(0xF60Du, 0x5108u, &di) != 0 || di.op != MANGO_OP_ADD ||
      di.imm != 0xD08u) {
    fprintf(stderr, "FAIL(t32_add_strd_reject): ADDW f60d5108 op=%d imm=0x%x want 0xD08\n", di.op,
            di.imm);
    return 1;
  }
  printf("ok: T32 ADD reject S=1; STRD reject W=1; ADDW #0xD08 untouched (Q-OTTD-0e)\n");
  return 0;
}


static int test_t32_mvn_w_modimm(void) {
  /* Q-OTTD-0f: mvn.w r2,#0x21c0 = f46f 5207.
   * ThumbExpandImm(0xD07)=0x21C0 — NOT #0x87000 / imm12 0xA07 (that is f46f 2207).
   * R2 = ~0x21C0 = 0xFFFFDE3F; S=0 leaves NZCV. */
  static const uint16_t kProg[] = {0xF46Fu, 0x5207u, 0x4770u};
  uint8_t mem_buf[0x1000];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF46Fu, 0x5207u, &di) != 0 || di.op != MANGO_OP_MVN || di.rd != 2 ||
      di.is_imm != 1 || di.sets_flags != 0 || di.imm != 0x21C0u) {
    fprintf(stderr,
            "FAIL(t32_mvn_w): decode op=%d rd=%u imm=0x%x (want MVN r2,#0x21c0) s=%d is_imm=%d\n",
            di.op, di.rd, di.imm, di.sets_flags, di.is_imm);
    return 1;
  }
  if (di.imm == 0x87000u || di.imm == 0xA07u || di.imm == 0xD07u) {
    fprintf(stderr, "FAIL(t32_mvn_w): imm=0x%x — must be ThumbExpandImm→0x21C0 (not 0x87000/0xA07/0xD07)\n",
            di.imm);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[2] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_mvn_w): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[2] != 0xffffde3fu) {
    fprintf(stderr, "FAIL(t32_mvn_w): r2=0x%x want 0xffffde3f (~0x21c0)\n", cpu.r[2]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_mvn_w): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 MVN.W r2,#0x21c0 modified-imm (Q-OTTD-0f)\n");
  return 0;
}

static int test_t32_ldr_w_reg(void) {
  /* Q-OTTD-0f-ldr: ldr.w r10,[r4,r3] = f854 a003. imm2=0 / LSL#0, no writeback. */
  static const uint16_t kProg[] = {0xF854u, 0xA003u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF854u, 0xA003u, &di) != 0 || di.op != MANGO_OP_LDR || di.rd != 10 ||
      di.rn != 4 || di.rm != 3 || di.is_imm != 0 || di.shift_amount != 0 || di.p != 1 ||
      di.u != 1 || di.w != 0 || di.b != 0) {
    fprintf(stderr,
            "FAIL(t32_ldr_w_reg): decode op=%d rd=%u rn=%u rm=%u imm=%d sh=%u p=%d u=%d w=%d b=%d "
            "(want LDR r10,[r4,r3] W=0)\n",
            di.op, di.rd, di.rn, di.rm, di.is_imm, di.shift_amount, di.p, di.u, di.w, di.b);
    return 1;
  }

  uint32_t base = 64u;
  /* mem window like QEMU repro: +0=0x10011111, +8=0xa00aaaaa */
  mem_buf[base + 0] = 0x11;
  mem_buf[base + 1] = 0x11;
  mem_buf[base + 2] = 0x01;
  mem_buf[base + 3] = 0x10;
  mem_buf[base + 8] = 0xaa;
  mem_buf[base + 9] = 0xaa;
  mem_buf[base + 10] = 0x0a;
  mem_buf[base + 11] = 0xa0;

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[4] = base;
  cpu.r[3] = 8u;
  cpu.r[10] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[10] != 0xa00aaaaau) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): r10=0x%x want 0xa00aaaaa (mem[r4+r3])\n", cpu.r[10]);
    return 1;
  }
  if (cpu.r[4] != base) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): r4 writeback 0x%x -> 0x%x (must leave Rn)\n", base,
            cpu.r[4]);
    return 1;
  }
  if (cpu.r[3] != 8u) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): r3 changed to 0x%x\n", cpu.r[3]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 0) != 0x10011111u ||
      bytes_to_u32_le(mem_buf + base + 8) != 0xa00aaaaau) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): memory mutated\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ldr_w_reg): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 LDR.W r10,[r4,r3] imm2=0 no WB (Q-OTTD-0f-ldr)\n");
  return 0;
}

static int test_t32_mvn_ldr_reject(void) {
  /* Optional negatives: MVNS S=1; ORN Rn!=15; LDR imm2!=0; LDR hw2 bit11 imm8 form. */
  MangoInsn di;
  if (mango_decode_t32(0xF47Fu, 0x5207u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_mvn_ldr_reject): S=1 MVNS decoded as op=%d\n", di.op);
    return 1;
  }
  /* ORN: same op=0011 but Rn=r14 (f46e …) — must uncover this bite */
  if (mango_decode_t32(0xF46Eu, 0x5207u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_mvn_ldr_reject): ORN Rn!=15 decoded as op=%d\n", di.op);
    return 1;
  }
  /* LDR imm2=1 (LSL#1): hw2 bits[5:4]=01 → (hw2 & 0x0FF0) != 0 — still 0f-only */
  if (mango_decode_t32(0xF854u, 0xA013u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_mvn_ldr_reject): LDR imm2!=0 decoded as op=%d sh=%u\n", di.op,
            di.shift_amount);
    return 1;
  }
  /* f85d 4b04 post-index cleared by Q-OTTD-0m — no longer reject here */
  printf("ok: T32 MVN reject S=1/ORN; LDR reject imm2!=0 (Q-OTTD-0f)\n");
  return 0;
}


static int test_t32_ldrsh_w_imm12(void) {
  /* Q-OTTD-0g: ldrsh.w r11,[sp,#0x34] = f9bd b034.
   * Signed halfword: mem16=0xc0de → r11=0xffffc0de; SP unchanged. */
  static const uint16_t kProg[] = {0xF9BDu, 0xB034u, 0x4770u};
  uint8_t mem_buf[256];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF9BDu, 0xB034u, &di) != 0 || di.op != MANGO_OP_LDRSH ||
      di.rd != 11 || di.rn != MANGO_REG_SP || di.is_imm != 1 || di.imm != 0x34u ||
      di.p != 1 || di.u != 1 || di.w != 0) {
    fprintf(stderr,
            "FAIL(t32_ldrsh_w): decode op=%d rd=%u rn=%u imm=0x%x p=%d u=%d w=%d "
            "(want LDRSH r11,[sp,#0x34])\n",
            di.op, di.rd, di.rn, di.imm, di.p, di.u, di.w);
    return 1;
  }

  uint32_t sp = 64u;
  /* mem16[sp+0x34]=0xc0de; wrong-slot mem16[sp+0]=0x1234 */
  mem_buf[sp + 0x34] = 0xde;
  mem_buf[sp + 0x35] = 0xc0;
  mem_buf[sp + 0] = 0x34;
  mem_buf[sp + 1] = 0x12;

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[MANGO_REG_SP] = sp;
  cpu.r[11] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ldrsh_w): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[11] != 0xffffc0deu) {
    fprintf(stderr, "FAIL(t32_ldrsh_w): r11=0x%x want 0xffffc0de (sign-extend 0xc0de)\n",
            cpu.r[11]);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != sp) {
    fprintf(stderr, "FAIL(t32_ldrsh_w): sp writeback 0x%x -> 0x%x\n", sp, cpu.r[MANGO_REG_SP]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ldrsh_w): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  /* sibling decode: f9bd a020 = ldrsh.w r10,[sp,#0x20] */
  if (mango_decode_t32(0xF9BDu, 0xA020u, &di) != 0 || di.op != MANGO_OP_LDRSH || di.rd != 10 ||
      di.imm != 0x20u) {
    fprintf(stderr, "FAIL(t32_ldrsh_w): sibling f9bda020 decode fail op=%d rd=%u imm=0x%x\n",
            di.op, di.rd, di.imm);
    return 1;
  }
  printf("ok: T32 LDRSH.W r11,[sp,#0x34] sign-extend (Q-OTTD-0g)\n");
  return 0;
}

static int test_t32_str_w_imm8_neg(void) {
  /* Q-OTTD-0g-str: str.w r6,[r9,#-0x3c] = f849 6c3c.
   * U=0 → store at r9-0x3c; W=0 → r9 unchanged; mem[r9] untouched. */
  static const uint16_t kProg[] = {0xF849u, 0x6C3Cu, 0x4770u};
  uint8_t mem_buf[256];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF849u, 0x6C3Cu, &di) != 0 || di.op != MANGO_OP_STR || di.rd != 6 ||
      di.rn != 9 || di.is_imm != 1 || di.imm != 0x3Cu || di.p != 1 || di.u != 0 || di.w != 0 ||
      di.b != 0) {
    fprintf(stderr,
            "FAIL(t32_str_w_neg): decode op=%d rd=%u rn=%u imm=0x%x p=%d u=%d w=%d b=%d "
            "(want STR r6,[r9,#-0x3c] U=0)\n",
            di.op, di.rd, di.rn, di.imm, di.p, di.u, di.w, di.b);
    return 1;
  }
  if (di.u != 0) {
    fprintf(stderr, "FAIL(t32_str_w_neg): u=%d — guest is U=0 (negative offset), not U=1\n", di.u);
    return 1;
  }

  uint32_t r9 = 0x80u; /* interior so r9-0x3c is in-bounds */
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[9] = r9;
  cpu.r[6] = 0x60066666u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_str_w_neg): run rc=%d\n", rc);
    return 1;
  }
  uint32_t got_neg = bytes_to_u32_le(mem_buf + (r9 - 0x3Cu));
  if (got_neg != 0x60066666u) {
    fprintf(stderr, "FAIL(t32_str_w_neg): mem[r9-0x3c]=0x%x want 0x60066666\n", got_neg);
    return 1;
  }
  if (cpu.r[9] != r9) {
    fprintf(stderr, "FAIL(t32_str_w_neg): r9 writeback 0x%x -> 0x%x (W=0)\n", r9, cpu.r[9]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + r9) != 0) {
    fprintf(stderr, "FAIL(t32_str_w_neg): mem[r9]=0x%x want 0 (positive slot untouched)\n",
            bytes_to_u32_le(mem_buf + r9));
    return 1;
  }
  /* positive imm12 form must NOT be this arm — still F880 */
  if (mango_decode_t32(0xF8C9u, 0x603Cu, &di) != 0 || di.op != MANGO_OP_STR || di.u != 1 ||
      di.imm != 0x3Cu) {
    fprintf(stderr, "FAIL(t32_str_w_neg): imm12 f8c9603c should decode as STR U=1 imm=0x3c\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_str_w_neg): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 STR.W r6,[r9,#-0x3c] U=0 no WB (Q-OTTD-0g-str)\n");
  return 0;
}

static int test_t32_ldrsh_str_reject(void) {
  /* Optional negatives: STR W=1 / U=1 forms; footnote LDR imm8; LDRSH PC. */
  MangoInsn di;
  /* STR W=1 pre-index: f849 6f3c = str.w r6,[r9,#60]! — P=1 U=1 W=1 */
  if (mango_decode_t32(0xF849u, 0x6F3Cu, &di) == 0) {
    fprintf(stderr, "FAIL(t32_ldrsh_str_reject): STR W=1 f8496f3c decoded as op=%d u=%d w=%d\n",
            di.op, di.u, di.w);
    return 1;
  }
  /* STR post-index: f849 6b3c = str.w r6,[r9],#60 — P=0 U=1 W=1 */
  if (mango_decode_t32(0xF849u, 0x6B3Cu, &di) == 0) {
    fprintf(stderr, "FAIL(t32_ldrsh_str_reject): STR post f8496b3c decoded as op=%d\n", di.op);
    return 1;
  }
  /* f85d 4b04 post-index cleared by Q-OTTD-0m — no longer reject here */
  /* LDRSH Rt=PC */
  if (mango_decode_t32(0xF9BDu, 0xF034u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_ldrsh_str_reject): LDRSH Rt=PC decoded as op=%d\n", di.op);
    return 1;
  }
  printf("ok: T32 LDRSH/STR reject W=1/post/PC (Q-OTTD-0g)\n");
  return 0;
}


static int test_t16_cbz_b1ff(void) {
  /* Q-OTTD-0h: b1ff = cbz r7, #+62. Taken → pc+66 (+0x42); fall → pc+2.
   * NZCV untouched. Must NOT read CPSR.Z (Z set + r7!=0 still falls). */
  MangoInsn di;
  if (mango_decode_t16(0xB1FFu, &di) != 0 || di.op != MANGO_OP_CBZ || di.rn != 7 ||
      di.is_imm != 1 || di.imm != 62u || di.cond != 0xEu) {
    fprintf(stderr,
            "FAIL(t16_cbz_b1ff): decode op=%d rn=%u imm=%u cond=0x%x "
            "(want CBZ r7,#62)\n",
            di.op, di.rn, di.imm, di.cond);
    return 1;
  }

  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  mem_buf[0] = 0xff;
  mem_buf[1] = 0xb1; /* b1ff at addr 0 */
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  /* taken: r7==0 → next = 0+4+62 = 66 */
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_N | MANGO_CPSR_C | MANGO_CPSR_V; /* Z clear */
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[7] = 0;
    cpu.r[MANGO_REG_PC] = 0;
    int rc = mango_interp_run(&cpu, &mem, 66u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 66u) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff taken): rc=%d pc=0x%x want 66\n", rc,
              cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff taken): cpsr 0x%x -> 0x%x (NZCV must hold)\n",
              cpsr_before, cpu.cpsr);
      return 1;
    }
    if (cpu.r[7] != 0) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff taken): r7 mutated\n");
      return 1;
    }
  }

  /* fall: r7==1 → next = 0+2 = 2 */
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C; /* Z set intentionally */
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[7] = 1;
    cpu.r[MANGO_REG_PC] = 0;
    int rc = mango_interp_run(&cpu, &mem, 2u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 2u) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff fall): rc=%d pc=0x%x want 2\n", rc,
              cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff fall): cpsr 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
      return 1;
    }
  }

  /* Z≠Rn proof: CPSR.Z set but r7!=0 → CBZ must FALL (would TAKE if B+EQ). */
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_N | MANGO_CPSR_C | MANGO_CPSR_V;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[7] = 0x42u; /* nonzero */
    cpu.r[MANGO_REG_PC] = 0;
    int rc = mango_interp_run(&cpu, &mem, 2u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 2u) {
      fprintf(stderr,
              "FAIL(t16_cbz_b1ff Z!=Rn): rc=%d pc=0x%x — CBZ must test Rn not CPSR.Z "
              "(Z set + r7!=0 must fall)\n",
              rc, cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbz_b1ff Z!=Rn): cpsr changed\n");
      return 1;
    }
  }

  printf("ok: T16 CBZ b1ff r7 #+62 taken/fall + NZCV + Z!=Rn (Q-OTTD-0h)\n");
  return 0;
}

static int test_t16_cbz_b369(void) {
  /* Q-OTTD-0h: b369 = cbz r1, #+90 (op bit11=0 → CBZ, not CBNZ).
   * Taken → pc+94 (+0x5e); fall → pc+2. */
  MangoInsn di;
  if (mango_decode_t16(0xB369u, &di) != 0 || di.op != MANGO_OP_CBZ || di.rn != 1 ||
      di.is_imm != 1 || di.imm != 90u) {
    fprintf(stderr,
            "FAIL(t16_cbz_b369): decode op=%d rn=%u imm=%u "
            "(want CBZ r1,#90 — bit11=0 is CBZ not CBNZ)\n",
            di.op, di.rn, di.imm);
    return 1;
  }
  if (di.op == MANGO_OP_CBNZ) {
    fprintf(stderr, "FAIL(t16_cbz_b369): decoded as CBNZ — guest b369 is CBZ\n");
    return 1;
  }

  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  mem_buf[0] = 0x69;
  mem_buf[1] = 0xb3;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[1] = 0;
    int rc = mango_interp_run(&cpu, &mem, 94u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 94u) {
      fprintf(stderr, "FAIL(t16_cbz_b369 taken): rc=%d pc=0x%x want 94\n", rc,
              cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbz_b369 taken): cpsr changed\n");
      return 1;
    }
  }
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[1] = 5;
    int rc = mango_interp_run(&cpu, &mem, 2u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 2u) {
      fprintf(stderr, "FAIL(t16_cbz_b369 fall): rc=%d pc=0x%x want 2\n", rc,
              cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbz_b369 fall): cpsr changed\n");
      return 1;
    }
  }

  printf("ok: T16 CBZ b369 r1 #+90 taken/fall (Q-OTTD-0h)\n");
  return 0;
}

static int test_t16_cbnz_b978(void) {
  /* Q-OTTD-0h sibling: b978 = cbnz r0, #+30. Taken → pc+34 (+0x22); fall → pc+2. */
  MangoInsn di;
  if (mango_decode_t16(0xB978u, &di) != 0 || di.op != MANGO_OP_CBNZ || di.rn != 0 ||
      di.is_imm != 1 || di.imm != 30u || di.cond != 0xEu) {
    fprintf(stderr,
            "FAIL(t16_cbnz_b978): decode op=%d rn=%u imm=%u "
            "(want CBNZ r0,#30)\n",
            di.op, di.rn, di.imm);
    return 1;
  }

  uint8_t mem_buf[64];
  memset(mem_buf, 0, sizeof(mem_buf));
  mem_buf[0] = 0x78;
  mem_buf[1] = 0xb9;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_N | MANGO_CPSR_V; /* Z clear */
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[0] = 7;
    int rc = mango_interp_run(&cpu, &mem, 34u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 34u) {
      fprintf(stderr, "FAIL(t16_cbnz_b978 taken): rc=%d pc=0x%x want 34\n", rc,
              cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbnz_b978 taken): cpsr changed\n");
      return 1;
    }
  }
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    /* Z clear + r0==0: CBNZ must FALL (would TAKE if wrongly B+NE on Z). */
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_N | MANGO_CPSR_C | MANGO_CPSR_V;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[0] = 0;
    int rc = mango_interp_run(&cpu, &mem, 2u, 10);
    if (rc != 0 || cpu.r[MANGO_REG_PC] != 2u) {
      fprintf(stderr,
              "FAIL(t16_cbnz_b978 fall/Z!=Rn): rc=%d pc=0x%x — CBNZ must test Rn "
              "not CPSR.Z\n",
              rc, cpu.r[MANGO_REG_PC]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t16_cbnz_b978 fall): cpsr changed\n");
      return 1;
    }
  }

  printf("ok: T16 CBNZ b978 r0 #+30 taken/fall + Z!=Rn (Q-OTTD-0h)\n");
  return 0;
}


static int test_t32_str_w_reg_lsl2(void) {
  /* Q-OTTD-0i: str.w r4,[r0,r5,lsl#2] = f840 4025.
   * Store at r0+(r5<<2); Rn/Rm unchanged; NZCV hold; pc+=4 via bx lr. */
  static const uint16_t kProg[] = {0xF840u, 0x4025u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF840u, 0x4025u, &di) != 0 || di.op != MANGO_OP_STR || di.rd != 4 ||
      di.rn != 0 || di.rm != 5 || di.is_imm != 0 || di.shift_type != 0 || di.shift_amount != 2 ||
      di.p != 1 || di.u != 1 || di.w != 0 || di.b != 0) {
    fprintf(stderr,
            "FAIL(t32_str_w_reg): decode op=%d rd=%u rn=%u rm=%u imm=%d st=%u sh=%u p=%d u=%d "
            "w=%d b=%d (want STR r4,[r0,r5,lsl#2])\n",
            di.op, di.rd, di.rn, di.rm, di.is_imm, di.shift_type, di.shift_amount, di.p, di.u,
            di.w, di.b);
    return 1;
  }

  uint32_t base = 64u;
  u32_to_bytes_le(mem_buf + base + 0, 0x10011111u);
  u32_to_bytes_le(mem_buf + base + 8, 0x30033333u);
  u32_to_bytes_le(mem_buf + base + 12, 0xDEADBEEFu); /* junk at [r0+(3<<2)] */

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[0] = base;
  cpu.r[5] = 3u;
  cpu.r[4] = 0x40044444u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_str_w_reg): run rc=%d\n", rc);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 12) != 0x40044444u) {
    fprintf(stderr, "FAIL(t32_str_w_reg): mem[r0+(r5<<2)]=0x%x want 0x40044444\n",
            bytes_to_u32_le(mem_buf + base + 12));
    return 1;
  }
  if (cpu.r[0] != base) {
    fprintf(stderr, "FAIL(t32_str_w_reg): r0 writeback 0x%x -> 0x%x\n", base, cpu.r[0]);
    return 1;
  }
  if (cpu.r[5] != 3u) {
    fprintf(stderr, "FAIL(t32_str_w_reg): r5 changed to 0x%x\n", cpu.r[5]);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 0) != 0x10011111u ||
      bytes_to_u32_le(mem_buf + base + 8) != 0x30033333u) {
    fprintf(stderr, "FAIL(t32_str_w_reg): adjacent slots mutated\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_str_w_reg): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 STR.W r4,[r0,r5,lsl#2] no WB (Q-OTTD-0i)\n");
  return 0;
}

static int test_t32_pop_w_pc_thumb(void) {
  /* Q-OTTD-0i-pop: pop.w {r4-r8,pc} = e8bd 81f0. Thumb return (PC odd → T=1). */
  static const uint16_t kProg[] = {0xE8BDu, 0x81F0u};
  uint8_t mem_buf[0x200];
  memset(mem_buf, 0xA5, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);

  MangoInsn di;
  if (mango_decode_t32(0xE8BDu, 0x81F0u, &di) != 0 || di.op != MANGO_OP_LDM ||
      di.rn != MANGO_REG_SP || di.p != 0 || di.u != 1 || di.w != 1 || di.reglist != 0x81F0u) {
    fprintf(stderr,
            "FAIL(t32_pop_w_pc_t): decode op=%d rn=%u p=%d u=%d w=%d list=0x%x "
            "(want LDM list=0x81F0 with bit15)\n",
            di.op, di.rn, di.p, di.u, di.w, di.reglist);
    return 1;
  }
  if ((di.reglist & 0x8000u) == 0) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): reglist stripped bit15 (0x%x)\n", di.reglist);
    return 1;
  }

  uint32_t pre_sp = 0x40u;
  uint32_t target = 0x100u; /* aligned Thumb continue */
  static const uint32_t kSeed[] = {0x40044444u, 0x50055555u, 0x60066666u, 0x70077777u,
                                   0x80088888u};
  for (int i = 0; i < 5; i++) {
    u32_to_bytes_le(mem_buf + pre_sp + (uint32_t)i * 4u, kSeed[i]);
  }
  u32_to_bytes_le(mem_buf + pre_sp + 20u, target | 1u); /* LoadWritePC odd → T */

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[MANGO_REG_SP] = pre_sp;

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, target, 10);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): run rc=%d pc=0x%x\n", rc, cpu.r[MANGO_REG_PC]);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp + 24u) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): sp=0x%x want 0x%x\n", cpu.r[MANGO_REG_SP],
            pre_sp + 24u);
    return 1;
  }
  if (cpu.r[4] != kSeed[0] || cpu.r[5] != kSeed[1] || cpu.r[6] != kSeed[2] ||
      cpu.r[7] != kSeed[3] || cpu.r[8] != kSeed[4]) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): regs r4=0x%x r5=0x%x r6=0x%x r7=0x%x r8=0x%x\n",
            cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.r[8]);
    return 1;
  }
  if (cpu.r[MANGO_REG_PC] != target) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): pc=0x%x want 0x%x\n", cpu.r[MANGO_REG_PC], target);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_T) == 0) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): T cleared (want Thumb)\n");
    return 1;
  }
  uint32_t nzcv = MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V;
  if ((cpu.cpsr & nzcv) != (cpsr_before & nzcv)) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_t): NZCV changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 POP.W {r4-r8,pc} Thumb interwork (Q-OTTD-0i-pop)\n");
  return 0;
}

static int test_t32_pop_w_pc_arm(void) {
  /* Q-OTTD-0i-pop ARM interworking: even PC → clear CPSR.T. */
  static const uint16_t kProg[] = {0xE8BDu, 0x81F0u};
  uint8_t mem_buf[0x200];
  memset(mem_buf, 0xA5, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);

  uint32_t pre_sp = 0x40u;
  uint32_t arm_target = 0x100u; /* even → ARM */
  static const uint32_t kSeed[] = {0xA00AAAAAu, 0xB00BBBBBu, 0xC00CCCCCu, 0xD00DDDDDu,
                                   0xE00EEEEEu};
  for (int i = 0; i < 5; i++) {
    u32_to_bytes_le(mem_buf + pre_sp + (uint32_t)i * 4u, kSeed[i]);
  }
  u32_to_bytes_le(mem_buf + pre_sp + 20u, arm_target);

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[MANGO_REG_SP] = pre_sp;

  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, arm_target, 10);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): run rc=%d pc=0x%x\n", rc, cpu.r[MANGO_REG_PC]);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != pre_sp + 24u) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): sp=0x%x want 0x%x\n", cpu.r[MANGO_REG_SP],
            pre_sp + 24u);
    return 1;
  }
  if (cpu.r[4] != kSeed[0] || cpu.r[5] != kSeed[1] || cpu.r[6] != kSeed[2] ||
      cpu.r[7] != kSeed[3] || cpu.r[8] != kSeed[4]) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): regs r4=0x%x r5=0x%x r6=0x%x r7=0x%x r8=0x%x\n",
            cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.r[8]);
    return 1;
  }
  if (cpu.r[MANGO_REG_PC] != arm_target) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): pc=0x%x want 0x%x\n", cpu.r[MANGO_REG_PC],
            arm_target);
    return 1;
  }
  if ((cpu.cpsr & MANGO_CPSR_T) != 0) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): T still set (want ARM)\n");
    return 1;
  }
  uint32_t nzcv = MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V;
  if ((cpu.cpsr & nzcv) != (cpsr_before & nzcv)) {
    fprintf(stderr, "FAIL(t32_pop_w_pc_a): NZCV changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 POP.W {r4-r8,pc} ARM interwork T=0 (Q-OTTD-0i-pop)\n");
  return 0;
}

static int test_t32_str_pop_pc_reject(void) {
  /* Optional negatives: STR imm2=0; LR+PC UNPRED; footnote LDR post-index. */
  MangoInsn di;
  /* f840 4005 = str.w r4,[r0,r5] LSL#0 — exact-imm2=2 bite must uncover */
  if (mango_decode_t32(0xF840u, 0x4005u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_str_pop_pc_reject): STR imm2=0 f8404005 decoded as op=%d sh=%u\n",
            di.op, di.shift_amount);
    return 1;
  }
  /* e8bd c010 = ldmia sp!,{r4,lr,pc} — P=1 M=1 UNPRED */
  if (mango_decode_t32(0xE8BDu, 0xC010u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_str_pop_pc_reject): LR+PC e8bdc010 decoded as op=%d list=0x%x\n",
            di.op, di.reglist);
    return 1;
  }
  /* f85d 4b04 post-index cleared by Q-OTTD-0m — no longer reject here */
  printf("ok: T32 STR imm2=0 / POP LR+PC reject (Q-OTTD-0i)\n");
  return 0;
}



static int test_t32_stmia_w0(void) {
  /* Q-OTTD-0j: stmia.w r0,{r2,r3,r5} = e880 002c (W=0).
   * Stores ascending; R0 unchanged; NZCV hold; pc+=4 via bx lr. */
  static const uint16_t kProg[] = {0xE880u, 0x002Cu, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xE880u, 0x002Cu, &di) != 0 || di.op != MANGO_OP_STM || di.rn != 0 ||
      di.reglist != 0x002Cu || di.p != 0 || di.u != 1 || di.w != 0) {
    fprintf(stderr,
            "FAIL(t32_stmia_w0): decode op=%d rn=%u list=0x%x p=%d u=%d w=%d "
            "(want STM r0,{r2,r3,r5} p=0 u=1 w=0)\n",
            di.op, di.rn, di.reglist, di.p, di.u, di.w);
    return 1;
  }

  uint32_t base = 64u;
  u32_to_bytes_le(mem_buf + base + 0, 0x11111111u);
  u32_to_bytes_le(mem_buf + base + 4, 0x22222222u);
  u32_to_bytes_le(mem_buf + base + 8, 0x33333333u);
  u32_to_bytes_le(mem_buf + base + 12, 0x44444444u);

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[0] = base;
  cpu.r[2] = 0x20022222u;
  cpu.r[3] = 0x30033333u;
  cpu.r[5] = 0x50055555u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_stmia_w0): run rc=%d\n", rc);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 0) != 0x20022222u ||
      bytes_to_u32_le(mem_buf + base + 4) != 0x30033333u ||
      bytes_to_u32_le(mem_buf + base + 8) != 0x50055555u) {
    fprintf(stderr, "FAIL(t32_stmia_w0): mem+0=0x%x +4=0x%x +8=0x%x\n",
            bytes_to_u32_le(mem_buf + base + 0), bytes_to_u32_le(mem_buf + base + 4),
            bytes_to_u32_le(mem_buf + base + 8));
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 12) != 0x44444444u) {
    fprintf(stderr, "FAIL(t32_stmia_w0): mem+12 mutated 0x%x\n",
            bytes_to_u32_le(mem_buf + base + 12));
    return 1;
  }
  if (cpu.r[0] != base) {
    fprintf(stderr, "FAIL(t32_stmia_w0): r0 writeback 0x%x -> 0x%x\n", base, cpu.r[0]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_stmia_w0): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 STMIA r0,{r2,r3,r5} W=0 (Q-OTTD-0j)\n");
  return 0;
}

static int test_t32_ubfx(void) {
  /* Q-OTTD-0j-ubfx: ubfx r7,r1,#0,#11 = f3c1 070a.
   * R7 = R1 & 0x7FF; R1/NZCV unchanged; pc+=4 via bx lr. */
  static const uint16_t kProg[] = {0xF3C1u, 0x070Au, 0x4770u};
  uint8_t mem_buf[64];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF3C1u, 0x070Au, &di) != 0 || di.op != MANGO_OP_UBFX || di.rd != 7 ||
      di.rn != 1 || di.imm != 0 || di.rs != 10) {
    fprintf(stderr,
            "FAIL(t32_ubfx): decode op=%d rd=%u rn=%u imm=%u rs=%u "
            "(want UBFX r7,r1 lsb=0 widthm1=10)\n",
            di.op, di.rd, di.rn, di.imm, di.rs);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[1] = 0xDEADBEEFu;
  cpu.r[7] = 0x11111111u;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ubfx): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[7] != 0x6EFu) {
    fprintf(stderr, "FAIL(t32_ubfx): r7=0x%x want 0x6ef (deadbeef & 0x7ff)\n", cpu.r[7]);
    return 1;
  }
  if (cpu.r[1] != 0xDEADBEEFu) {
    fprintf(stderr, "FAIL(t32_ubfx): r1 changed to 0x%x\n", cpu.r[1]);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ubfx): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }

  /* Extra width checks via decode+execute for 0xffffffff / 0xfff → 0x7ff */
  static const uint16_t kProg2[] = {0xF3C1u, 0x070Au, 0x4770u};
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg2, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_N | MANGO_CPSR_V;
  cpsr_before = cpu.cpsr;
  cpu.r[1] = 0xFFFFFFFFu;
  cpu.r[7] = 0;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[7] != 0x7FFu || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ubfx): ffffffff case r7=0x%x cpsr=0x%x rc=%d\n", cpu.r[7],
            cpu.cpsr, rc);
    return 1;
  }
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg2, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T;
  cpsr_before = cpu.cpsr;
  cpu.r[1] = 0x00000FFFu;
  cpu.r[7] = 0;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[7] != 0x7FFu || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ubfx): 0xfff case r7=0x%x cpsr=0x%x rc=%d\n", cpu.r[7], cpu.cpsr,
            rc);
    return 1;
  }

  printf("ok: T32 UBFX r7,r1,#0,#11 (Q-OTTD-0j-ubfx)\n");
  return 0;
}

static int test_t32_stmia_ubfx_reject(void) {
  /* Optional negatives: W=1 STMIA; PC in STM list; UBFX PC Rd/Rn; SBFX uncover. */
  MangoInsn di;
  /* e8a0 002c = stmia.w r0!, {r2,r3,r5} — W=1 out of this bite */
  if (mango_decode_t32(0xE8A0u, 0x002Cu, &di) == 0) {
    fprintf(stderr, "FAIL(t32_stmia_ubfx_reject): W=1 e8a0002c decoded as op=%d w=%d\n", di.op,
            di.w);
    return 1;
  }
  /* e880 8000 = STMIA with PC in list */
  if (mango_decode_t32(0xE880u, 0x8000u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_stmia_ubfx_reject): PC-in-list e8808000 decoded as op=%d list=0x%x\n",
            di.op, di.reglist);
    return 1;
  }
  /* f3cf 070a = UBFX r7,pc,#0,#11 */
  if (mango_decode_t32(0xF3CFu, 0x070Au, &di) == 0) {
    fprintf(stderr, "FAIL(t32_stmia_ubfx_reject): UBFX Rn=PC decoded as op=%d\n", di.op);
    return 1;
  }
  /* f3c1 0f0a = UBFX pc,r1,#0,#11 */
  if (mango_decode_t32(0xF3C1u, 0x0F0Au, &di) == 0) {
    fprintf(stderr, "FAIL(t32_stmia_ubfx_reject): UBFX Rd=PC decoded as op=%d\n", di.op);
    return 1;
  }
  /* f341 070a = SBFX — not this bite */
  if (mango_decode_t32(0xF341u, 0x070Au, &di) == 0) {
    fprintf(stderr, "FAIL(t32_stmia_ubfx_reject): SBFX f341070a decoded as op=%d\n", di.op);
    return 1;
  }
  /* f85d 4b04 post-index cleared by Q-OTTD-0m — no longer reject here */
  printf("ok: T32 STMIA W=1 / UBFX PC / SBFX reject (Q-OTTD-0j)\n");
  return 0;
}


static int test_t32_cmp_w_modimm_80000000(void) {
  /* Q-OTTD-0k: cmp.w r5,#0x80000000 = f1b5 4f00.
   * imm12=0x400 → ThumbExpandImm = 0x80000000 (NOT #0x400 / f5b5 6f80).
   * NZCV from R5 - 0x80000000; R5 unchanged; pc+=4 via bx lr. */
  static const uint16_t kProg[] = {0xF1B5u, 0x4F00u, 0x4770u};
  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF1B5u, 0x4F00u, &di) != 0 || di.op != MANGO_OP_CMP || di.rn != 5 ||
      di.is_imm != 1 || di.sets_flags != 1 || di.imm != 0x80000000u) {
    fprintf(stderr,
            "FAIL(t32_cmp_w_80000000): decode op=%d rn=%u imm=0x%x (want 0x80000000) s=%d "
            "is_imm=%d\n",
            di.op, di.rn, di.imm, di.sets_flags, di.is_imm);
    return 1;
  }
  if (di.imm == 0x400u) {
    fprintf(stderr,
            "FAIL(t32_cmp_w_80000000): treated as plain #0x400 — must be ThumbExpandImm→0x80000000\n");
    return 1;
  }

  struct {
    uint32_t r5;
    uint32_t want_nzcv;
    const char* name;
  } cases[] = {
      {0x80000000u, MANGO_CPSR_Z | MANGO_CPSR_C, "EQ"},
      {0x7fffffffu, MANGO_CPSR_N | MANGO_CPSR_V, "LO"},
      {0x80000001u, MANGO_CPSR_C, "HI"},
      {1u, MANGO_CPSR_N | MANGO_CPSR_V, "drive"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(mem_buf, 0, sizeof(mem_buf));
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T; /* NZCV clear before CMP */
    cpu.r[5] = cases[i].r5;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
    uint32_t nzcv = cpu.cpsr & (MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V);
    if (rc != 0 || cpu.r[5] != cases[i].r5 || nzcv != cases[i].want_nzcv ||
        (cpu.cpsr & MANGO_CPSR_T) == 0) {
      fprintf(stderr,
              "FAIL(t32_cmp_w_80000000 %s): rc=%d r5=0x%x nzcv=0x%x want 0x%x cpsr=0x%x\n",
              cases[i].name, rc, cpu.r[5], nzcv, cases[i].want_nzcv, cpu.cpsr);
      return 1;
    }
  }
  printf("ok: T32 CMP.W r5,#0x80000000 modified-imm (Q-OTTD-0k)\n");
  return 0;
}

static int test_t32_cmp_w_modimm_1a(void) {
  /* Q-OTTD-0k-1a: cmp.w r9,#0x1a = f1b9 0f1a. ThumbExpandImm(0x01A)=0x1A.
   * NZCV from R9 - 0x1A; R9 unchanged; pc+=4 via bx lr. */
  static const uint16_t kProg[] = {0xF1B9u, 0x0F1Au, 0x4770u};
  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF1B9u, 0x0F1Au, &di) != 0 || di.op != MANGO_OP_CMP || di.rn != 9 ||
      di.is_imm != 1 || di.sets_flags != 1 || di.imm != 0x1Au) {
    fprintf(stderr,
            "FAIL(t32_cmp_w_1a): decode op=%d rn=%u imm=0x%x (want 0x1a) s=%d is_imm=%d\n",
            di.op, di.rn, di.imm, di.sets_flags, di.is_imm);
    return 1;
  }

  struct {
    uint32_t r9;
    uint32_t want_nzcv;
    const char* name;
  } cases[] = {
      {0x1Au, MANGO_CPSR_Z | MANGO_CPSR_C, "EQ"},
      {0x19u, MANGO_CPSR_N, "LO"},
      {0x1Bu, MANGO_CPSR_C, "HI"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(mem_buf, 0, sizeof(mem_buf));
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T;
    cpu.r[9] = cases[i].r9;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
    uint32_t nzcv = cpu.cpsr & (MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V);
    if (rc != 0 || cpu.r[9] != cases[i].r9 || nzcv != cases[i].want_nzcv ||
        (cpu.cpsr & MANGO_CPSR_T) == 0) {
      fprintf(stderr, "FAIL(t32_cmp_w_1a %s): rc=%d r9=0x%x nzcv=0x%x want 0x%x cpsr=0x%x\n",
              cases[i].name, rc, cpu.r[9], nzcv, cases[i].want_nzcv, cpu.cpsr);
      return 1;
    }
  }
  printf("ok: T32 CMP.W r9,#0x1a modified-imm (Q-OTTD-0k-1a)\n");
  return 0;
}

static int test_t32_cmp_w_modimm_reject(void) {
  /* S=1 Rd≠15 is SUBS — out of scope; Rn=PC UNPRED; CMN/BIC footnotes uncover. */
  MangoInsn di;
  /* f1b5 0f00 with Rd≠15 forced: use f1b5 0400 = SUBS-like Rd=4 S=1 */
  if (mango_decode_t32(0xF1B5u, 0x0400u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_cmp_w_reject): S=1 Rd≠15 f1b50400 decoded as op=%d\n", di.op);
    return 1;
  }
  /* f1bf 4f00 = CMP.W pc,#0x80000000 — Rn=PC */
  if (mango_decode_t32(0xF1BFu, 0x4F00u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_cmp_w_reject): Rn=PC f1bf4f00 decoded as op=%d\n", di.op);
    return 1;
  }
  /* f1a5 4400 = SUB.W r4,r5,#0x80000000 S=0 Rd=4 — already 0d, not CMP
   * (research typo f1a54f00 has Rd=15 which SUB rejects as PC). */
  if (mango_decode_t32(0xF1A5u, 0x4400u, &di) != 0 || di.op != MANGO_OP_SUB || di.rd != 4 ||
      di.rn != 5 || di.sets_flags != 0 || di.imm != 0x80000000u) {
    fprintf(stderr,
            "FAIL(t32_cmp_w_reject): S=0 SUB f1a54400 op=%d rd=%u rn=%u imm=0x%x s=%d "
            "want SUB r4,r5,#0x80000000 s=0\n",
            di.op, di.rd, di.rn, di.imm, di.sets_flags);
    return 1;
  }
  /* f110 0f00 = CMN.W — not this bite */
  if (mango_decode_t32(0xF110u, 0x0F00u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_cmp_w_reject): CMN f1100f00 decoded as op=%d\n", di.op);
    return 1;
  }
  /* footnote bic.w f026 060f */
  if (mango_decode_t32(0xF026u, 0x060Fu, &di) == 0) {
    fprintf(stderr, "FAIL(t32_cmp_w_reject): BIC f026060f decoded as op=%d\n", di.op);
    return 1;
  }
  printf("ok: T32 CMP.W reject SUBS/Rn=PC/CMN/BIC (Q-OTTD-0k)\n");
  return 0;
}


static int test_t16_uxth_b2b6(void) {
  /* Q-OTTD-0l: uxth r6,r6 = b2b6. R6 = R6 & 0xFFFF; NZCV hold; pc+=2. */
  static const uint16_t kProg[] = {0xB2B6u, 0x4770u};
  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);

  MangoInsn di;
  if (mango_decode_t16(0xB2B6u, &di) != 0 || di.op != MANGO_OP_XTEND || di.rd != 6 ||
      di.rm != 6 || di.rn != 15 || di.imm != 0 || di.u != 1 || di.b != 1 || di.sets_flags != 0) {
    fprintf(stderr,
            "FAIL(t16_uxth_b2b6): decode op=%d rd=%u rm=%u rn=%u imm=%u u=%d b=%d s=%d "
            "(want XTEND r6,r6 rn=15 u=1 b=1)\n",
            di.op, di.rd, di.rm, di.rn, di.imm, di.u, di.b, di.sets_flags);
    return 1;
  }

  struct {
    uint32_t pre;
    uint32_t want;
    const char* name;
  } cases[] = {
      {0x1234abcdu, 0x0000abcdu, "abcd"},
      {0xffffffffu, 0x0000ffffu, "ffff"},
      {1u, 1u, "drive"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(mem_buf, 0, sizeof(mem_buf));
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[6] = cases[i].pre;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
    if (rc != 0 || cpu.r[6] != cases[i].want || cpu.cpsr != cpsr_before) {
      fprintf(stderr,
              "FAIL(t16_uxth_b2b6 %s): rc=%d r6=0x%x want 0x%x cpsr=0x%x want 0x%x\n",
              cases[i].name, rc, cpu.r[6], cases[i].want, cpu.cpsr, cpsr_before);
      return 1;
    }
  }
  printf("ok: T16 UXTH r6,r6 b2b6 → XTEND (Q-OTTD-0l)\n");
  return 0;
}

static int test_t16_uxtb_b2f6(void) {
  /* Q-OTTD-0l sibling: uxtb r6,r6 = b2f6. Same Misc 0xB2xx mask. */
  static const uint16_t kProg[] = {0xB2F6u, 0x4770u};
  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);

  MangoInsn di;
  if (mango_decode_t16(0xB2F6u, &di) != 0 || di.op != MANGO_OP_XTEND || di.rd != 6 ||
      di.rm != 6 || di.rn != 15 || di.u != 1 || di.b != 0 || di.sets_flags != 0) {
    fprintf(stderr,
            "FAIL(t16_uxtb_b2f6): decode op=%d rd=%u rm=%u rn=%u u=%d b=%d (want XTEND u=1 b=0)\n",
            di.op, di.rd, di.rm, di.rn, di.u, di.b);
    return 1;
  }

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[6] = 0x1234abcdu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[6] != 0xCDu || cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t16_uxtb_b2f6): rc=%d r6=0x%x want 0xcd cpsr=0x%x\n", rc, cpu.r[6],
            cpu.cpsr);
    return 1;
  }
  printf("ok: T16 UXTB r6,r6 b2f6 → XTEND (Q-OTTD-0l)\n");
  return 0;
}

static int test_t32_add_w_reg_lsl2(void) {
  /* Q-OTTD-0l-add: add.w r0,lr,r0,lsl#2 = eb0e 0080.
   * Drive: lr=0x9fee80, r0=0xffffffff → r0=0x9fee7c; LR/NZCV hold; pc+=4.
   * Stop at pc=4 (after ADD.W) — cannot bx lr because Rn=LR is the source. */
  static const uint16_t kProg[] = {0xEB0Eu, 0x0080u};
  uint8_t mem_buf[64];
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);

  MangoInsn di;
  if (mango_decode_t32(0xEB0Eu, 0x0080u, &di) != 0 || di.op != MANGO_OP_ADD || di.rd != 0 ||
      di.rn != MANGO_REG_LR || di.rm != 0 || di.is_imm != 0 || di.sets_flags != 0 ||
      di.shift_type != 0 || di.shift_amount != 2) {
    fprintf(stderr,
            "FAIL(t32_add_w_reg): decode op=%d rd=%u rn=%u rm=%u is_imm=%d s=%d type=%u amt=%u "
            "(want ADD r0,lr,r0 LSL#2)\n",
            di.op, di.rd, di.rn, di.rm, di.is_imm, di.sets_flags, di.shift_type, di.shift_amount);
    return 1;
  }

  struct {
    uint32_t r0;
    uint32_t lr;
    uint32_t want_r0;
    const char* name;
  } cases[] = {
      {0xffffffffu, 0x009fee80u, 0x009fee7cu, "drive"},
      {3u, 0x1000u, 0x100cu, "simple"},
      {0u, 0xabcd0000u, 0xabcd0000u, "copy"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(mem_buf, 0, sizeof(mem_buf));
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[0] = cases[i].r0;
    cpu.r[MANGO_REG_LR] = cases[i].lr;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    int rc = mango_interp_run(&cpu, &mem, 4u, 100);
    if (rc != 0 || cpu.r[0] != cases[i].want_r0 || cpu.r[MANGO_REG_LR] != cases[i].lr ||
        cpu.cpsr != cpsr_before) {
      fprintf(stderr,
              "FAIL(t32_add_w_reg %s): rc=%d r0=0x%x want 0x%x lr=0x%x want 0x%x cpsr=0x%x\n",
              cases[i].name, rc, cpu.r[0], cases[i].want_r0, cpu.r[MANGO_REG_LR], cases[i].lr,
              cpu.cpsr);
      return 1;
    }
  }
  printf("ok: T32 ADD.W r0,lr,r0,lsl#2 eb0e0080 (Q-OTTD-0l-add)\n");
  return 0;
}

static int test_t32_add_w_reg_reject(void) {
  /* AND.W ea0e0080 stays uncover; PC Rd/Rn/Rm reject; S=1 uncover. */
  MangoInsn di;
  if (mango_decode_t32(0xEA0Eu, 0x0080u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_w_reg_reject): AND ea0e0080 decoded as op=%d\n", di.op);
    return 1;
  }
  /* eb0e 0f80 = ADD.W pc,lr,r0,lsl#2 — Rd=PC */
  if (mango_decode_t32(0xEB0Eu, 0x0F80u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_w_reg_reject): Rd=PC eb0e0f80 decoded as op=%d\n", di.op);
    return 1;
  }
  /* eb0f 0080 = ADD.W r0,pc,r0,lsl#2 — Rn=PC */
  if (mango_decode_t32(0xEB0Fu, 0x0080u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_w_reg_reject): Rn=PC eb0f0080 decoded as op=%d\n", di.op);
    return 1;
  }
  /* eb0e 008f = ADD.W r0,lr,pc,lsl#2 — Rm=PC */
  if (mango_decode_t32(0xEB0Eu, 0x008Fu, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_w_reg_reject): Rm=PC eb0e008f decoded as op=%d\n", di.op);
    return 1;
  }
  /* eb1e 0080 = ADD.W S=1 — out of scope */
  if (mango_decode_t32(0xEB1Eu, 0x0080u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_add_w_reg_reject): S=1 eb1e0080 decoded as op=%d\n", di.op);
    return 1;
  }
  printf("ok: T32 ADD.W reg reject AND/PC/S=1 (Q-OTTD-0l-add)\n");
  return 0;
}

static int test_t32_ldrb_w_reg_lsl3(void) {
  /* Q-OTTD-0m: ldrb.w r0,[r11,r2,lsl#3] = f81b 0032.
   * R0 = ZeroExtend8(Mem[R11+(R2<<3)]); Rn/Rm unchanged; NZCV hold; pc+=4. */
  static const uint16_t kProg[] = {0xF81Bu, 0x0032u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF81Bu, 0x0032u, &di) != 0 || di.op != MANGO_OP_LDR || di.rd != 0 ||
      di.rn != 11 || di.rm != 2 || di.is_imm != 0 || di.shift_type != 0 || di.shift_amount != 3 ||
      di.p != 1 || di.u != 1 || di.w != 0 || di.b != 1) {
    fprintf(stderr,
            "FAIL(t32_ldrb_w_reg): decode op=%d rd=%u rn=%u rm=%u imm=%d st=%u sh=%u p=%d u=%d "
            "w=%d b=%d (want LDR b=1 r0,[r11,r2,lsl#3])\n",
            di.op, di.rd, di.rn, di.rm, di.is_imm, di.shift_type, di.shift_amount, di.p, di.u,
            di.w, di.b);
    return 1;
  }

  uint32_t base = 64u;
  mem_buf[base + 0] = 0xaa;
  mem_buf[base + 8] = 0xbb;
  mem_buf[base + 24] = 0xdd;

  /* r2=1 → [base+8]=0xbb */
  {
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
    uint32_t cpsr_before = cpu.cpsr;
    cpu.r[11] = base;
    cpu.r[2] = 1u;
    cpu.r[0] = 0xffffffffu;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
    if (rc != 0) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): run rc=%d (r2=1)\n", rc);
      return 1;
    }
    if (cpu.r[0] != 0x000000bbu) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): r0=0x%x want 0xbb (r2=1 → +8)\n", cpu.r[0]);
      return 1;
    }
    if (cpu.r[11] != base || cpu.r[2] != 1u) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): Rn/Rm mutated r11=0x%x r2=0x%x\n", cpu.r[11],
              cpu.r[2]);
      return 1;
    }
    if (cpu.cpsr != cpsr_before) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
      return 1;
    }
  }

  /* r2=0 → [base]=0xaa */
  {
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
    mem_buf[base + 0] = 0xaa;
    mem_buf[base + 8] = 0xbb;
    mem_buf[base + 24] = 0xdd;
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T;
    cpu.r[11] = base;
    cpu.r[2] = 0u;
    cpu.r[0] = 0xffffffffu;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    if (mango_interp_run(&cpu, &mem, 0xABCDu, 100) != 0 || cpu.r[0] != 0xaau) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): r2=0 r0=0x%x want 0xaa\n", cpu.r[0]);
      return 1;
    }
  }

  /* r2=3 → [base+24]=0xdd */
  {
    load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);
    mem_buf[base + 0] = 0xaa;
    mem_buf[base + 8] = 0xbb;
    mem_buf[base + 24] = 0xdd;
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cpsr = MANGO_CPSR_T;
    cpu.r[11] = base;
    cpu.r[2] = 3u;
    cpu.r[0] = 0xffffffffu;
    cpu.r[MANGO_REG_LR] = 0xABCDu;
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    if (mango_interp_run(&cpu, &mem, 0xABCDu, 100) != 0 || cpu.r[0] != 0xddu) {
      fprintf(stderr, "FAIL(t32_ldrb_w_reg): r2=3 r0=0x%x want 0xdd\n", cpu.r[0]);
      return 1;
    }
  }

  printf("ok: T32 LDRB.W r0,[r11,r2,lsl#3] ZE8 no WB (Q-OTTD-0m)\n");
  return 0;
}

static int test_t32_ldrb_w_reg_imm2_0(void) {
  /* Q-OTTD-0m sib: ldrb.w r8,[r11,r3] = f81b 8003 (imm2=0). */
  static const uint16_t kProg[] = {0xF81Bu, 0x8003u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF81Bu, 0x8003u, &di) != 0 || di.op != MANGO_OP_LDR || di.rd != 8 ||
      di.rn != 11 || di.rm != 3 || di.is_imm != 0 || di.shift_amount != 0 || di.b != 1 ||
      di.w != 0) {
    fprintf(stderr,
            "FAIL(t32_ldrb_w_imm0): decode op=%d rd=%u rn=%u rm=%u sh=%u b=%d w=%d "
            "(want LDR b=1 r8,[r11,r3])\n",
            di.op, di.rd, di.rn, di.rm, di.shift_amount, di.b, di.w);
    return 1;
  }

  uint32_t base = 64u;
  mem_buf[base + 16] = 0xcc;

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_N;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[11] = base;
  cpu.r[3] = 16u;
  cpu.r[8] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ldrb_w_imm0): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[8] != 0x000000ccu) {
    fprintf(stderr, "FAIL(t32_ldrb_w_imm0): r8=0x%x want 0xcc\n", cpu.r[8]);
    return 1;
  }
  if (cpu.r[11] != base || cpu.r[3] != 16u) {
    fprintf(stderr, "FAIL(t32_ldrb_w_imm0): Rn/Rm mutated\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ldrb_w_imm0): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 LDRB.W r8,[r11,r3] imm2=0 sib (Q-OTTD-0m)\n");
  return 0;
}

static int test_t32_ldr_w_imm8_post(void) {
  /* Q-OTTD-0m-ldr: ldr.w r2,[r5],#4 = f855 2b04. P=0 U=1 W=1 post-index.
   * R2=MemU32[R5]; R5+=4; NZCV hold. NOT [r5,#4] / [r5,#4]!. */
  static const uint16_t kProg[] = {0xF855u, 0x2B04u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF855u, 0x2B04u, &di) != 0 || di.op != MANGO_OP_LDR || di.rd != 2 ||
      di.rn != 5 || di.is_imm != 1 || di.imm != 4 || di.p != 0 || di.u != 1 || di.w != 1 ||
      di.b != 0) {
    fprintf(stderr,
            "FAIL(t32_ldr_w_post): decode op=%d rd=%u rn=%u imm=%u p=%d u=%d w=%d b=%d "
            "(want LDR r2,[r5],#4 p=0 u=1 w=1 b=0)\n",
            di.op, di.rd, di.rn, di.imm, di.p, di.u, di.w, di.b);
    return 1;
  }

  uint32_t base = 64u;
  u32_to_bytes_le(mem_buf + base + 0, 0xa00aaaaau);
  u32_to_bytes_le(mem_buf + base + 4, 0xb00bbbbbu);

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_Z | MANGO_CPSR_C;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[5] = base;
  cpu.r[2] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ldr_w_post): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[2] != 0xa00aaaaau) {
    fprintf(stderr, "FAIL(t32_ldr_w_post): r2=0x%x want 0xa00aaaaa (load from [r5])\n", cpu.r[2]);
    return 1;
  }
  if (cpu.r[5] != base + 4u) {
    fprintf(stderr, "FAIL(t32_ldr_w_post): r5=0x%x want 0x%x (post WB +4)\n", cpu.r[5], base + 4u);
    return 1;
  }
  if (bytes_to_u32_le(mem_buf + base + 0) != 0xa00aaaaau) {
    fprintf(stderr, "FAIL(t32_ldr_w_post): memory mutated\n");
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ldr_w_post): cpsr changed 0x%x -> 0x%x\n", cpsr_before, cpu.cpsr);
    return 1;
  }
  printf("ok: T32 LDR.W r2,[r5],#4 post-index WB (Q-OTTD-0m-ldr)\n");
  return 0;
}

static int test_t32_ldr_w_imm8_post_sp(void) {
  /* Footnote: ldr.w r4,[sp],#4 = f85d 4b04. Same P=0 U=1 W=1 arm. */
  static const uint16_t kProg[] = {0xF85Du, 0x4B04u, 0x4770u};
  uint8_t mem_buf[128];
  memset(mem_buf, 0, sizeof(mem_buf));
  load_halfwords(mem_buf, sizeof(mem_buf), kProg, 3);

  MangoInsn di;
  if (mango_decode_t32(0xF85Du, 0x4B04u, &di) != 0 || di.op != MANGO_OP_LDR || di.rd != 4 ||
      di.rn != MANGO_REG_SP || di.is_imm != 1 || di.imm != 4 || di.p != 0 || di.u != 1 ||
      di.w != 1 || di.b != 0) {
    fprintf(stderr,
            "FAIL(t32_ldr_w_post_sp): decode op=%d rd=%u rn=%u imm=%u p=%d u=%d w=%d b=%d "
            "(want LDR r4,[sp],#4)\n",
            di.op, di.rd, di.rn, di.imm, di.p, di.u, di.w, di.b);
    return 1;
  }

  uint32_t slot = 64u;
  u32_to_bytes_le(mem_buf + slot, 0xd00dddddu);

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.cpsr = MANGO_CPSR_T | MANGO_CPSR_V;
  uint32_t cpsr_before = cpu.cpsr;
  cpu.r[MANGO_REG_SP] = slot;
  cpu.r[4] = 0xffffffffu;
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(t32_ldr_w_post_sp): run rc=%d\n", rc);
    return 1;
  }
  if (cpu.r[4] != 0xd00dddddu) {
    fprintf(stderr, "FAIL(t32_ldr_w_post_sp): r4=0x%x want 0xd00ddddd\n", cpu.r[4]);
    return 1;
  }
  if (cpu.r[MANGO_REG_SP] != slot + 4u) {
    fprintf(stderr, "FAIL(t32_ldr_w_post_sp): sp=0x%x want 0x%x\n", cpu.r[MANGO_REG_SP],
            slot + 4u);
    return 1;
  }
  if (cpu.cpsr != cpsr_before) {
    fprintf(stderr, "FAIL(t32_ldr_w_post_sp): cpsr changed 0x%x -> 0x%x\n", cpsr_before,
            cpu.cpsr);
    return 1;
  }
  printf("ok: T32 LDR.W r4,[sp],#4 footnote (Q-OTTD-0m)\n");
  return 0;
}

static int test_t32_ldrb_ldr_post_reject(void) {
  /* Negatives: LDR.W reg LSL#2 (drive distractor); LDR pre-WB; Rt=PC. */
  MangoInsn di;
  /* f855 5021 = ldr.w r5,[r5,r1,lsl#2] — reg imm2=2; extend 0f later */
  if (mango_decode_t32(0xF855u, 0x5021u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_0m_reject): LDR reg LSL#2 f8555021 decoded as op=%d sh=%u\n",
            di.op, di.shift_amount);
    return 1;
  }
  /* f855 2f04 = ldr.w r2,[r5,#4]! — P=1 U=1 W=1; exact 0x0B00 rejects */
  if (mango_decode_t32(0xF855u, 0x2F04u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_0m_reject): LDR pre-WB f8552f04 decoded as op=%d p=%d w=%d\n",
            di.op, di.p, di.w);
    return 1;
  }
  /* f81b f032 = LDRB Rt=PC */
  if (mango_decode_t32(0xF81Bu, 0xF032u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_0m_reject): LDRB Rt=PC decoded as op=%d\n", di.op);
    return 1;
  }
  /* f85f 2b04 = LDR Rn=PC */
  if (mango_decode_t32(0xF85Fu, 0x2B04u, &di) == 0) {
    fprintf(stderr, "FAIL(t32_0m_reject): LDR post Rn=PC decoded as op=%d\n", di.op);
    return 1;
  }
  printf("ok: T32 LDRB/LDR post reject reg-LSL2/pre-WB/PC (Q-OTTD-0m)\n");
  return 0;
}

static int test_thumb_bx_pc_veneer(void) {
  /* Thumb BX PC into ARM PLT-style veneer: dest is addr+4, not r15. */
  uint8_t mem_buf[32];
  memset(mem_buf, 0, sizeof(mem_buf));
  mem_buf[0] = 0x78; /* bx pc */
  mem_buf[1] = 0x47;
  mem_buf[2] = 0xC0; /* mov r8, r8 */
  mem_buf[3] = 0x46;
  mem_buf[4] = 0x07; /* mov r0, #7 */
  mem_buf[5] = 0x00;
  mem_buf[6] = 0xA0;
  mem_buf[7] = 0xE3;
  mem_buf[8] = 0x1E; /* bx lr */
  mem_buf[9] = 0xFF;
  mem_buf[10] = 0x2F;
  mem_buf[11] = 0xE1;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = MANGO_CPSR_T;
  cpu.r[MANGO_REG_LR] = 0xF00Du;

  int rc = mango_interp_run(&cpu, &mem, 0xF00Du, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(thumb_bx_pc_veneer): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 7) {
    fprintf(stderr, "FAIL(thumb_bx_pc_veneer): expected r0==7, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: Thumb BX PC veneer into ARM (r0=%u)\n", cpu.r[0]);
  return 0;
}

static int test_arm_add_pc(void) {
  /* add pc, r0, pc — ARM PLT tail. At the add, PC as operand is addr+8. */
  static const uint32_t kProgram[] = {
      0xE3A00004u, /* mov r0, #4 */
      0xE080F00Fu, /* add pc, r0, pc  -> 4 + (4+8) = 16 */
      0xE3A00063u, /* mov r0, #99 (skipped) */
      0xE3A00063u, /* mov r0, #99 (skipped) */
      0xE3A0002Au, /* mov r0, #42 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 6);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  for (int i = 0; i < 16; i++) {
    cpu.r[i] = 0;
  }
  cpu.cpsr = 0;
  cpu.r[MANGO_REG_LR] = 0xB0B0u;

  int rc = mango_interp_run(&cpu, &mem, 0xB0B0u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(arm_add_pc): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.r[0] != 42) {
    fprintf(stderr, "FAIL(arm_add_pc): expected r0==42, got %u\n", cpu.r[0]);
    return 1;
  }
  printf("ok: ARM ADD PC (PLT) (r0=%u)\n", cpu.r[0]);
  return 0;
}

static int test_vldr_s_from_stack(void) {
  static const uint32_t kProgram[] = {
      0xE3A0D040u, /* mov sp, #64 */
      0xED9D0A00u, /* vldr s0, [sp] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  mem_buf[64] = 0x00;
  mem_buf[65] = 0x00;
  mem_buf[66] = 0x80;
  mem_buf[67] = 0x3F; /* 1.0f */
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x1111u;

  int rc = mango_interp_run(&cpu, &mem, 0x1111u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(vldr_s_from_stack): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.s[0] != 0x3F800000u) {
    fprintf(stderr, "FAIL(vldr_s_from_stack): s0=0x%08x want 0x3f800000\n", cpu.s[0]);
    return 1;
  }
  printf("ok: VLDR s0, [sp] (s0=0x%08x)\n", cpu.s[0]);
  return 0;
}

static int test_vmov_i32_and_clz(void) {
  static const uint32_t kProgram[] = {
      0xF2C00050u, /* vmov.i32 q8, #0 */
      0xE3A01008u, /* mov r1, #8 */
      0xE16F0F11u, /* clz r0, r1 */
      0xE320F000u, /* nop */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[32] = cpu.s[33] = cpu.s[34] = cpu.s[35] = 0xA5A5A5A5u;
  cpu.r[MANGO_REG_LR] = 0x2222u;

  int rc = mango_interp_run(&cpu, &mem, 0x2222u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(vmov_i32_and_clz): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (cpu.s[32] != 0 || cpu.s[33] != 0 || cpu.s[34] != 0 || cpu.s[35] != 0) {
    fprintf(stderr, "FAIL(vmov_i32_and_clz): q8 not zeroed\n");
    return 1;
  }
  if (cpu.r[0] != 28u) {
    fprintf(stderr, "FAIL(vmov_i32_and_clz): clz(8) r0=%u want 28\n", cpu.r[0]);
    return 1;
  }
  printf("ok: VMOV.I32 q8,#0 and CLZ (r0=%u)\n", cpu.r[0]);
  return 0;
}

static int test_vld1_vst1(void) {
  static const uint32_t kProgram[] = {
      0xE3A01040u, /* mov r1, #64 */
      0xE3A02050u, /* mov r2, #80 */
      0xF4210ACFu, /* vld1.64 {d0, d1}, [r1] */
      0xF4020ACFu, /* vst1.64 {d0, d1}, [r2] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  for (int i = 0; i < 16; i++) {
    mem_buf[64 + i] = (uint8_t)(0xA0 + i);
  }
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x3333u;

  int rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
  if (rc != 0) {
    fprintf(stderr, "FAIL(vld1_vst1): mango_interp_run returned %d\n", rc);
    return 1;
  }
  if (memcmp(mem_buf + 80, mem_buf + 64, 16) != 0) {
    fprintf(stderr, "FAIL(vld1_vst1): 16-byte copy mismatch\n");
    return 1;
  }
  printf("ok: VLD1/VST1.64 {d0,d1} 16-byte copy\n");
  return 0;
}

static int test_ldrex_strex(void) {
  static const uint32_t kProgram[] = {
      0xE3A01040u, /* mov r1, #64 */
      0xE1910F9Fu, /* ldrex r0, [r1] */
      0xE2800001u, /* add r0, r0, #1 */
      0xE1812F90u, /* strex r2, r0, [r1] */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  mem_buf[64] = 41;
  mem_buf[65] = 0;
  mem_buf[66] = 0;
  mem_buf[67] = 0;
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x4444u;

  int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
  if (rc != 0 || cpu.r[0] != 42u || cpu.r[2] != 0 || mem_buf[64] != 42) {
    fprintf(stderr, "FAIL(ldrex_strex): rc=%d r0=%u r2=%u mem=%u\n", rc, cpu.r[0], cpu.r[2],
            mem_buf[64]);
    return 1;
  }
  printf("ok: LDREX/STREX increment (r0=42, strex status=0)\n");
  return 0;
}

static int test_vdup_q9(void) {
  static const uint32_t kProgram[] = {
      0xE3A020AAu, /* mov r2, #170 */
      0xEEA22B90u, /* vdup.32 q9, r2 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x5555u;

  int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
  if (rc != 0 || cpu.s[36] != 0xAAu || cpu.s[37] != 0xAAu || cpu.s[38] != 0xAAu ||
      cpu.s[39] != 0xAAu) {
    fprintf(stderr, "FAIL(vdup_q9): rc=%d s36-39=%x %x %x %x\n", rc, cpu.s[36], cpu.s[37],
            cpu.s[38], cpu.s[39]);
    return 1;
  }
  printf("ok: VDUP.32 q9, r2 replicates 0xaa\n");
  return 0;
}

static int test_vaddi_i32(void) {
  static const uint32_t kProgram[] = {
      0xF2C00052u, /* vmov.i32 q8, #2 */
      0xF2C02051u, /* vmov.i32 q9, #1 */
      0xF26208E0u, /* vadd.i32 q8, q9, q8 */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x6666u;

  int rc = mango_interp_run(&cpu, &mem, 0x6666u, 100);
  if (rc != 0 || cpu.s[32] != 3u || cpu.s[33] != 3u || cpu.s[34] != 3u || cpu.s[35] != 3u) {
    fprintf(stderr, "FAIL(vaddi_i32): rc=%d s32-35=%x %x %x %x\n", rc, cpu.s[32], cpu.s[33],
            cpu.s[34], cpu.s[35]);
    return 1;
  }
  printf("ok: VADD.I32 q8, q9, q8 (lanes=3)\n");
  return 0;
}

static int test_vpush_vpop(void) {
  static const uint32_t kProgram[] = {
      0xE3A0D050u, /* mov sp, #80 */
      0xED2D8B02u, /* vpush {d8} */
      0xECBD8B02u, /* vpop {d8} */
      0xE12FFF1Eu, /* bx lr */
  };

  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 4);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};

  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[16] = 0x11111111u;
  cpu.s[17] = 0x22222222u;
  cpu.r[MANGO_REG_LR] = 0x7777u;

  int rc = mango_interp_run(&cpu, &mem, 0x7777u, 100);
  if (rc != 0 || cpu.r[MANGO_REG_SP] != 80u || cpu.s[16] != 0x11111111u ||
      cpu.s[17] != 0x22222222u) {
    fprintf(stderr, "FAIL(vpush_vpop): rc=%d sp=%u s16=%x s17=%x\n", rc, cpu.r[MANGO_REG_SP],
            cpu.s[16], cpu.s[17]);
    return 1;
  }
  printf("ok: VPUSH/VPOP {d8} roundtrip\n");
  return 0;
}

static int test_bfc_ubfx(void) {
  static const uint32_t kBfc[] = {
      0xE3E00000u, /* mvn r0, #0 */
      0xE7C1001Fu, /* bfc r0, #0, #2 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kBfc, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x8888u;
  int rc = mango_interp_run(&cpu, &mem, 0x8888u, 100);
  if (rc != 0 || cpu.r[0] != 0xFFFFFFFCu) {
    fprintf(stderr, "FAIL(bfc): rc=%d r0=0x%x\n", rc, cpu.r[0]);
    return 1;
  }

  static const uint32_t kUbfx[] = {
      0xE3A010FCu, /* mov r1, #252 */
      0xE7E40151u, /* ubfx r0, r1, #2, #5 */
      0xE12FFF1Eu,
  };
  load_words(mem_buf, sizeof(mem_buf), kUbfx, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x8888u;
  rc = mango_interp_run(&cpu, &mem, 0x8888u, 100);
  if (rc != 0 || cpu.r[0] != 31u) {
    fprintf(stderr, "FAIL(ubfx): rc=%d r0=%u\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: BFC r0,#0,#2 and UBFX r0,r1,#2,#5\n");
  return 0;
}

static int test_rbit_uxt_rev(void) {
  static const uint32_t kRbit[] = {
      0xE3A01001u, /* mov r1, #1 */
      0xE6FF0F31u, /* rbit r0, r1 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kRbit, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x9999u;
  int rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 0 || cpu.r[0] != 0x80000000u) {
    fprintf(stderr, "FAIL(rbit): rc=%d r0=0x%x\n", rc, cpu.r[0]);
    return 1;
  }

  static const uint32_t kExt[] = {
      0xE30A1BCDu, /* movw r1, #0xabcd */
      0xE6FF0071u, /* uxth r0, r1 */
      0xE6EF2071u, /* uxtb r2, r1 */
      0xE6BF3071u, /* sxth r3, r1 */
      0xE12FFF1Eu,
  };
  load_words(mem_buf, sizeof(mem_buf), kExt, 5);
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x9999u;
  rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 0 || cpu.r[0] != 0xABCDu || cpu.r[2] != 0xCDu || cpu.r[3] != 0xFFFFABCDu) {
    fprintf(stderr, "FAIL(uxt): rc=%d r0=0x%x r2=0x%x r3=0x%x\n", rc, cpu.r[0], cpu.r[2],
            cpu.r[3]);
    return 1;
  }

  static const uint32_t kRev[] = {
      0xE30A1BCDu, /* movw r1, #0xabcd */
      0xE6BF0F31u, /* rev r0, r1 */
      0xE12FFF1Eu,
  };
  load_words(mem_buf, sizeof(mem_buf), kRev, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x9999u;
  rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
  if (rc != 0 || cpu.r[0] != 0xCDAB0000u) {
    fprintf(stderr, "FAIL(rev): rc=%d r0=0x%x\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: RBIT, UXTH/UXTB/SXTH, REV\n");
  return 0;
}

static int test_vmov_s_gpr(void) {
  static const uint32_t kProgram[] = {
      0xE3001000u, /* movw r1, #0 */
      0xE3431F80u, /* movt r1, #0x3f80 */
      0xEE001A10u, /* vmov s0, r1 */
      0xEE100A10u, /* vmov r0, s0 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0xAAAAu;
  int rc = mango_interp_run(&cpu, &mem, 0xAAAAu, 100);
  if (rc != 0 || cpu.r[0] != 0x3F800000u || cpu.s[0] != 0x3F800000u) {
    fprintf(stderr, "FAIL(vmov_s_gpr): rc=%d r0=0x%x s0=0x%x\n", rc, cpu.r[0], cpu.s[0]);
    return 1;
  }
  printf("ok: VMOV s0, r1 / r0, s0 (1.0f)\n");
  return 0;
}

static int test_vcvt_f32_f64(void) {
  static const uint32_t kProgram[] = {
      0xEEB70BC0u, /* vcvt.f32.f64 s0, d0 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0x00000000u;
  cpu.s[1] = 0x3FF00000u; /* d0 = 1.0 */
  cpu.r[MANGO_REG_LR] = 0xBBBBu;
  int rc = mango_interp_run(&cpu, &mem, 0xBBBBu, 100);
  if (rc != 0 || cpu.s[0] != 0x3F800000u) {
    fprintf(stderr, "FAIL(vcvt_f32_f64): rc=%d s0=0x%x\n", rc, cpu.s[0]);
    return 1;
  }
  printf("ok: VCVT.F32.F64 s0, d0 (1.0)\n");
  return 0;
}

static int test_vcmpe_f32_zero(void) {
  static const uint32_t kProgram[] = {
      0xEEB50AC0u, /* vcmpe.f32 s0, #0 */
      0xEEF1FA10u, /* vmrs apsr_nzcv, fpscr */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0xCCCCu;
  int rc = mango_interp_run(&cpu, &mem, 0xCCCCu, 100);
  if (rc != 0 || (cpu.cpsr & (MANGO_CPSR_Z | MANGO_CPSR_C)) != (MANGO_CPSR_Z | MANGO_CPSR_C)) {
    fprintf(stderr, "FAIL(vcmpe_f32_zero): rc=%d cpsr=0x%x\n", rc, cpu.cpsr);
    return 1;
  }
  printf("ok: VCMPE.F32 s0, #0 (Z+C)\n");
  return 0;
}

static int test_vadd_f32(void) {
  static const uint32_t kProgram[] = {
      0xEE300A20u, /* vadd.f32 s0, s0, s1 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0x3F800000u;
  cpu.s[1] = 0x3F800000u;
  cpu.r[MANGO_REG_LR] = 0xDDDDu;
  int rc = mango_interp_run(&cpu, &mem, 0xDDDDu, 100);
  if (rc != 0 || cpu.s[0] != 0x40000000u) {
    fprintf(stderr, "FAIL(vadd_f32): rc=%d s0=0x%x\n", rc, cpu.s[0]);
    return 1;
  }
  printf("ok: VADD.F32 s0, s0, s1 (2.0f)\n");
  return 0;
}


static int test_vcvt_f32_u32(void) {
  /* OFDP nativeRender stop 0xeeb81a41: vcvt.f32.u32 s2, s2 */
  static const uint32_t kProgram[] = {
      0xEEB81A41u,
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[2] = 42u;
  cpu.r[MANGO_REG_LR] = 0xEEEEu;
  int rc = mango_interp_run(&cpu, &mem, 0xEEEEu, 100);
  if (rc != 0 || cpu.s[2] != 0x42280000u) {
    fprintf(stderr, "FAIL(vcvt_f32_u32): rc=%d s2=0x%x\n", rc, cpu.s[2]);
    return 1;
  }
  printf("ok: VCVT.F32.U32 s2, s2 (42 -> 42.0f)\n");
  return 0;
}

static int test_vcvt_s32_f32(void) {
  static const uint32_t kProgram[] = {
      0xEEBD0AC0u, /* vcvt.s32.f32 s0, s0 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0x40000000u; /* 2.0f */
  cpu.r[MANGO_REG_LR] = 0xEEEEu;
  int rc = mango_interp_run(&cpu, &mem, 0xEEEEu, 100);
  if (rc != 0 || cpu.s[0] != 2u) {
    fprintf(stderr, "FAIL(vcvt_s32_f32): rc=%d s0=%u\n", rc, cpu.s[0]);
    return 1;
  }
  printf("ok: VCVT.S32.F32 s0, s0 (2.0f -> 2)\n");
  return 0;
}

static int test_vcvt_s32_f64(void) {
  static const uint32_t kProgram[] = {
      0xEEBD0BC0u, /* vcvt.s32.f64 s0, d0 */
      0xEEBD1BE0u, /* vcvt.s32.f64 s2, d16 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProgram, 3);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0;
  cpu.s[1] = 0x40000000u; /* d0 = 2.0 */
  cpu.s[32] = 0;
  cpu.s[33] = 0x40080000u; /* d16 = 3.0 */
  cpu.r[MANGO_REG_LR] = 0xFFFFu;
  int rc = mango_interp_run(&cpu, &mem, 0xFFFFu, 100);
  if (rc != 0 || cpu.s[0] != 2u || cpu.s[2] != 3u) {
    fprintf(stderr, "FAIL(vcvt_s32_f64): rc=%d s0=%u s2=%u\n", rc, cpu.s[0], cpu.s[2]);
    return 1;
  }
  printf("ok: VCVT.S32.F64 s0,d0 and s2,d16\n");
  return 0;
}

static int test_vmov_f32_imm_and_smmul(void) {
  static const uint32_t kVmov[] = {
      0xF2C70F50u, /* vmov.f32 q8, #1.0 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kVmov, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  int rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.s[32] != 0x3F800000u || cpu.s[33] != 0x3F800000u) {
    fprintf(stderr, "FAIL(vmov_f32_imm): rc=%d s32=0x%x\n", rc, cpu.s[32]);
    return 1;
  }

  static const uint32_t kSmmul[] = {
      0xE3A01801u, /* mov r1, #0x10000 */
      0xE1A02001u, /* mov r2, r1 */
      0xE750F112u, /* smmul r0, r2, r1 */
      0xE12FFF1Eu,
  };
  load_words(mem_buf, sizeof(mem_buf), kSmmul, 4);
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0xABCDu;
  rc = mango_interp_run(&cpu, &mem, 0xABCDu, 100);
  if (rc != 0 || cpu.r[0] != 1u) {
    fprintf(stderr, "FAIL(smmul): rc=%d r0=%u\n", rc, cpu.r[0]);
    return 1;
  }
  printf("ok: VMOV.F32 q8,#1.0 and SMMUL\n");
  return 0;
}



static int test_vmov_f32_ss(void) {
  /* OFDP nativeRender stop 0xeeb00a48: VMOV.F32 s0, s16 (high S via M/Vm). */
  MangoInsn insn;
  memset(&insn, 0, sizeof(insn));
  if (mango_decode(0xEEB00A48u, &insn) != 0 || insn.op != MANGO_OP_VMOV ||
      insn.u != 6 || insn.rd != 0u || insn.rm != 16u || insn.b != 0) {
    fprintf(stderr, "FAIL(vmov_f32_ss): decode op=%d u=%d rd=%u rm=%u b=%d\n", (int)insn.op,
            insn.u, insn.rd, insn.rm, insn.b);
    return 1;
  }

  static const uint32_t kProg[] = {
      0xEEB00A48u, /* vmov.f32 s0, s16 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProg, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[16] = 0x3F800000u; /* 1.0f */
  cpu.s[0] = 0xDEADBEEFu;
  cpu.r[MANGO_REG_LR] = 0x5656u;
  int rc = mango_interp_run(&cpu, &mem, 0x5656u, 100);
  if (rc != 0 || cpu.s[0] != 0x3F800000u || cpu.s[16] != 0x3F800000u) {
    fprintf(stderr, "FAIL(vmov_f32_ss): rc=%d s0=0x%x s16=0x%x\n", rc, cpu.s[0], cpu.s[16]);
    return 1;
  }

  /* Same encoding group: VMOV.F64 d0, d1 */
  memset(&insn, 0, sizeof(insn));
  if (mango_decode(0xEEB00B41u, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 6 ||
      insn.rd != 0u || insn.rm != 1u || insn.b != 1) {
    fprintf(stderr, "FAIL(vmov_f64_dd): decode op=%d u=%d rd=%u rm=%u b=%d\n", (int)insn.op,
            insn.u, insn.rd, insn.rm, insn.b);
    return 1;
  }
  static const uint32_t kD[] = {
      0xEEB00B41u, /* vmov.f64 d0, d1 */
      0xE12FFF1Eu,
  };
  load_words(mem_buf, sizeof(mem_buf), kD, 2);
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[2] = 0x00000000u;
  cpu.s[3] = 0x3FF00000u; /* d1 = 1.0 */
  cpu.s[0] = 0xCAFEu;
  cpu.s[1] = 0xBABEu;
  cpu.r[MANGO_REG_LR] = 0x5757u;
  rc = mango_interp_run(&cpu, &mem, 0x5757u, 100);
  if (rc != 0 || cpu.s[0] != 0u || cpu.s[1] != 0x3FF00000u) {
    fprintf(stderr, "FAIL(vmov_f64_dd): rc=%d s0=0x%x s1=0x%x\n", rc, cpu.s[0], cpu.s[1]);
    return 1;
  }
  printf("ok: VMOV.F32 s0,s16 (0xeeb00a48) and VMOV.F64 d0,d1\n");
  return 0;
}

static int test_vmov_f32_scalar_imm(void) {
  /* OFDP nativeRender stop: VMOV.F32 s0, #0.5 (imm8=0x60 → 0x3f000000). */
  static const uint32_t kProg[] = {
      0xEEB60A00u, /* vmov.f32 s0, #0.5 */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  load_words(mem_buf, sizeof(mem_buf), kProg, 2);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x1234u;
  int rc = mango_interp_run(&cpu, &mem, 0x1234u, 100);
  if (rc != 0 || cpu.s[0] != 0x3F000000u) {
    fprintf(stderr, "FAIL(vmov_f32_scalar_imm): rc=%d s0=0x%x\n", rc, cpu.s[0]);
    return 1;
  }
  printf("ok: VMOV.F32 s0, #0.5 (0xeeb60a00)\n");
  return 0;
}


static int test_ofdp_native_render_vmul_vcmp(void) {
  /* OFDP nativeRender scale loop fragment: s4 = (float)r0 * 0.5; VCMPE; VMRS.
   * Prior VCMP GT left NZCV clear → BLS always taken → r0 doubles to 0 → BCC spin. */
  static const uint32_t kProg[] = {
      0xEEB60A00u, /* vmov.f32 s0, #0.5 */
      0xF2C00050u, /* vmov.i32 q8, #0  (must not clobber s0) */
      0xEE010A10u, /* vmov s2, r0 */
      0xEEB81A41u, /* vcvt.f32.u32 s2, s2 */
      0xEE212A00u, /* vmul.f32 s4, s2, s0 */
      0xEEB52AC0u, /* vcmpe.f32 s4, #0 */
      0xEEF1FA10u, /* vmrs apsr_nzcv, fpscr */
      0xE12FFF1Eu, /* bx lr */
  };
  uint8_t mem_buf[64];
  load_words(mem_buf, sizeof(mem_buf), kProg, 8);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[0] = 0x20000u;
  cpu.r[1] = 0x20000u;
  cpu.r[MANGO_REG_LR] = 0x0FDBu;
  int rc = mango_interp_run(&cpu, &mem, 0x0FDBu, 100);
  uint32_t nzcv = cpu.cpsr & 0xF0000000u;
  int ls = ((cpu.cpsr & MANGO_CPSR_Z) != 0) || ((cpu.cpsr & MANGO_CPSR_C) == 0);
  if (rc != 0 || cpu.s[0] != 0x3F000000u || cpu.s[2] != 0x48000000u ||
      cpu.s[4] != 0x47800000u || nzcv != MANGO_CPSR_C || ls) {
    fprintf(stderr,
            "FAIL(ofdp_vmul_vcmp): rc=%d s0=0x%x s2=0x%x s4=0x%x cpsr=0x%x ls=%d\n", rc,
            cpu.s[0], cpu.s[2], cpu.s[4], cpu.cpsr, ls);
    return 1;
  }
  printf("ok: OFDP VMOV#0.5/VMUL/VCMPE GT sets C (BLS not taken)\n");
  return 0;
}

static int test_vcmp_fpscr_nzcv(void) {
  /* ARM-correct VCMP FPSCR: EQ=Z|C, LT=N, GT=C, Unordered=C|V */
  static const uint32_t kEq[] = {0xEEB50AC0u, 0xEEF1FA10u, 0xE12FFF1Eu};
  static const uint32_t kLt[] = {
      0xEEB50AC0u, /* vcmpe.f32 s0, #0 — s0 = -1.0f */
      0xEEF1FA10u,
      0xE12FFF1Eu,
  };
  static const uint32_t kGt[] = {
      0xEEB50AC0u, /* vcmpe.f32 s0, #0 — s0 = +1.0f */
      0xEEF1FA10u,
      0xE12FFF1Eu,
  };
  static const uint32_t kUn[] = {
      0xEEB50AC0u, /* vcmpe.f32 s0, #0 — s0 = qNaN */
      0xEEF1FA10u,
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[32];
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;

  load_words(mem_buf, sizeof(mem_buf), kEq, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0x1111u;
  if (mango_interp_run(&cpu, &mem, 0x1111u, 100) != 0 ||
      (cpu.cpsr & 0xF0000000u) != (MANGO_CPSR_Z | MANGO_CPSR_C)) {
    fprintf(stderr, "FAIL(vcmp_eq): cpsr=0x%x\n", cpu.cpsr);
    return 1;
  }

  load_words(mem_buf, sizeof(mem_buf), kLt, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0xBF800000u; /* -1.0f */
  cpu.r[MANGO_REG_LR] = 0x2222u;
  if (mango_interp_run(&cpu, &mem, 0x2222u, 100) != 0 ||
      (cpu.cpsr & 0xF0000000u) != MANGO_CPSR_N) {
    fprintf(stderr, "FAIL(vcmp_lt): cpsr=0x%x\n", cpu.cpsr);
    return 1;
  }

  load_words(mem_buf, sizeof(mem_buf), kGt, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0x3F800000u; /* +1.0f */
  cpu.r[MANGO_REG_LR] = 0x3333u;
  if (mango_interp_run(&cpu, &mem, 0x3333u, 100) != 0 ||
      (cpu.cpsr & 0xF0000000u) != MANGO_CPSR_C) {
    fprintf(stderr, "FAIL(vcmp_gt): cpsr=0x%x\n", cpu.cpsr);
    return 1;
  }

  load_words(mem_buf, sizeof(mem_buf), kUn, 3);
  memset(&cpu, 0, sizeof(cpu));
  cpu.s[0] = 0x7FC00000u; /* qNaN */
  cpu.r[MANGO_REG_LR] = 0x4444u;
  if (mango_interp_run(&cpu, &mem, 0x4444u, 100) != 0 ||
      (cpu.cpsr & 0xF0000000u) != (MANGO_CPSR_C | MANGO_CPSR_V)) {
    fprintf(stderr, "FAIL(vcmp_unord): cpsr=0x%x\n", cpu.cpsr);
    return 1;
  }
  printf("ok: VCMP FPSCR EQ/LT/GT/Unordered\n");
  return 0;
}

static int test_strexd(void) {
  static const uint32_t kProg[] = {
      0xE3A00040u, /* mov r0, #64 */
      0xE3A02011u, /* mov r2, #17 */
      0xE3A03022u, /* mov r3, #34 */
      0xE1A01F92u, /* strexd r1, r2, r3, [r0] */
      0xE12FFF1Eu,
  };
  uint8_t mem_buf[128];
  load_words(mem_buf, sizeof(mem_buf), kProg, 5);
  MangoMemory mem = {mem_buf, sizeof(mem_buf)};
  MangoCpu cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.r[MANGO_REG_LR] = 0xDEADu;
  int rc = mango_interp_run(&cpu, &mem, 0xDEADu, 100);
  uint32_t lo = (uint32_t)mem_buf[64] | ((uint32_t)mem_buf[65] << 8) | ((uint32_t)mem_buf[66] << 16) |
                ((uint32_t)mem_buf[67] << 24);
  uint32_t hi = (uint32_t)mem_buf[68] | ((uint32_t)mem_buf[69] << 8) | ((uint32_t)mem_buf[70] << 16) |
                ((uint32_t)mem_buf[71] << 24);
  if (rc != 0 || cpu.r[1] != 0 || lo != 17u || hi != 34u) {
    fprintf(stderr, "FAIL(strexd): rc=%d r1=%u lo=%u hi=%u\n", rc, cpu.r[1], lo, hi);
    return 1;
  }
  printf("ok: STREXD r1, r2, r3, [r0]\n");
  return 0;
}

static int test_neon_ctor_vmov_vmvn_vrecpe_vext(void) {
  /* OFDP ctor uncovereds: VMOV.I32 cmode C, VMVN, VRECPE.F32, VEXT. */
  MangoInsn insn;

  if (mango_decode(0xF3C70C5Fu, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 3 ||
      insn.b != 1 || insn.rd != 16u || insn.imm != 0x0000FFFFu || insn.rs != 0x0000FFFFu) {
    fprintf(stderr, "FAIL(ctor_neon): vmov.decode op=%d u=%d rd=%u imm=0x%x rs=0x%x\n",
            (int)insn.op, insn.u, insn.rd, insn.imm, insn.rs);
    return 1;
  }
  if (mango_decode(0xF3C00670u, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 3 ||
      insn.b != 1 || insn.rd != 16u || insn.imm != 0x7FFFFFFFu || insn.rs != 0x7FFFFFFFu) {
    fprintf(stderr, "FAIL(ctor_neon): vmvn.decode op=%d imm=0x%x rs=0x%x\n", (int)insn.op,
            insn.imm, insn.rs);
    return 1;
  }
  if (mango_decode(0xF3FB2560u, &insn) != 0 || insn.op != MANGO_OP_VRECPE || insn.b != 1 ||
      insn.rd != 18u || insn.rm != 16u) {
    fprintf(stderr, "FAIL(ctor_neon): vrecpe.decode op=%d rd=%u rm=%u\n", (int)insn.op, insn.rd,
            insn.rm);
    return 1;
  }
  if (mango_decode(0xF2F104A4u, &insn) != 0 || insn.op != MANGO_OP_VEXT || insn.b != 0 ||
      insn.rd != 16u || insn.rn != 17u || insn.rm != 20u || insn.imm != 4u) {
    fprintf(stderr, "FAIL(ctor_neon): vext.decode op=%d rd=%u rn=%u rm=%u imm=%u\n", (int)insn.op,
            insn.rd, insn.rn, insn.rm, insn.imm);
    return 1;
  }

  {
    static const uint32_t kProg[] = {0xF3C70C5Fu, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[MANGO_REG_LR] = 0x1111u;
    int rc = mango_interp_run(&cpu, &mem, 0x1111u, 100);
    if (rc != 0 || cpu.s[32] != 0x0000FFFFu || cpu.s[33] != 0x0000FFFFu ||
        cpu.s[34] != 0x0000FFFFu || cpu.s[35] != 0x0000FFFFu) {
      fprintf(stderr, "FAIL(ctor_neon): vmov.exec rc=%d\n", rc);
      return 1;
    }
  }

  {
    static const uint32_t kProg[] = {0xF3C00670u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[MANGO_REG_LR] = 0x2222u;
    int rc = mango_interp_run(&cpu, &mem, 0x2222u, 100);
    if (rc != 0 || cpu.s[32] != 0x7FFFFFFFu || cpu.s[33] != 0x7FFFFFFFu ||
        cpu.s[34] != 0x7FFFFFFFu || cpu.s[35] != 0x7FFFFFFFu) {
      fprintf(stderr, "FAIL(ctor_neon): vmvn.exec rc=%d\n", rc);
      return 1;
    }
  }

  {
    /* Arm FPRecipEstimate — not exact 1/x. */
    static const uint32_t kProg[] = {0xF3FB2560u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[32] = 0x40000000u; /* 2.0f → 0x3eff8000 */
    cpu.s[33] = 0x40800000u; /* 4.0f → 0x3e7f8000 */
    cpu.s[34] = 0x3F000000u; /* 0.5f → 0x3fff8000 */
    cpu.s[35] = 0xC0000000u; /* -2.0f → 0xbeff8000 */
    cpu.r[MANGO_REG_LR] = 0x3333u;
    int rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
    if (rc != 0 || cpu.s[36] != 0x3EFF8000u || cpu.s[37] != 0x3E7F8000u ||
        cpu.s[38] != 0x3FFF8000u || cpu.s[39] != 0xBEFF8000u) {
      fprintf(stderr, "FAIL(ctor_neon): vrecpe.exec rc=%d q9=%08x %08x %08x %08x\n", rc, cpu.s[36],
              cpu.s[37], cpu.s[38], cpu.s[39]);
      return 1;
    }
    /* Specials: 0 → +Inf + DZC; NaN → DefaultNaN */
    cpu.s[32] = 0;
    cpu.s[33] = 0x7FC00001u;
    cpu.s[34] = 0x7F800000u;
    cpu.s[35] = 0x80000000u;
    cpu.s[36] = cpu.s[37] = cpu.s[38] = cpu.s[39] = 0;
    cpu.fpscr = 0;
    cpu.r[MANGO_REG_PC] = 0;
    rc = mango_interp_run(&cpu, &mem, 0x3333u, 100);
    if (rc != 0 || cpu.s[36] != 0x7F800000u || cpu.s[37] != 0x7FC00000u ||
        cpu.s[38] != 0u || cpu.s[39] != 0xFF800000u || (cpu.fpscr & (1u << 1)) == 0) {
      fprintf(stderr,
              "FAIL(ctor_neon): vrecpe.specials rc=%d q9=%08x %08x %08x %08x fpscr=0x%x\n", rc,
              cpu.s[36], cpu.s[37], cpu.s[38], cpu.s[39], cpu.fpscr);
      return 1;
    }
  }

  {
    static const uint32_t kProg[] = {0xF2F104A4u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[34] = 0xA3A2A1A0u;
    cpu.s[35] = 0xB3B2B1B0u;
    cpu.s[40] = 0xC3C2C1C0u;
    cpu.s[41] = 0xD3D2D1D0u;
    cpu.r[MANGO_REG_LR] = 0x4444u;
    int rc = mango_interp_run(&cpu, &mem, 0x4444u, 100);
    if (rc != 0 || cpu.s[32] != 0xB3B2B1B0u || cpu.s[33] != 0xC3C2C1C0u) {
      fprintf(stderr, "FAIL(ctor_neon): vext.exec rc=%d d16=%08x %08x\n", rc, cpu.s[32],
              cpu.s[33]);
      return 1;
    }
  }

  printf("ok: OFDP ctor NEON VMOV.I32#0xffff / VMVN / VRECPE.F32 / VEXT\n");
  return 0;
}


static int test_neon_ctor_vrecps_vorr(void) {
  /* OFDP ctor uncovereds: VRECPS.F32 q10,q9,q8 and VORR d21,d16,d16. */
  MangoInsn insn;

  if (mango_decode(0xF2424FF0u, &insn) != 0 || insn.op != MANGO_OP_VRECPS || insn.b != 1 ||
      insn.rd != 20u || insn.rn != 18u || insn.rm != 16u) {
    fprintf(stderr, "FAIL(ctor_vrecps): decode op=%d rd=%u rn=%u rm=%u b=%d\n", (int)insn.op,
            insn.rd, insn.rn, insn.rm, insn.b);
    return 1;
  }
  if (mango_decode(0xF26051B0u, &insn) != 0 || insn.op != MANGO_OP_VORR || insn.b != 0 ||
      insn.rd != 21u || insn.rn != 16u || insn.rm != 16u) {
    fprintf(stderr, "FAIL(ctor_vorr): decode op=%d rd=%u rn=%u rm=%u b=%d\n", (int)insn.op,
            insn.rd, insn.rn, insn.rm, insn.b);
    return 1;
  }

  {
    /* VRECPS.F32: 2 - op1*op2 per lane; specials Inf×0 → 2.0, NaN → DefaultNaN. */
    static const uint32_t kProg[] = {0xF2424FF0u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    /* q9 = op1, q8 = op2 → q10 */
    cpu.s[36] = 0x3F800000u; /* 1.0 → 2-1*1 = 1.0 */
    cpu.s[37] = 0x40000000u; /* 2.0 → 2-2*0.5 = 1.0 */
    cpu.s[38] = 0x3EFF8000u; /* VRECPE(2) estimate */
    cpu.s[39] = 0x7FC00001u; /* NaN → DefaultNaN */
    cpu.s[32] = 0x3F800000u; /* 1.0 */
    cpu.s[33] = 0x3F000000u; /* 0.5 */
    cpu.s[34] = 0x40000000u; /* 2.0 → VRECPS(est,2)=0x3f804000 */
    cpu.s[35] = 0x3F800000u; /* ignored (NaN lane) */
    cpu.r[MANGO_REG_LR] = 0x5555u;
    int rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
    if (rc != 0 || cpu.s[40] != 0x3F800000u || cpu.s[41] != 0x3F800000u ||
        cpu.s[42] != 0x3F804000u || cpu.s[43] != 0x7FC00000u) {
      fprintf(stderr, "FAIL(ctor_vrecps): exec rc=%d q10=%08x %08x %08x %08x\n", rc, cpu.s[40],
              cpu.s[41], cpu.s[42], cpu.s[43]);
      return 1;
    }
    /* Inf × 0 → +2.0 */
    cpu.s[36] = 0x7F800000u;
    cpu.s[37] = 0x80000000u; /* -0 */
    cpu.s[38] = 0x00000001u; /* denorm → 0 under FZ */
    cpu.s[39] = 0xFF800000u;
    cpu.s[32] = 0u;
    cpu.s[33] = 0xFF800000u;
    cpu.s[34] = 0x7F800000u;
    cpu.s[35] = 0u;
    cpu.s[40] = cpu.s[41] = cpu.s[42] = cpu.s[43] = 0;
    cpu.r[MANGO_REG_PC] = 0;
    rc = mango_interp_run(&cpu, &mem, 0x5555u, 100);
    if (rc != 0 || cpu.s[40] != 0x40000000u || cpu.s[41] != 0x40000000u ||
        cpu.s[42] != 0x40000000u || cpu.s[43] != 0x40000000u) {
      fprintf(stderr, "FAIL(ctor_vrecps): specials rc=%d q10=%08x %08x %08x %08x\n", rc,
              cpu.s[40], cpu.s[41], cpu.s[42], cpu.s[43]);
      return 1;
    }
  }

  {
    /* VORR d21, d16, d16 — identity OR / VMOV alias. */
    static const uint32_t kProg[] = {0xF26051B0u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[32] = 0xA5A5A5A5u;
    cpu.s[33] = 0x5A5A5A5Au;
    cpu.s[42] = 0xDEADBEEFu;
    cpu.s[43] = 0xCAFEBABEu;
    cpu.r[MANGO_REG_LR] = 0x6666u;
    int rc = mango_interp_run(&cpu, &mem, 0x6666u, 100);
    if (rc != 0 || cpu.s[42] != 0xA5A5A5A5u || cpu.s[43] != 0x5A5A5A5Au) {
      fprintf(stderr, "FAIL(ctor_vorr): exec rc=%d d21=%08x %08x\n", rc, cpu.s[42], cpu.s[43]);
      return 1;
    }
  }

  {
    /* VORR d21, d16, d17 — non-identical bitwise OR (Vm=1 vs identity word). */
    static const uint32_t kProg[] = {0xF26051B1u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    if (mango_decode(0xF26051B1u, &insn) != 0 || insn.op != MANGO_OP_VORR || insn.rd != 21u ||
        insn.rn != 16u || insn.rm != 17u) {
      fprintf(stderr, "FAIL(ctor_vorr): or.decode op=%d rd=%u rn=%u rm=%u\n", (int)insn.op,
              insn.rd, insn.rn, insn.rm);
      return 1;
    }
    cpu.s[32] = 0x0F0F0F0Fu;
    cpu.s[33] = 0x00FF00FFu;
    cpu.s[34] = 0xF0F0F0F0u;
    cpu.s[35] = 0xFF00FF00u;
    cpu.r[MANGO_REG_LR] = 0x7777u;
    int rc = mango_interp_run(&cpu, &mem, 0x7777u, 100);
    if (rc != 0 || cpu.s[42] != 0xFFFFFFFFu || cpu.s[43] != 0xFFFFFFFFu) {
      fprintf(stderr, "FAIL(ctor_vorr): or.exec rc=%d d21=%08x %08x\n", rc, cpu.s[42],
              cpu.s[43]);
      return 1;
    }
  }

  printf("ok: OFDP ctor NEON VRECPS.F32 / VORR\n");
  return 0;
}


static int test_neon_ctor_vmul_vswp(void) {
  /* OFDP ctor uncovereds: VMUL.F32 q9,q9,q10 and VSWP d16,d17. */
  MangoInsn insn;

  if (mango_decode(0xF3422DF4u, &insn) != 0 || insn.op != MANGO_OP_VMUL || insn.u != 1 ||
      insn.b != 1 || insn.rd != 18u || insn.rn != 18u || insn.rm != 20u) {
    fprintf(stderr, "FAIL(ctor_vmul): decode op=%d u=%d rd=%u rn=%u rm=%u b=%d\n", (int)insn.op,
            insn.u, insn.rd, insn.rn, insn.rm, insn.b);
    return 1;
  }
  if (mango_decode(0xF3F20021u, &insn) != 0 || insn.op != MANGO_OP_VSWP || insn.b != 0 ||
      insn.rd != 16u || insn.rm != 17u) {
    fprintf(stderr, "FAIL(ctor_vswp): decode op=%d rd=%u rm=%u b=%d\n", (int)insn.op, insn.rd,
            insn.rm, insn.b);
    return 1;
  }

  {
    /* VMUL.F32 q9, q9, q10 — four f32 lanes. */
    static const uint32_t kProg[] = {0xF3422DF4u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    /* q9 = {2, 3, 0.5, -4}, q10 = {4, 5, 8, -0.5} → {8, 15, 4, 2} */
    cpu.s[36] = 0x40000000u; /* 2.0 */
    cpu.s[37] = 0x40400000u; /* 3.0 */
    cpu.s[38] = 0x3F000000u; /* 0.5 */
    cpu.s[39] = 0xC0800000u; /* -4.0 */
    cpu.s[40] = 0x40800000u; /* 4.0 */
    cpu.s[41] = 0x40A00000u; /* 5.0 */
    cpu.s[42] = 0x41000000u; /* 8.0 */
    cpu.s[43] = 0xBF000000u; /* -0.5 */
    cpu.r[MANGO_REG_LR] = 0x8888u;
    int rc = mango_interp_run(&cpu, &mem, 0x8888u, 100);
    if (rc != 0 || cpu.s[36] != 0x41000000u || cpu.s[37] != 0x41700000u ||
        cpu.s[38] != 0x40800000u || cpu.s[39] != 0x40000000u) {
      fprintf(stderr, "FAIL(ctor_vmul): exec rc=%d q9=%08x %08x %08x %08x\n", rc, cpu.s[36],
              cpu.s[37], cpu.s[38], cpu.s[39]);
      return 1;
    }
    /* q10 must be unchanged */
    if (cpu.s[40] != 0x40800000u || cpu.s[41] != 0x40A00000u || cpu.s[42] != 0x41000000u ||
        cpu.s[43] != 0xBF000000u) {
      fprintf(stderr, "FAIL(ctor_vmul): q10 clobbered\n");
      return 1;
    }
  }

  {
    /* VSWP d16, d17 */
    static const uint32_t kProg[] = {0xF3F20021u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[32] = 0x11111111u;
    cpu.s[33] = 0x22222222u;
    cpu.s[34] = 0xAAAABBBBu;
    cpu.s[35] = 0xCCCCDDDDu;
    cpu.r[MANGO_REG_LR] = 0x9999u;
    int rc = mango_interp_run(&cpu, &mem, 0x9999u, 100);
    if (rc != 0 || cpu.s[32] != 0xAAAABBBBu || cpu.s[33] != 0xCCCCDDDDu ||
        cpu.s[34] != 0x11111111u || cpu.s[35] != 0x22222222u) {
      fprintf(stderr, "FAIL(ctor_vswp): exec rc=%d d16=%08x %08x d17=%08x %08x\n", rc, cpu.s[32],
              cpu.s[33], cpu.s[34], cpu.s[35]);
      return 1;
    }
  }

  {
    /* VSWP q8, q9 — Q form (synthesize encoding with Q=1, even D indices).
     * Word: same as d16/d17 but Q=1 → d16↔d17 and d18↔d19 as q8↔q9.
     * Capstone ofdp word is D-form; also cover Q path. */
    uint32_t word = 0xF3F20061u; /* Q=1, d16, d17 → actually Q requires even: d16/d18? */
    /* Rebuild: D=1 Vd=0 → d16; M=1 Vm=2 → d18; Q=1 → vswp q8, q9 */
    word = 0xF3F20062u;
    if (mango_decode(word, &insn) != 0 || insn.op != MANGO_OP_VSWP || insn.b != 1 ||
        insn.rd != 16u || insn.rm != 18u) {
      fprintf(stderr, "FAIL(ctor_vswp): q.decode op=%d rd=%u rm=%u b=%d\n", (int)insn.op, insn.rd,
              insn.rm, insn.b);
      return 1;
    }
    static uint32_t kProg[2];
    kProg[0] = word;
    kProg[1] = 0xE12FFF1Eu;
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[32] = 0x1u;
    cpu.s[33] = 0x2u;
    cpu.s[34] = 0x3u;
    cpu.s[35] = 0x4u; /* q8 */
    cpu.s[36] = 0xAAu;
    cpu.s[37] = 0xBBu;
    cpu.s[38] = 0xCCu;
    cpu.s[39] = 0xDDu; /* q9 */
    cpu.r[MANGO_REG_LR] = 0xAAAAu;
    int rc = mango_interp_run(&cpu, &mem, 0xAAAAu, 100);
    if (rc != 0 || cpu.s[32] != 0xAAu || cpu.s[33] != 0xBBu || cpu.s[34] != 0xCCu ||
        cpu.s[35] != 0xDDu || cpu.s[36] != 0x1u || cpu.s[37] != 0x2u || cpu.s[38] != 0x3u ||
        cpu.s[39] != 0x4u) {
      fprintf(stderr, "FAIL(ctor_vswp): q.exec rc=%d\n", rc);
      return 1;
    }
  }

  printf("ok: OFDP ctor NEON VMUL.F32 / VSWP\n");
  return 0;
}



static int test_neon_ctor_vmov_scalar32(void) {
  /* OFDP ctor uncovered 0xee212b90: vmov.32 d17[1], r2 */
  MangoInsn insn;

  if (mango_decode(0xEE212B90u, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 7 ||
      insn.b != 0 || insn.rd != 17u || insn.rn != 2u || insn.imm != 1u) {
    fprintf(stderr, "FAIL(ctor_vmov32): decode op=%d u=%d rd=%u rn=%u imm=%u b=%d\n",
            (int)insn.op, insn.u, insn.rd, insn.rn, insn.imm, insn.b);
    return 1;
  }
  /* Reverse L=1 and lane 0 encodings (trivial same path). */
  if (mango_decode(0xEE312B90u, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 7 ||
      insn.b != 1 || insn.rd != 17u || insn.rn != 2u || insn.imm != 1u) {
    fprintf(stderr, "FAIL(ctor_vmov32): reverse.decode op=%d u=%d b=%d imm=%u\n", (int)insn.op,
            insn.u, insn.b, insn.imm);
    return 1;
  }
  if (mango_decode(0xEE012B90u, &insn) != 0 || insn.op != MANGO_OP_VMOV || insn.u != 7 ||
      insn.b != 0 || insn.imm != 0u) {
    fprintf(stderr, "FAIL(ctor_vmov32): lane0.decode op=%d u=%d imm=%u\n", (int)insn.op, insn.u,
            insn.imm);
    return 1;
  }

  {
    /* Insert into upper lane; lower must stay. */
    static const uint32_t kProg[] = {0xEE212B90u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[34] = 0xAAAABBBBu; /* d17 low */
    cpu.s[35] = 0xCCCCDDDDu; /* d17 high — overwritten */
    cpu.r[2] = 0x12345678u;
    cpu.r[MANGO_REG_LR] = 0xBBBBu;
    int rc = mango_interp_run(&cpu, &mem, 0xBBBBu, 100);
    if (rc != 0 || cpu.s[34] != 0xAAAABBBBu || cpu.s[35] != 0x12345678u) {
      fprintf(stderr, "FAIL(ctor_vmov32): insert rc=%d d17=%08x %08x\n", rc, cpu.s[34],
              cpu.s[35]);
      return 1;
    }
  }

  {
    /* Extract upper lane into r2; D unchanged. */
    static const uint32_t kProg[] = {0xEE312B90u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[34] = 0x11112222u;
    cpu.s[35] = 0x33334444u;
    cpu.r[2] = 0xDEADBEEFu;
    cpu.r[MANGO_REG_LR] = 0xCCCCu;
    int rc = mango_interp_run(&cpu, &mem, 0xCCCCu, 100);
    if (rc != 0 || cpu.r[2] != 0x33334444u || cpu.s[34] != 0x11112222u ||
        cpu.s[35] != 0x33334444u) {
      fprintf(stderr, "FAIL(ctor_vmov32): extract rc=%d r2=%08x d17=%08x %08x\n", rc, cpu.r[2],
              cpu.s[34], cpu.s[35]);
      return 1;
    }
  }

  {
    /* Insert into lower lane; upper must stay. */
    static const uint32_t kProg[] = {0xEE012B90u, 0xE12FFF1Eu};
    uint8_t mem_buf[64];
    load_words(mem_buf, sizeof(mem_buf), kProg, 2);
    MangoMemory mem = {mem_buf, sizeof(mem_buf)};
    MangoCpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.s[34] = 0xAAAABBBBu;
    cpu.s[35] = 0xCCCCDDDDu;
    cpu.r[2] = 0x55AA55AAu;
    cpu.r[MANGO_REG_LR] = 0xDDDDu;
    int rc = mango_interp_run(&cpu, &mem, 0xDDDDu, 100);
    if (rc != 0 || cpu.s[34] != 0x55AA55AAu || cpu.s[35] != 0xCCCCDDDDu) {
      fprintf(stderr, "FAIL(ctor_vmov32): lane0.insert rc=%d d17=%08x %08x\n", rc, cpu.s[34],
              cpu.s[35]);
      return 1;
    }
  }

  printf("ok: OFDP ctor VMOV.32 Dd[x], Rt / Rt, Dd[x]\n");
  return 0;
}

int main(void) {
  int failures = 0;
  failures += test_mov_add_bx();
  failures += test_branch_and_flags();
  failures += test_load_store_roundtrip();
  failures += test_pc_relative_add();
  failures += test_load_out_of_bounds_rejected();
  failures += test_load_unaligned_ok();
  failures += test_load_store_byte_roundtrip();
  failures += test_conditional_branch_taken();
  failures += test_signed_vs_unsigned_condition_flags();
  failures += test_shifted_operand2();
  failures += test_adds_signed_overflow_without_carry();
  failures += test_subs_borrow_no_overflow();
  failures += test_bl_call_and_return();
  failures += test_full_alu_opcodes();
  failures += test_adc_carry_chain();
  failures += test_sbc_rsc();
  failures += test_mul();
  failures += test_tst_teq_cmn_dont_write_rd();
  failures += test_s0_compares_rejected();
  failures += test_svc_stops_and_can_resume();
  failures += test_push_pop_roundtrip();
  failures += test_stmdb_sp_in_list();
  failures += test_stmia_ldmia_no_writeback();
  failures += test_stmib_and_writeback();
  failures += test_ldm_stm_rejected_shapes();
  failures += test_stm_out_of_bounds_rejected();
  failures += test_mla();
  failures += test_mull_long();
  failures += test_smlaxy_smulxy();
  failures += test_ldr_str_writeback_and_postindex();
  failures += test_ldr_register_offset();
  failures += test_register_specified_shift();
  failures += test_shifter_carry_and_rrx();
  failures += test_ldst_rejected_shapes();
  failures += test_ldrh_strh_roundtrip();
  failures += test_ldrsb_ldrsh_sign_extend();
  failures += test_extra_ldst_writeback_and_reg_offset();
  failures += test_ldrh_unaligned_ok();
  failures += test_extra_ldst_rejected_shapes();
  failures += test_swp_and_swpb();
  failures += test_ldrd_strd_roundtrip();
  failures += test_thumb_ldmia_wb();
  failures += test_thumb_mov_add_bx();
  failures += test_thumb_push_pop();
  failures += test_arm_bx_into_thumb();
  failures += test_thumb_bl();
  failures += test_thumb_movw_movt();
  failures += test_thumb_it_eq_taken();
  failures += test_thumb_it_eq_skipped();
  failures += test_thumb_b_w();
  failures += test_arm_movw_movt();
  failures += test_arm_blx_reg();
  failures += test_thumb_blx_reg();
  failures += test_thumb32_ldr_str_imm();
  failures += test_thumb32_addw();
  failures += test_t32_push_w_stmdb_sp();
  failures += test_t32_mul_ra15();
  failures += test_t32_pop_w_ldmia_sp();
  failures += test_t32_pop_w_ldmia_sp_sib();
  failures += test_t32_pop_w_ldmia_sp_reject();
  failures += test_t32_mov_w_modimm_0();
  failures += test_t32_mov_w_modimm_25();
  failures += test_t32_sub_w_modimm_sp();
  failures += test_t32_mov_w_modimm_reject_s1();
  failures += test_t32_add_w_modimm_sp();
  failures += test_t32_strd_imm_offset();
  failures += test_t32_add_strd_reject();
  failures += test_t32_mvn_w_modimm();
  failures += test_t32_ldr_w_reg();
  failures += test_t32_mvn_ldr_reject();
  failures += test_t32_ldrsh_w_imm12();
  failures += test_t32_str_w_imm8_neg();
  failures += test_t32_ldrsh_str_reject();
  failures += test_t16_cbz_b1ff();
  failures += test_t16_cbz_b369();
  failures += test_t16_cbnz_b978();
  failures += test_t32_str_w_reg_lsl2();
  failures += test_t32_pop_w_pc_thumb();
  failures += test_t32_pop_w_pc_arm();
  failures += test_t32_str_pop_pc_reject();
  failures += test_t32_stmia_w0();
  failures += test_t32_ubfx();
  failures += test_t32_stmia_ubfx_reject();
  failures += test_t32_cmp_w_modimm_80000000();
  failures += test_t32_cmp_w_modimm_1a();
  failures += test_t32_cmp_w_modimm_reject();
  failures += test_t16_uxth_b2b6();
  failures += test_t16_uxtb_b2f6();
  failures += test_t32_add_w_reg_lsl2();
  failures += test_t32_add_w_reg_reject();
  failures += test_t32_ldrb_w_reg_lsl3();
  failures += test_t32_ldrb_w_reg_imm2_0();
  failures += test_t32_ldr_w_imm8_post();
  failures += test_t32_ldr_w_imm8_post_sp();
  failures += test_t32_ldrb_ldr_post_reject();
  failures += test_thumb_bx_pc_veneer();
  failures += test_arm_add_pc();
  failures += test_vldr_s_from_stack();
  failures += test_vmov_i32_and_clz();
  failures += test_vld1_vst1();
  failures += test_ldrex_strex();
  failures += test_vdup_q9();
  failures += test_vaddi_i32();
  failures += test_vpush_vpop();
  failures += test_bfc_ubfx();
  failures += test_rbit_uxt_rev();
  failures += test_vmov_s_gpr();
  failures += test_vcvt_f32_f64();
  failures += test_vcmpe_f32_zero();
  failures += test_vadd_f32();
  failures += test_vcvt_f32_u32();
  failures += test_vcvt_s32_f32();
  failures += test_vcvt_s32_f64();
  failures += test_vmov_f32_imm_and_smmul();
  failures += test_vmov_f32_ss();
  failures += test_vmov_f32_scalar_imm();
  failures += test_ofdp_native_render_vmul_vcmp();
  failures += test_vcmp_fpscr_nzcv();
  failures += test_neon_ctor_vmov_vmvn_vrecpe_vext();
  failures += test_neon_ctor_vrecps_vorr();
  failures += test_neon_ctor_vmul_vswp();
  failures += test_neon_ctor_vmov_scalar32();
  failures += test_strexd();

  if (failures != 0) {
    fprintf(stderr, "%d test(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
