/**
 * @file vector_internal.h
 * @brief Helpers shared by the vector execution units (arithmetic and memory). Not part of the
 * public vector API.
 */

#ifndef VECTOR_INTERNAL_H
#define VECTOR_INTERNAL_H

#include "processor/processor_base.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace Kites::rvv::detail
{
[[noreturn]] inline void ThrowIllegal(uint32_t instruction, const std::string &reason)
{
    char word[16];
    snprintf(word, sizeof(word), "0x%08x", instruction);
    throw std::runtime_error("Illegal instruction " + std::string(word) + ": " + reason);
}

inline uint64_t SewMask(uint64_t sew)
{
    return sew == 64 ? ~0ULL : (1ULL << sew) - 1;
}

/// The current vtype, or an illegal-instruction exception if vtype.vill is set.
inline Vtype RequireValidVtype(uint32_t instruction, const RegisterFile &registers)
{
    Vtype vtype;
    if (!DecodeVtype(registers.ReadCsr(RegisterFile::CSR_VTYPE), vtype))
    {
        ThrowIllegal(instruction, "vtype.vill is set (run vsetvli first)");
    }
    return vtype;
}

/// Number of registers in a group: LMUL, or 1 for a fractional LMUL.
inline size_t GroupSize(const Vtype &vtype)
{
    return vtype.lmulLog2 > 0 ? size_t{1} << vtype.lmulLog2 : 1;
}

inline void RequireAligned(uint32_t instruction, uint32_t reg, size_t group, const char *operand)
{
    if (reg % group != 0)
    {
        ThrowIllegal(instruction, std::string(operand) + " v" + std::to_string(reg) +
                                      " is not aligned to a group of " + std::to_string(group));
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

/// Writes a vector CSR, recording the change for undo if the value differs.
inline void WriteCsrRecorded(RegisterFile &registers, size_t csr, uint64_t value,
                             std::vector<RegisterChange> &changes)
{
    const uint64_t old = registers.ReadCsr(csr);
    if (old != value)
    {
        registers.WriteCsr(csr, value);
        changes.push_back({static_cast<unsigned int>(csr), 1, old, value});
    }
}

inline void ClearVstart(RegisterFile &registers, std::vector<RegisterChange> &changes)
{
    WriteCsrRecorded(registers, RegisterFile::CSR_VSTART, 0, changes);
}

} // namespace Kites::rvv::detail

#endif // VECTOR_INTERNAL_H
