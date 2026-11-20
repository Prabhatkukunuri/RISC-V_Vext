/**
 * @file rvss_vm.h
 * @brief RVSS VM definition
 * @author Vishank Singh, https://github.com/VishankSingh
 */
#ifndef RVSS_VM_H
#define RVSS_VM_H

#include "processor/processor_base.h"
#include "processor/processor_manager.h"
#include "rvss_control_unit.h"
#include "processor/vector/vector_execute.h"
#include "processor/vector/vector_stages.h"

#include <cstdint>
#include <iostream>
#include <stack>
#include <vector>


// TODO: use a circular buffer instead of a stack for undo/redo
namespace Kites
{
struct RVSingleStageStepDelta : public StepDelta
{
    // uint64_t old_pc;
    // uint64_t new_pc;
    // std::vector<RegisterChange> register_changes;
    // std::vector<MemoryChange> memory_changes;
};

class RVSSProcessor : public ProcessorBase
{
  public:
    RVSSControlUnit control_unit_;
    // intermediate variables
    int64_t execution_result_{};
    int64_t memory_result_{};
    // int64_t memory_address_{};
    // int64_t memory_data_{};
    uint64_t return_address_{};

    bool branch_flag_ = false;
    int64_t next_pc_{}; // for jal, jalr,

    // CSR intermediate variables
    uint16_t csr_target_address_{};
    uint64_t csr_old_value_{};
    uint64_t csr_write_val_{};
    uint8_t csr_uimm_{};

    // Vector (OP-V) intermediate variables
    // A vector instruction's state between phases: ExecuteVector (EX) fills it, WriteMemory (MEM)
    // does its memory accesses, WriteBackVector (WB) commits it.
    rvv::VectorExStage vector_stage_{};
    rvv::VectorScratch vector_scratch_; ///< private vector state EX computes into
    rvv::VectorExecResult vector_result_{};

    std::stack<StepDelta> undo_stack_;
    std::stack<StepDelta> redo_stack_;

    RVSingleStageStepDelta current_delta_;

    /// True while Run() is calling Step(): Step() then skips writing the dump files.
    bool in_run_ = false;

    void Fetch();

    void Decode();

    void Execute();
    void ExecuteFloat();
    void ExecuteDouble();
    void ExecuteCsr();
    void HandleSyscall();
    void ExecuteVector();

    void WriteMemory();
    void WriteMemoryFloat();
    void WriteMemoryDouble();

    void WriteBack();
    void WriteBackFloat();
    void WriteBackDouble();
    void WriteBackCsr();
    void WriteBackVector();

    RVSSProcessor();
    ~RVSSProcessor();

    void Run() override;
    void DebugRun() override;
    void Step() override;
    void Undo() override;
    void Redo() override;
    void Reset() override;

    void SetActiveWireNames() override;
    void setProcessorState() override;

    void PrintType()
    {
        std::cout << "rvssvm" << std::endl;
    }
};
}//namespace Kites
#endif // RVSS_VM_H
