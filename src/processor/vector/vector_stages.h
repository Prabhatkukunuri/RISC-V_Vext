/**
 * @file vector_stages.h
 * @brief Vector instructions split into the EX / MEM / WB phases of a scalar instruction.
 *
 *   EX   VectorExecuteStage   computes everything and changes no architectural state:
 *                             arithmetic/vset* results become pending register writes, a
 *                             load/store becomes a list of element addresses (plus store data).
 *   MEM  VectorMemoryStage    performs the element accesses of a load/store (stores write memory
 *                             here); nothing for arithmetic.
 *   WB   VectorWriteBackStage commits vector registers and vector CSRs, and returns what the VM
 *                             still has to write to rd.
 *
 * ExecuteVectorInstruction() is these three back to back.
 */

#ifndef VECTOR_STAGES_H
#define VECTOR_STAGES_H

#include "processor/processor_base.h"
#include "processor/registers.h"
#include "processor/vector/vector_execute.h"
#include "processor/vector/vector_memory.h"

#include <cstdint>
#include <vector>

namespace Kites
{
class MemoryController;

namespace rvv
{

/**
 * @brief A private copy of the vector state (v0-v31 and the vector CSRs) that EX computes into,
 * so the real register file only changes in WB.
 */
class VectorScratch
{
  public:
    /// Copies the vector registers and vector CSRs of @p registers into the scratch file.
    void SyncFrom(const RegisterFile &registers);
    RegisterFile &File() { return m_file; }

  private:
    RegisterFile m_file;
};

/**
 * @brief Everything a vector instruction carries from EX to MEM to WB (its "execution result").
 */
struct VectorExStage
{
    uint32_t instruction = 0;
    bool isMemory = false; ///< a vector load/store

    // Loads/stores
    VectorMemPlan memPlan; ///< EX: addresses and store data
    VectorMemData memData; ///< MEM: loaded values, trimmed vl

    // Everything else (vset*, arithmetic, mask, reduction, permute)
    /// Vector registers (reg_type 3) and vector CSRs (reg_type 1) to write in WB, with their
    /// old and new values.
    std::vector<RegisterChange> pendingWrites;

    VectorExecResult result; ///< rd to write (vset*, vmv.x.s, vcpop.m, vfirst.m)
};

/**
 * @brief EX: executes @p instruction against a scratch copy of the vector state. Changes neither
 * @p registers nor memory.
 *
 * @throws std::runtime_error for an illegal instruction.
 */
VectorExStage VectorExecuteStage(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                                 const RegisterFile &registers, VectorScratch &scratch);

/**
 * @brief MEM: performs the memory accesses of a vector load/store; does nothing otherwise.
 *
 * @throws std::runtime_error for a memory access fault.
 */
void VectorMemoryStage(VectorExStage &stage, MemoryController &memory,
                       std::vector<MemoryChange> &memoryChanges);

/**
 * @brief WB: commits the vector registers and vector CSRs, appending a RegisterChange for each
 * one that changed, and returns the rd write the VM still has to do.
 */
VectorExecResult VectorWriteBackStage(const VectorExStage &stage, RegisterFile &registers,
                                      std::vector<RegisterChange> &changes);

/**
 * @brief The non-memory part of ExecuteVectorInstruction (vset*, arithmetic, mask, reduction,
 * permute), executed in place on @p registers.
 */
VectorExecResult ExecuteVectorCompute(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                                      RegisterFile &registers,
                                      std::vector<RegisterChange> &changes);

} // namespace rvv
} // namespace Kites

#endif // VECTOR_STAGES_H
