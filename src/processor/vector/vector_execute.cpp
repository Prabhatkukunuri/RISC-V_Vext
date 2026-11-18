/**
 * @file vector_execute.cpp
 * @brief Execution of vector (OP-V) instructions.
 *
 * Policies follow Spike, the golden reference: tail and masked-off elements are left
 * undisturbed even when vtype says agnostic (undisturbed is always a legal agnostic result).
 */

#include "processor/vector/vector_execute.h"

#include "processor/processor_base.h"
#include "processor/registers.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace Kites::rvv
{
namespace
{
[[noreturn]] void ThrowIllegal(uint32_t instruction, const std::string &reason)
{
    char word[16];
    snprintf(word, sizeof(word), "0x%08x", instruction);
    throw std::runtime_error("Illegal instruction " + std::string(word) + ": " + reason);
}

uint64_t SewMask(uint64_t sew)
{
    return sew == 64 ? ~0ULL : (1ULL << sew) - 1;
}

/// The current vtype, or an illegal-instruction exception if vtype.vill is set.
Vtype RequireValidVtype(uint32_t instruction, const RegisterFile &registers)
{
    Vtype vtype;
    if (!DecodeVtype(registers.ReadCsr(RegisterFile::CSR_VTYPE), vtype))
    {
        ThrowIllegal(instruction, "vtype.vill is set (run vsetvli first)");
    }
    return vtype;
}

/// Registers in a group of EMUL = 2^emulLog2 (a fractional EMUL still occupies one register).
int GroupRegs(int emulLog2)
{
    return emulLog2 > 0 ? 1 << emulLog2 : 1;
}

void RequireAligned(uint32_t instruction, uint32_t reg, int emulLog2, const char *operand)
{
    const int group = GroupRegs(emulLog2);
    if (reg % group != 0)
    {
        ThrowIllegal(instruction, std::string(operand) + " v" + std::to_string(reg) +
                                      " is not aligned to a " + std::to_string(group) +
                                      "-register group");
    }
}

// Spike's is_overlapped / is_overlapped_widen, sizes in registers.
bool IsOverlapped(int a, int aSize, int b, int bSize)
{
    return std::max(a + aSize, b + bSize) - std::min(a, b) < aSize + bSize;
}

bool IsOverlappedWiden(int a, int aSize, int b, int bSize)
{
    // A narrower source may sit in the highest-numbered part of the destination group.
    if (a < b && IsOverlapped(a, aSize, b, bSize) && !IsOverlapped(a, aSize, b + bSize, bSize))
    {
        return false;
    }
    return IsOverlapped(a, aSize, b, bSize);
}

/// A destination wider than its source may overlap it only in the destination's
/// highest-numbered part, and only when the source group is at least one whole register.
void RequireWideningOverlapOk(uint32_t instruction, uint32_t vd, int destLog2, uint32_t source,
                              int sourceLog2, const char *operand)
{
    const int destSize = GroupRegs(destLog2);
    const int sourceSize = GroupRegs(sourceLog2);
    const bool overlaps = sourceLog2 < 0
                              ? IsOverlapped(vd, destSize, source, sourceSize)
                              : IsOverlappedWiden(vd, destSize, source, sourceSize);
    if (overlaps)
    {
        ThrowIllegal(instruction, "vd overlaps " + std::string(operand) +
                                      ", which has a different element width");
    }
}

/// value, an element @p bits wide, extended to 64 bits.
uint64_t Extend(uint64_t value, uint64_t bits, bool isSigned)
{
    value &= SewMask(bits);
    if (!isSigned || bits == 64)
    {
        return value;
    }
    const uint64_t signBit = 1ULL << (bits - 1);
    return (value ^ signBit) - signBit;
}

/// Snapshots a destination register group so its changes can be recorded for undo.
class DestinationRecorder
{
  public:
    DestinationRecorder(const RegisterFile &registers, uint32_t first, size_t count)
        : m_registers(registers), m_first(first)
    {
        for (size_t i = 0; i < count; ++i)
        {
            m_before.push_back(registers.ReadVectorRegister(first + i));
        }
    }

    void Record(std::vector<RegisterChange> &changes) const
    {
        for (size_t i = 0; i < m_before.size(); ++i)
        {
            std::vector<uint8_t> after = m_registers.ReadVectorRegister(m_first + i);
            if (after != m_before[i])
            {
                RegisterChange change{static_cast<unsigned int>(m_first + i), 3, 0, 0};
                change.old_bytes = m_before[i];
                change.new_bytes = std::move(after);
                changes.push_back(std::move(change));
            }
        }
    }

  private:
    const RegisterFile &m_registers;
    uint32_t m_first;
    std::vector<std::vector<uint8_t>> m_before;
};

void ClearVstart(RegisterFile &registers, std::vector<RegisterChange> &changes)
{
    const uint64_t old = registers.ReadCsr(RegisterFile::CSR_VSTART);
    if (old != 0)
    {
        registers.WriteCsr(RegisterFile::CSR_VSTART, 0);
        changes.push_back({static_cast<unsigned int>(RegisterFile::CSR_VSTART), 1, old, 0});
    }
}

/// One element's result. @p width is the width the operation works at (2*SEW for a narrowing
/// shift, else the destination width); it bounds the shift amount.
uint64_t ApplyArith(VectorArithOp op, uint64_t vs2Element, uint64_t source, uint64_t width)
{
    // Shifts use only the low log2(width) bits of the shift amount (0..width-1).
    const unsigned shift = static_cast<unsigned>(source & (width - 1));
    switch (op)
    {
    case VectorArithOp::Add:
        return vs2Element + source;
    case VectorArithOp::Sub:
        return vs2Element - source;
    case VectorArithOp::Rsub:
        return source - vs2Element;
    case VectorArithOp::And:
        return vs2Element & source;
    case VectorArithOp::Or:
        return vs2Element | source;
    case VectorArithOp::Xor:
        return vs2Element ^ source;
    case VectorArithOp::Sll:
        return vs2Element << shift;
    case VectorArithOp::Srl:
        return vs2Element >> shift; // vs2Element was zero-extended
    case VectorArithOp::Sra:
        // vs2Element was sign-extended to 64 bits, so an arithmetic shift fills with the sign.
        return static_cast<uint64_t>(static_cast<int64_t>(vs2Element) >> shift);
    case VectorArithOp::Extend:
        return vs2Element;
    case VectorArithOp::Eq:
    case VectorArithOp::Ne:
    case VectorArithOp::Lt:
    case VectorArithOp::Le:
    case VectorArithOp::Gt:
    case VectorArithOp::Mul:
        return vs2Element * source; // low 64 bits: exact for any width <= 64
    default:
        break; // the others need more context: see ElementResult()
    }
    return 0;
}

/// High @p sew bits of the 2*SEW-bit product of a and b (each extended to 64 bits already).
uint64_t MultiplyHigh(uint64_t a, bool aSigned, uint64_t b, bool bSigned, uint64_t sew)
{
    using u128 = unsigned __int128;
    // Two's-complement 128-bit operands: the low 128 bits of the product are exact in every
    // signedness combination, since the true product always fits in 128 bits.
    const auto widen = [](uint64_t x, bool isSigned)
    {
        return isSigned ? static_cast<u128>(static_cast<__int128>(static_cast<int64_t>(x)))
                        : static_cast<u128>(x);
    };
    return static_cast<uint64_t>((widen(a, aSigned) * widen(b, bSigned)) >> sew);
}

/// Carry out of a + b + carryIn (Madc), or borrow out of a - b - carryIn (Msbc): whether the
/// exact result, before truncation to @p sew bits, overflows or is negative. a and b are
/// zero-extended SEW values.
bool CarryOut(VectorArithOp op, uint64_t a, uint64_t b, uint64_t carryIn, uint64_t sew)
{
    using u128 = unsigned __int128;
    if (op == VectorArithOp::Msbc)
    {
        return static_cast<u128>(a) < static_cast<u128>(b) + carryIn;
    }
    return ((static_cast<u128>(a) + b + carryIn) >> sew) != 0;
}

/// Division and remainder with the scalar M-extension results for the edge cases:
/// x / 0 = all ones, x % 0 = x, and for signed MIN / -1 = MIN, MIN % -1 = 0.
uint64_t DivRem(bool isRem, uint64_t a, uint64_t b, bool isSigned)
{
    if (b == 0)
    {
        return isRem ? a : ~0ULL;
    }
    if (!isSigned)
    {
        return isRem ? a % b : a / b;
    }
    const auto sa = static_cast<int64_t>(a);
    const auto sb = static_cast<int64_t>(b);
    if (sb == -1)
    {
        // -MIN wraps back to MIN, at any SEW once the result is truncated; avoids the C++ UB.
        return isRem ? 0 : 0 - a;
    }
    return static_cast<uint64_t>(isRem ? sa % sb : sa / sb);
}

/// A compare's mask bit. Both operands were already sign- or zero-extended to 64 bits.
bool Compare(VectorArithOp op, uint64_t a, uint64_t b, bool isSigned)
{
    const auto less = [&](uint64_t x, uint64_t y)
    { return isSigned ? static_cast<int64_t>(x) < static_cast<int64_t>(y) : x < y; };
    switch (op)
    {
    case VectorArithOp::Eq:
        return a == b;
    case VectorArithOp::Ne:
        return a != b;
    case VectorArithOp::Lt:
        return less(a, b);
    case VectorArithOp::Le:
        return !less(b, a);
    case VectorArithOp::Gt:
        return less(b, a);
    default:
        return false;
    }
}

/// Element width (bits) and register group size (log2 EMUL) of one operand.
struct OperandLayout
{
    uint64_t eew;
    int emulLog2;
};

/**
 * vd[i] = op(vs2[i], vs1[i] | x[rs1] | simm5) for the active elements in [vstart, vl), with the
 * operand widths given by info.shape:
 *   SingleWidth  vd SEW   = vs2 SEW   op src SEW
 *   Widening     vd 2SEW  = vs2 SEW   op src SEW   (narrow operands extended first)
 *   WideningW    vd 2SEW  = vs2 2SEW  op src SEW
 *   Extension    vd SEW   = extend(vs2 SEW/factor)
 *   Narrowing    vd SEW   = vs2 2SEW  op src SEW   (result truncated to SEW)
 *   Compare      vd mask  = vs2 SEW   op src SEW   (one bit per element, a single register)
 */
void ExecuteIntegerArith(const VectorArithInfo &info, uint32_t instruction, uint64_t rs1Value,
                         RegisterFile &registers, std::vector<RegisterChange> &changes)
{
    const uint32_t vd = (instruction >> 7) & 0x1F;
    const uint32_t field19_15 = (instruction >> 15) & 0x1F; // vs1, rs1, simm5 or selector
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;
    const bool sourceIsVector = info.vs1Field < 0 && (info.funct3 == FUNCT3_OPIVV ||
                                                      info.funct3 == FUNCT3_OPMVV);
    const bool hasSource = info.shape != VectorOpShape::Extension;
    const bool isCompare = info.shape == VectorOpShape::Compare;
    const bool sourceSigned = SourceIsSigned(info);

    const Vtype vtype = RequireValidVtype(instruction, registers);
    const uint64_t sew = vtype.sew;
    const int lmul = vtype.lmulLog2;

    OperandLayout dest{sew, lmul};
    OperandLayout src2{sew, lmul};
    const OperandLayout src1{sew, lmul};
    switch (info.shape)
    {
    case VectorOpShape::SingleWidth:
        break;
    case VectorOpShape::Widening:
        dest = {2 * sew, lmul + 1};
        break;
    case VectorOpShape::WideningW:
        dest = {2 * sew, lmul + 1};
        src2 = {2 * sew, lmul + 1};
        break;
    case VectorOpShape::Extension:
    {
        int factorLog2 = 0;
        while ((1u << factorLog2) < info.factor)
        {
            ++factorLog2;
        }
        src2 = {sew / info.factor, lmul - factorLog2};
        break;
    }
    case VectorOpShape::Narrowing:
        src2 = {2 * sew, lmul + 1};
        break;
    case VectorOpShape::Compare:
        dest = {1, 0}; // a mask: one register, whatever LMUL is
        break;
    }
    const uint64_t opWidth = std::max(dest.eew, src2.eew);

    // Widths and group sizes this implementation can't hold (Spike's checks).
    if (opWidth > ELEN)
    {
        ThrowIllegal(instruction, "2*SEW = " + std::to_string(opWidth) + " exceeds ELEN = 64");
    }
    if (dest.emulLog2 > 3 || src2.emulLog2 > 3)
    {
        ThrowIllegal(instruction, "a wide operand's EMUL would exceed 8");
    }
    if (src2.eew < 8 || src2.emulLog2 < -3)
    {
        ThrowIllegal(instruction, "source EEW SEW/" + std::to_string(info.factor) +
                                      " is below 8 bits or EMUL below 1/8");
    }

    // A compare may write its mask into v0 (bit i of v0 is read before it is replaced).
    if (masked && vd == 0 && !isCompare)
    {
        ThrowIllegal(instruction, "a masked instruction cannot write v0");
    }
    if (info.op == VectorArithOp::Move && vs2 != 0)
    {
        ThrowIllegal(instruction, "vmv.v.* is reserved unless its vs2 field is v0");
    }
    RequireAligned(instruction, vd, dest.emulLog2, "vd");
    RequireAligned(instruction, vs2, src2.emulLog2, "vs2");
    if (sourceIsVector)
    {
        RequireAligned(instruction, field19_15, src1.emulLog2, "vs1");
    }

    // Overlap between operands of different element widths.
    if (info.shape == VectorOpShape::Extension && vd == vs2)
    {
        ThrowIllegal(instruction, "vd cannot be the same register as vs2");
    }
    if (isCompare)
    {
        // The mask may be vs2 / vs1 itself (their lowest register) but not elsewhere in a group.
        if (vd != vs2 && IsOverlapped(vd, 1, vs2, GroupRegs(src2.emulLog2)))
        {
            ThrowIllegal(instruction, "the mask vd overlaps the vs2 group other than at its start");
        }
        if (sourceIsVector && vd != field19_15 &&
            IsOverlapped(vd, 1, field19_15, GroupRegs(src1.emulLog2)))
        {
            ThrowIllegal(instruction, "the mask vd overlaps the vs1 group other than at its start");
        }
    }
    else if (info.shape == VectorOpShape::Narrowing)
    {
        // vd may only overlap the lowest-numbered part of the wide vs2 group (Spike: vd == vs2).
        if (vd != vs2 &&
            IsOverlapped(vd, GroupRegs(dest.emulLog2), vs2, GroupRegs(src2.emulLog2)))
        {
            ThrowIllegal(instruction, "vd overlaps the wide vs2 group other than at its start");
        }
    }
    else if (dest.eew != src2.eew)
    {
        RequireWideningOverlapOk(instruction, vd, dest.emulLog2, vs2, src2.emulLog2, "vs2");
    }
    if (!isCompare && sourceIsVector && dest.eew != src1.eew)
    {
        RequireWideningOverlapOk(instruction, vd, dest.emulLog2, field19_15, src1.emulLog2,
                                 "vs1");
    }

    // The scalar operand: x[rs1], uimm (shifts) or simm5 sign-extended, used at SEW bits.
    uint64_t scalar = 0;
    if (info.funct3 == FUNCT3_OPIVX || info.funct3 == FUNCT3_OPMVX)
    {
        scalar = rs1Value;
    }
    else if (info.funct3 == FUNCT3_OPIVI && HasUnsignedImmediate(info))
    {
        scalar = field19_15; // uimm: 0..31, zero-extended
    }
    else if (info.funct3 == FUNCT3_OPIVI)
    {
        const int32_t simm5 = static_cast<int32_t>(field19_15 << 27) >> 27;
        scalar = static_cast<uint64_t>(static_cast<int64_t>(simm5));
    }

    const uint64_t vl = registers.ReadCsr(RegisterFile::CSR_VL);
    const uint64_t vstart = registers.ReadCsr(RegisterFile::CSR_VSTART);

    DestinationRecorder recorder(registers, vd, GroupRegs(dest.emulLog2));
    for (uint64_t i = vstart; i < vl; ++i)
    {
        // vmerge and vadc & co. (fixed vm = 0) read v0 as data and write every body element.
        const bool maskBit = masked ? registers.ReadMaskBit(0, i) : true;
        if (masked && !maskBit && info.vmField != 0)
        {
            continue; // masked-off: undisturbed
        }
        // Narrow operands are sign- or zero-extended to the destination width first.
        const uint64_t a =
            Extend(registers.ReadVectorElement(vs2, i, src2.eew), src2.eew, info.isSigned);
        uint64_t b = 0;
        if (hasSource)
        {
            const uint64_t raw =
                sourceIsVector ? registers.ReadVectorElement(field19_15, i, src1.eew) : scalar;
            b = Extend(raw, src1.eew, sourceSigned);
        }
        // Carry / borrow in: v0's bit when the instruction reads v0 (vadc, vmadc.vvm, ...).
        const uint64_t carryIn = masked && maskBit ? 1 : 0;
        if (info.op == VectorArithOp::Madc || info.op == VectorArithOp::Msbc)
        {
            registers.WriteMaskBit(vd, i, CarryOut(info.op, a, b, carryIn, sew));
            continue;
        }
        if (isCompare)
        {
            registers.WriteMaskBit(vd, i, Compare(info.op, a, b, info.isSigned));
            continue;
        }

        uint64_t result = 0;
        switch (info.op)
        {
        case VectorArithOp::Min:
        case VectorArithOp::Max:
        {
            // a and b were sign- or zero-extended per isSigned, so one 64-bit compare decides.
            const bool aIsLess = Compare(VectorArithOp::Lt, a, b, info.isSigned);
            result = (info.op == VectorArithOp::Min) == aIsLess ? a : b;
            break;
        }
        case VectorArithOp::Mulh:
            result = MultiplyHigh(a, info.isSigned, b, sourceSigned, sew);
            break;
        case VectorArithOp::Div:
        case VectorArithOp::Rem:
            result = DivRem(info.op == VectorArithOp::Rem, a, b, info.isSigned);
            break;
        case VectorArithOp::Macc:
        case VectorArithOp::Nmsac:
        case VectorArithOp::Madd:
        case VectorArithOp::Nmsub:
        {
            // Destructive: vd is also an operand (the addend or a multiplicand).
            const uint64_t old = registers.ReadVectorElement(vd, i, dest.eew);
            switch (info.op)
            {
            case VectorArithOp::Macc:
                result = old + b * a;
                break;
            case VectorArithOp::Nmsac:
                result = old - b * a;
                break;
            case VectorArithOp::Madd:
                result = b * old + a;
                break;
            default: // Nmsub
                result = a - b * old;
                break;
            }
            break;
        }
        case VectorArithOp::Merge:
            result = maskBit ? b : a;
            break;
        case VectorArithOp::Move:
            result = b;
            break;
        case VectorArithOp::Adc:
            result = a + b + carryIn;
            break;
        case VectorArithOp::Sbc:
            result = a - b - carryIn;
            break;
        default:
            result = ApplyArith(info.op, a, b, opWidth);
            break;
        }
        registers.WriteVectorElement(vd, i, dest.eew, result & SewMask(dest.eew));
    }
    recorder.Record(changes);
    ClearVstart(registers, changes);
}
} // namespace

bool IsSupportedVectorInstruction(uint32_t instruction)
{
    return DecodeVsetKind(instruction) != VsetKind::None ||
           DecodeVectorArith(instruction) != nullptr;
}

bool VectorInstructionWritesGpr(uint32_t instruction)
{
    return DecodeVsetKind(instruction) != VsetKind::None;
}

VectorExecResult ExecuteVectorInstruction(uint32_t instruction, uint64_t rs1Value,
                                          uint64_t rs2Value, RegisterFile &registers,
                                          std::vector<RegisterChange> &changes)
{
    VectorExecResult result;
    if (DecodeVsetKind(instruction) != VsetKind::None)
    {
        const VsetResult vset = ExecuteVset(instruction, rs1Value, rs2Value, registers);
        CommitVsetCsrs(registers, vset, changes);
        result.writesRd = true;
        result.rdValue = vset.vl;
        return result;
    }
    if (const VectorArithInfo *info = DecodeVectorArith(instruction))
    {
        ExecuteIntegerArith(*info, instruction, rs1Value, registers, changes);
        return result;
    }
    ThrowIllegal(instruction, "unsupported vector instruction");
}

VsetResult ExecuteVset(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                       const RegisterFile &registers)
{
    const uint32_t rd = (instruction >> 7) & 0x1F;
    const uint32_t rs1 = (instruction >> 15) & 0x1F;
    const uint64_t vlenBits = registers.GetVlenBytes() * 8;
    const uint64_t currentVl = registers.ReadCsr(RegisterFile::CSR_VL);

    switch (DecodeVsetKind(instruction))
    {
    case VsetKind::Vsetvli:
        return ComputeVset(VsetImmediateVtype(instruction), rs1Value, rd == 0, rs1 == 0, currentVl,
                           vlenBits);
    case VsetKind::Vsetivli:
        // The rs1 field holds the AVL itself, so even uimm = 0 is a real AVL of 0.
        return ComputeVset(VsetImmediateVtype(instruction), rs1, rd == 0, false, currentVl,
                           vlenBits);
    case VsetKind::Vsetvl:
        return ComputeVset(rs2Value, rs1Value, rd == 0, rs1 == 0, currentVl, vlenBits);
    case VsetKind::None:
        break;
    }
    ThrowIllegal(instruction, "not a vset* instruction");
}

void CommitVsetCsrs(RegisterFile &registers, const VsetResult &result,
                    std::vector<RegisterChange> &changes)
{
    const std::pair<size_t, uint64_t> writes[] = {
        {RegisterFile::CSR_VL, result.vl},
        {RegisterFile::CSR_VTYPE, result.vtype},
        {RegisterFile::CSR_VSTART, 0},
    };
    for (const auto &[csr, value] : writes)
    {
        const uint64_t old = registers.ReadCsr(csr);
        if (old != value)
        {
            registers.WriteCsr(csr, value);
            changes.push_back({static_cast<unsigned int>(csr), 1, old, value});
        }
    }
}

} // namespace Kites::rvv
