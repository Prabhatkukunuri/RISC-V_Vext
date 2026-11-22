/**
 * @file vector_stages.cpp
 * @brief The EX / MEM / WB phases of a vector instruction.
 */

#include "processor/vector/vector_stages.h"

#include "processor/memory_controller.h"

namespace Kites::rvv
{

void VectorScratch::SyncFrom(const RegisterFile &registers)
{
    if (m_file.GetVlenBytes() != registers.GetVlenBytes())
    {
        m_file.Reset(); // re-reads VLEN from the VM config, as the real file did
    }
    for (size_t reg = 0; reg < RegisterFile::NUM_VR; ++reg)
    {
        m_file.WriteVectorRegister(reg, registers.ReadVectorRegister(reg));
    }
    // (vxsat, vxrm and vcsr are commented out: no fixed-point vector instructions yet)
    for (const size_t csr : {RegisterFile::CSR_VSTART, RegisterFile::CSR_VL,
                             RegisterFile::CSR_VTYPE, RegisterFile::CSR_VLENB})
    {
        m_file.WriteCsr(csr, registers.ReadCsr(csr));
    }
}

VectorExStage VectorExecuteStage(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                                 const RegisterFile &registers, VectorScratch &scratch)
{
    VectorExStage stage;
    stage.instruction = instruction;
    if (IsVectorMemoryInstruction(instruction))
    {
        stage.isMemory = true;
        stage.memPlan = PlanVectorMemory(instruction, rs1Value, rs2Value, registers);
        return stage;
    }
    // The arithmetic writes into the scratch copy; the changes it records (old and new values)
    // are exactly the writes WB has to make.
    scratch.SyncFrom(registers);
    stage.result = ExecuteVectorCompute(instruction, rs1Value, rs2Value, scratch.File(),
                                        stage.pendingWrites);
    return stage;
}

void VectorMemoryStage(VectorExStage &stage, MemoryController &memory,
                       std::vector<MemoryChange> &memoryChanges)
{
    if (stage.isMemory)
    {
        stage.memData = AccessVectorMemory(stage.memPlan, memory, memoryChanges);
    }
}

VectorExecResult VectorWriteBackStage(const VectorExStage &stage, RegisterFile &registers,
                                      std::vector<RegisterChange> &changes)
{
    if (stage.isMemory)
    {
        WriteBackVectorMemory(stage.memPlan, stage.memData, registers, changes);
        return stage.result;
    }
    for (const RegisterChange &write : stage.pendingWrites)
    {
        if (write.reg_type == 3)
        {
            registers.WriteVectorRegister(write.reg_index, write.new_bytes);
        }
        else
        {
            registers.WriteCsr(write.reg_index, write.new_value);
        }
        changes.push_back(write);
    }
    return stage.result;
}

} // namespace Kites::rvv
