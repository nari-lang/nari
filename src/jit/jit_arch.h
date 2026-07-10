#pragma once
// Architecture abstraction for AsmJIT-based JIT compilers.
// Provides type aliases and instruction helpers that compile to the correct
// architecture-specific code on x86-64 or AArch64.

#include "bytecode.h"
#ifndef DISABLE_JIT

#include "compiler_support.h"
#include <asmjit/core.h>

// Detect target architecture at compile time.
// When cross-compiling for ARM64, __aarch64__ is defined.
#if defined(__aarch64__) || defined(_M_ARM64)
#define NARI_JIT_ARM64 1
#include <asmjit/a64.h>
#if defined(_MSC_VER)
// microsoft please.
#undef mvn
#undef mvni
#undef movi
#undef orr
#undef orn
#undef eor
#undef bic
#undef neg
#undef tst
#endif
#else
#define NARI_JIT_X86 1
#include <asmjit/x86.h>
#endif

namespace nari {
namespace jit {
namespace arch {

// Type aliases: Compiler, Gp (general purpose register), Vec (vector/FP
// register), Mem
#if NARI_JIT_ARM64
using Compiler = asmjit::a64::Compiler;
using Gp = asmjit::a64::Gp;
using Vec = asmjit::a64::Vec;
using Mem = asmjit::a64::Mem;
#else
using Compiler = asmjit::x86::Compiler;
using Gp = asmjit::x86::Gp;
using Vec = asmjit::x86::Vec;
using Mem = asmjit::x86::Mem;
#endif

// Canonical condition codes for signed integer comparisons.
// These match asmjit's core CondCode on ARM64 and map to x86 equivalents.
struct CC {
#if NARI_JIT_ARM64
    using Cond = asmjit::arm::CondCode;
    static constexpr auto kEQ = Cond::kEQ;
    static constexpr auto kNE = Cond::kNE;
    static constexpr auto kLT = Cond::kLT;
    static constexpr auto kLE = Cond::kLE;
    static constexpr auto kGT = Cond::kGT;
    static constexpr auto kGE = Cond::kGE;
    // Unsigned integer comparisons (HS/HI/LO/LS on ARM64).
    static constexpr auto kUGE = Cond::kHS; // unsigned >=
    static constexpr auto kUGT = Cond::kHI; // unsigned >
    static constexpr auto kULT = Cond::kLO; // unsigned <
    static constexpr auto kULE = Cond::kLS; // unsigned <=
    // Float comparison condition codes (for fcmp)
    // ARM64 fcmp sets NZCV same as integer cmp for ordered comparisons
    static constexpr auto kFLT = asmjit::arm::CondCode::kLT; // MI (N=1)
    static constexpr auto kFLE = asmjit::arm::CondCode::kLE;
    static constexpr auto kFGT = asmjit::arm::CondCode::kGT;
    static constexpr auto kFGE = asmjit::arm::CondCode::kGE;
#else
    using Cond = asmjit::x86::CondCode;
    static constexpr auto kEQ = Cond::kE;
    static constexpr auto kNE = Cond::kNE;
    static constexpr auto kLT = Cond::kL;
    static constexpr auto kLE = Cond::kLE;
    static constexpr auto kGT = Cond::kG;
    static constexpr auto kGE = Cond::kGE;
    // Unsigned integer comparisons on x86: JAE/JA/JB/JBE.
    static constexpr auto kUGE = Cond::kAE; // unsigned >= (CF=0)
    static constexpr auto kUGT = Cond::kA;  // unsigned >  (CF=0 & ZF=0)
    static constexpr auto kULT = Cond::kB;  // unsigned <  (CF=1)
    static constexpr auto kULE = Cond::kBE; // unsigned <= (CF=1 | ZF=1)
    // Float comparison condition codes (for ucomisd)
    // ucomisd sets CF/ZF: < sets CF=1, > clears CF,ZF, = sets ZF=1
    static constexpr auto kFLT = Cond::kB;  // CF=1
    static constexpr auto kFLE = Cond::kBE; // CF=1 or ZF=1
    static constexpr auto kFGT = Cond::kA;  // CF=0 and ZF=0
    static constexpr auto kFGE = Cond::kAE; // CF=0

#endif

    // Invert a condition code (for JUMP_IF_FALSE pattern)
    static constexpr Cond invert(Cond cond) {
#if NARI_JIT_ARM64
        using C = asmjit::arm::CondCode;
        return cond == C::kEQ   ? C::kNE
               : cond == C::kNE ? C::kEQ
               : cond == C::kLT ? C::kGE
               : cond == C::kGE ? C::kLT
               : cond == C::kGT ? C::kLE
               : cond == C::kLE ? C::kGT
                                : cond;
#else
        return cond == Cond::kE    ? Cond::kNE
               : cond == Cond::kNE ? Cond::kE
               : cond == Cond::kL  ? Cond::kGE
               : cond == Cond::kGE ? Cond::kL
               : cond == Cond::kG  ? Cond::kLE
               : cond == Cond::kLE ? Cond::kG
               : cond == Cond::kB  ? Cond::kAE
               : cond == Cond::kAE ? Cond::kB
               : cond == Cond::kA  ? Cond::kBE
               : cond == Cond::kBE ? Cond::kA
                                   : cond;
#endif
    }
};

// Map Nari comparison opcode to condition code.
// op must be one of: OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE
inline CC::Cond opcode_to_cc(uint8_t op) {
    using OpCode = nari::bytecode::OpCode;
    switch ((OpCode)op) {
        case OpCode::OP_EQ:
            return CC::kEQ;
        case OpCode::OP_NE:
            return CC::kNE;
        case OpCode::OP_LT:
            return CC::kLT;
        case OpCode::OP_LE:
            return CC::kLE;
        case OpCode::OP_GT:
            return CC::kGT;
        case OpCode::OP_GE:
            return CC::kGE;
        default:
            return CC::kEQ;
    }
}

// Memory operand construction.
// x86: qword_ptr(base, offset)  ARM64: a64::ptr(base, offset)
NARI_ALWAYS_INLINE Mem ptr(const Gp &base, int32_t offset = 0) {
#if NARI_JIT_ARM64
    return asmjit::a64::ptr(base, offset);
#else
    return asmjit::x86::qword_ptr(base, offset);
#endif
}

// 16-bit memory access (for NaN-box tag checks)
NARI_ALWAYS_INLINE Mem ptr16(const Gp &base, int32_t offset = 0) {
#if NARI_JIT_ARM64
    return asmjit::a64::ptr(base, offset); // ldrh determines width
#else
    return asmjit::x86::word_ptr(base, offset);
#endif
}

// 8-bit memory access
NARI_ALWAYS_INLINE Mem ptr8(const Gp &base, int32_t offset = 0) {
#if NARI_JIT_ARM64
    return asmjit::a64::ptr(base, offset); // ldrb determines width
#else
    return asmjit::x86::byte_ptr(base, offset);
#endif
}

// sign-extend lower 48 bits
NARI_ALWAYS_INLINE void sign_extend_48(Compiler &cc, const Gp &reg) {
#if NARI_JIT_ARM64
    cc.sbfx(reg, reg, 0, 48);
#else
    cc.shl(reg, 16);
    cc.sar(reg, 16);
#endif
}

// zero-extend lower 48 bits
NARI_ALWAYS_INLINE void zero_extend_48(Compiler &cc, const Gp &reg) {
#if NARI_JIT_ARM64
    cc.ubfx(reg, reg, 0, 48);
#else
    cc.shl(reg, 16);
    cc.shr(reg, 16);
#endif
}

// NaN-box encode an int: clear upper 16 bits, set tag to tagInt (0xFFFC)
// x86: shl reg, 16; shr reg, 16; or reg, 0xFFFC000000000000
// ARM64: ubfx reg, reg, #0, #48; orr reg, reg, #0xFFFC000000000000
NARI_ALWAYS_INLINE void nanbox_encode_int(Compiler &cc, const Gp &reg, const Gp &tmp) {
#if NARI_JIT_ARM64
    cc.ubfx(reg, reg, 0, 48);
    // 0xFFFC000000000000 is a valid ARM64 logical immediate (contiguous bit run)
    cc.orr(reg, reg, asmjit::Imm(int64_t(0xFFFC000000000000LL)));
#else
    cc.shl(reg, 16);
    cc.shr(reg, 16);
    cc.mov(tmp, int64_t(0xFFFC000000000000LL));
    cc.or_(reg, tmp);
#endif
}

// Load a 16-bit tag from a Value at (base + offset + tagWordOff)
// x86: movzx dst, word_ptr(base, offset + 6)
// ARM64: ldrh dst, [base, offset + 6]
NARI_ALWAYS_INLINE void load_tag16(Compiler &cc, const Gp &dst, const Gp &base, int32_t offset, int32_t tag_off) {
#if NARI_JIT_ARM64
    // ldrh requires a 32-bit (w) register destination
    cc.ldrh(dst.w(), asmjit::a64::ptr(base, offset + tag_off));
#else
    cc.movzx(dst, asmjit::x86::word_ptr(base, offset + tag_off));
#endif
}

// Compare + conditional jump
// x86: cmp a, b; jcc label
// ARM64: cmp a, b; b.cc label
inline void cmp_jcc(Compiler &cc, const Gp &a, const Gp &b, CC::Cond cond, const asmjit::Label &label) {
    cc.cmp(a, b);
#if NARI_JIT_ARM64
    cc.b(cond, label);
#else
    cc.j(cond, label);
#endif
}

// Compare reg with immediate + conditional jump
inline void cmp_imm_jcc(Compiler &cc, const Gp &a, int64_t imm, CC::Cond cond, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    // ARM64 cmp immediate is 12-bit (0..4095). For larger values, use a temp
    // register.
    if (imm >= 0 && imm <= 4095) {
        cc.cmp(a, asmjit::Imm(imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.cmp(a, tmp);
    }
    cc.b(cond, label);
#else
    cc.cmp(a, asmjit::Imm(imm));
    cc.j(cond, label);
#endif
}

// Unconditional jump
inline void jmp(Compiler &cc, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.b(label);
#else
    cc.jmp(label);
#endif
}

// Conditional branch (after a cmp/fcmp already emitted)
inline void jcc(Compiler &cc, CC::Cond cond, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.b(cond, label);
#else
    cc.j(cond, label);
#endif
}

// Float compare: ucomisd (x86) / fcmp (ARM64)
inline void float_cmp(Compiler &cc, const Vec &a, const Vec &b) {
#if NARI_JIT_ARM64
    cc.fcmp(a, b);
#else
    cc.ucomisd(a, b);
#endif
}

// Integer compare with register
inline void int_cmp(Compiler &cc, const Gp &a, const Gp &b) {
    cc.cmp(a, b);
}

// Integer compare with immediate
inline void int_cmp_imm(Compiler &cc, const Gp &a, int64_t imm) {
#if NARI_JIT_ARM64
    if (imm >= 0 && imm <= 4095) {
        cc.cmp(a, asmjit::Imm(imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.cmp(a, tmp);
    }
#else
    cc.cmp(a, asmjit::Imm(imm));
#endif
}

// Set bool from condition code: setcc (x86) / cset (ARM64)
// Result is 0 or 1 in the lower byte of dst.
inline void cset(Compiler &cc, const Gp &dst, CC::Cond cond) {
#if NARI_JIT_ARM64
    cc.cset(dst.w(), cond);
#else
    cc.set(cond, dst.r8());
    cc.movzx(dst, dst.r8());
#endif
}

// Set a NaN-correct boolean (0/1) into dst from the flags of a PRIOR float_cmp.
//  isEq == true  : dst = (a == b)  -> ordered AND equal  (NaN operands => 0)
//  isEq == false : dst = (a != b)  -> unordered OR differ (NaN operands => 1)
// x86 ucomisd sets PF=1 when operands are unordered (NaN), so a single setcc
// cannot express IEEE eq/ne; we must combine ZF (sete/setne) with the parity
// flag (setnp/setp). ARM64 fcmp already yields IEEE-correct EQ/NE flags (Z=0
// for unordered), so a single cset suffices.
inline void float_to_bool_eq(Compiler &cc, const Gp &dst, bool isEq) {
#if NARI_JIT_ARM64
    cc.cset(dst.w(), isEq ? CC::kEQ : CC::kNE);
#else
    Gp parity = cc.new_gp64();
    if (isEq) {
        // eq = ((ZF==1) && (PF==0))
        cc.set(asmjit::x86::CondCode::kE, dst.r8());
        cc.set(asmjit::x86::CondCode::kNP, parity.r8());
        cc.movzx(dst, dst.r8());
        cc.movzx(parity, parity.r8());
        cc.and_(dst, parity);
    } else {
        // not eq = ((ZF==0) || (PF==1))
        cc.set(asmjit::x86::CondCode::kNE, dst.r8());
        cc.set(asmjit::x86::CondCode::kP, parity.r8());
        cc.movzx(dst, dst.r8());
        cc.movzx(parity, parity.r8());
        cc.or_(dst, parity);
    }
#endif
}

// test register for zero (sets flags)
inline void test_zero(Compiler &cc, const Gp &reg) {
#if NARI_JIT_ARM64
    cc.cmp(reg, asmjit::Imm(0));
#else
    cc.test(reg, reg);
#endif
}

// 32-bit dword memory access
inline Mem ptr32(const Gp &base, int32_t offset = 0) {
#if NARI_JIT_ARM64
    return asmjit::a64::ptr(base, offset);
#else
    return asmjit::x86::dword_ptr(base, offset);
#endif
}

// Signed integer division:
// x86: mov quot, a; cqo; idiv b  (result in quot, remainder in rdx-equiv)
// ARM64: sdiv quot, a, b  (no remainder register; for mod, compute: rem = a -
// (quot * b))
inline void signed_div(Compiler &cc, const Gp &quot, const Gp &rem, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.sdiv(quot, a, b);
    // rem = a - quot * b
    Gp tmp = cc.new_gp64("div_tmp");
    cc.mul(tmp, quot, b);
    cc.sub(rem, a, tmp);
#else
    cc.mov(quot, a);
    cc.cqo(rem, quot);
    cc.idiv(rem, quot, b);
#endif
}

// Signed division, quotient only: q = a / b
inline void sdiv_only(Compiler &cc, const Gp &q, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.sdiv(q, a, b);
#else
    Gp rem = cc.new_gp64("div_rem_");
    cc.mov(q, a);
    cc.cqo(rem, q);
    cc.idiv(rem, q, b);
#endif
}

// Signed modulo only: rem = a % b
inline void smod_only(Compiler &cc, const Gp &rem, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    Gp q = cc.new_gp64("mod_q_");
    cc.sdiv(q, a, b);
    Gp tmp = cc.new_gp64("mod_tmp_");
    cc.mul(tmp, q, b);
    cc.sub(rem, a, tmp);
#else
    Gp q = cc.new_gp64("mod_q_");
    cc.mov(q, a);
    cc.cqo(rem, q);
    cc.idiv(rem, q, b);
#endif
}

// Float operations via Vec registers
// These have the same shape on both architectures (asmjit handles the mapping),
// but the operand types and instructions differ.

// Load double from memory into Vec register
inline void load_f64(Compiler &cc, const Vec &dst, const Gp &base, int32_t offset) {
#if NARI_JIT_ARM64
    cc.ldr(dst, asmjit::a64::ptr(base, offset));
#else
    cc.movsd(dst, asmjit::x86::qword_ptr(base, offset));
#endif
}

// Load double from Mem to Vec register
inline void load_f64(Compiler &cc, const Vec &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldr(dst, mem);
#else
    cc.movsd(dst, mem);
#endif
}

// Store double from Vec register to memory
inline void store_f64(Compiler &cc, const Gp &base, int32_t offset, const Vec &src) {
#if NARI_JIT_ARM64
    cc.str(src, asmjit::a64::ptr(base, offset));
#else
    cc.movsd(asmjit::x86::qword_ptr(base, offset), src);
#endif
}

// Store double from Vec register to Mem
inline void store_f64(Compiler &cc, const Mem &mem, const Vec &src) {
#if NARI_JIT_ARM64
    cc.str(src, mem);
#else
    cc.movsd(mem, src);
#endif
}

// Move between GP and Vec (for NaN-boxing doubles, which are stored as raw bits
// in GP)
inline void gp_to_vec(Compiler &cc, const Vec &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.fmov(dst, src);
#else
    // x86-64: direct GP->XMM register move (movq xmm, r64). Zeroes bits
    // [127:64], bit-identical to the old spill+movsd (movsd from memory also
    // zeroes the high lane) but with no memory round-trip.
    cc.movq(dst, src);
#endif
}

inline void vec_to_gp(Compiler &cc, const Gp &dst, const Vec &src) {
#if NARI_JIT_ARM64
    cc.fmov(dst, src);
#else
    cc.movq(dst, src);
#endif
}

// allocate a new Vec register for double operations
inline Vec new_vec_f64(Compiler &cc, const char *name = nullptr) {
#if NARI_JIT_ARM64
    return cc.new_vec_d(name);
#else
    return cc.new_xmm_sd(name);
#endif
}

// load a 64-bit value from memory into a GP register
inline void load(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldr(dst, mem);
#else
    cc.mov(dst, mem);
#endif
}

// store a 64-bit GP register to memory
inline void store(Compiler &cc, const Mem &mem, const Gp &src) {
#if NARI_JIT_ARM64
    cc.str(src, mem);
#else
    cc.mov(mem, src);
#endif
}

// load a 32-bit value from memory, zero-extended to 64-bit GP register
inline void load32_zx(Compiler &cc, const Gp &dst, const Gp &base, int32_t offset) {
#if NARI_JIT_ARM64
    cc.ldr(dst.w(), asmjit::a64::ptr(base, offset));
#else
    cc.mov(dst.r32(), asmjit::x86::dword_ptr(base, offset));
#endif
}

// move between GP registers
inline void mov_reg(Compiler &cc, const Gp &dst, const Gp &src) {
    cc.mov(dst, src);
}

// copy one f64 vector register to another (xmm->xmm)
inline void vec_copy(Compiler &cc, const Vec &dst, const Vec &src) {
#if NARI_JIT_ARM64
    cc.fmov(dst, src);
#else
    cc.movapd(dst, src);
#endif
}

// Zero a vector register (low f64 lane = +0.0). Used only to give a Float
// slot's fixed vreg a defining write for the register allocator (mirrors the
// `mov Imm(0)` slot_reg init); a correctly Float-typed slot is always stored
// before any meaningful read, so the value itself is never observed.
inline void vec_zero(Compiler &cc, const Vec &dst) {
#if NARI_JIT_ARM64
    cc.movi(dst.b16(), 0);
#else
    cc.xorpd(dst, dst);
#endif
}

// AND two GP registers
inline void and_reg(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.and_(dst, dst, src);
#else
    cc.and_(dst, src);
#endif
}

// OR two GP registers
inline void or_reg(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.orr(dst, dst, src);
#else
    cc.or_(dst, src);
#endif
}

// SUB two GP registers: dst = dst - src
inline void sub_reg(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.sub(dst, dst, src);
#else
    cc.sub(dst, src);
#endif
}

// IMUL (signed multiply) two GP registers: dst = a * b (low 64 bits)
inline void imul(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.mul(dst, a, b);
#else
    cc.mov(dst, a);
    cc.imul(dst, b);
#endif
}

// Widening signed multiply: hi = upper64(a * b).
// On x86: one-operand IMUL form -> RDX:RAX = RAX * src; hi->RDX, lo->RAX.
// On ARM64: SMULH gives upper 64 bits directly.
// NOTE: on x86, `lo` is clobbered with the lower 64 bits.
inline void imul_wide_hi(Compiler &cc, const Gp &hi, const Gp &lo, const Gp &src) {
#if NARI_JIT_ARM64
    cc.smulh(hi, lo, src);
#else
    cc.imul(hi, lo, src);
#endif
}

// IMUL with immediate: dst = src * imm
inline void imul_imm(Compiler &cc, const Gp &dst, const Gp &src, int32_t imm) {
#if NARI_JIT_ARM64
    // Power-of-2 multiply -> shift
    if (imm > 0 && (imm & (imm - 1)) == 0) {
        int shift = 0;
        int v = imm;
        while (v > 1) {
            v >>= 1;
            shift++;
        }
        cc.lsl(dst, src, asmjit::Imm(shift));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, (int64_t)imm);
        cc.mul(dst, src, tmp);
    }
#else
    cc.imul(dst, src, imm);
#endif
}

// smulh (signed multiply high): dst = (a * b) >> 64
inline void smulh(Compiler &cc, const Gp &hi, const Gp &lo, const Gp &b) {
#if NARI_JIT_ARM64
    cc.smulh(hi, lo, b);
#else
    cc.imul(hi, lo, b); // On x86: this is lo*b into hi (low 64 bits)
#endif
}

// shift right (arithmetic): dst >>= shift
inline void sar(Compiler &cc, const Gp &dst, int shift) {
#if NARI_JIT_ARM64
    cc.asr(dst, dst, asmjit::Imm(shift));
#else
    cc.sar(dst, shift);
#endif
}

// shift right (logical): dst >>= shift
inline void shr(Compiler &cc, const Gp &dst, int shift) {
#if NARI_JIT_ARM64
    cc.lsr(dst, dst, asmjit::Imm(shift));
#else
    cc.shr(dst, shift);
#endif
}

// add two GP registers: dst = dst + src
inline void add_reg(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.add(dst, dst, src);
#else
    cc.add(dst, src);
#endif
}

// cvtsi2sd / scvtf: convert signed int64 to double
inline void int_to_f64(Compiler &cc, const Vec &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.scvtf(dst, src);
#else
    cc.cvtsi2sd(dst, src);
#endif
}

// sqrtsd / fsqrt: square root of double
inline void sqrt_f64(Compiler &cc, const Vec &dst, const Vec &src) {
#if NARI_JIT_ARM64
    cc.fsqrt(dst, src);
#else
    cc.sqrtsd(dst, src);
#endif
}

// zero a GP register
// x86: xor r, r  ARM64: mov r, #0
inline void zero_reg(Compiler &cc, const Gp &dst) {
#if NARI_JIT_ARM64
    cc.mov(dst, asmjit::Imm(0));
#else
    cc.xor_(dst, dst);
#endif
}

// three-operand add: dst = a + b
inline void add3(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.add(dst, a, b);
#else
    if (dst.id() == a.id()) {
        cc.add(dst, b);
    } else if (dst.id() == b.id()) {
        cc.add(dst, a);
    } else {
        cc.mov(dst, a);
        cc.add(dst, b);
    }
#endif
}

// negate: dst = -dst
inline void neg(Compiler &cc, const Gp &dst) {
#if NARI_JIT_ARM64
    cc.neg(dst, dst);
#else
    cc.neg(dst);
#endif
}

// branch if register is zero
inline void branch_if_zero(Compiler &cc, const Gp &reg, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.cmp(reg, asmjit::Imm(0));
    cc.b(asmjit::arm::CondCode::kEQ, label);
#else
    cc.test(reg, reg);
    cc.je(label);
#endif
}

// Branch if register is nonzero
inline void branch_if_nonzero(Compiler &cc, const Gp &reg, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.cmp(reg, asmjit::Imm(0));
    cc.b(asmjit::arm::CondCode::kNE, label);
#else
    cc.test(reg, reg);
    cc.jne(label);
#endif
}

// Add immediate to GP register: dst += imm
inline void add_imm(Compiler &cc, const Gp &dst, int64_t imm) {
#if NARI_JIT_ARM64
    if (imm >= 0 && imm <= 4095) {
        cc.add(dst, dst, asmjit::Imm(imm));
    } else if (imm < 0 && (-imm) >= 0 && (-imm) <= 4095) {
        cc.sub(dst, dst, asmjit::Imm(-imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.add(dst, dst, tmp);
    }
#else
    cc.add(dst, asmjit::Imm(imm));
#endif
}

// Sub immediate from GP register: dst -= imm
inline void sub_imm(Compiler &cc, const Gp &dst, int64_t imm) {
#if NARI_JIT_ARM64
    if (imm >= 0 && imm <= 4095) {
        cc.sub(dst, dst, asmjit::Imm(imm));
    } else if (imm < 0 && (-imm) >= 0 && (-imm) <= 4095) {
        cc.add(dst, dst, asmjit::Imm(-imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.sub(dst, dst, tmp);
    }
#else
    cc.sub(dst, asmjit::Imm(imm));
#endif
}

// Add register to register: dst += src (2-operand form)
inline void add2(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.add(dst, dst, src);
#else
    cc.add(dst, src);
#endif
}

// Subtract register from register: dst -= src (2-operand form)
inline void sub2(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.sub(dst, dst, src);
#else
    cc.sub(dst, src);
#endif
}

// Store immediate to memory (x86 can do this directly; ARM64 needs temp register)
inline void store_imm(Compiler &cc, const Mem &mem, int64_t imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, asmjit::Imm(imm));
    cc.str(tmp, mem);
#else
    cc.mov(mem, asmjit::Imm(imm));
#endif
}

// shift left: dst = src << shift
inline void shl(Compiler &cc, const Gp &dst, const Gp &src, int shift) {
#if NARI_JIT_ARM64
    cc.lsl(dst, src, asmjit::Imm(shift));
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.shl(dst, shift);
#endif
}

// shift left by register: dst = src << shift_reg
inline void shl_reg(Compiler &cc, const Gp &dst, const Gp &src,
                    const Gp &shift_reg) {
#if NARI_JIT_ARM64
    cc.lsl(dst, src, shift_reg);
#else
    // x86 shift count must be in cl
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.shl(dst, shift_reg.r8());
#endif
}

// shift right arithmetic by register: dst = src >> shift_reg (sign-extending)
inline void sar_reg(Compiler &cc, const Gp &dst, const Gp &src, const Gp &shift_reg) {
#if NARI_JIT_ARM64
    cc.asr(dst, src, shift_reg);
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.sar(dst, shift_reg.r8());
#endif
}

// shift right logical by register: dst = src >>> shift_reg
inline void shr_reg(Compiler &cc, const Gp &dst, const Gp &src, const Gp &shift_reg) {
#if NARI_JIT_ARM64
    cc.lsr(dst, src, shift_reg);
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.shr(dst, shift_reg.r8());
#endif
}

// XOR two GP registers: dst = a ^ b
inline void xor_reg(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.eor(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.mov(dst, a);
    }
    cc.xor_(dst, b);
#endif
}

// x86 `<logic> r64, imm` only encodes a sign-extended imm32. Constants outside
// that range (e.g. 64-bit nan-box tags) must go through a scratch register.
inline bool fits_imm32(int64_t imm) {
    return imm >= -2147483648LL && imm <= 2147483647LL;
}

// XOR register with immediate: dst ^= imm
inline void xor_imm(Compiler &cc, const Gp &dst, int64_t imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, asmjit::Imm(imm));
    cc.eor(dst, dst, tmp);
#else
    if (!fits_imm32(imm)) {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.xor_(dst, tmp);
    } else {
        cc.xor_(dst, asmjit::Imm(imm));
    }
#endif
}

// AND with immediate: dst = src & imm
inline void and_imm(Compiler &cc, const Gp &dst, const Gp &src, int64_t imm) {
#if NARI_JIT_ARM64
    cc.and_(dst, src, asmjit::Imm(imm));
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    if (!fits_imm32(imm)) {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.and_(dst, tmp);
    } else {
        cc.and_(dst, asmjit::Imm(imm));
    }
#endif
}

// OR with immediate: dst = src | imm
inline void or_imm(Compiler &cc, const Gp &dst, const Gp &src, int64_t imm) {
#if NARI_JIT_ARM64
    cc.orr(dst, src, asmjit::Imm(imm));
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    if (!fits_imm32(imm)) {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.or_(dst, tmp);
    } else {
        cc.or_(dst, asmjit::Imm(imm));
    }
#endif
}

// OR three GP registers: dst = a | b
inline void or3(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.orr(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.mov(dst, a);
    }
    cc.or_(dst, b);
#endif
}

// AND three GP registers: dst = a & b
inline void and3(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.and_(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.mov(dst, a);
    }
    cc.and_(dst, b);
#endif
}

// bitwise NOT: dst = ~src
inline void not_reg(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.mvn(dst, src);
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.not_(dst);
#endif
}

// three-operand sub: dst = a - b
inline void sub3(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.sub(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.mov(dst, a);
    }
    cc.sub(dst, b);
#endif
}

// 3-operand sub with immediate: dst = src - imm
inline void sub3_imm(Compiler &cc, const Gp &dst, const Gp &src, int64_t imm) {
#if NARI_JIT_ARM64
    if (imm >= 0 && imm <= 4095) {
        cc.sub(dst, src, asmjit::Imm(imm));
    } else if (imm < 0 && (-imm) >= 0 && (-imm) <= 4095) {
        cc.add(dst, src, asmjit::Imm(-imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.sub(dst, src, tmp);
    }
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.sub(dst, asmjit::Imm(imm));
#endif
}

// 3-operand add with immediate: dst = src + imm
inline void add3_imm(Compiler &cc, const Gp &dst, const Gp &src, int64_t imm) {
#if NARI_JIT_ARM64
    if (imm >= 0 && imm <= 4095) {
        cc.add(dst, src, asmjit::Imm(imm));
    } else if (imm < 0 && (-imm) >= 0 && (-imm) <= 4095) {
        cc.sub(dst, src, asmjit::Imm(-imm));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(imm));
        cc.add(dst, src, tmp);
    }
#else
    if (dst.id() != src.id()) {
        cc.mov(dst, src);
    }
    cc.add(dst, asmjit::Imm(imm));
#endif
}

// float add: dst = a + b
inline void fadd(Compiler &cc, const Vec &dst, const Vec &a, const Vec &b) {
#if NARI_JIT_ARM64
    cc.fadd(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.movapd(dst, a);
    }
    cc.addsd(dst, b);
#endif
}

// float sub: dst = a - b
inline void fsub(Compiler &cc, const Vec &dst, const Vec &a, const Vec &b) {
#if NARI_JIT_ARM64
    cc.fsub(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.movapd(dst, a);
    }
    cc.subsd(dst, b);
#endif
}

// float mul: dst = a * b
inline void fmul(Compiler &cc, const Vec &dst, const Vec &a, const Vec &b) {
#if NARI_JIT_ARM64
    cc.fmul(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.movapd(dst, a);
    }
    cc.mulsd(dst, b);
#endif
}

// float div: dst = a / b
inline void fdiv(Compiler &cc, const Vec &dst, const Vec &a, const Vec &b) {
#if NARI_JIT_ARM64
    cc.fdiv(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.movapd(dst, a);
    }
    cc.divsd(dst, b);
#endif
}

// float negate: dst = -src
inline void fneg(Compiler &cc, const Vec &dst, const Vec &src) {
#if NARI_JIT_ARM64
    cc.fneg(dst, src);
#else
    // x86: xorpd with sign-bit mask
    auto zero = cc.new_xmm_sd("fneg_zero");
    cc.xorpd(zero, zero);
    cc.movsd(dst, zero);
    cc.subsd(dst, src);
#endif
}

// float convert to signed int64: dst = (int64_t)src
inline void f64_to_int(Compiler &cc, const Gp &dst, const Vec &src) {
#if NARI_JIT_ARM64
    cc.fcvtzs(dst, src);
#else
    cc.cvttsd2si(dst, src);
#endif
}

// test register with immediate and branch if zero
inline void test_imm_jz(Compiler &cc, const Gp &reg, int64_t imm, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, asmjit::Imm(imm));
    cc.tst(reg, tmp);
    cc.b(asmjit::arm::CondCode::kEQ, label);
#else
    cc.test(reg, asmjit::Imm(imm));
    cc.jz(label);
#endif
}

// test register with immediate and branch if nonzero
inline void test_imm_jnz(Compiler &cc, const Gp &reg, int64_t imm, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, asmjit::Imm(imm));
    cc.tst(reg, tmp);
    cc.b(asmjit::arm::CondCode::kNE, label);
#else
    cc.test(reg, asmjit::Imm(imm));
    cc.jnz(label);
#endif
}

// test register with immediate (sets flags, no branch)
inline void test_imm(Compiler &cc, const Gp &reg, int64_t imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, asmjit::Imm(imm));
    cc.tst(reg, tmp);
#else
    cc.test(reg, asmjit::Imm(imm));
#endif
}

// zero-extend byte register to 64-bit: dst = (uint64_t)(uint8_t)src
inline void zx_r8(Compiler &cc, const Gp &dst, const Gp &src) {
#if NARI_JIT_ARM64
    cc.uxtb(dst.w(), src.w());
#else
    cc.movzx(dst, src.r8());
#endif
}

// load 16-bit from memory, zero-extended to 64-bit register
inline void load16_zx(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldrh(dst.w(), mem);
#else
    cc.movzx(dst, mem);
#endif
}

// load 8-bit from memory, zero-extended to 64-bit register
inline void load8_zx(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldrb(dst.w(), mem);
#else
    cc.movzx(dst, mem);
#endif
}

// load 32-bit from memory, zero-extended to 64-bit register
inline void load32_zx_mem(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldr(dst.w(), mem);
#else
    cc.mov(dst.r32(), mem);
#endif
}

// load 32-bit from memory, sign-extended to 64-bit register
inline void load32_sx(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    cc.ldrsw(dst, mem);
#else
    cc.movsxd(dst, mem);
#endif
}

// LEA equivalent: dst = base + offset
inline void lea(Compiler &cc, const Gp &dst, const Gp &base, int64_t offset) {
#if NARI_JIT_ARM64
    if (offset == 0) {
        cc.mov(dst, base);
    } else if (offset > 0 && offset <= 4095) {
        cc.add(dst, base, asmjit::Imm(offset));
    } else if (offset < 0 && (-offset) <= 4095) {
        cc.sub(dst, base, asmjit::Imm(-offset));
    } else {
        Gp tmp = cc.new_gp64();
        cc.mov(tmp, asmjit::Imm(offset));
        cc.add(dst, base, tmp);
    }
#else
    cc.lea(dst, asmjit::x86::qword_ptr(base, (int32_t)offset));
#endif
}

// multiply: dst = src * reg (3-operand register form)
inline void mul3(Compiler &cc, const Gp &dst, const Gp &a, const Gp &b) {
#if NARI_JIT_ARM64
    cc.mul(dst, a, b);
#else
    if (dst.id() != a.id()) {
        cc.mov(dst, a);
    }
    cc.imul(dst, b);
#endif
}

// cc.js (jump if sign/negative): test sign bit = MSB
inline void js(Compiler &cc, const Gp &reg, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.cmp(reg, asmjit::Imm(0));
    cc.b(asmjit::arm::CondCode::kLT, label);
#else
    cc.test(reg, reg);
    cc.js(label);
#endif
}

// cc.jns (jump if not sign/non-negative)
inline void jns(Compiler &cc, const Gp &reg, const asmjit::Label &label) {
#if NARI_JIT_ARM64
    cc.cmp(reg, asmjit::Imm(0));
    cc.b(asmjit::arm::CondCode::kGE, label);
#else
    cc.test(reg, reg);
    cc.jns(label);
#endif
}

// decrement: dst -= 1
inline void dec(Compiler &cc, const Gp &dst) {
    sub_imm(cc, dst, 1);
}

// increment: dst += 1
inline void inc(Compiler &cc, const Gp &dst) {
    add_imm(cc, dst, 1);
}

// new_stack: allocate stack slot (works on both archs via Compiler API)
inline Mem new_stack(Compiler &cc, uint32_t size, uint32_t alignment) {
    return cc.new_stack(size, alignment);
}

// decrement a 32-bit memory location: [base+offset] -= 1
inline void dec_mem32(Compiler &cc, const Gp &base, int32_t offset) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldr(tmp, arch::ptr32(base, offset));
    cc.sub(tmp, tmp, asmjit::Imm(1));
    cc.str(tmp, arch::ptr32(base, offset));
#else
    cc.dec(asmjit::x86::dword_ptr(base, offset));
#endif
}

// increment a 32-bit memory location: [base+offset] += 1
inline void inc_mem32(Compiler &cc, const Gp &base, int32_t offset) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldr(tmp, arch::ptr32(base, offset));
    cc.add(tmp, tmp, asmjit::Imm(1));
    cc.str(tmp, arch::ptr32(base, offset));
#else
    cc.inc(asmjit::x86::dword_ptr(base, offset));
#endif
}

// memory load/store wrappers (x86 cc.mov(gp,mem)/cc.mov(mem,gp) -> ARM64 ldr/str)

// store immediate to Mem (64-bit): Imm overload
inline void store_imm(Compiler &cc, const Mem &mem, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, imm);
    cc.str(tmp, mem);
#else
    cc.mov(mem, imm);
#endif
}

// Compare GP register with immediate value
// ARM64 cmp only supports 12-bit immediates (0-4095), so larger values need a temp
inline void invoke_imm(Compiler &cc, asmjit::InvokeNode **out, uint64_t addr, const asmjit::FuncSignature &sig) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.mov(tmp, int64_t(addr));
    cc.invoke(asmjit::Out(*out), tmp, sig);
#else
    cc.invoke(asmjit::Out(*out), asmjit::Imm(addr), sig);
#endif
}

inline void cmp_imm(Compiler &cc, const Gp &reg, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    int64_t val = imm.value();
    if (val >= 0 && val <= 4095) {
        cc.cmp(reg, imm);
    } else {
        Gp tmp = reg.is_gp32() ? cc.new_gp32() : cc.new_gp64();
        cc.mov(tmp, imm);
        cc.cmp(reg, tmp);
    }
#else
    cc.cmp(reg, imm);
#endif
}

// compare GP register with 64-bit memory value
inline void cmp_mem(Compiler &cc, const Gp &reg, const Mem &mem) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.ldr(tmp, mem);
    cc.cmp(reg, tmp);
#else
    cc.cmp(reg, mem);
#endif
}

// compare GP register (32-bit) with 32-bit memory value
inline void cmp_mem32(Compiler &cc, const Gp &reg, const Mem &mem) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldr(tmp, mem);
    cc.cmp(reg.r32(), tmp);
#else
    cc.cmp(reg.r32(), mem);
#endif
}

// add memory value to GP register: dst += [mem]
inline void add_mem(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.ldr(tmp, mem);
    cc.add(dst, dst, tmp);
#else
    cc.add(dst, mem);
#endif
}

// subtract memory value from GP register: dst -= [mem]
inline void sub_mem(Compiler &cc, const Gp &dst, const Mem &mem) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.ldr(tmp, mem);
    cc.sub(dst, dst, tmp);
#else
    cc.sub(dst, mem);
#endif
}

// compare 8-bit memory value vs immediate
inline void cmp_mem8_imm(Compiler &cc, const Mem &mem, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldrb(tmp, mem);
    cmp_imm(cc, tmp, imm);
#else
    cc.cmp(mem, imm);
#endif
}

// compare 16-bit memory value vs immediate
inline void cmp_mem16_imm(Compiler &cc, const Mem &mem, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldrh(tmp, mem);
    cmp_imm(cc, tmp, imm);
#else
    cc.cmp(mem, imm);
#endif
}

// compare 16-bit memory value vs GP register (for hoisted constants)
inline void cmp_mem16_reg(Compiler &cc, const Mem &mem, const Gp &reg) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldrh(tmp, mem);
    cc.cmp(tmp, reg);
#else
    Gp tmp = cc.new_gp32();
    cc.movzx(tmp, mem);
    cc.cmp(tmp, reg);
#endif
}

// compare 32-bit memory value vs immediate
inline void cmp_mem32_imm(Compiler &cc, const Mem &mem, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp32();
    cc.ldr(tmp, mem);
    cmp_imm(cc, tmp, imm);
#else
    cc.cmp(mem, imm);
#endif
}

// compare 64-bit memory value vs immediate
inline void cmp_mem_imm(Compiler &cc, const Mem &mem, const asmjit::Imm &imm) {
#if NARI_JIT_ARM64
    Gp tmp = cc.new_gp64();
    cc.ldr(tmp, mem);
    cmp_imm(cc, tmp, imm);
#else
    cc.cmp(mem, imm);
#endif
}

} // namespace arch
} // namespace jit
} // namespace nari

#endif // DISABLE_JIT
