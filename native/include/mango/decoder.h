#ifndef MANGO_DECODER_H_
#define MANGO_DECODER_H_

#include <stdint.h>

/* Phase-1 subset of A32, see native/README.md for exact coverage. */

typedef enum MangoOp {
  MANGO_OP_UNKNOWN = 0,
  MANGO_OP_MOV,
  MANGO_OP_MVN,
  MANGO_OP_ADD,
  MANGO_OP_ADC,
  MANGO_OP_SUB,
  MANGO_OP_SBC,
  MANGO_OP_RSB,
  MANGO_OP_RSC,
  MANGO_OP_CMP,
  MANGO_OP_CMN,
  MANGO_OP_AND,
  MANGO_OP_EOR,
  MANGO_OP_ORR,
  MANGO_OP_BIC,
  MANGO_OP_TST,
  MANGO_OP_TEQ,
  MANGO_OP_MUL,
  MANGO_OP_MLA,
  MANGO_OP_UMULL,
  MANGO_OP_UMLAL,
  MANGO_OP_SMULL,
  MANGO_OP_SMLAL,
  MANGO_OP_SVC,
  MANGO_OP_B,
  MANGO_OP_BL,
  MANGO_OP_BLX,
  MANGO_OP_BX,
  MANGO_OP_MOVT,
  MANGO_OP_IT,
  MANGO_OP_LDR,
  MANGO_OP_STR,
  MANGO_OP_LDRH,
  MANGO_OP_STRH,
  MANGO_OP_LDRSB,
  MANGO_OP_LDRSH,
  MANGO_OP_LDRD,
  MANGO_OP_STRD,
  MANGO_OP_SWP,
  MANGO_OP_LDM,
  MANGO_OP_STM,
  MANGO_OP_VLDR,
  MANGO_OP_VSTR,
  MANGO_OP_VLDM,
  MANGO_OP_VSTM,
  MANGO_OP_VMOV,
  MANGO_OP_VCVT,
  MANGO_OP_VADD,
  MANGO_OP_VSUB,
  MANGO_OP_VMUL,
  MANGO_OP_VDIV,
  MANGO_OP_VABS,
  MANGO_OP_VNEG,
  MANGO_OP_VSQRT,
  MANGO_OP_VCMP,
  MANGO_OP_VMRS,
  MANGO_OP_VLD1,
  MANGO_OP_VST1,
  MANGO_OP_NOP,
  MANGO_OP_CLZ,
  MANGO_OP_LDREX,
  MANGO_OP_STREX,
  MANGO_OP_VDUP,
  MANGO_OP_VADDI,
  MANGO_OP_VSUBI,
  MANGO_OP_BFC,
  MANGO_OP_BFI,
  MANGO_OP_UBFX,
  MANGO_OP_SBFX,
  MANGO_OP_REV,
  MANGO_OP_XTEND,
  MANGO_OP_PKH,
  MANGO_OP_SMMUL,
} MangoOp;

typedef struct MangoInsn {
  MangoOp op;
  uint32_t cond; /* checked against NZCV by the interpreter, not here */
  uint32_t rd;
  uint32_t rn;
  uint32_t rm;           /* register form of operand2, MUL/MLA's Rm, or LDR/STR Rm */
  uint32_t rs;           /* MUL/MLA's Rs, or register-specified shift amount */
  uint32_t imm;          /* immediate operand2, branch offset, or LDR/STR offset */
  uint32_t reglist;      /* LDM/STM: bits 0-15, one bit per register */
  uint32_t shift_type;   /* 0=LSL,1=LSR,2=ASR,3=ROR */
  uint32_t shift_amount; /* DP imm: rotate amount; else 5-bit shift field (0-31) */
  int is_imm;            /* 1 if operand2 / LDR/STR offset is the immediate form */
  int shift_by_reg;      /* 1 if shift amount is Rs[7:0], not an immediate */
  int sets_flags;        /* the S bit */
  int u;                 /* LDR/STR and LDM/STM: 1 = increment, 0 = decrement */
  int b;                 /* LDR/STR: 1 = byte access, 0 = word */
  int p;                 /* LDM/STM and LDR/STR: 1 = before (pre-index / IB/DB) */
  int w;                 /* writeback: LDM/STM, or LDR/STR pre-index '!' / post-index */
} MangoInsn;

/* 0 and fills *out on success, -1 for anything outside native/README.md's
 * documented subset. */
int mango_decode(uint32_t word, MangoInsn* out);

/* Thumb-16. 32-bit Thumb encodings (hw[15:11] >= 0x1D) return -1; use
 * mango_decode_t32 for those. */
int mango_decode_t16(uint16_t hw, MangoInsn* out);

/* Thumb-32: BL/BLX/B.W/Bcond.W, MOVW/MOVT, ADDW/SUBW, and LDR/STR/LDRB/STRB/
 * LDRH/STRH imm12 plus LDR literal. */
int mango_decode_t32(uint16_t hw1, uint16_t hw2, MangoInsn* out);

#endif /* MANGO_DECODER_H_ */
