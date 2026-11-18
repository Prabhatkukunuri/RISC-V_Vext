/**
 * @file vector_execute.h
 * @brief Execution of vector instructions (OP-V, and the vector loads/stores under LOAD-FP and
 * STORE-FP), shared by the single-cycle and pipelined VMs.
 *
 * Each VM reads its scalar source registers its own way (straight from the register file, or
 * through forwarding), then calls ExecuteVectorInstruction(), which updates the vector registers,
 * vector CSRs and (for vector stores) memory, and records each change for undo. Writing rd
 * (vset* only) is left to the VM's writeback.
 */

#ifndef VECTOR_EXECUTE_H
#define VECTOR_EXECUTE_H

#include "processor/vector/vector_config.h"

#include <cstdint>
#include <vector>

namespace Kites
{
class MemoryController;
class RegisterFile;
struct MemoryChange;
struct RegisterChange;

namespace rvv
{
/**
 * @brief True for any word the vector unit owns: OP-V, or a LOAD-FP/STORE-FP word whose width
 * field selects a vector load/store. Such words must be routed to ExecuteVectorInstruction()
 * even if they are reserved encodings, which then trap as illegal.
 */
bool IsVectorInstruction(uint32_t instruction);

/**
 * @brief True for a vector word this VM can execute.
 */
bool IsSupportedVectorInstruction(uint32_t instruction);

/**
 * @brief True if the instruction writes a GPR (rd): so far only the vset* family.
 */
bool VectorInstructionWritesGpr(uint32_t instruction);

/**
 * @brief What a vector instruction leaves for the VM's own writeback.
 */
struct VectorExecResult
{
    bool writesRd = false; ///< rd must be written with rdValue
    uint64_t rdValue = 0;
};

/**
 * @brief Executes one vector instruction (any word IsVectorInstruction() accepts).
 *
 * Vector registers, vector CSRs and memory are written here; a RegisterChange is appended to
 * @p changes for each register that changed and a MemoryChange to @p memoryChanges for each
 * stored element that changed memory.
 *
 * @param rs1Value x[rs1] (AVL of vsetvli/vsetvl, scalar operand of .vx forms, base address of
 * loads/stores).
 * @param rs2Value x[rs2] (vtype of vsetvl, byte stride of strided loads/stores).
 * @throws std::runtime_error for an illegal instruction (an unsupported or reserved encoding,
 * vtype.vill set, a misaligned register group, a masked instruction writing v0, ...) or a memory
 * access fault.
 */
VectorExecResult ExecuteVectorInstruction(uint32_t instruction, uint64_t rs1Value,
                                          uint64_t rs2Value, RegisterFile &registers,
                                          MemoryController &memory,
                                          std::vector<RegisterChange> &changes,
                                          std::vector<MemoryChange> &memoryChanges);

/**
 * @brief Works out what a vset* instruction writes, without changing any state.
 */
VsetResult ExecuteVset(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                       const RegisterFile &registers);

/**
 * @brief Writes vl and vtype from @p result and clears vstart, appending a CSR
 * RegisterChange for each value that changed.
 */
void CommitVsetCsrs(RegisterFile &registers, const VsetResult &result,
                    std::vector<RegisterChange> &changes);

} // namespace rvv
} // namespace Kites

#endif // VECTOR_EXECUTE_H
