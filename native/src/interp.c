#include "mango/interp.h"

#include "mango/decoder.h"

#include <math.h>
#include <string.h>

/* Little-endian (real ARM32 Android/Linux), explicit shifts to stay strict-aliasing-safe. */
static uint32_t mango_load_u32_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mango_store_u32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int mango_check_range(const MangoMemory* mem, uint32_t addr, uint32_t n) {
  if ((uint64_t)addr + n > mem->size) { /* uint64_t so addr near UINT32_MAX can't wrap */
    return -1;
  }
  return 0;
}

/* Aligned word: instruction fetch, LDM/STM, SWP. LDR/STR data uses mango_check_range. */
static int mango_check_word_access(const MangoMemory* mem, uint32_t addr) {
  if ((addr % 4u) != 0) {
    return -1;
  }
  return mango_check_range(mem, addr, 4u);
}

static int mango_check_byte_access(const MangoMemory* mem, uint32_t addr) {
  return addr < mem->size ? 0 : -1;
}

static uint32_t mango_load_u16_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void mango_store_u16_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static int mango_check_half_access(const MangoMemory* mem, uint32_t addr) {
  if ((addr % 2u) != 0) {
    return -1;
  }
  return mango_check_range(mem, addr, 2u);
}

static uint32_t mango_sign_extend8(uint32_t b) { return (b & 0x80u) ? (b | 0xFFFFFF00u) : b; }

static uint32_t mango_sign_extend16(uint32_t h) { return (h & 0x8000u) ? (h | 0xFFFF0000u) : h; }

/* Real hardware: reading PC as an operand gives addr+8 in A32, addr+4 in T16. */
static uint32_t mango_read_reg(const MangoCpu* cpu, uint32_t insn_addr, uint32_t reg) {
  if (reg == MANGO_REG_PC) {
    return insn_addr + ((cpu->cpsr & MANGO_CPSR_T) ? 4u : 8u);
  }
  return cpu->r[reg];
}

static void mango_branch_to(MangoCpu* cpu, uint32_t dest, uint32_t stop_addr, uint32_t* next_addr) {
  if (dest == stop_addr && (dest & 1u)) {
    *next_addr = dest;
  } else if (dest & 1u) {
    cpu->cpsr |= MANGO_CPSR_T;
    *next_addr = dest & ~1u;
  } else {
    cpu->cpsr &= ~MANGO_CPSR_T;
    *next_addr = dest;
  }
}

static void mango_write_rd(MangoCpu* cpu, uint32_t rd, uint32_t value, uint32_t stop_addr,
                           uint32_t* next_addr) {
  if (rd != MANGO_REG_PC) {
    cpu->r[rd] = value;
    return;
  }
  mango_branch_to(cpu, value, stop_addr, next_addr);
}

static uint64_t mango_vfp_get_d(const MangoCpu* cpu, uint32_t d) {
  d &= 31u;
  uint32_t lo = cpu->s[d * 2u];
  uint32_t hi = cpu->s[d * 2u + 1u];
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

static void mango_vfp_set_d(MangoCpu* cpu, uint32_t d, uint64_t v) {
  d &= 31u;
  cpu->s[d * 2u] = (uint32_t)v;
  cpu->s[d * 2u + 1u] = (uint32_t)(v >> 32);
}

static double mango_u64_to_f64(uint64_t v) {
  double d;
  memcpy(&d, &v, 8);
  return d;
}

static uint64_t mango_f64_to_u64(double d) {
  uint64_t v;
  memcpy(&v, &d, 8);
  return v;
}

/* T16 ADR and LDR-literal: (PC + 4) AND NOT 3. High-register ADD Rd, PC does not. */
static uint32_t mango_thumb_align_pc(const MangoCpu* cpu, const MangoInsn* insn, uint32_t value) {
  if ((cpu->cpsr & MANGO_CPSR_T) && insn->rn == MANGO_REG_PC && insn->is_imm) {
    return value & ~3u;
  }
  return value;
}

typedef struct MangoOp2 {
  uint32_t value;
  uint32_t carry; /* 0 or 1, only meaningful if update_c */
  int update_c;   /* 0 = logical S-bit ops leave C alone (LSL #0, Rs=0, rot=0) */
} MangoOp2;

/* ARM barrel shifter, including the #0 encodings (LSR/ASR #32, RRX) and
 * register-controlled amounts. ASR is hand-rolled: signed right-shift is
 * implementation-defined in C. amount is the 5-bit field (immediate) or
 * Rs[7:0] (register). */
static MangoOp2 mango_shift(uint32_t value, uint32_t shift_type, uint32_t amount, int by_reg,
                            uint32_t carry_in) {
  MangoOp2 o;
  o.value = value;
  o.carry = 0;
  o.update_c = 1;

  if (!by_reg) {
    if (shift_type == 0) { /* LSL */
      if (amount == 0) {
        o.update_c = 0;
        return o;
      }
      o.value = value << amount;
      o.carry = (value >> (32u - amount)) & 1u;
      return o;
    }
    if (shift_type == 1) { /* LSR; #0 means #32 */
      uint32_t n = amount == 0 ? 32u : amount;
      o.value = n == 32u ? 0u : value >> n;
      o.carry = (value >> (n - 1u)) & 1u;
      return o;
    }
    if (shift_type == 2) { /* ASR; #0 means #32 */
      uint32_t n = amount == 0 ? 32u : amount;
      if (n == 32u) {
        o.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        o.carry = (value >> 31) & 1u;
      } else {
        uint32_t sign_fill = (value & 0x80000000u) ? (~0u << (32u - n)) : 0u;
        o.value = (value >> n) | sign_fill;
        o.carry = (value >> (n - 1u)) & 1u;
      }
      return o;
    }
    /* ROR; #0 means RRX */
    if (amount == 0) {
      o.value = (carry_in << 31) | (value >> 1);
      o.carry = value & 1u;
      return o;
    }
    o.value = (value >> amount) | (value << (32u - amount));
    o.carry = (value >> (amount - 1u)) & 1u;
    return o;
  }

  if (amount == 0) {
    o.update_c = 0;
    return o;
  }
  if (shift_type == 0) { /* LSL Rs */
    if (amount < 32u) {
      o.value = value << amount;
      o.carry = (value >> (32u - amount)) & 1u;
    } else if (amount == 32u) {
      o.value = 0;
      o.carry = value & 1u;
    } else {
      o.value = 0;
      o.carry = 0;
    }
    return o;
  }
  if (shift_type == 1) { /* LSR Rs */
    if (amount < 32u) {
      o.value = value >> amount;
      o.carry = (value >> (amount - 1u)) & 1u;
    } else if (amount == 32u) {
      o.value = 0;
      o.carry = (value >> 31) & 1u;
    } else {
      o.value = 0;
      o.carry = 0;
    }
    return o;
  }
  if (shift_type == 2) { /* ASR Rs */
    if (amount < 32u) {
      uint32_t sign_fill = (value & 0x80000000u) ? (~0u << (32u - amount)) : 0u;
      o.value = (value >> amount) | sign_fill;
      o.carry = (value >> (amount - 1u)) & 1u;
    } else {
      o.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
      o.carry = (value >> 31) & 1u;
    }
    return o;
  }
  /* ROR Rs: Rs[4:0]==0 and Rs[7:0]!=0 is rotate-by-32 (identity, C=Rm[31]) */
  {
    uint32_t low5 = amount & 31u;
    if (low5 == 0) {
      o.value = value;
      o.carry = (value >> 31) & 1u;
      return o;
    }
    o.value = (value >> low5) | (value << (32u - low5));
    o.carry = (value >> (low5 - 1u)) & 1u;
    return o;
  }
}

static MangoOp2 mango_eval_operand2(const MangoCpu* cpu, uint32_t addr, const MangoInsn* insn) {
  if (insn->is_imm) {
    MangoOp2 o;
    o.value = insn->imm;
    if (insn->shift_amount != 0) {
      o.update_c = 1;
      o.carry = insn->imm >> 31;
    } else {
      o.update_c = 0;
      o.carry = 0;
    }
    return o;
  }
  uint32_t value = mango_read_reg(cpu, addr, insn->rm);
  uint32_t amount =
      insn->shift_by_reg ? (mango_read_reg(cpu, addr, insn->rs) & 0xFFu) : insn->shift_amount;
  uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
  return mango_shift(value, insn->shift_type, amount, insn->shift_by_reg, carry_in);
}

/* NZCV for ADD/ADDS: result = lhs + rhs. */
static uint32_t mango_flags_for_add(uint32_t lhs, uint32_t rhs, uint32_t result) {
  uint32_t flags = 0;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (result < lhs) ? MANGO_CPSR_C : 0;
  flags |= ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZCV for SUB/SUBS/CMP: result = lhs - rhs. */
static uint32_t mango_flags_for_sub(uint32_t lhs, uint32_t rhs, uint32_t result) {
  uint32_t flags = 0;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (lhs >= rhs) ? MANGO_CPSR_C : 0;
  flags |= ((lhs ^ rhs) & (lhs ^ result) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZCV for ADC (result = lhs + rhs + carry_in); SBC/RSC reuse this too,
 * feeding it ~rhs (see their cases below), the same trick real ALU
 * hardware uses since A-B-1+C == A+~B+C in two's complement. */
static uint32_t mango_flags_for_adc(uint32_t lhs, uint32_t rhs, uint32_t carry_in,
                                    uint32_t result) {
  uint32_t flags = 0;
  uint64_t wide = (uint64_t)lhs + rhs + carry_in;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (wide > 0xFFFFFFFFu) ? MANGO_CPSR_C : 0;
  flags |= ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZ for AND/EOR/ORR/BIC/MVN/MOV/TST/TEQ, and MULS/MLAS. V is unaffected.
 * C comes from the shifter when update_c; otherwise C is left as-is
 * (LSL #0, Rs=0, unrotated immediate, MUL/MLA). */
static uint32_t mango_flags_for_logical(uint32_t old_cpsr, uint32_t result, uint32_t shifter_c,
                                        int update_c) {
  uint32_t n = (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  uint32_t z = (result == 0) ? MANGO_CPSR_Z : 0;
  uint32_t c = update_c ? (shifter_c ? MANGO_CPSR_C : 0) : (old_cpsr & MANGO_CPSR_C);
  return (old_cpsr & MANGO_CPSR_V) | n | z | c;
}

static void mango_set_nzcv(MangoCpu* cpu, uint32_t flags) {
  cpu->cpsr &= ~(MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V);
  cpu->cpsr |= flags;
}

/* ITSTATE is CPSR[15:10,26:25] packed as an 8-bit firstcond:mask value. */
static uint32_t mango_get_itstate(uint32_t cpsr) {
  return (((cpsr >> 10) & 0x3Fu) << 2) | ((cpsr >> 25) & 3u);
}

static void mango_set_itstate(MangoCpu* cpu, uint32_t it) {
  cpu->cpsr &= ~((0x3Fu << 10) | (3u << 25));
  cpu->cpsr |= ((it >> 2) & 0x3Fu) << 10;
  cpu->cpsr |= (it & 3u) << 25;
}

static void mango_advance_itstate(MangoCpu* cpu) {
  uint32_t it = mango_get_itstate(cpu->cpsr);
  if (it == 0) {
    return;
  }
  if ((it & 7u) == 0) {
    mango_set_itstate(cpu, 0);
  } else {
    mango_set_itstate(cpu, (it & 0xE0u) | ((it & 0x1Fu) << 1));
  }
}

static int mango_cond_holds(uint32_t cond, uint32_t cpsr) {
  int n = (cpsr & MANGO_CPSR_N) != 0;
  int z = (cpsr & MANGO_CPSR_Z) != 0;
  int c = (cpsr & MANGO_CPSR_C) != 0;
  int v = (cpsr & MANGO_CPSR_V) != 0;

  switch (cond) {
    case 0x0:
      return z; /* EQ */
    case 0x1:
      return !z; /* NE */
    case 0x2:
      return c; /* CS/HS */
    case 0x3:
      return !c; /* CC/LO */
    case 0x4:
      return n; /* MI */
    case 0x5:
      return !n; /* PL */
    case 0x6:
      return v; /* VS */
    case 0x7:
      return !v; /* VC */
    case 0x8:
      return c && !z; /* HI */
    case 0x9:
      return !c || z; /* LS */
    case 0xA:
      return n == v; /* GE */
    case 0xB:
      return n != v; /* LT */
    case 0xC:
      return !z && (n == v); /* GT */
    case 0xD:
      return z || (n != v); /* LE */
    case 0xE:
    case 0xF:
      return 1; /* AL, and ARMv5+ unconditional (NEON/PLD/BLX imm) */
    default:
      return 0;
  }
}

int mango_interp_run(MangoCpu* cpu, MangoMemory* mem, uint32_t stop_addr, uint32_t max_steps) {
  uint32_t addr = cpu->r[MANGO_REG_PC];

  for (uint32_t step = 0; step < max_steps; step++) {
    if (addr == stop_addr) {
      return 0;
    }

    int thumb = (cpu->cpsr & MANGO_CPSR_T) != 0;
    MangoInsn insn;
    uint32_t next_addr;

    if (thumb) {
      if ((addr % 2u) != 0 || mango_check_half_access(mem, addr) != 0) {
        return -1;
      }
      uint16_t hw = (uint16_t)mango_load_u16_le(mem->bytes + addr);
      if ((hw >> 11) >= 0x1Du) {
        if (mango_check_half_access(mem, addr + 2u) != 0) {
          return -1;
        }
        uint16_t hw2 = (uint16_t)mango_load_u16_le(mem->bytes + addr + 2u);
        if (mango_decode_t32(hw, hw2, &insn) != 0) {
          return -1;
        }
        next_addr = addr + 4u;
      } else if (mango_decode_t16(hw, &insn) != 0) {
        return -1;
      } else {
        next_addr = addr + 2u;
      }
    } else {
      if (mango_check_word_access(mem, addr) != 0) {
        return -1;
      }
      if (mango_decode(mango_load_u32_le(mem->bytes + addr), &insn) != 0) {
        return -1;
      }
      next_addr = addr + 4u;
    }

    uint32_t it = mango_get_itstate(cpu->cpsr);
    uint32_t cond = insn.cond;
    if (it != 0 && insn.op != MANGO_OP_IT) {
      cond = (it >> 4) & 0xFu;
    }

    /* condition false = no-op, covers B/BX too, no per-case handling needed */
    if (mango_cond_holds(cond, cpu->cpsr)) {
      switch (insn.op) {
        case MANGO_OP_MOV: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          mango_write_rd(cpu, insn.rd, op2.value, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, op2.value, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_MVN: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t result = ~op2.value;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_AND:
        case MANGO_OP_EOR:
        case MANGO_OP_ORR:
        case MANGO_OP_BIC: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = insn.op == MANGO_OP_AND   ? (lhs & op2.value)
                            : insn.op == MANGO_OP_EOR ? (lhs ^ op2.value)
                            : insn.op == MANGO_OP_ORR ? (lhs | op2.value)
                                                      : (lhs & ~op2.value);
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_TST:
        case MANGO_OP_TEQ: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = insn.op == MANGO_OP_TST ? (lhs & op2.value) : (lhs ^ op2.value);
          mango_set_nzcv(cpu, mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          break;
        }

        case MANGO_OP_ADD: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_thumb_align_pc(cpu, &insn, mango_read_reg(cpu, addr, insn.rn));
          uint32_t result = lhs + rhs;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_add(lhs, rhs, result));
          }
          break;
        }

        case MANGO_OP_CMN: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          mango_set_nzcv(cpu, mango_flags_for_add(lhs, rhs, lhs + rhs));
          break;
        }

        case MANGO_OP_ADC: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = lhs + rhs + carry_in;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(lhs, rhs, carry_in, result));
          }
          break;
        }

        case MANGO_OP_SUB: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = lhs - rhs;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_sub(lhs, rhs, result));
          }
          break;
        }

        case MANGO_OP_RSB: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = rhs - lhs;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_sub(rhs, lhs, result));
          }
          break;
        }

        case MANGO_OP_SBC: {
          uint32_t rhs = ~mango_eval_operand2(cpu, addr, &insn).value; /* A-B-1+C == A+~B+C */
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = lhs + rhs + carry_in;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(lhs, rhs, carry_in, result));
          }
          break;
        }

        case MANGO_OP_RSC: {
          uint32_t op2 = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t not_rn = ~mango_read_reg(cpu, addr, insn.rn); /* B-A-1+C == B+~A+C */
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = op2 + not_rn + carry_in;
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(op2, not_rn, carry_in, result));
          }
          break;
        }

        case MANGO_OP_CMP: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          mango_set_nzcv(cpu, mango_flags_for_sub(lhs, rhs, lhs - rhs));
          break;
        }

        case MANGO_OP_MUL:
        case MANGO_OP_MLA: {
          uint32_t result = mango_read_reg(cpu, addr, insn.rm) * mango_read_reg(cpu, addr, insn.rs);
          if (insn.op == MANGO_OP_MLA) {
            result += mango_read_reg(cpu, addr, insn.rn);
          }
          mango_write_rd(cpu, insn.rd, result, stop_addr, &next_addr);
          if (insn.sets_flags) {
            /* MULS/MLAS: C,V left as-is. */
            mango_set_nzcv(cpu, mango_flags_for_logical(cpu->cpsr, result, 0, 0));
          }
          break;
        }

        case MANGO_OP_SVC:
          return 1; /* cpu->r[PC] == addr still, caller thunks r7/r0-r6 and resumes, see interp.h */

        case MANGO_OP_B: {
          uint32_t pc_off = (cpu->cpsr & MANGO_CPSR_T) ? 4u : 8u;
          next_addr = addr + pc_off + insn.imm;
          break;
        }

        case MANGO_OP_BL:
          if (cpu->cpsr & MANGO_CPSR_T) {
            cpu->r[MANGO_REG_LR] = (addr + 4u) | 1u;
            next_addr = addr + 4u + insn.imm;
          } else {
            cpu->r[MANGO_REG_LR] = addr + 4u;
            next_addr = addr + 8u + insn.imm;
          }
          break;

        case MANGO_OP_BLX:
          if (insn.is_imm) {
            if (cpu->cpsr & MANGO_CPSR_T) {
              /* T32 BLX immediate: always switches to ARM. */
              cpu->r[MANGO_REG_LR] = (addr + 4u) | 1u;
              cpu->cpsr &= ~MANGO_CPSR_T;
              next_addr = ((addr + 4u) & ~3u) + insn.imm;
            } else {
              /* A32 BLX imm: switch to Thumb. */
              cpu->r[MANGO_REG_LR] = addr + 4u;
              cpu->cpsr |= MANGO_CPSR_T;
              next_addr = (addr + 8u + insn.imm) & ~1u;
            }
          } else {
            /* A32 or T16 BLX Rm: write LR then interwork like BX. */
            if (cpu->cpsr & MANGO_CPSR_T) {
              cpu->r[MANGO_REG_LR] = (addr + 2u) | 1u;
            } else {
              cpu->r[MANGO_REG_LR] = addr + 4u;
            }
            mango_branch_to(cpu, mango_read_reg(cpu, addr, insn.rm), stop_addr, &next_addr);
          }
          break;

        case MANGO_OP_MOVT:
          cpu->r[insn.rd] = (cpu->r[insn.rd] & 0xFFFFu) | (insn.imm << 16);
          break;

        case MANGO_OP_IT:
          mango_set_itstate(cpu, insn.imm);
          break;

        case MANGO_OP_BX:
          mango_branch_to(cpu, mango_read_reg(cpu, addr, insn.rm), stop_addr, &next_addr);
          break;

        case MANGO_OP_LDR:
        case MANGO_OP_STR: {
          if (insn.op == MANGO_OP_STR && insn.rd == MANGO_REG_PC) {
            return -1;
          }
          uint32_t offset = insn.is_imm ? insn.imm : mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t base = mango_thumb_align_pc(cpu, &insn, mango_read_reg(cpu, addr, insn.rn));
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;

          if (insn.b) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_LDR) {
              mango_write_rd(cpu, insn.rd, mem->bytes[eaddr], stop_addr, &next_addr);
            } else {
              mem->bytes[eaddr] = (uint8_t)(cpu->r[insn.rd] & 0xFFu);
            }
          } else {
            if (mango_check_range(mem, eaddr, 4u) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_LDR) {
              mango_write_rd(cpu, insn.rd, mango_load_u32_le(mem->bytes + eaddr), stop_addr,
                             &next_addr);
            } else {
              mango_store_u32_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            }
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_LDRH:
        case MANGO_OP_STRH:
        case MANGO_OP_LDRSB:
        case MANGO_OP_LDRSH: {
          /* Extra load/store: Rm is never shifted, unlike regular LDR/STR. */
          uint32_t offset = insn.is_imm ? insn.imm : mango_read_reg(cpu, addr, insn.rm);
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;

          if (insn.op == MANGO_OP_LDRSB) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            cpu->r[insn.rd] = mango_sign_extend8(mem->bytes[eaddr]);
          } else {
            if (mango_check_range(mem, eaddr, 2u) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_STRH) {
              mango_store_u16_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            } else if (insn.op == MANGO_OP_LDRH) {
              cpu->r[insn.rd] = mango_load_u16_le(mem->bytes + eaddr);
            } else {
              cpu->r[insn.rd] = mango_sign_extend16(mango_load_u16_le(mem->bytes + eaddr));
            }
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_LDRD:
        case MANGO_OP_STRD: {
          uint32_t offset = insn.is_imm ? insn.imm : mango_read_reg(cpu, addr, insn.rm);
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;
          if (mango_check_range(mem, eaddr, 8u) != 0) {
            return -1;
          }
          if (insn.op == MANGO_OP_STRD) {
            mango_store_u32_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            mango_store_u32_le(mem->bytes + eaddr + 4u, cpu->r[insn.rd + 1u]);
          } else {
            cpu->r[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
            cpu->r[insn.rd + 1u] = mango_load_u32_le(mem->bytes + eaddr + 4u);
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_SWP: {
          uint32_t eaddr = mango_read_reg(cpu, addr, insn.rn);
          uint32_t store_val = mango_read_reg(cpu, addr, insn.rm);
          if (insn.b) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            uint32_t loaded = mem->bytes[eaddr];
            mem->bytes[eaddr] = (uint8_t)(store_val & 0xFFu);
            cpu->r[insn.rd] = loaded;
          } else {
            if (mango_check_word_access(mem, eaddr) != 0) {
              return -1;
            }
            uint32_t loaded = mango_load_u32_le(mem->bytes + eaddr);
            mango_store_u32_le(mem->bytes + eaddr, store_val);
            cpu->r[insn.rd] = loaded;
          }
          break;
        }

        case MANGO_OP_LDM:
        case MANGO_OP_STM: {
          /* Lowest-numbered register always lands at the lowest address,
           * regardless of IA/IB/DA/DB. P/U only choose the start address
           * and whether Rn moves; see ARM ARM LDM/STM addressing modes.
           * PUSH = STMDB sp!, POP = LDMIA sp!. */
          uint32_t count = 0;
          for (uint32_t i = 0; i < 16; i++) {
            if (insn.reglist & (1u << i)) {
              count++;
            }
          }
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t start = insn.u ? (insn.p ? base + 4u : base)
                                  : (insn.p ? base - 4u * count : base - 4u * count + 4u);
          if ((start % 4u) != 0) {
            return -1;
          }
          if ((uint64_t)start + (uint64_t)count * 4u > mem->size) {
            return -1;
          }

          uint32_t eaddr = start;
          int loaded_pc = 0;
          uint32_t new_pc = 0;
          for (uint32_t i = 0; i < 16; i++) {
            if ((insn.reglist & (1u << i)) == 0) {
              continue;
            }
            if (insn.op == MANGO_OP_STM) {
              mango_store_u32_le(mem->bytes + eaddr, mango_read_reg(cpu, addr, i));
            } else if (i == MANGO_REG_PC) {
              uint32_t value = mango_load_u32_le(mem->bytes + eaddr);
              if (value == stop_addr && (value & 1u)) {
                /* odd sentinel: keep the exact value so the run stops */
              } else if (value & 1u) {
                cpu->cpsr |= MANGO_CPSR_T;
                value &= ~1u;
              } else if (value & 2u) {
                return -1; /* unaligned ARM PC */
              } else {
                cpu->cpsr &= ~MANGO_CPSR_T;
              }
              loaded_pc = 1;
              new_pc = value;
            } else {
              cpu->r[i] = mango_load_u32_le(mem->bytes + eaddr);
            }
            eaddr += 4u;
          }
          if (insn.w) {
            cpu->r[insn.rn] = insn.u ? base + 4u * count : base - 4u * count;
          }
          if (loaded_pc) {
            next_addr = new_pc;
          }
          break;
        }

        case MANGO_OP_VLDM:
        case MANGO_OP_VSTM: {
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t n = insn.b ? insn.imm * 8u : insn.imm * 4u;
          uint32_t start = insn.u ? base : base - n;
          if (mango_check_range(mem, start, n) != 0) {
            return -1;
          }
          for (uint32_t i = 0; i < insn.imm; i++) {
            if (insn.b) {
              uint32_t eaddr = start + i * 8u;
              if (insn.op == MANGO_OP_VLDM) {
                uint64_t v = (uint64_t)mango_load_u32_le(mem->bytes + eaddr) |
                             ((uint64_t)mango_load_u32_le(mem->bytes + eaddr + 4u) << 32);
                mango_vfp_set_d(cpu, insn.rd + i, v);
              } else {
                uint64_t v = mango_vfp_get_d(cpu, insn.rd + i);
                mango_store_u32_le(mem->bytes + eaddr, (uint32_t)v);
                mango_store_u32_le(mem->bytes + eaddr + 4u, (uint32_t)(v >> 32));
              }
            } else if (insn.rd + i < 32u) {
              uint32_t eaddr = start + i * 4u;
              if (insn.op == MANGO_OP_VLDM) {
                cpu->s[insn.rd + i] = mango_load_u32_le(mem->bytes + eaddr);
              } else {
                mango_store_u32_le(mem->bytes + eaddr, cpu->s[insn.rd + i]);
              }
            }
          }
          if (insn.w) {
            cpu->r[insn.rn] = insn.u ? base + n : base - n;
          }
          break;
        }

        case MANGO_OP_VLDR:
        case MANGO_OP_VSTR: {
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t eaddr = insn.u ? base + insn.imm : base - insn.imm;
          uint32_t n = insn.b ? 8u : 4u;
          if ((uint64_t)eaddr + n > mem->size) {
            return -1;
          }
          if (insn.op == MANGO_OP_VLDR) {
            if (insn.b) {
              uint64_t v = (uint64_t)mango_load_u32_le(mem->bytes + eaddr) |
                           ((uint64_t)mango_load_u32_le(mem->bytes + eaddr + 4u) << 32);
              mango_vfp_set_d(cpu, insn.rd, v);
            } else if (insn.rd < 32u) {
              cpu->s[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
            }
          } else if (insn.b) {
            uint64_t v = mango_vfp_get_d(cpu, insn.rd);
            mango_store_u32_le(mem->bytes + eaddr, (uint32_t)v);
            mango_store_u32_le(mem->bytes + eaddr + 4u, (uint32_t)(v >> 32));
          } else if (insn.rd < 32u) {
            mango_store_u32_le(mem->bytes + eaddr, cpu->s[insn.rd]);
          }
          break;
        }

        case MANGO_OP_VCVT: {
          if (insn.imm == 1) {
            double d = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
            float f = (float)d;
            uint32_t u;
            memcpy(&u, &f, 4);
            if (insn.rd < 32u) {
              cpu->s[insn.rd] = u;
            }
          } else if (insn.imm == 2) {
            float f;
            uint32_t u = cpu->s[insn.rn & 31u];
            memcpy(&f, &u, 4);
            mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64((double)f));
          } else if (insn.imm == 3 || insn.imm == 4) {
            float f;
            uint32_t u = cpu->s[insn.rn & 31u];
            memcpy(&f, &u, 4);
            cpu->s[insn.rd & 31u] = insn.imm == 3 ? (uint32_t)(int32_t)f : (uint32_t)f;
          } else if (insn.imm == 5) {
            int32_t si = (int32_t)cpu->s[insn.rn & 31u];
            float f = (float)si;
            uint32_t u;
            memcpy(&u, &f, 4);
            cpu->s[insn.rd & 31u] = u;
          } else {
            int32_t si = (int32_t)cpu->s[insn.rn & 31u];
            mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64((double)si));
          }
          break;
        }

        case MANGO_OP_VADD:
        case MANGO_OP_VSUB:
        case MANGO_OP_VMUL:
        case MANGO_OP_VDIV: {
          if (insn.b) {
            double a = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rn));
            double b = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
            double r;
            if (insn.op == MANGO_OP_VSUB) {
              r = a - b;
            } else if (insn.op == MANGO_OP_VMUL) {
              r = a * b;
            } else if (insn.op == MANGO_OP_VDIV) {
              r = b == 0.0 ? 0.0 : a / b;
            } else {
              r = a + b;
            }
            mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64(r));
          } else {
            float a, b, r;
            uint32_t ua = cpu->s[insn.rn & 31u];
            uint32_t ub = cpu->s[insn.rm & 31u];
            memcpy(&a, &ua, 4);
            memcpy(&b, &ub, 4);
            if (insn.op == MANGO_OP_VSUB) {
              r = a - b;
            } else if (insn.op == MANGO_OP_VMUL) {
              r = a * b;
            } else if (insn.op == MANGO_OP_VDIV) {
              r = b == 0.0f ? 0.0f : a / b;
            } else {
              r = a + b;
            }
            memcpy(&ua, &r, 4);
            cpu->s[insn.rd & 31u] = ua;
          }
          break;
        }

        case MANGO_OP_VABS: {
          double a = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
          mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64(a < 0.0 ? -a : a));
          break;
        }

        case MANGO_OP_VNEG: {
          double a = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
          mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64(-a));
          break;
        }

        case MANGO_OP_VSQRT: {
          double a = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
          mango_vfp_set_d(cpu, insn.rd, mango_f64_to_u64(a < 0.0 ? 0.0 : sqrt(a)));
          break;
        }

        case MANGO_OP_VCMP: {
          int lt, eq;
          if (insn.b) {
            double a = mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rd));
            double b = insn.imm ? 0.0 : mango_u64_to_f64(mango_vfp_get_d(cpu, insn.rm));
            eq = a == b;
            lt = a < b;
          } else {
            float a, b;
            uint32_t ua = cpu->s[insn.rd & 31u];
            memcpy(&a, &ua, 4);
            if (insn.imm) {
              b = 0.0f;
            } else {
              uint32_t ub = cpu->s[insn.rm & 31u];
              memcpy(&b, &ub, 4);
            }
            eq = a == b;
            lt = a < b;
          }
          cpu->fpscr &= ~0xF0000000u;
          if (eq) {
            cpu->fpscr |= MANGO_CPSR_Z | MANGO_CPSR_C;
          } else if (lt) {
            cpu->fpscr |= MANGO_CPSR_N | MANGO_CPSR_C;
          }
          break;
        }

        case MANGO_OP_VMRS:
          cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | (cpu->fpscr & 0xF0000000u);
          break;

        case MANGO_OP_VMOV:
          if (insn.u == 1) {
            uint64_t v = mango_vfp_get_d(cpu, insn.rm);
            cpu->r[insn.rd] = (uint32_t)v;
            cpu->r[insn.rn] = (uint32_t)(v >> 32);
          } else if (insn.u == 2) {
            uint64_t v = (uint64_t)cpu->r[insn.rd] | ((uint64_t)cpu->r[insn.rn] << 32);
            mango_vfp_set_d(cpu, insn.rm, v);
          } else if (insn.u == 3) {
            uint64_t v = (uint64_t)insn.imm | ((uint64_t)insn.rs << 32);
            uint32_t nd = insn.b ? 2u : 1u;
            for (uint32_t i = 0; i < nd; i++) {
              mango_vfp_set_d(cpu, insn.rd + i, v);
            }
          } else if (insn.u == 4) {
            if (insn.rd >= 32u) {
              return -1;
            }
            if (insn.b) {
              cpu->r[insn.rn] = cpu->s[insn.rd];
            } else {
              cpu->s[insn.rd] = cpu->r[insn.rn];
            }
          } else {
            mango_vfp_set_d(cpu, insn.rd, mango_vfp_get_d(cpu, insn.rm));
          }
          break;

        case MANGO_OP_VLD1:
        case MANGO_OP_VST1: {
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t nbytes = insn.imm * 8u;
          if (mango_check_range(mem, base, nbytes) != 0) {
            return -1;
          }
          for (uint32_t i = 0; i < insn.imm; i++) {
            uint32_t eaddr = base + i * 8u;
            if (insn.op == MANGO_OP_VLD1) {
              uint64_t v = (uint64_t)mango_load_u32_le(mem->bytes + eaddr) |
                           ((uint64_t)mango_load_u32_le(mem->bytes + eaddr + 4u) << 32);
              mango_vfp_set_d(cpu, insn.rd + i, v);
            } else {
              uint64_t v = mango_vfp_get_d(cpu, insn.rd + i);
              mango_store_u32_le(mem->bytes + eaddr, (uint32_t)v);
              mango_store_u32_le(mem->bytes + eaddr + 4u, (uint32_t)(v >> 32));
            }
          }
          if (insn.rm != 0xFu) {
            uint32_t add = (insn.rm == MANGO_REG_SP) ? nbytes : mango_read_reg(cpu, addr, insn.rm);
            cpu->r[insn.rn] = base + add;
          }
          break;
        }

        case MANGO_OP_VADDI:
        case MANGO_OP_VSUBI: {
          uint32_t esize = insn.imm ? insn.imm : 4u;
          uint32_t nbytes = insn.b ? 16u : 8u;
          uint32_t nlanes = nbytes / esize;
          for (uint32_t i = 0; i < nlanes; i++) {
            uint32_t byte = i * esize;
            uint32_t dreg = insn.rd + (byte / 8u);
            uint32_t nreg = insn.rn + (byte / 8u);
            uint32_t mreg = insn.rm + (byte / 8u);
            uint32_t off = (byte % 8u) * 8u;
            uint64_t mask = esize == 8u ? ~0ull : ((1ull << (esize * 8u)) - 1ull);
            uint64_t a = (mango_vfp_get_d(cpu, nreg) >> off) & mask;
            uint64_t b = (mango_vfp_get_d(cpu, mreg) >> off) & mask;
            uint64_t r = insn.op == MANGO_OP_VSUBI ? a - b : a + b;
            uint64_t d = mango_vfp_get_d(cpu, dreg);
            d = (d & ~(mask << off)) | ((r & mask) << off);
            mango_vfp_set_d(cpu, dreg, d);
          }
          break;
        }

        case MANGO_OP_VDUP: {
          uint32_t val = cpu->r[insn.rn];
          uint32_t esize = insn.imm ? insn.imm : 4u;
          uint32_t nd = insn.b ? 2u : 1u;
          uint64_t pat = 0;
          if (esize == 1u) {
            uint32_t b = val & 0xFFu;
            for (uint32_t i = 0; i < 8u; i++) {
              pat |= (uint64_t)b << (8u * i);
            }
          } else if (esize == 2u) {
            uint32_t h = val & 0xFFFFu;
            pat = (uint64_t)h | ((uint64_t)h << 16) | ((uint64_t)h << 32) | ((uint64_t)h << 48);
          } else {
            pat = (uint64_t)val | ((uint64_t)val << 32);
          }
          for (uint32_t i = 0; i < nd; i++) {
            mango_vfp_set_d(cpu, insn.rd + i, pat);
          }
          break;
        }

        case MANGO_OP_BFC:
        case MANGO_OP_BFI: {
          uint32_t lsb = insn.imm;
          uint32_t msb = insn.rs;
          uint32_t width = msb - lsb + 1u;
          uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
          mask <<= lsb;
          if (insn.op == MANGO_OP_BFC) {
            cpu->r[insn.rd] &= ~mask;
          } else {
            uint32_t ins = (cpu->r[insn.rm] << lsb) & mask;
            cpu->r[insn.rd] = (cpu->r[insn.rd] & ~mask) | ins;
          }
          break;
        }

        case MANGO_OP_UBFX:
        case MANGO_OP_SBFX: {
          uint32_t lsb = insn.imm;
          uint32_t width = insn.rs + 1u;
          uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
          uint32_t v = (cpu->r[insn.rn] >> lsb) & mask;
          if (insn.op == MANGO_OP_SBFX && width < 32u && (v & (1u << (width - 1u)))) {
            v |= ~mask;
          }
          cpu->r[insn.rd] = v;
          break;
        }

        case MANGO_OP_NOP:
          break;

        case MANGO_OP_REV: {
          uint32_t v = mango_read_reg(cpu, addr, insn.rm);
          uint32_t r;
          if (insn.imm == 0) { /* REV */
            r = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
          } else if (insn.imm == 1) { /* REV16 */
            r = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
          } else if (insn.imm == 2) { /* REVSH */
            uint32_t h = ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu);
            r = (h & 0x8000u) ? (h | 0xFFFF0000u) : h;
          } else { /* RBIT */
            r = 0;
            for (uint32_t i = 0; i < 32u; i++) {
              r = (r << 1) | (v & 1u);
              v >>= 1;
            }
          }
          cpu->r[insn.rd] = r;
          break;
        }

        case MANGO_OP_XTEND: {
          uint32_t v = mango_read_reg(cpu, addr, insn.rm);
          uint32_t rot = insn.imm & 31u;
          if (rot) {
            v = (v >> rot) | (v << (32u - rot));
          }
          if (insn.b) {
            v &= 0xFFFFu;
            if (!insn.u && (v & 0x8000u)) {
              v |= 0xFFFF0000u;
            }
          } else {
            v &= 0xFFu;
            if (!insn.u && (v & 0x80u)) {
              v |= 0xFFFFFF00u;
            }
          }
          if (insn.rn != 0xFu) {
            v += mango_read_reg(cpu, addr, insn.rn);
          }
          cpu->r[insn.rd] = v;
          break;
        }

        case MANGO_OP_PKH: {
          uint32_t n = mango_read_reg(cpu, addr, insn.rn);
          uint32_t m = mango_read_reg(cpu, addr, insn.rm);
          uint32_t sh = insn.shift_amount;
          if (insn.b) {
            if (sh == 0) {
              m = (m & 0x80000000u) ? 0xFFFFFFFFu : 0;
            } else {
              m = (uint32_t)((int32_t)m >> (int)sh);
            }
            cpu->r[insn.rd] = (m & 0xFFFFu) | (n & 0xFFFF0000u);
          } else {
            if (sh) {
              m <<= sh;
            }
            cpu->r[insn.rd] = (n & 0xFFFFu) | (m & 0xFFFF0000u);
          }
          break;
        }

        case MANGO_OP_CLZ: {
          uint32_t v = mango_read_reg(cpu, addr, insn.rm);
          uint32_t n = 0;
          if (v == 0) {
            n = 32;
          } else {
            while ((v & 0x80000000u) == 0) {
              v <<= 1;
              n++;
            }
          }
          cpu->r[insn.rd] = n;
          break;
        }

        case MANGO_OP_LDREX:
        case MANGO_OP_STREX: {
          uint32_t eaddr = mango_read_reg(cpu, addr, insn.rn);
          uint32_t n = insn.b == 1 ? 8u : insn.b == 2 ? 1u : insn.b == 3 ? 2u : 4u;
          if (mango_check_range(mem, eaddr, n) != 0) {
            return -1;
          }
          if (insn.op == MANGO_OP_LDREX) {
            if (n == 1u) {
              cpu->r[insn.rd] = mem->bytes[eaddr];
            } else if (n == 2u) {
              cpu->r[insn.rd] = mango_load_u16_le(mem->bytes + eaddr);
            } else if (n == 8u) {
              cpu->r[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
              cpu->r[insn.rd + 1u] = mango_load_u32_le(mem->bytes + eaddr + 4u);
            } else {
              cpu->r[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
            }
          } else {
            uint32_t val = cpu->r[insn.rm];
            if (n == 1u) {
              mem->bytes[eaddr] = (uint8_t)(val & 0xFFu);
            } else if (n == 2u) {
              mango_store_u16_le(mem->bytes + eaddr, val);
            } else if (n == 8u) {
              mango_store_u32_le(mem->bytes + eaddr, cpu->r[insn.rm]);
              mango_store_u32_le(mem->bytes + eaddr + 4u, cpu->r[insn.rm + 1u]);
            } else {
              mango_store_u32_le(mem->bytes + eaddr, val);
            }
            cpu->r[insn.rd] = 0; /* exclusive store succeeded */
          }
          break;
        }

        default:
          return -1;
      }
    }

    if (it != 0 && insn.op != MANGO_OP_IT) {
      mango_advance_itstate(cpu);
    }

    addr = next_addr;
    cpu->r[MANGO_REG_PC] = addr;
  }

  return -1; /* step limit hit */
}
