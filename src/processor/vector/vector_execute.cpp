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
#include "processor/vector/vector_memory.h"
#include "processor/vector/vector_stages.h"

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
    case VectorOpShape::Permute:
        break; // handled by ExecutePermute()
    case VectorOpShape::Reduction:
    case VectorOpShape::WideningReduction:
        break; // handled by ExecuteReduction()
    case VectorOpShape::Mask:
        break; // handled by ExecuteMask()
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

/**
 * Section 16, permutation instructions. Like the rest of this file, tail and masked-off
 * elements are left undisturbed, and nothing is written when vstart >= vl.
 */
/**
 * Integer reductions (RVV 1.0, 14.1 / 14.2): vd[0] = vs1[0] op vs2[0] op ... op vs2[vl-1],
 * skipping masked-off vs2 elements. vs2 is an LMUL group; vd and vs1 are single registers of
 * which only element 0 is used (2*SEW wide for the widening sums). Only vd[0] is written: the
 * rest of vd is tail and stays undisturbed. With vl = 0 nothing is written.
 *
 * As in Spike, vd may overlap any source (v0 included, even when masked), and a non-zero vstart
 * is illegal.
 */
void ExecuteReduction(const VectorArithInfo &info, uint32_t instruction, RegisterFile &registers,
                      std::vector<RegisterChange> &changes)
{
    const uint32_t vd = (instruction >> 7) & 0x1F;
    const uint32_t vs1 = (instruction >> 15) & 0x1F;
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;

    const Vtype vtype = RequireValidVtype(instruction, registers);
    const uint64_t sew = vtype.sew;
    const bool widening = info.shape == VectorOpShape::WideningReduction;
    const uint64_t accWidth = widening ? 2 * sew : sew; // width of vs1[0] and vd[0]
    if (accWidth > ELEN)
    {
        ThrowIllegal(instruction, "2*SEW = " + std::to_string(accWidth) + " exceeds ELEN = 64");
    }
    RequireAligned(instruction, vs2, vtype.lmulLog2, "vs2");
    if (registers.ReadCsr(RegisterFile::CSR_VSTART) != 0)
    {
        ThrowIllegal(instruction, "vstart must be 0 for a reduction");
    }

    const uint64_t vl = registers.ReadCsr(RegisterFile::CSR_VL);
    if (vl == 0)
    {
        return; // no operation: vd is not written
    }

    // The accumulator and every vs2 element are extended to 64 bits with the instruction's
    // signedness, so one 64-bit operation (and one signed or unsigned compare) is exact.
    uint64_t acc = Extend(registers.ReadVectorElement(vs1, 0, accWidth), accWidth, info.isSigned);
    for (uint64_t i = 0; i < vl; ++i)
    {
        if (masked && !registers.ReadMaskBit(0, i))
        {
            continue; // masked-off elements take no part
        }
        const uint64_t element =
            Extend(registers.ReadVectorElement(vs2, i, sew), sew, info.isSigned);
        switch (info.op)
        {
        case VectorArithOp::Min:
            acc = Compare(VectorArithOp::Lt, element, acc, info.isSigned) ? element : acc;
            break;
        case VectorArithOp::Max:
            acc = Compare(VectorArithOp::Gt, element, acc, info.isSigned) ? element : acc;
            break;
        default: // Add, And, Or, Xor
            acc = ApplyArith(info.op, acc, element, accWidth);
            break;
        }
    }

    DestinationRecorder recorder(registers, vd, 1);
    registers.WriteVectorElement(vd, 0, accWidth, acc & SewMask(accWidth));
    recorder.Record(changes);
}

/**
 * Mask instructions (RVV 1.0, section 15). Mask registers hold one bit per element, read and
 * written with ReadMaskBit / WriteMaskBit. As elsewhere, tail and masked-off bits or elements
 * are left undisturbed.
 *
 *   vm*.mm           vd.mask[i] = f(vs2.mask[i], vs1.mask[i]) for i in [vstart, vl); never masked
 *   vcpop.m/vfirst.m x[rd] = count / first index of the active set bits of vs2 (vstart must be 0)
 *   vmsbf/vmsif/vmsof.m  set-before / including / only the first active set bit (vstart must be 0)
 *   viota.m          vd[i] = number of active set bits of vs2 below i (vstart must be 0)
 *   vid.v            vd[i] = i
 */
void ExecuteMask(const VectorArithInfo &info, uint32_t instruction, RegisterFile &registers,
                 std::vector<RegisterChange> &changes, VectorExecResult &result)
{
    const uint32_t vd = (instruction >> 7) & 0x1F;   // or rd for vcpop/vfirst
    const uint32_t vs1 = (instruction >> 15) & 0x1F; // a mask register only for the .mm ops
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;

    const Vtype vtype = RequireValidVtype(instruction, registers);
    const uint64_t vl = registers.ReadCsr(RegisterFile::CSR_VL);
    const uint64_t vstart = registers.ReadCsr(RegisterFile::CSR_VSTART);
    const auto active = [&](uint64_t i) { return !masked || registers.ReadMaskBit(0, i); };
    const auto requireVstartZero = [&]
    {
        if (vstart != 0)
        {
            ThrowIllegal(instruction, "vstart must be 0 for " + std::string(info.mnemonic));
        }
    };
    const auto requireNotV0WhenMasked = [&]
    {
        if (masked && vd == 0)
        {
            ThrowIllegal(instruction, "a masked instruction cannot write v0");
        }
    };

    switch (info.op)
    {
    case VectorArithOp::MaskAndn:
    case VectorArithOp::MaskAnd:
    case VectorArithOp::MaskOr:
    case VectorArithOp::MaskXor:
    case VectorArithOp::MaskOrn:
    case VectorArithOp::MaskNand:
    case VectorArithOp::MaskNor:
    case VectorArithOp::MaskXnor:
    {
        DestinationRecorder recorder(registers, vd, 1);
        for (uint64_t i = vstart; i < vl; ++i)
        {
            // Both sources are read before vd's bit i is replaced, so vd may be vs1 or vs2.
            const bool a = registers.ReadMaskBit(vs2, i);
            const bool b = registers.ReadMaskBit(vs1, i);
            bool bit = false;
            switch (info.op)
            {
            case VectorArithOp::MaskAndn: bit = a && !b; break;
            case VectorArithOp::MaskAnd:  bit = a && b; break;
            case VectorArithOp::MaskOr:   bit = a || b; break;
            case VectorArithOp::MaskXor:  bit = a != b; break;
            case VectorArithOp::MaskOrn:  bit = a || !b; break;
            case VectorArithOp::MaskNand: bit = !(a && b); break;
            case VectorArithOp::MaskNor:  bit = !(a || b); break;
            default:                      bit = a == b; break; // MaskXnor
            }
            registers.WriteMaskBit(vd, i, bit);
        }
        recorder.Record(changes);
        ClearVstart(registers, changes);
        return;
    }

    case VectorArithOp::Cpop:
    case VectorArithOp::First:
    {
        requireVstartZero();
        uint64_t count = 0;
        uint64_t first = ~0ULL; // -1: no active set bit
        for (uint64_t i = 0; i < vl; ++i)
        {
            if (active(i) && registers.ReadMaskBit(vs2, i))
            {
                if (count == 0)
                {
                    first = i;
                }
                ++count;
            }
        }
        result.writesRd = true;
        result.rdValue = info.op == VectorArithOp::Cpop ? count : first;
        return;
    }

    case VectorArithOp::Sbf:
    case VectorArithOp::Sif:
    case VectorArithOp::Sof:
    {
        requireVstartZero();
        requireNotV0WhenMasked();
        if (vd == vs2)
        {
            ThrowIllegal(instruction, "vd cannot overlap vs2");
        }
        DestinationRecorder recorder(registers, vd, 1);
        bool seenOne = false; // Spike's has_one: an active set bit has been passed
        for (uint64_t i = 0; i < vl; ++i)
        {
            if (!active(i))
            {
                continue; // masked-off: undisturbed
            }
            const bool bitIn = registers.ReadMaskBit(vs2, i);
            bool bitOut = false;
            switch (info.op)
            {
            case VectorArithOp::Sbf: bitOut = !seenOne && !bitIn; break;
            case VectorArithOp::Sif: bitOut = !seenOne; break;
            default:                 bitOut = !seenOne && bitIn; break; // Sof
            }
            seenOne = seenOne || bitIn;
            registers.WriteMaskBit(vd, i, bitOut);
        }
        recorder.Record(changes);
        return;
    }

    case VectorArithOp::Iota:
    {
        requireVstartZero();
        requireNotV0WhenMasked();
        RequireAligned(instruction, vd, vtype.lmulLog2, "vd");
        if (IsOverlapped(vd, GroupRegs(vtype.lmulLog2), vs2, 1))
        {
            ThrowIllegal(instruction, "the vd group cannot overlap the source mask vs2");
        }
        DestinationRecorder recorder(registers, vd, GroupRegs(vtype.lmulLog2));
        uint64_t count = 0;
        for (uint64_t i = 0; i < vl; ++i)
        {
            if (!active(i))
            {
                continue; // masked-off: undisturbed, and its vs2 bit is not counted
            }
            registers.WriteVectorElement(vd, i, vtype.sew, count & SewMask(vtype.sew));
            if (registers.ReadMaskBit(vs2, i))
            {
                ++count;
            }
        }
        recorder.Record(changes);
        return;
    }

    case VectorArithOp::Id:
    {
        if (vs2 != 0)
        {
            ThrowIllegal(instruction, "vid.v is reserved unless its vs2 field is v0");
        }
        requireNotV0WhenMasked();
        RequireAligned(instruction, vd, vtype.lmulLog2, "vd");
        DestinationRecorder recorder(registers, vd, GroupRegs(vtype.lmulLog2));
        for (uint64_t i = vstart; i < vl; ++i)
        {
            if (active(i))
            {
                registers.WriteVectorElement(vd, i, vtype.sew, i & SewMask(vtype.sew));
            }
        }
        recorder.Record(changes);
        ClearVstart(registers, changes);
        return;
    }

    default:
        break;
    }
    ThrowIllegal(instruction, "unsupported mask instruction");
}

void ExecutePermute(const VectorArithInfo &info, uint32_t instruction, uint64_t rs1Value,
                    RegisterFile &registers, std::vector<RegisterChange> &changes,
                    VectorExecResult &result)
{
    const uint32_t vd = (instruction >> 7) & 0x1F;
    const uint32_t field19_15 = (instruction >> 15) & 0x1F; // vs1, rs1, uimm or NREG - 1
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;
    const uint64_t vlenBits = registers.GetVlenBytes() * 8;
    const uint64_t vstart = registers.ReadCsr(RegisterFile::CSR_VSTART);

    // vmv<nr>r.v ignores vtype and vl: it copies NREG whole registers. EEW = SEW, so vstart
    // counts SEW-wide elements (bytes if vtype.vill is set).
    if (info.op == VectorArithOp::WholeMove)
    {
        const uint32_t nreg = info.factor;
        if (vd % nreg != 0 || vs2 % nreg != 0)
        {
            ThrowIllegal(instruction, "vd and vs2 must be aligned to " + std::to_string(nreg) +
                                          " registers");
        }
        Vtype vtype;
        const uint64_t eewBytes =
            DecodeVtype(registers.ReadCsr(RegisterFile::CSR_VTYPE), vtype) ? vtype.sew / 8 : 1;
        const uint64_t totalBytes = nreg * registers.GetVlenBytes();
        DestinationRecorder recorder(registers, vd, nreg);
        if (vd != vs2) // vd == vs2 is a NOP
        {
            for (uint64_t byte = vstart * eewBytes; byte < totalBytes; ++byte)
            {
                registers.WriteVectorElement(vd, byte, 8,
                                             registers.ReadVectorElement(vs2, byte, 8));
            }
        }
        recorder.Record(changes);
        ClearVstart(registers, changes);
        return;
    }

    const Vtype vtype = RequireValidVtype(instruction, registers);
    const uint64_t sew = vtype.sew;
    const int lmul = vtype.lmulLog2;
    const int group = GroupRegs(lmul);
    const uint64_t vlmax = Vlmax(vtype, vlenBits);
    const uint64_t vl = registers.ReadCsr(RegisterFile::CSR_VL);
    const auto active = [&](uint64_t i) { return !masked || registers.ReadMaskBit(0, i); };

    // vmv.x.s and vmv.s.x act on element 0 of a single register, whatever LMUL is.
    if (info.op == VectorArithOp::MoveXS)
    {
        // Runs even when vl = 0 or vstart >= vl. SEW < XLEN: sign-extended to XLEN.
        result.writesRd = true;
        result.rdValue = Extend(registers.ReadVectorElement(vs2, 0, sew), sew, true);
        ClearVstart(registers, changes);
        return;
    }
    if (info.op == VectorArithOp::MoveSX)
    {
        if (vs2 != 0)
        {
            ThrowIllegal(instruction, "vmv.s.x is reserved unless its vs2 field is v0");
        }
        DestinationRecorder recorder(registers, vd, 1);
        if (vstart < vl)
        {
            registers.WriteVectorElement(vd, 0, sew, rs1Value & SewMask(sew)); // low SEW bits
        }
        recorder.Record(changes);
        ClearVstart(registers, changes);
        return;
    }

    if (masked && vd == 0)
    {
        ThrowIllegal(instruction, "a masked instruction cannot write v0");
    }
    RequireAligned(instruction, vd, lmul, "vd");
    RequireAligned(instruction, vs2, lmul, "vs2");

    // The OFFSET of a slide and the index of vrgather.vx/.vi: x[rs1] as an unsigned XLEN value
    // (not truncated to SEW), or the 5-bit uimm zero-extended.
    const uint32_t funct3 = (instruction >> 12) & 0x7;
    const uint64_t scalar = funct3 == FUNCT3_OPIVI ? field19_15 : rs1Value;

    const bool vs1IsVector = info.op == VectorArithOp::GatherEi16 ||
                             info.op == VectorArithOp::Compress ||
                             (info.op == VectorArithOp::Gather && funct3 == FUNCT3_OPIVV);
    int vs1Log2 = lmul;
    if (info.op == VectorArithOp::GatherEi16)
    {
        // The indices are 16 bits wide: EMUL = (16 / SEW) * LMUL.
        int sewLog2 = 0;
        while ((8u << sewLog2) < sew)
        {
            ++sewLog2;
        }
        vs1Log2 = lmul + 1 - sewLog2;
        if (vs1Log2 < -3 || vs1Log2 > 3)
        {
            ThrowIllegal(instruction, "the 16-bit index EMUL would be outside 1/8..8");
        }
    }
    if (info.op == VectorArithOp::Compress)
    {
        vs1Log2 = 0; // the selecting mask is a single register
        if (vstart != 0)
        {
            ThrowIllegal(instruction, "vcompress.vm requires vstart = 0");
        }
    }
    else if (vs1IsVector)
    {
        RequireAligned(instruction, field19_15, vs1Log2, "vs1");
    }

    // Slide-ups, vrgather and vcompress may not overlap their sources at all.
    const bool noVs2Overlap = info.op == VectorArithOp::SlideUp ||
                              info.op == VectorArithOp::Slide1Up ||
                              info.op == VectorArithOp::Gather ||
                              info.op == VectorArithOp::GatherEi16 ||
                              info.op == VectorArithOp::Compress;
    if (noVs2Overlap && IsOverlapped(vd, group, vs2, group))
    {
        ThrowIllegal(instruction, "vd overlaps the vs2 group");
    }
    if (vs1IsVector && IsOverlapped(vd, group, field19_15, GroupRegs(vs1Log2)))
    {
        ThrowIllegal(instruction, "vd overlaps the vs1 group");
    }

    // Sources can be read anywhere below VLMAX (not just vl), so snapshot the vs2 group before
    // writing vd: vslidedown may slide a group onto itself.
    std::vector<uint64_t> src(vlmax);
    for (uint64_t i = 0; i < vlmax; ++i)
    {
        src[i] = registers.ReadVectorElement(vs2, i, sew);
    }
    const uint64_t scalarAtSew = scalar & SewMask(sew); // slide1up/down insert the low SEW bits

    DestinationRecorder recorder(registers, vd, group);
    switch (info.op)
    {
    case VectorArithOp::SlideUp:
        // Elements below OFFSET are unchanged.
        for (uint64_t i = std::max(vstart, scalar); i < vl; ++i)
        {
            if (active(i))
            {
                registers.WriteVectorElement(vd, i, sew, src[i - scalar]);
            }
        }
        break;
    case VectorArithOp::SlideDown:
        for (uint64_t i = vstart; i < vl; ++i)
        {
            if (active(i))
            {
                const bool inRange = scalar < vlmax && i + scalar < vlmax; // else it reads 0
                registers.WriteVectorElement(vd, i, sew, inRange ? src[i + scalar] : 0);
            }
        }
        break;
    case VectorArithOp::Slide1Up:
        for (uint64_t i = vstart; i < vl; ++i)
        {
            if (active(i))
            {
                registers.WriteVectorElement(vd, i, sew, i == 0 ? scalarAtSew : src[i - 1]);
            }
        }
        break;
    case VectorArithOp::Slide1Down:
        for (uint64_t i = vstart; i < vl; ++i)
        {
            if (active(i))
            {
                registers.WriteVectorElement(vd, i, sew,
                                             i + 1 == vl ? scalarAtSew : src[i + 1]);
            }
        }
        break;
    case VectorArithOp::Gather:
    case VectorArithOp::GatherEi16:
        for (uint64_t i = vstart; i < vl; ++i)
        {
            if (!active(i))
            {
                continue;
            }
            uint64_t index = scalar; // .vx / .vi: the same index for every element
            if (info.op == VectorArithOp::GatherEi16)
            {
                index = registers.ReadVectorElement(field19_15, i, 16);
            }
            else if (funct3 == FUNCT3_OPIVV)
            {
                index = registers.ReadVectorElement(field19_15, i, sew);
            }
            registers.WriteVectorElement(vd, i, sew, index < vlmax ? src[index] : 0);
        }
        break;
    case VectorArithOp::Compress:
    {
        uint64_t packed = 0;
        for (uint64_t i = 0; i < vl; ++i)
        {
            if (registers.ReadMaskBit(field19_15, i))
            {
                registers.WriteVectorElement(vd, packed++, sew, src[i]);
            }
        }
        break;
    }
    default:
        ThrowIllegal(instruction, "unsupported permutation instruction");
    }
    recorder.Record(changes);
    ClearVstart(registers, changes);
}
} // namespace

bool IsVectorInstruction(uint32_t instruction)
{
    return (instruction & 0x7F) == OPCODE_OP_V || IsVectorMemoryInstruction(instruction);
}

bool IsSupportedVectorInstruction(uint32_t instruction)
{
    VectorMemFields memFields;
    return DecodeVsetKind(instruction) != VsetKind::None ||
           DecodeVectorArith(instruction) != nullptr || DecodeVectorMem(instruction, memFields);
}

bool VectorInstructionWritesGpr(uint32_t instruction)
{
    if (DecodeVsetKind(instruction) != VsetKind::None)
    {
        return true;
    }
    const VectorArithInfo *info = DecodeVectorArith(instruction);
    // vmv.x.s rd, vs2; vcpop.m rd, vs2; vfirst.m rd, vs2
    return info && (info->op == VectorArithOp::MoveXS || info->op == VectorArithOp::Cpop ||
                    info->op == VectorArithOp::First);
}

VectorExecResult ExecuteVectorCompute(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                                      RegisterFile &registers,
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
        if (info->shape == VectorOpShape::Reduction ||
            info->shape == VectorOpShape::WideningReduction)
        {
            ExecuteReduction(*info, instruction, registers, changes);
            return result;
        }
        if (info->shape == VectorOpShape::Mask)
        {
            ExecuteMask(*info, instruction, registers, changes, result);
            return result;
        }
        if (info->shape == VectorOpShape::Permute)
        {
            ExecutePermute(*info, instruction, rs1Value, registers, changes, result);
            return result;
        }
        ExecuteIntegerArith(*info, instruction, rs1Value, registers, changes);
        return result;
    }
    ThrowIllegal(instruction, "unsupported vector instruction");
}

VectorExecResult ExecuteVectorInstruction(uint32_t instruction, uint64_t rs1Value,
                                          uint64_t rs2Value, RegisterFile &registers,
                                          MemoryController &memory,
                                          std::vector<RegisterChange> &changes,
                                          std::vector<MemoryChange> &memoryChanges)
{
    // The three phases back to back: what the 5-stage VMs do in EX, and the reference the
    // single-cycle VM's split EX / MEM / WB must match.
    thread_local VectorScratch scratch;
    VectorExStage stage =
        VectorExecuteStage(instruction, rs1Value, rs2Value, registers, scratch);
    VectorMemoryStage(stage, memory, memoryChanges);
    return VectorWriteBackStage(stage, registers, changes);
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
