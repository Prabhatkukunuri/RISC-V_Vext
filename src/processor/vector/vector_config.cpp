/**
 * @file vector_config.cpp
 * @brief vtype decoding and the vsetvli/vsetivli/vsetvl rules.
 */

#include "processor/vector/vector_config.h"

#include "processor/registers.h"

#include <algorithm>
#include <cstdio>

namespace Kites::rvv
{
VsetKind DecodeVsetKind(uint32_t instruction)
{
    if ((instruction & 0x7F) != OPCODE_OP_V || ((instruction >> 12) & 0x7) != FUNCT3_OPCFG)
    {
        return VsetKind::None;
    }
    if ((instruction >> 31) == 0)
    {
        return VsetKind::Vsetvli;
    }
    if (((instruction >> 30) & 0b11) == 0b11)
    {
        return VsetKind::Vsetivli;
    }
    if (((instruction >> 25) & 0x7F) == 0b1000000)
    {
        return VsetKind::Vsetvl;
    }
    return VsetKind::None;
}

namespace
{
using Op = VectorArithOp;
using Shape = VectorOpShape;
constexpr bool U = false; // zero-extend narrow operands
constexpr bool S = true;  // sign-extend narrow operands

// Every OP-V arithmetic encoding implemented so far. Missing pairs, such as vsub.vi or
// vrsub.vv, are reserved encodings and decode as unsupported.
//  funct6    funct3        vs1   op        shape               sign fac  mnemonic
constexpr VectorArithInfo kVectorArith[] = {
    // 11.1 Single-width integer add and subtract
    {0b000000, FUNCT3_OPIVV, -1, Op::Add, Shape::SingleWidth, U, 1, "vadd.vv"},
    {0b000000, FUNCT3_OPIVX, -1, Op::Add, Shape::SingleWidth, U, 1, "vadd.vx"},
    {0b000000, FUNCT3_OPIVI, -1, Op::Add, Shape::SingleWidth, U, 1, "vadd.vi"},
    {0b000010, FUNCT3_OPIVV, -1, Op::Sub, Shape::SingleWidth, U, 1, "vsub.vv"},
    {0b000010, FUNCT3_OPIVX, -1, Op::Sub, Shape::SingleWidth, U, 1, "vsub.vx"},
    {0b000011, FUNCT3_OPIVX, -1, Op::Rsub, Shape::SingleWidth, U, 1, "vrsub.vx"},
    {0b000011, FUNCT3_OPIVI, -1, Op::Rsub, Shape::SingleWidth, U, 1, "vrsub.vi"},

    // 11.2 Widening integer add and subtract
    {0b110000, FUNCT3_OPMVV, -1, Op::Add, Shape::Widening, U, 1, "vwaddu.vv"},
    {0b110000, FUNCT3_OPMVX, -1, Op::Add, Shape::Widening, U, 1, "vwaddu.vx"},
    {0b110001, FUNCT3_OPMVV, -1, Op::Add, Shape::Widening, S, 1, "vwadd.vv"},
    {0b110001, FUNCT3_OPMVX, -1, Op::Add, Shape::Widening, S, 1, "vwadd.vx"},
    {0b110010, FUNCT3_OPMVV, -1, Op::Sub, Shape::Widening, U, 1, "vwsubu.vv"},
    {0b110010, FUNCT3_OPMVX, -1, Op::Sub, Shape::Widening, U, 1, "vwsubu.vx"},
    {0b110011, FUNCT3_OPMVV, -1, Op::Sub, Shape::Widening, S, 1, "vwsub.vv"},
    {0b110011, FUNCT3_OPMVX, -1, Op::Sub, Shape::Widening, S, 1, "vwsub.vx"},
    {0b110100, FUNCT3_OPMVV, -1, Op::Add, Shape::WideningW, U, 1, "vwaddu.wv"},
    {0b110100, FUNCT3_OPMVX, -1, Op::Add, Shape::WideningW, U, 1, "vwaddu.wx"},
    {0b110101, FUNCT3_OPMVV, -1, Op::Add, Shape::WideningW, S, 1, "vwadd.wv"},
    {0b110101, FUNCT3_OPMVX, -1, Op::Add, Shape::WideningW, S, 1, "vwadd.wx"},
    {0b110110, FUNCT3_OPMVV, -1, Op::Sub, Shape::WideningW, U, 1, "vwsubu.wv"},
    {0b110110, FUNCT3_OPMVX, -1, Op::Sub, Shape::WideningW, U, 1, "vwsubu.wx"},
    {0b110111, FUNCT3_OPMVV, -1, Op::Sub, Shape::WideningW, S, 1, "vwsub.wv"},
    {0b110111, FUNCT3_OPMVX, -1, Op::Sub, Shape::WideningW, S, 1, "vwsub.wx"},

    // 11.3 Integer extension (VXUNARY0: the vs1 field picks the variant)
    {0b010010, FUNCT3_OPMVV, 0b00110, Op::Extend, Shape::Extension, U, 2, "vzext.vf2"},
    {0b010010, FUNCT3_OPMVV, 0b00111, Op::Extend, Shape::Extension, S, 2, "vsext.vf2"},
    {0b010010, FUNCT3_OPMVV, 0b00100, Op::Extend, Shape::Extension, U, 4, "vzext.vf4"},
    {0b010010, FUNCT3_OPMVV, 0b00101, Op::Extend, Shape::Extension, S, 4, "vsext.vf4"},
    {0b010010, FUNCT3_OPMVV, 0b00010, Op::Extend, Shape::Extension, U, 8, "vzext.vf8"},
    {0b010010, FUNCT3_OPMVV, 0b00011, Op::Extend, Shape::Extension, S, 8, "vsext.vf8"},

    // 11.5 Bitwise logical
    {0b001001, FUNCT3_OPIVV, -1, Op::And, Shape::SingleWidth, U, 1, "vand.vv"},
    {0b001001, FUNCT3_OPIVX, -1, Op::And, Shape::SingleWidth, U, 1, "vand.vx"},
    {0b001001, FUNCT3_OPIVI, -1, Op::And, Shape::SingleWidth, U, 1, "vand.vi"},
    {0b001010, FUNCT3_OPIVV, -1, Op::Or, Shape::SingleWidth, U, 1, "vor.vv"},
    {0b001010, FUNCT3_OPIVX, -1, Op::Or, Shape::SingleWidth, U, 1, "vor.vx"},
    {0b001010, FUNCT3_OPIVI, -1, Op::Or, Shape::SingleWidth, U, 1, "vor.vi"},
    {0b001011, FUNCT3_OPIVV, -1, Op::Xor, Shape::SingleWidth, U, 1, "vxor.vv"},
    {0b001011, FUNCT3_OPIVX, -1, Op::Xor, Shape::SingleWidth, U, 1, "vxor.vx"},
    {0b001011, FUNCT3_OPIVI, -1, Op::Xor, Shape::SingleWidth, U, 1, "vxor.vi"},

    // 11.6 Single-width bit shift. Only vsra is "signed": its vs2 element is sign-extended
    // before shifting, so the vacated high bits fill with copies of the sign bit.
    {0b100101, FUNCT3_OPIVV, -1, Op::Sll, Shape::SingleWidth, U, 1, "vsll.vv"},
    {0b100101, FUNCT3_OPIVX, -1, Op::Sll, Shape::SingleWidth, U, 1, "vsll.vx"},
    {0b100101, FUNCT3_OPIVI, -1, Op::Sll, Shape::SingleWidth, U, 1, "vsll.vi"},
    {0b101000, FUNCT3_OPIVV, -1, Op::Srl, Shape::SingleWidth, U, 1, "vsrl.vv"},
    {0b101000, FUNCT3_OPIVX, -1, Op::Srl, Shape::SingleWidth, U, 1, "vsrl.vx"},
    {0b101000, FUNCT3_OPIVI, -1, Op::Srl, Shape::SingleWidth, U, 1, "vsrl.vi"},
    {0b101001, FUNCT3_OPIVV, -1, Op::Sra, Shape::SingleWidth, S, 1, "vsra.vv"},
    {0b101001, FUNCT3_OPIVX, -1, Op::Sra, Shape::SingleWidth, S, 1, "vsra.vx"},
    {0b101001, FUNCT3_OPIVI, -1, Op::Sra, Shape::SingleWidth, S, 1, "vsra.vi"},

    // 11.7 Narrowing right shift: vs2 is 2*SEW wide, the result is cut to SEW. vnsra
    // sign-extends the wide element first, vnsrl zero-extends it.
    {0b101100, FUNCT3_OPIVV, -1, Op::Srl, Shape::Narrowing, U, 1, "vnsrl.wv"},
    {0b101100, FUNCT3_OPIVX, -1, Op::Srl, Shape::Narrowing, U, 1, "vnsrl.wx"},
    {0b101100, FUNCT3_OPIVI, -1, Op::Srl, Shape::Narrowing, U, 1, "vnsrl.wi"},
    {0b101101, FUNCT3_OPIVV, -1, Op::Sra, Shape::Narrowing, S, 1, "vnsra.wv"},
    {0b101101, FUNCT3_OPIVX, -1, Op::Sra, Shape::Narrowing, S, 1, "vnsra.wx"},
    {0b101101, FUNCT3_OPIVI, -1, Op::Sra, Shape::Narrowing, S, 1, "vnsra.wi"},

    // 11.8 Integer compare: vd is a mask register. The .vi immediate is a sign-extended simm5
    // even for the unsigned compares (so vmsleu.vi v1, v2, -1 compares with 2^SEW - 1).
    {0b011000, FUNCT3_OPIVV, -1, Op::Eq, Shape::Compare, U, 1, "vmseq.vv"},
    {0b011000, FUNCT3_OPIVX, -1, Op::Eq, Shape::Compare, U, 1, "vmseq.vx"},
    {0b011000, FUNCT3_OPIVI, -1, Op::Eq, Shape::Compare, U, 1, "vmseq.vi"},
    {0b011001, FUNCT3_OPIVV, -1, Op::Ne, Shape::Compare, U, 1, "vmsne.vv"},
    {0b011001, FUNCT3_OPIVX, -1, Op::Ne, Shape::Compare, U, 1, "vmsne.vx"},
    {0b011001, FUNCT3_OPIVI, -1, Op::Ne, Shape::Compare, U, 1, "vmsne.vi"},
    {0b011010, FUNCT3_OPIVV, -1, Op::Lt, Shape::Compare, U, 1, "vmsltu.vv"},
    {0b011010, FUNCT3_OPIVX, -1, Op::Lt, Shape::Compare, U, 1, "vmsltu.vx"},
    {0b011011, FUNCT3_OPIVV, -1, Op::Lt, Shape::Compare, S, 1, "vmslt.vv"},
    {0b011011, FUNCT3_OPIVX, -1, Op::Lt, Shape::Compare, S, 1, "vmslt.vx"},
    {0b011100, FUNCT3_OPIVV, -1, Op::Le, Shape::Compare, U, 1, "vmsleu.vv"},
    {0b011100, FUNCT3_OPIVX, -1, Op::Le, Shape::Compare, U, 1, "vmsleu.vx"},
    {0b011100, FUNCT3_OPIVI, -1, Op::Le, Shape::Compare, U, 1, "vmsleu.vi"},
    {0b011101, FUNCT3_OPIVV, -1, Op::Le, Shape::Compare, S, 1, "vmsle.vv"},
    {0b011101, FUNCT3_OPIVX, -1, Op::Le, Shape::Compare, S, 1, "vmsle.vx"},
    {0b011101, FUNCT3_OPIVI, -1, Op::Le, Shape::Compare, S, 1, "vmsle.vi"},
    {0b011110, FUNCT3_OPIVX, -1, Op::Gt, Shape::Compare, U, 1, "vmsgtu.vx"},
    {0b011110, FUNCT3_OPIVI, -1, Op::Gt, Shape::Compare, U, 1, "vmsgtu.vi"},
    {0b011111, FUNCT3_OPIVX, -1, Op::Gt, Shape::Compare, S, 1, "vmsgt.vx"},
    {0b011111, FUNCT3_OPIVI, -1, Op::Gt, Shape::Compare, S, 1, "vmsgt.vi"},

    // 11.9 Integer min/max (no .vi forms in the ISA)
    {0b000100, FUNCT3_OPIVV, -1, Op::Min, Shape::SingleWidth, U, 1, "vminu.vv"},
    {0b000100, FUNCT3_OPIVX, -1, Op::Min, Shape::SingleWidth, U, 1, "vminu.vx"},
    {0b000101, FUNCT3_OPIVV, -1, Op::Min, Shape::SingleWidth, S, 1, "vmin.vv"},
    {0b000101, FUNCT3_OPIVX, -1, Op::Min, Shape::SingleWidth, S, 1, "vmin.vx"},
    {0b000110, FUNCT3_OPIVV, -1, Op::Max, Shape::SingleWidth, U, 1, "vmaxu.vv"},
    {0b000110, FUNCT3_OPIVX, -1, Op::Max, Shape::SingleWidth, U, 1, "vmaxu.vx"},
    {0b000111, FUNCT3_OPIVV, -1, Op::Max, Shape::SingleWidth, S, 1, "vmax.vv"},
    {0b000111, FUNCT3_OPIVX, -1, Op::Max, Shape::SingleWidth, S, 1, "vmax.vx"},

    // 11.10 Single-width multiply. vmulhsu: vs2 signed, vs1/rs1 unsigned.
    {0b100101, FUNCT3_OPMVV, -1, Op::Mul, Shape::SingleWidth, S, 1, "vmul.vv"},
    {0b100101, FUNCT3_OPMVX, -1, Op::Mul, Shape::SingleWidth, S, 1, "vmul.vx"},
    {0b100111, FUNCT3_OPMVV, -1, Op::Mulh, Shape::SingleWidth, S, 1, "vmulh.vv"},
    {0b100111, FUNCT3_OPMVX, -1, Op::Mulh, Shape::SingleWidth, S, 1, "vmulh.vx"},
    {0b100100, FUNCT3_OPMVV, -1, Op::Mulh, Shape::SingleWidth, U, 1, "vmulhu.vv"},
    {0b100100, FUNCT3_OPMVX, -1, Op::Mulh, Shape::SingleWidth, U, 1, "vmulhu.vx"},
    {0b100110, FUNCT3_OPMVV, -1, Op::Mulh, Shape::SingleWidth, S, 1, "vmulhsu.vv", 0},
    {0b100110, FUNCT3_OPMVX, -1, Op::Mulh, Shape::SingleWidth, S, 1, "vmulhsu.vx", 0},

    // 11.11 Divide and remainder
    {0b100000, FUNCT3_OPMVV, -1, Op::Div, Shape::SingleWidth, U, 1, "vdivu.vv"},
    {0b100000, FUNCT3_OPMVX, -1, Op::Div, Shape::SingleWidth, U, 1, "vdivu.vx"},
    {0b100001, FUNCT3_OPMVV, -1, Op::Div, Shape::SingleWidth, S, 1, "vdiv.vv"},
    {0b100001, FUNCT3_OPMVX, -1, Op::Div, Shape::SingleWidth, S, 1, "vdiv.vx"},
    {0b100010, FUNCT3_OPMVV, -1, Op::Rem, Shape::SingleWidth, U, 1, "vremu.vv"},
    {0b100010, FUNCT3_OPMVX, -1, Op::Rem, Shape::SingleWidth, U, 1, "vremu.vx"},
    {0b100011, FUNCT3_OPMVV, -1, Op::Rem, Shape::SingleWidth, S, 1, "vrem.vv"},
    {0b100011, FUNCT3_OPMVX, -1, Op::Rem, Shape::SingleWidth, S, 1, "vrem.vx"},

    // 11.12 Widening multiply (the full 2*SEW product). vwmulsu: vs2 signed, vs1 unsigned.
    {0b111011, FUNCT3_OPMVV, -1, Op::Mul, Shape::Widening, S, 1, "vwmul.vv"},
    {0b111011, FUNCT3_OPMVX, -1, Op::Mul, Shape::Widening, S, 1, "vwmul.vx"},
    {0b111000, FUNCT3_OPMVV, -1, Op::Mul, Shape::Widening, U, 1, "vwmulu.vv"},
    {0b111000, FUNCT3_OPMVX, -1, Op::Mul, Shape::Widening, U, 1, "vwmulu.vx"},
    {0b111010, FUNCT3_OPMVV, -1, Op::Mul, Shape::Widening, S, 1, "vwmulsu.vv", 0},
    {0b111010, FUNCT3_OPMVX, -1, Op::Mul, Shape::Widening, S, 1, "vwmulsu.vx", 0},

    // 11.13 Single-width multiply-add (assembly order: vd, vs1/rs1, vs2)
    {0b101101, FUNCT3_OPMVV, -1, Op::Macc, Shape::SingleWidth, S, 1, "vmacc.vv"},
    {0b101101, FUNCT3_OPMVX, -1, Op::Macc, Shape::SingleWidth, S, 1, "vmacc.vx"},
    {0b101111, FUNCT3_OPMVV, -1, Op::Nmsac, Shape::SingleWidth, S, 1, "vnmsac.vv"},
    {0b101111, FUNCT3_OPMVX, -1, Op::Nmsac, Shape::SingleWidth, S, 1, "vnmsac.vx"},
    {0b101001, FUNCT3_OPMVV, -1, Op::Madd, Shape::SingleWidth, S, 1, "vmadd.vv"},
    {0b101001, FUNCT3_OPMVX, -1, Op::Madd, Shape::SingleWidth, S, 1, "vmadd.vx"},
    {0b101011, FUNCT3_OPMVV, -1, Op::Nmsub, Shape::SingleWidth, S, 1, "vnmsub.vv"},
    {0b101011, FUNCT3_OPMVX, -1, Op::Nmsub, Shape::SingleWidth, S, 1, "vnmsub.vx"},

    // 11.14 Widening multiply-add. isSigned is vs2's signedness, sourceSigned vs1/rs1's.
    {0b111100, FUNCT3_OPMVV, -1, Op::Macc, Shape::Widening, U, 1, "vwmaccu.vv"},
    {0b111100, FUNCT3_OPMVX, -1, Op::Macc, Shape::Widening, U, 1, "vwmaccu.vx"},
    {0b111101, FUNCT3_OPMVV, -1, Op::Macc, Shape::Widening, S, 1, "vwmacc.vv"},
    {0b111101, FUNCT3_OPMVX, -1, Op::Macc, Shape::Widening, S, 1, "vwmacc.vx"},
    {0b111111, FUNCT3_OPMVV, -1, Op::Macc, Shape::Widening, U, 1, "vwmaccsu.vv", 1},
    {0b111111, FUNCT3_OPMVX, -1, Op::Macc, Shape::Widening, U, 1, "vwmaccsu.vx", 1},
    {0b111110, FUNCT3_OPMVX, -1, Op::Macc, Shape::Widening, S, 1, "vwmaccus.vx", 0},

    // 11.15 / 11.16 Merge (vm = 0) and move (vm = 1, vs2 = v0) share funct6 010111.
    {0b010111, FUNCT3_OPIVV, -1, Op::Merge, Shape::SingleWidth, S, 1, "vmerge.vvm", -1, 0},
    {0b010111, FUNCT3_OPIVX, -1, Op::Merge, Shape::SingleWidth, S, 1, "vmerge.vxm", -1, 0},
    {0b010111, FUNCT3_OPIVI, -1, Op::Merge, Shape::SingleWidth, S, 1, "vmerge.vim", -1, 0},
    {0b010111, FUNCT3_OPIVV, -1, Op::Move, Shape::SingleWidth, S, 1, "vmv.v.v", -1, 1},
    {0b010111, FUNCT3_OPIVX, -1, Op::Move, Shape::SingleWidth, S, 1, "vmv.v.x", -1, 1},
    {0b010111, FUNCT3_OPIVI, -1, Op::Move, Shape::SingleWidth, S, 1, "vmv.v.i", -1, 1},

    // 11.4 Add-with-carry / subtract-with-borrow. Operands are unsigned SEW values (the .vim
    // simm5 is sign-extended, then truncated to SEW). vadc/vsbc with vm = 1 are reserved.
    {0b010000, FUNCT3_OPIVV, -1, Op::Adc, Shape::SingleWidth, U, 1, "vadc.vvm", -1, 0},
    {0b010000, FUNCT3_OPIVX, -1, Op::Adc, Shape::SingleWidth, U, 1, "vadc.vxm", -1, 0},
    {0b010000, FUNCT3_OPIVI, -1, Op::Adc, Shape::SingleWidth, U, 1, "vadc.vim", -1, 0},
    {0b010001, FUNCT3_OPIVV, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vvm", -1, 0},
    {0b010001, FUNCT3_OPIVX, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vxm", -1, 0},
    {0b010001, FUNCT3_OPIVI, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vim", -1, 0},
    {0b010001, FUNCT3_OPIVV, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vv", -1, 1},
    {0b010001, FUNCT3_OPIVX, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vx", -1, 1},
    {0b010001, FUNCT3_OPIVI, -1, Op::Madc, Shape::Compare, U, 1, "vmadc.vi", -1, 1},
    {0b010010, FUNCT3_OPIVV, -1, Op::Sbc, Shape::SingleWidth, U, 1, "vsbc.vvm", -1, 0},
    {0b010010, FUNCT3_OPIVX, -1, Op::Sbc, Shape::SingleWidth, U, 1, "vsbc.vxm", -1, 0},
    {0b010011, FUNCT3_OPIVV, -1, Op::Msbc, Shape::Compare, U, 1, "vmsbc.vvm", -1, 0},
    {0b010011, FUNCT3_OPIVX, -1, Op::Msbc, Shape::Compare, U, 1, "vmsbc.vxm", -1, 0},
    {0b010011, FUNCT3_OPIVV, -1, Op::Msbc, Shape::Compare, U, 1, "vmsbc.vv", -1, 1},
    {0b010011, FUNCT3_OPIVX, -1, Op::Msbc, Shape::Compare, U, 1, "vmsbc.vx", -1, 1},

    // 16 Permutation. vmv.x.s / vmv.s.x, vcompress and vmv<nr>r.v exist only unmasked (vm = 1).
    // vmv<nr>r.v keeps NREG - 1 in the vs1 field (0, 1, 3, 7; other values are reserved).
    {0b010000, FUNCT3_OPMVV, 0, Op::MoveXS, Shape::Permute, S, 1, "vmv.x.s", -1, 1},
    {0b010000, FUNCT3_OPMVX, -1, Op::MoveSX, Shape::Permute, U, 1, "vmv.s.x", -1, 1},
    {0b001110, FUNCT3_OPIVX, -1, Op::SlideUp, Shape::Permute, U, 1, "vslideup.vx"},
    {0b001110, FUNCT3_OPIVI, -1, Op::SlideUp, Shape::Permute, U, 1, "vslideup.vi"},
    {0b001111, FUNCT3_OPIVX, -1, Op::SlideDown, Shape::Permute, U, 1, "vslidedown.vx"},
    {0b001111, FUNCT3_OPIVI, -1, Op::SlideDown, Shape::Permute, U, 1, "vslidedown.vi"},
    {0b001110, FUNCT3_OPMVX, -1, Op::Slide1Up, Shape::Permute, U, 1, "vslide1up.vx"},
    {0b001111, FUNCT3_OPMVX, -1, Op::Slide1Down, Shape::Permute, U, 1, "vslide1down.vx"},
    {0b001100, FUNCT3_OPIVV, -1, Op::Gather, Shape::Permute, U, 1, "vrgather.vv"},
    {0b001100, FUNCT3_OPIVX, -1, Op::Gather, Shape::Permute, U, 1, "vrgather.vx"},
    {0b001100, FUNCT3_OPIVI, -1, Op::Gather, Shape::Permute, U, 1, "vrgather.vi"},
    {0b001110, FUNCT3_OPIVV, -1, Op::GatherEi16, Shape::Permute, U, 1, "vrgatherei16.vv"},
    {0b010111, FUNCT3_OPMVV, -1, Op::Compress, Shape::Permute, U, 1, "vcompress.vm", -1, 1},
    {0b100111, FUNCT3_OPIVI, 0, Op::WholeMove, Shape::Permute, U, 1, "vmv1r.v", -1, 1},
    {0b100111, FUNCT3_OPIVI, 1, Op::WholeMove, Shape::Permute, U, 2, "vmv2r.v", -1, 1},
    {0b100111, FUNCT3_OPIVI, 3, Op::WholeMove, Shape::Permute, U, 4, "vmv4r.v", -1, 1},
    {0b100111, FUNCT3_OPIVI, 7, Op::WholeMove, Shape::Permute, U, 8, "vmv8r.v", -1, 1},

    // 14.1 / 14.2 Integer reductions: vd[0] = vs1[0] op vs2[0..vl-1] (active elements only).
    // isSigned picks signed min/max, and sign- vs zero-extension for the widening sums.
    {0b000000, FUNCT3_OPMVV, -1, Op::Add, Shape::Reduction, U, 1, "vredsum.vs"},
    {0b000001, FUNCT3_OPMVV, -1, Op::And, Shape::Reduction, U, 1, "vredand.vs"},
    {0b000010, FUNCT3_OPMVV, -1, Op::Or, Shape::Reduction, U, 1, "vredor.vs"},
    {0b000011, FUNCT3_OPMVV, -1, Op::Xor, Shape::Reduction, U, 1, "vredxor.vs"},
    {0b000100, FUNCT3_OPMVV, -1, Op::Min, Shape::Reduction, U, 1, "vredminu.vs"},
    {0b000101, FUNCT3_OPMVV, -1, Op::Min, Shape::Reduction, S, 1, "vredmin.vs"},
    {0b000110, FUNCT3_OPMVV, -1, Op::Max, Shape::Reduction, U, 1, "vredmaxu.vs"},
    {0b000111, FUNCT3_OPMVV, -1, Op::Max, Shape::Reduction, S, 1, "vredmax.vs"},
    {0b110000, FUNCT3_OPIVV, -1, Op::Add, Shape::WideningReduction, U, 1, "vwredsumu.vs"},
    {0b110001, FUNCT3_OPIVV, -1, Op::Add, Shape::WideningReduction, S, 1, "vwredsum.vs"},

    // 15 Mask instructions. The .mm logical ops exist only unmasked (vm = 1). vcpop/vfirst
    // (VWXUNARY0) and vmsbf/vmsof/vmsif/viota/vid (VMUNARY0) pick the op with the vs1 field.
    {0b011000, FUNCT3_OPMVV, -1, Op::MaskAndn, Shape::Mask, U, 1, "vmandn.mm", -1, 1},
    {0b011001, FUNCT3_OPMVV, -1, Op::MaskAnd, Shape::Mask, U, 1, "vmand.mm", -1, 1},
    {0b011010, FUNCT3_OPMVV, -1, Op::MaskOr, Shape::Mask, U, 1, "vmor.mm", -1, 1},
    {0b011011, FUNCT3_OPMVV, -1, Op::MaskXor, Shape::Mask, U, 1, "vmxor.mm", -1, 1},
    {0b011100, FUNCT3_OPMVV, -1, Op::MaskOrn, Shape::Mask, U, 1, "vmorn.mm", -1, 1},
    {0b011101, FUNCT3_OPMVV, -1, Op::MaskNand, Shape::Mask, U, 1, "vmnand.mm", -1, 1},
    {0b011110, FUNCT3_OPMVV, -1, Op::MaskNor, Shape::Mask, U, 1, "vmnor.mm", -1, 1},
    {0b011111, FUNCT3_OPMVV, -1, Op::MaskXnor, Shape::Mask, U, 1, "vmxnor.mm", -1, 1},
    {0b010000, FUNCT3_OPMVV, 0b10000, Op::Cpop, Shape::Mask, U, 1, "vcpop.m"},
    {0b010000, FUNCT3_OPMVV, 0b10001, Op::First, Shape::Mask, U, 1, "vfirst.m"},
    {0b010100, FUNCT3_OPMVV, 0b00001, Op::Sbf, Shape::Mask, U, 1, "vmsbf.m"},
    {0b010100, FUNCT3_OPMVV, 0b00010, Op::Sof, Shape::Mask, U, 1, "vmsof.m"},
    {0b010100, FUNCT3_OPMVV, 0b00011, Op::Sif, Shape::Mask, U, 1, "vmsif.m"},
    {0b010100, FUNCT3_OPMVV, 0b10000, Op::Iota, Shape::Mask, U, 1, "viota.m"},
    {0b010100, FUNCT3_OPMVV, 0b10001, Op::Id, Shape::Mask, U, 1, "vid.v"},
};
} // namespace

bool HasUnsignedImmediate(const VectorArithInfo &info)
{
    return info.op == VectorArithOp::Sll || info.op == VectorArithOp::Srl ||
           info.op == VectorArithOp::Sra || info.op == VectorArithOp::SlideUp ||
           info.op == VectorArithOp::SlideDown || info.op == VectorArithOp::Gather;
}

const VectorArithInfo *DecodeVectorArith(uint32_t instruction)
{
    if ((instruction & 0x7F) != OPCODE_OP_V)
    {
        return nullptr;
    }
    const uint32_t funct3 = (instruction >> 12) & 0x7;
    const uint32_t funct6 = (instruction >> 26) & 0x3F;
    const int vs1 = static_cast<int>((instruction >> 15) & 0x1F);
    const int vm = static_cast<int>((instruction >> 25) & 1);
    for (const VectorArithInfo &info : kVectorArith)
    {
        if (info.funct6 == funct6 && info.funct3 == funct3 &&
            (info.vs1Field < 0 || info.vs1Field == vs1) &&
            (info.vmField < 0 || info.vmField == vm))
        {
            return &info;
        }
    }
    return nullptr;
}

const VectorArithInfo *FindVectorArith(const std::string &mnemonic)
{
    for (const VectorArithInfo &info : kVectorArith)
    {
        if (mnemonic == info.mnemonic)
        {
            return &info;
        }
    }
    return nullptr;
}

std::string VectorArithMnemonic(uint32_t instruction)
{
    const VectorArithInfo *info = DecodeVectorArith(instruction);
    return info ? info->mnemonic : "";
}

uint64_t VsetImmediateVtype(uint32_t instruction)
{
    switch (DecodeVsetKind(instruction))
    {
    case VsetKind::Vsetvli:
        return (instruction >> 20) & 0x7FF; // zimm[10:0]
    case VsetKind::Vsetivli:
        return (instruction >> 20) & 0x3FF; // zimm[9:0]
    default:
        return 0;
    }
}

bool DecodeVtype(uint64_t vtype, Vtype &out)
{
    if ((vtype >> 8) != 0)
    {
        return false; // reserved bits, including vill itself
    }

    const uint64_t vlmul = vtype & 0b111;
    const uint64_t vsew = (vtype >> 3) & 0b111;
    if (vlmul == 0b100)
    {
        return false; // reserved LMUL encoding
    }

    // vlmul is a 3-bit signed log2(LMUL): 000=m1 ... 011=m8, 111=mf2, 110=mf4, 101=mf8.
    const int lmulLog2 = (vlmul & 0b100) ? static_cast<int>(vlmul) - 8 : static_cast<int>(vlmul);
    const uint64_t sew = 8ULL << vsew;

    // SEW must fit in LMUL * ELEN when LMUL < 1, and in ELEN otherwise.
    const uint64_t maxSew = lmulLog2 < 0 ? (ELEN >> -lmulLog2) : ELEN;
    if (sew > maxSew)
    {
        return false;
    }

    out.sew = sew;
    out.lmulLog2 = lmulLog2;
    out.tailAgnostic = (vtype >> 6) & 1;
    out.maskAgnostic = (vtype >> 7) & 1;
    return true;
}

uint64_t Vlmax(const Vtype &vtype, uint64_t vlenBits)
{
    const uint64_t elementsPerRegister = vlenBits / vtype.sew;
    return vtype.lmulLog2 >= 0 ? elementsPerRegister << vtype.lmulLog2
                               : elementsPerRegister >> -vtype.lmulLog2;
}

VsetResult ComputeVset(uint64_t requestedVtype, uint64_t avl, bool rdIsX0, bool rs1IsX0,
                       uint64_t currentVl, uint64_t vlenBits)
{
    VsetResult result;
    Vtype decoded;
    if (!DecodeVtype(requestedVtype, decoded))
    {
        result.vtype = RegisterFile::VTYPE_VILL;
        result.vl = 0;
        return result;
    }

    result.vtype = requestedVtype;
    const uint64_t vlmax = Vlmax(decoded, vlenBits);

    if (!rs1IsX0)
    {
        result.vl = std::min(avl, vlmax); // normal case: AVL from rs1 or uimm
    }
    else if (!rdIsX0)
    {
        result.vl = vlmax; // rs1 = x0, rd != x0: AVL = ~0, so vl = VLMAX
    }
    else
    {
        // rd = rs1 = x0: keep vl. The spec reserves this when VLMAX changes; Spike caps vl to
        // the new VLMAX, and so do we.
        result.vl = std::min(currentVl, vlmax);
    }
    return result;
}

uint64_t EncodeVtype(uint64_t sew, int lmulLog2, bool tailAgnostic, bool maskAgnostic)
{
    uint64_t vsew = 0;
    while ((8ULL << vsew) < sew)
    {
        ++vsew;
    }
    const uint64_t vlmul = static_cast<uint64_t>(lmulLog2) & 0b111;
    return vlmul | (vsew << 3) | (static_cast<uint64_t>(tailAgnostic) << 6) |
           (static_cast<uint64_t>(maskAgnostic) << 7);
}

std::string FormatVtype(uint64_t vtype)
{
    Vtype decoded;
    if (!DecodeVtype(vtype, decoded))
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(vtype));
        return buf;
    }

    std::string lmul = decoded.lmulLog2 >= 0 ? "m" + std::to_string(1 << decoded.lmulLog2)
                                             : "mf" + std::to_string(1 << -decoded.lmulLog2);
    return "e" + std::to_string(decoded.sew) + ", " + lmul + ", " +
           (decoded.tailAgnostic ? "ta" : "tu") + ", " + (decoded.maskAgnostic ? "ma" : "mu");
}

bool ParseSewName(const std::string &name, uint64_t &sew)
{
    if (name == "e8" || name == "e16" || name == "e32" || name == "e64")
    {
        sew = std::stoull(name.substr(1));
        return true;
    }
    return false;
}

bool ParseLmulName(const std::string &name, int &lmulLog2)
{
    static const std::pair<const char *, int> table[] = {
        {"m1", 0}, {"m2", 1}, {"m4", 2}, {"m8", 3}, {"mf2", -1}, {"mf4", -2}, {"mf8", -3}};
    for (const auto &[text, value] : table)
    {
        if (name == text)
        {
            lmulLog2 = value;
            return true;
        }
    }
    return false;
}

} // namespace Kites::rvv
