# native/

The translator. See `docs/ARCHITECTURE.md` for the overall design and
`docs/BUILDING.md` for build commands. This file is about what's actually
here right now.

## Current status

Phase 1 from the roadmap in `docs/ARCHITECTURE.md`: a decoder and
interpreter for a subset of A32 (not Thumb-2), tested against hand-encoded
synthetic programs, not real app code yet.

- `include/mango/cpu.h`, `include/mango/decoder.h`, `include/mango/interp.h`,
  `src/decoder.c`, `src/interp.c`: the portable core. Handles the full A32
  data-processing set (`AND`, `EOR`, `SUB`, `RSB`, `ADD`, `ADC`, `SBC`,
  `RSC`, `TST`, `TEQ`, `CMP`, `CMN`, `ORR`, `MOV`, `BIC`, `MVN`, all 16
  ARM ALU opcodes), `MUL`/`MLA`, long multiply `UMULL`/`UMLAL`/`SMULL`/`SMLAL`, and halfword
  signed `SMLAxy`/`SMULxy` (PC operands, MUL's non-zero Ra, long-mul
  RdLo==RdHi, and SMULxy non-zero Ra are rejected rather than misdecoded), `B`, `BL`, `BX`,
  `LDR`/`STR`/`LDRB`/`STRB` (immediate or register offset, pre-index with
  optional writeback, and post-index; `LDRB` zero-extends; `LDRT`/`STRT`,
  writeback into PC, `LDR` writeback into the same register as the dest,
  and `LDR`/`STR` of PC are rejected), extra load/store `LDRH`/`STRH`/
  `LDRSB`/`LDRSH`/`LDRD`/`STRD` (8-bit immediate or unshifted register
  offset, same pre/post/writeback rules; halfword accesses are 2-byte
  aligned; `LDRD`/`STRD` require an even Rt that isn't r14; odd Rt is
  rejected), `SWP`/`SWPB` (PC operands and Rn overlapping Rt/Rm are
  rejected),
  and `LDM`/`STM` including the `PUSH`/`POP`
  aliases (`STMDB sp!` / `LDMIA sp!`). Block transfers cover all four
  addressing modes (IA/IB/DA/DB) with optional writeback. Lowest-numbered
  register always lands at the lowest address. `LDM` of PC is a real
  return (used by `POP {..., pc}`); `STM` of PC, an empty register list,
  the S-bit user-bank/SPSR form, PC as the base register, and writeback
  with a non-SP Rn in the list are all rejected rather than guessed at.
  Writeback with SP in the list is allowed (`STMDB sp!,{sp,...}` /
  `LDMIA sp!,{...,sp,...}`): STM stores the original pre-writeback SP
  (common AAPCS/bionic expectation; ARMv7 leaves that value
  implementation-defined).
  Register operand2 can carry a shift: LSL/LSR/ASR/ROR by an immediate
  amount or by `Rs[7:0]`. Immediate `LSR #0`/`ASR #0`/`ROR #0` are the
  real hardware meanings (`LSR #32`/`ASR #32`/RRX), not "shift by zero".
  `Rs=PC` as a shift amount is UNPREDICTABLE and rejected.
  Every one of ARM's 14 real condition codes works, not just `AL`: `BEQ`,
  `BNE`, `MOVLT`, and so on all execute (or don't) based on the current
  NZCV flags, the same as real hardware. Every S-suffixed arithmetic op
  (`ADDS`/`SUBS`/`ADCS`/`SBCS`/`RSBS`/`RSCS`/`CMP`/`CMN`) computes full,
  correct NZCV, including carry propagation through `ADC`/`SBC`/`RSC` for
  multi-word arithmetic (see `test_adc_carry_chain` for the actual 64-bit-
  add-via-two-32-bit-registers case this exists for). The logical ops
  (`AND`/`EOR`/`ORR`/`BIC`/`MVN`/`MOV`/`TST`/`TEQ`) update N/Z correctly,
  preserve V exactly (this project's own earlier `MOVS` didn't, a real bug
  caught while adding the rest of this set, fixed for all of them at
  once), and take C from the shifter's carry-out when the shift actually
  produces one (`LSL #0`, `Rs=0`, and unrotated immediates leave C as-is,
  matching hardware). `TST`/`TEQ`/`CMP`/`CMN` with `S=0`
  aren't decoded as those ops at all: that bit pattern is actually
  `MRS`/`MSR` territory, a different instruction family this project
  doesn't support, so it's correctly rejected instead of silently
  misinterpreted as a flag-less compare that can't exist on real
  hardware. Register reads correctly treat r15 (PC) as "current
  instruction address + 8" per real hardware semantics, which matters for
  the very common `LDR Rd, [PC, #imm]` literal-pool pattern. This is
  genuinely a subset, real apps will use far more of the ISA (32-bit
  Thumb, NEON, and so on all still need doing). A Thumb-16 subset is
  implemented: low-register ALU, `MOV`/`ADD`/`SUB`/`CMP` immediates,
  shifts, load/store (imm, register, SP-relative, PC-literal), `ADR`,
  `ADD`/`SUB SP`, `PUSH`/`POP`, `STMIA`/`LDMIA`, conditional and
  unconditional `B`, `SVC`, and `BX`. `BX` and `POP {pc}` interwork:
  bit 0 of the target selects Thumb vs ARM (odd stop-sentinels used by
  the tests are left intact so a `BX LR` halt still matches). 32-bit
  Thumb covers `BL`/`BLX` (immediate), `B.W` / `B<cond>.W`, `MOVW`/`MOVT`,
  and `IT` (ITSTATE in CPSR, including ITE-style inverted conditions on
  later slots). T32 `LDR`/`STR`/`LDRB`/`STRB`/`LDRH`/`STRH` imm12 and
  `LDR` literal are implemented; other T32 data-processing is still
  rejected. A32 `MOVW`/`MOVT` and `BLX Rm` (and T16 `BLX Rm`) are
  implemented so JNI vtable calls and `JNI_VERSION` returns work.
  `BX PC`, ALU to PC, and `LDR PC` are real branches (Thumb-to-ARM PLT
  veneers). A NEON subset covers `VMOV.I32`/`VMOV.I8`/`VMOV.I64`
  immediates, integer `VADD`/`VSUB`, `VLD1`/`VST1` multiple (1–4 D
  registers including D16–D31), `VLDM`/`VSTM` (`VPUSH`/`VPOP`),
  `CLZ`, `LDREX`/`STREX` (always succeed: single-threaded guest),
  `VDUP` from a GPR, extra VFP `VSUB`/`VDIV`/`VABS`/`VNEG`/`VSQRT`,
  `BFC`/`BFI`/`UBFX`/`SBFX`, `RBIT`/`REV`/`REV16`/`REVSH`,
  `UXTB`/`UXTH`/`SXTB`/`SXTH` (and `*TA*` accumulate), `PKHBT`/`PKHTB`,
  `VMOV` between an S register and a GPR, VFP `VMOV.F32`/`F64` Sd/Dd←Sm/Dm, VFP single `VADD`/`VSUB`/`VMUL`/
  `VDIV`/`VCMP`, `VCVT.F32.F64`/`VCVT.S32.F64`/`VCVT.S32.F32`, dummy JNI `Call*Method`/`Get*Field`
  returning interned objects, EGL init/create/query thunks, and
  `pthread_key_create`/`getspecific`/`setspecific` TLS. `loadLibrary`
  runs `DT_INIT` / `DT_INIT_ARRAY` after relocs so C++ globals construct.
  Guest JNI slots are cleared on `unloadLibrary`.
- The interpreter has an actual memory model (`MangoMemory`): a flat,
  byte-addressable buffer that code and data share, same as real memory.
  Every fetch and every `LDR`/`STR`/`LDRB`/`STRB`/`LDRH`/`STRH`/`LDRSB`/
  `LDRSH`/`SWP`/`LDM`/`STM` is bounds-checked. Instruction fetch, `LDM`/
  `STM`, and `SWP` stay naturally aligned; `LDR`/`STR`/`LDRH`/`STRH`/
  `LDRD` data accesses follow ARMv7 unaligned rules (Android's default
  SCTLR.A=0) so Unity's odd-address halfword stores work. Out of range
  fails the run rather than reading or writing past the buffer, and
  there are tests specifically proving that (not just asserting it in a
  comment), see `docs/SECURITY.md` for why that's the priority here.
- `mango_interp_run` takes a `stop_addr` now, not just `max_steps`: it
  returns 0 when PC reaches that address, checked before every fetch.
  `BX` used to unconditionally return 0 on its own, which happened to
  work for standalone tests but broke the first real nested call: a
  callee's `BX LR` was ending the whole run instead of returning to the
  caller. `BL`/`BX` are both just normal jumps now; the caller supplies
  a `stop_addr` outside `mem` (the same sentinel-in-LR trick the tests
  already used) to detect the top-level function actually returning.
- `include/mango/native_bridge.h`: the AOSP native bridge interface Mango
  implements, adapted from the real header (see file for the source and
  license). `loadLibrary` really parses and loads a guest `.so`'s
  `PT_LOAD` segments into guest memory now (via `elf32.c` below);
  `getTrampoline` looks the symbol up and returns a callable host stub
  that marshals JNI arguments into the guest AAPCS registers, runs the
  interpreter until `BX LR`, and handles guest `SVC`s for a JNIEnv and
  JavaVM function table (FindClass, RegisterNatives, GetEnv /
  AttachCurrentThread, and the rest of the wired JNI subset) plus a
  small guest libc (malloc/memcpy/strlen/...). `JNI_OnLoad` uses the
  JavaVM calling convention. `isSupported`/`isCompatibleWith` work too, and
  `isCompatibleWith` is more load-bearing than its name suggests: AOSP's
  own `libnativebridge` calls it with `NAMESPACE_VERSION` (3)
  *unconditionally at load time* and discards the entire bridge if that's
  false, confirmed against AOSP's `libnativebridge/native_bridge.cc`, not
  guessed. This isn't gated by the `.version` field the way it looks like
  it should be; it used to return `false` for that call by accident here,
  which would have meant Mango's bridge got silently rejected by ART
  before anything else in this list ever ran, on every device, every
  time. Fixed by actually implementing the rest of the v3 interface
  (`unloadLibrary` now really frees what `loadLibrary` allocated; the
  namespace-related functions are safe no-ops, since Mango doesn't do
  real linker-namespace isolation) rather than papering over the
  symptom, see `tests/test_native_bridge_shim.c` for what's actually
  checked and `src/native_bridge_shim.c`'s comment on `isCompatibleWith`
  for the full explanation.
- `src/elf32.c` / `include/mango/elf32.h`: portable ELF32 parsing
  (`mango_elf32_parse`, `mango_elf32_find_symbol`), part of `mango_core`
  alongside the decoder/interpreter, so both this shim and `linux/`'s
  standalone loader share one implementation. Reads `PT_LOAD` segments
  from the program header table and `.dynsym`/`.dynstr` from the section
  header table, so (unlike a real OS loader) it needs section headers to
  be present for symbol lookup. See `tests/test_elf32.c`, checked against
  both a real `gcc -m32`-produced shared object and a small hand-built
  one, both independently verified with `readelf` before being trusted as
  fixtures, same reasoning as the decoder's hand-encoded test words.
- `src/native_bridge_shim.c`: the Android-specific glue exposing
  `NativeBridgeItf`. `isSupported()` really does check the ELF header
  (class + machine); `loadLibrary`/`getTrampoline`/`unloadLibrary`/the
  namespace functions are real as far as `elf32.c` above goes, and
  `getTrampoline` hands back a callable interpreter stub. Relocs
  (`R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`) are applied at
  load, with imports resolved to those libc thunks. `tests/test_native_bridge_shim.c` (built against
  `tests/fake_jni/jni.h`, a deliberately minimal stand-in, not the real
  NDK header, see that file) exercises this on the host without needing
  a device.
- `native/src/decoder.c`'s `SVC`/`SWI` decoding and `mango_interp_run`'s
  resulting stop-and-resume contract (see `interp.h`): the interpreter
  itself stays syscall-agnostic on purpose, it just stops with `r7`/
  `r0`-`r6` intact and `PC` still pointing at the `SVC`, see
  `test_svc_stops_and_can_resume` in `tests/test_interp.c`. Actually
  thunking syscalls to something real is a per-context decision (a
  standalone process versus a JNI-loaded library want different things),
  see `linux/loader_core.c` for where that's actually implemented.
- `tests/test_interp.c`: fifty-four test programs, hand-encoded by
  working out the A32 bit patterns by hand and cross-checked against an
  independently written encoder before trusting them (this caught a real
  mistake in a hand-derived test word during development, exactly why
  that second encoder exists; `encode_ldm_stm` / `encode_ldst` /
  `encode_mla` / `encode_extra_ldst` in the test file are the same
  idea). Host interp tests pass under
  `-Wall -Wextra -Werror -fsanitize=address,undefined`, including two
  negative tests that specifically try an out-of-bounds and a misaligned
  `LDR` and check they're rejected, not just that the happy path works,
  an out-of-bounds `STM`, rejected `LDM`/`STM` shapes (empty list, S-bit,
  STM of PC, writeback with non-SP Rn in the list, PC as base),
  `STMDB sp!,{sp,lr}` storing the original SP, rejected
  `LDRT` / writeback-into-PC / `LDR` writeback into the same dest /
  `LSL pc`, `LDRD`/`STRD`, a misaligned `LDRH`, and `S=0`
  `TST`/`TEQ`/`CMP`/`CMN` shapes. Earlier rounds of
  this (the `BX` mask-width bug) caught real bugs during development,
  which is exactly the kind of mistake this subset of the project is
  prone to; more test cases from more people is how this gets more
  trustworthy, see `docs/CONTRIBUTING.md`.

## Building and testing without an Android device

The core and its tests don't need the NDK:

```
cmake -B build
cmake --build build
./build/mango_core_tests
```

or without CMake at all, plain gcc/clang works fine for iterating on the
decoder and interpreter:

```
cc -std=c11 -Wall -Wextra -Werror -Iinclude src/decoder.c src/interp.c tests/test_interp.c -o /tmp/mango_core_tests
/tmp/mango_core_tests
```

Building the actual `.so` (`mango_translator`, which needs `native_bridge_shim.c`
and NDK/bionic headers) does need the NDK toolchain; see `docs/BUILDING.md`.

## Where to look if you want to help

- Remaining 32-bit Thumb (more addressing modes, data-processing), then NEON.
  Each addition should come with a hand-derived test case the way the
  existing ones work, see `docs/CONTRIBUTING.md`'s testing section.
- Guest `dlopen`/`dlsym`/`dlclose` map extra ARM32 `.so`s into a shared
  40 MiB address space (32 MiB of libraries). Unity `libunity.so` loads
  and its `JNI_OnLoad` returns `JNI_VERSION_1_6` using a VFP subset
  (`VLDR`/`VSTR`, `VCVT.F64.S32`, `VADD`/`VMUL.F64`, `VCMP`, `VMRS`,
  `VMOV`). `libmono.so` maps (no `JNI_OnLoad`). libm (`sin`/`cos`/`sqrt`/…)
  and a few pthread stubs are thunked. GLES/EGL/`ANativeWindow` calls
  return success/dummy sizes. `getTrampoline` finds `RegisterNatives`
  names (`NativeLoader.load`). `pthread_once` runs the init function.
  Fake `/proc/cpuinfo` and auxv advertise NEON/VFP. Guest `write` reports the full requested count (discard sink) so libmono write-all loops do not spin on the default `mov r0,#0` stub. Guest `exit`/`_exit`/`abort` are noreturn (set LR to the JNI stop sentinel) so a `BL exit` whose next word is a literal pool does not return into data (OFDP `pc=0x17101c`). Bionic `__page_size` resolves to a real 4096 word (not the `mov r0,#0` stub) so Boehm does not set `GC_page_size=0xe3a00000` and abort GET_MEM with `Bad GET_MEM arg`; `pthread_equal` compares thread ids so GC does not abort with `Collecting from unknown thread`; `sysconf`/`getpagesize`/`mmap`/`munmap`/`mprotect` back the same. JNI thunks preserve
  r4–r11 (SVC number is the word after `bx lr`). Unity
  `NativeLoader.load` returns true and `dlopen`s `libunity.so` and
  `libmono.so`. Dummy `FindClass` lets Unity `RegisterNatives` bind
  `nativeRender` / `nativePause` / `initJni` and the rest. OFDP
  `libunity.so` routes the internal MemoryManager allocator (VA
  `0x102b78`) and typed sibling alloc (VA `0x102c48`, size in `r0`) to guest
  `malloc`, and sibling realloc (VA `0x103a64`, `r0`=old / `r1`=bytes) so
  vector grow does not leave `dynamic_array` data
  pointing at shader label strings (`_Object2World` / ASCII `Worl`), seeds
  the keyword-tree sentinel, after constructors seeds MemoryManager list2
  (BSS `0x12c1630`), and seeds the Hash128 interval tree (BSS `0x12d7fa0`,
  redirect ctor alloc, disable reset VA `0x87fb30`), and null-checks the
  label-lookup site at VA `0x103f54` (siblings already check; without it a
  NULL walk uses guest `[0]=0xffffff` as a vtable and BLXs to `0x82ec07ee`)
  so `nativeRender` does not OOB on ASCII `NAL_` / `C_TE` / `Worl`, LDRD at
  bogus node `0x4000000`, or jump outside the guest AS via a corrupt
  callback. Host `Call*Method` and real GLES are still open.
