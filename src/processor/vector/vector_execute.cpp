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

/// Number of registers in a group: LMUL, or 1 for a fractional LMUL.
size_t GroupSize(const Vtype &vtype)
{
    return vtype.lmulLog2 > 0 ? size_t{1} << vtype.lmulLog2 : 1;
}

void RequireAligned(uint32_t instruction, uint32_t reg, size_t group, const char *operand)
{
    if (reg % group != 0)
    {
        ThrowIllegal(instruction, std::string(operand) + " v" + std::to_string(reg) +
                                      " is not aligned to LMUL=" + std::to_string(group));
    }
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
    }
    return 0;
}

/// vd[i] = op(vs2[i], vs1[i] | x[rs1] | simm5) for the active elements in [vstart, vl).
void ExecuteIntegerArith(const VectorArithInfo &info, uint32_t instruction, uint64_t rs1Value,
                         RegisterFile &registers, std::vector<RegisterChange> &changes)
{
    const uint32_t vd = (instruction >> 7) & 0x1F;
    const uint32_t field19_15 = (instruction >> 15) & 0x1F; // vs1, rs1 or simm5
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;

    const Vtype vtype = RequireValidVtype(instruction, registers);
    const size_t group = GroupSize(vtype);
    if (masked && vd == 0)
    {
        ThrowIllegal(instruction, "a masked instruction cannot write v0");
    }
    RequireAligned(instruction, vd, group, "vd");
    RequireAligned(instruction, vs2, group, "vs2");
    if (info.funct3 == FUNCT3_OPIVV)
    {
        RequireAligned(instruction, field19_15, group, "vs1");
    }

    // The scalar operand, truncated (or sign-extended, for simm5) to SEW.
    uint64_t scalar = 0;
    if (info.funct3 == FUNCT3_OPIVX)
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
    const uint64_t mask = SewMask(vtype.sew);

    DestinationRecorder recorder(registers, vd, group);
    for (uint64_t i = vstart; i < vl; ++i)
    {
        if (masked && !registers.ReadMaskBit(0, i))
        {
            continue; // masked-off: undisturbed
        }
        const uint64_t a = registers.ReadVectorElement(vs2, i, vtype.sew);
        const uint64_t b = info.funct3 == FUNCT3_OPIVV
                               ? registers.ReadVectorElement(field19_15, i, vtype.sew)
                               : scalar & mask;
        registers.WriteVectorElement(vd, i, vtype.sew, ApplyArith(info.op, a, b) & mask);
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
