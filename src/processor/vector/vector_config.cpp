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
};
} // namespace

const VectorArithInfo *DecodeVectorArith(uint32_t instruction)
{
    if ((instruction & 0x7F) != OPCODE_OP_V)
    {
        return nullptr;
    }
    const uint32_t funct3 = (instruction >> 12) & 0x7;
    const uint32_t funct6 = (instruction >> 26) & 0x3F;
    const int vs1 = static_cast<int>((instruction >> 15) & 0x1F);
    for (const VectorArithInfo &info : kVectorArith)
    {
        if (info.funct6 == funct6 && info.funct3 == funct3 &&
            (info.vs1Field < 0 || info.vs1Field == vs1))
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
