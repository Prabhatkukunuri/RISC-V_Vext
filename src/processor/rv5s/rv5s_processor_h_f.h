/**
 * @file rv5s_processor_h_f.h
 * @brief Header for the 5-stage pipelined VM (RV5S) in Mode 4: Hazard Detection, With Forwarding.
 * @author Atharva and Harshit
 */
#pragma once

#include "processor/pipeline_registers.h"
#include "processor/rv5s/rv5s_control_unit.h"
#include "processor/rv5s/rv5s_hdu.h" // <-- Including the HDU
#include "processor/rv5s/rv5s_processor_base.h"

#include <iostream>
#include <stack>
#include <vector>

namespace Kites
{
class RV5StageProcessorHF : public RV5StageVM_Base
{
  public:
    // Constructor and Destructor
    RV5StageProcessorHF();
    ~RV5StageProcessorHF() = default;

    // Overridden virtual functions from ProcessorBase
    // void Run() override;
    // void DebugRun() override;
    void Step() override;
    void Reset() override;

    void PrintType()
    {
        std::cout << "RV5StageProcessorHF" << std::endl;
    }

    void SetActiveWireNames() override;

    /// True if the vector forwarding unit selected the EX/MEM bypass in the last EX stage.
    bool VectorForwardSelected() const { return vector_forwarded_; }

  private:
    /// What EX of a vector or Zicsr instruction hands to the EX/MEM latch.
    struct ExResultVector
    {
        uint8_t base = 0;        ///< first register written (vd, widened if needed)
        uint8_t count = 0;       ///< registers written
        std::vector<uint8_t> data;
        uint64_t vtype = 0;      ///< new vtype of a vset*
        bool csr_write = false;  ///< Zicsr: write csr_wdata in WB
        uint64_t csr_wdata = 0;
    };
    ExResultVector ex_vector_;
    bool vector_forwarded_ = false;
    // For the datapath view only: whether this cycle's WB was a vector instruction, and
    // whether this cycle's stall came from hdu_vector.
    QList<QString> always_active_wires_;
    bool wb_vector_ = false;
    bool wb_vector_load_ = false;
    bool vector_stall_ = false;
    /// Marks the wires a vector instruction uses this cycle as "V:<wire>" (drawn green).
    void markVectorWires();
    /// The vector operand file EX computes in: the register file plus the EX/MEM bypass.
    rvv::VectorScratch ex_vector_scratch_;

    /// EX of a vector instruction. @p rs1/@p rs2 are x[rs1]/x[rs2] after GPR forwarding.
    /// Returns what goes in alu_result: rd's value, the new vl of a vset*, or the base address
    /// of a load/store.
    uint64_t pipeline_execute_vector(uint64_t rs1, uint64_t rs2);
    /// EX of a Zicsr instruction: reads the CSR through the bypass and computes the value WB will
    /// write. Returns the old value (rd's value).
    uint64_t pipeline_execute_csr(uint32_t instruction, uint64_t rs1_value);

    // The Hazard Detection Unit instance
    // RV5SHazardUnit hazard_unit_;

    // This flag controls freezing the front-end (IF/ID registers and PC)
    bool stall_fetch_and_decode_ = false;

    // --- Private methods for each pipeline stage ---
    void pipeline_fetch() override;
    // void pipeline_decode() override;
    void pipeline_execute() override;
    uint64_t pipeline_execute_float(uint64_t int_rs1) override;
    uint64_t pipeline_execute_double(uint64_t int_rs1) override;
    // void pipeline_memory() override;
    //  void pipeline_writeback() override;

    // Helper function to consolidate hazard checks (now only Load-Use)

    // void print_pipeline_registers_debug();
    void handle_syscall() override;
};
}//namespace Kites