/**
 * @file vector_operands.h
 * @brief Which vector registers, vector CSRs and GPRs a vector instruction reads and writes, as the
 * decoder of a pipelined vector unit would work them out for forwarding and hazard detection.
 *
 * The answer depends on vtype (LMUL and SEW set the group sizes), so the caller passes the vtype
 * the instruction will execute with. The decoder may over-approximate (an extra stall, at worst)
 * but never under-approximates.
 */

#ifndef VECTOR_OPERANDS_H
#define VECTOR_OPERANDS_H

#include <cstdint>

namespace Kites::rvv
{

struct VectorRegisterUse
{
    /// Vector registers read in EX (bit n = vn): vs1/vs2 groups, the old vd group (tail and
    /// masked-off elements are left undisturbed) and v0 for a masked instruction. Loads and
    /// stores read nothing in EX: their vs3, index group, old vd and v0 are read in MEM.
    uint32_t exReads = 0;
    uint8_t writeBase = 0;  ///< first vector register written (vd)
    uint8_t writeCount = 0; ///< registers written from writeBase on; 0 if none

    bool readsVlInEx = false;  ///< reads vl / vtype / vstart in EX (every OP-V instruction)
    bool writesVlVtype = false; ///< vset*, or a fault-only-first load (vl only)
    bool readsRs1 = false;     ///< uses x[rs1]: .vx operand, vset AVL, load/store base
    bool readsRs2 = false;     ///< uses x[rs2]: vsetvl's vtype, strided stride

    bool isVset = false;
    bool isMemory = false;
    bool isLoad = false;
    bool isStore = false;
    bool isFaultOnlyFirst = false;
};

/**
 * @brief Decodes the register use of a vector instruction (anything IsVectorInstruction()
 * accepts) executing with @p vtype. An undecodable word reads every register.
 */
VectorRegisterUse DecodeVectorRegisterUse(uint32_t instruction, uint64_t vtype);

/// Bit mask of the registers [base, base + count), clipped at v31.
uint32_t VectorGroupMask(uint32_t base, uint32_t count);

} // namespace Kites::rvv

#endif // VECTOR_OPERANDS_H
