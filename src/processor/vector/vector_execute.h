/**
 * @file vector_execute.h
 * @brief Execution of vector (OP-V) instructions, shared by the single-cycle and pipelined VMs.
 *
 * Each VM reads its scalar source registers its own way (straight from the register file, or
 * through forwarding), then calls ExecuteVectorInstruction(), which updates the vector registers
 * and vector CSRs and records each change for undo. Writing rd (vset* only) is left to the VM's
 * writeback.
 */

#ifndef VECTOR_EXECUTE_H
#define VECTOR_EXECUTE_H

#include "processor/vector/vector_config.h"

#include <cstdint>
#include <vector>

namespace Kites
{
class RegisterFile;
struct RegisterChange;

namespace rvv
{
/**
 * @brief True for an OP-V word this VM can execute.
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
 * @brief Executes one OP-V instruction.
 *
 * Vector registers and vector CSRs are written here; a RegisterChange is appended to
 * @p changes for each one that changed.
 *
 * @param rs1Value x[rs1] (AVL of vsetvli/vsetvl, scalar operand of .vx forms).
 * @param rs2Value x[rs2] (vtype of vsetvl).
 * @throws std::runtime_error for an illegal instruction: an unsupported encoding, vtype.vill
 * set, a register group that isn't LMUL-aligned, or a masked instruction writing v0.
 */
VectorExecResult ExecuteVectorInstruction(uint32_t instruction, uint64_t rs1Value,
                                          uint64_t rs2Value, RegisterFile &registers,
                                          std::vector<RegisterChange> &changes);

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
