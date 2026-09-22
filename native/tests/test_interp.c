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
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): writeback with rn in list was decoded\n");
    return 1;
  }
  uint32_t pc_base = encode_ldm_stm(0, 1, 0, 0, MANGO_REG_PC, 1u);
  if (mango_decode(pc_base, &insn) == 0) {
    fprintf(stderr, "FAIL(ldm_stm_rejected_shapes): pc as base was decoded\n");
    return 1;
  }
  printf("ok: empty/S-bit/stm-pc/wb-rn-in-list/pc-base ldm/stm shapes rejected\n");
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
  failures += test_stmia_ldmia_no_writeback();
  failures += test_stmib_and_writeback();
  failures += test_ldm_stm_rejected_shapes();
  failures += test_stm_out_of_bounds_rejected();
  failures += test_mla();
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
  failures += test_vcvt_s32_f32();

  if (failures != 0) {
    fprintf(stderr, "%d test(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
