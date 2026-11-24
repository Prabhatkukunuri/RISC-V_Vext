/**
 * @file rv5s_hdu_vector.cpp
 * @brief Vector hazard detection unit for the H_F pipeline. See rv5s_hdu_vector.h.
 */
#include "processor/rv5s/rv5s_hdu_vector.h"

#include "common/instructions.h"
#include "processor/registers.h"
#include "processor/rv5s/rv5s_hdu.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_execute.h"
#include "processor/vector/vector_operands.h"

namespace Kites
{
namespace
{
constexpr uint32_t OPCODE_SYSTEM = 0b1110011;

bool IsZicsr(uint32_t instruction)
{
    return (instruction & 0x7F) == OPCODE_SYSTEM && ((instruction >> 12) & 0x7) != 0;
}

uint32_t CsrNumber(uint32_t instruction)
{
    return (instruction >> 20) & 0xFFF;
}

bool IsVset(uint32_t instruction)
{
    return (instruction & 0x7F) == rvv::OPCODE_OP_V &&
           ((instruction >> 12) & 0x7) == rvv::FUNCT3_OPCFG;
}
} // namespace

uint64_t vtype_seen_by_next_instruction(const EX_MEM_Register &ex_mem,
                                        const MEM_WB_Register &mem_wb,
                                        const RegisterFile &registers)
{
    if (ex_mem.is_vector && ex_mem.vcsr_write && IsVset(ex_mem.instruction))
    {
        return ex_mem.v_vtype;
    }
    if (mem_wb.is_vector && mem_wb.vcsr_write && IsVset(mem_wb.instruction))
    {
        return mem_wb.v_vtype;
    }
    return registers.ReadCsr(RegisterFile::CSR_VTYPE);
}

int check_vector_hazard(const IF_ID_Register &if_id, const EX_MEM_Register &ex_mem,
                        const MEM_WB_Register &mem_wb, const RegisterFile &registers)
{
    const uint32_t consumer = if_id.instruction;
    if (consumer == NOP)
    {
        return STALL_NONE;
    }

    if (rvv::IsVectorInstruction(consumer))
    {
        const rvv::VectorRegisterUse use = rvv::DecodeVectorRegisterUse(
            consumer, vtype_seen_by_next_instruction(ex_mem, mem_wb, registers));

        // Vector load -> a reader of the loaded group in EX. The data exists only at the end
        // of MEM, so the reader waits one cycle and then finds it in the register file.
        if (ex_mem.vmem_read && ex_mem.vreg_write &&
            (use.exReads & rvv::VectorGroupMask(ex_mem.rd, ex_mem.v_wcount)) != 0)
        {
            return STALL_ONE_CYCLE;
        }
        // vle*ff -> a reader of vl in EX: the trimmed vl is also known only after MEM.
        if (ex_mem.vmem_read && ex_mem.vcsr_write && use.readsVlInEx)
        {
            return STALL_ONE_CYCLE;
        }
        // Scalar load -> a vector instruction that really uses that GPR. Only x[rs1]/x[rs2]
        // operands count: a vs1/vs2 field that happens to equal the load's rd does not.
        if (ex_mem.mem_read && ex_mem.reg_write && ex_mem.rd != 0)
        {
            const uint8_t rs1 = (consumer >> 15) & 0x1F;
            const uint8_t rs2 = (consumer >> 20) & 0x1F;
            if ((use.readsRs1 && rs1 == ex_mem.rd) || (use.readsRs2 && rs2 == ex_mem.rd))
            {
                return STALL_ONE_CYCLE;
            }
        }
        return STALL_NONE;
    }

    // csrr* of vl right after a vle*ff: the CSR bypass has no vl for it yet.
    if (IsZicsr(consumer) && CsrNumber(consumer) == RegisterFile::CSR_VL && ex_mem.vmem_read &&
        ex_mem.vcsr_write)
    {
        return STALL_ONE_CYCLE;
    }

    // Zicsr write of fflags/frm/fcsr -> F/D instruction. The FP instruction reads frm and writes
    // the flags in EX; the CSR write happens in WB, a cycle later, and would overwrite them.
    if (ex_mem.csr_write)
    {
        const uint32_t csr = CsrNumber(ex_mem.instruction);
        if (csr >= 0x001 && csr <= 0x003 &&
            (instruction_set::isFInstruction(consumer) ||
             instruction_set::isDInstruction(consumer)))
        {
            return STALL_ONE_CYCLE;
        }
    }
    return STALL_NONE;
}
} // namespace Kites
