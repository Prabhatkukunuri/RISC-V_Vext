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

uint64_t ApplyArith(VectorArithOp op, uint64_t vs2Element, uint64_t source)
{
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
    case VectorArithOp::Extend:
        return vs2Element;
    }
    return 0;
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
    }

    // Widths and group sizes this implementation can't hold (Spike's checks).
    if (dest.eew > ELEN)
    {
        ThrowIllegal(instruction, "2*SEW = " + std::to_string(dest.eew) + " exceeds ELEN = 64");
    }
    if (dest.emulLog2 > 3)
    {
        ThrowIllegal(instruction, "destination EMUL would exceed 8");
    }
    if (src2.eew < 8 || src2.emulLog2 < -3)
    {
        ThrowIllegal(instruction, "source EEW SEW/" + std::to_string(info.factor) +
                                      " is below 8 bits or EMUL below 1/8");
    }

    if (masked && vd == 0)
    {
        ThrowIllegal(instruction, "a masked instruction cannot write v0");
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
    if (dest.eew != src2.eew)
    {
        RequireWideningOverlapOk(instruction, vd, dest.emulLog2, vs2, src2.emulLog2, "vs2");
    }
    if (sourceIsVector && dest.eew != src1.eew)
    {
        RequireWideningOverlapOk(instruction, vd, dest.emulLog2, field19_15, src1.emulLog2,
                                 "vs1");
    }

    // The scalar operand: x[rs1], or simm5 sign-extended (both used at SEW bits).
    uint64_t scalar = 0;
    if (info.funct3 == FUNCT3_OPIVX || info.funct3 == FUNCT3_OPMVX)
    {
        scalar = rs1Value;
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
        if (masked && !registers.ReadMaskBit(0, i))
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
            b = Extend(raw, src1.eew, info.isSigned);
        }
        registers.WriteVectorElement(vd, i, dest.eew,
                                     ApplyArith(info.op, a, b) & SewMask(dest.eew));
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
    return DecodeVsetKind(instruction) != VsetKind::None;
}

VectorExecResult ExecuteVectorInstruction(uint32_t instruction, uint64_t rs1Value,
                                          uint64_t rs2Value, RegisterFile &registers,
                                          MemoryController &memory,
                                          std::vector<RegisterChange> &changes,
                                          std::vector<MemoryChange> &memoryChanges)
{
    VectorExecResult result;
    if (IsVectorMemoryInstruction(instruction))
    {
        // Vector loads/stores (LOAD-FP/STORE-FP with a vector width): see vector_memory.cpp.
        ExecuteVectorMemory(instruction, rs1Value, rs2Value, registers, memory, changes,
                            memoryChanges);
        return result;
    }
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
