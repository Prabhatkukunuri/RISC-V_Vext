/**
 * @file rv5s_processor_h_f.cpp
 * @brief Implementation for the 5-stage pipelined VM (RV5S) in Mode 4: Hazard Detection, With
 * Forwarding.
 * * NOTE: This is a PURE INTEGER (GPR) implementation.
 * * Stall Rule: Only Load-Use GPR dependencies require a 1-cycle stall. All others are solved by
 * forwarding.
 * * Control Hazards (JAL/JALR, B-Type) are AUTOMATICALLY handled by flush/redirect.
 * @author Atharva and Harshit
 */
#include "processor/rv5s/rv5s_processor_h_f.h" // Assuming this header now defines RV5StageProcessorHF
#include "common/instructions.h"
#include "config/config.h"
#include "ui/processor_tab/processor_designs/rv5s_processor_h_f_circuit_scene.h"
#include "processor/alu.h"
#include "processor/pipeline_registers.h"
#include "processor/processor_base.h"
#include "processor/registers.h"
#include "processor/rv5s/rv5s_hdu_vector.h"
#include "processor/vector/vector_operands.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <iostream>
#include <thread>
#include <tuple>

namespace Kites
{
using namespace alu;

// Define stall count constants (used internally, defined in rv5s_hdu.h)
// constexpr int STALL_NONE = 0;
// constexpr int STALL_ONE_CYCLE = 1;

// --- RV5StageProcessorHF Class Implementation ---

RV5StageProcessorHF::RV5StageProcessorHF() : RV5StageVM_Base()
{
    // Initialize ProcessorBase members
    // program_counter_ = 0;
    // instructions_retired_ = 0;
    // cycle_s_ = 0;
    // stall_cycles_ = 0;

    // // Initialize local members
    // stall_fetch_and_decode_ = false;

    // Reset components and history
#ifndef DISABLE_GUI
    circuit_scene_ = std::make_unique<Kites::RV5StageVM_H_F_CircuitScene>();
    connect(this, &ProcessorBase::updateCircuitStateSignal, circuit_scene_.get(),
            &Kites::RV5StageVM_H_F_CircuitScene::updateCircuitState);
#endif
    Reset();

    active_wires_.append("PC_to_IM");
    active_wires_.append("IM_to_P1");
    active_wires_.append("P1_to_P2_PCcarry");
    active_wires_.append("P1_to_Control_RF_andall");
    active_wires_.append("RF_to_P2_UP");
    active_wires_.append("RFdown_to_P2");
    active_wires_.append("PCMux_to_PC");
    active_wires_.append("P2_to_ALU2");
    active_wires_.append("P2_to_P3_MEMControl");
    active_wires_.append("P2_to_P3_WBcontrol");
    active_wires_.append("ALU2_to_P3");
    active_wires_.append("P3_to_P4_WBcontrol");
    active_wires_.append("P2_to_ALUControl");
    active_wires_.append("RdP4_to_FU");
    active_wires_.append("P4EB_to_FWD_unit");
    active_wires_.append("P2_to_FDU_rs1");
    active_wires_.append("P2_to_FDU_RS2");
    active_wires_.append("ALU_to_P3");
    active_wires_.append("P3ALUres_to_DMup");
    active_wires_.append("ALUcontrol_to_ALU");
    active_wires_.append("Fmux1_to_ALU");
    active_wires_.append("Fmux_to_ALUMux_up");
    active_wires_.append("P2_to_FMUX");
    active_wires_.append("P2_to_FMUX2_UP");
    active_wires_.append("P2_to_ALUMux");
    active_wires_.append("P2_to_ALUcontrol_Control");

    always_active_wires_count_ = active_wires_.size();
    always_active_wires_ = active_wires_;
}

void RV5StageProcessorHF::SetActiveWireNames()
{
    // Clear and populate canonical wires for the Hazard+Forwarding (H_F) circuit
    // (a saved copy: the green pass below reorders the list, so its prefix can't be reused)
    active_wires_ = always_active_wires_;
    // Backbone / always-visible wires (names sourced from H_F_Processor.json)

    // Immediate / ALU source selection
    if (id_ex_reg_.alu_src)
    {
        active_wires_.append("Imm_to_P2");
        active_wires_.append("P2Imm_to_ALU2_down");
    }

    // Control signals in ID stage reflected on P2 control wires
    if (id_ex_reg_.mem_write)
    {
        active_wires_.append("P2_to_P3_MEMControl");
    }
    if (id_ex_reg_.reg_write)
    {
        active_wires_.append("P2_to_P3_WBcontrol");
    }

    // Memory control / data path (EX/MEM stage)
    if (ex_mem_reg_.prev_mem_read || ex_mem_reg_.mem_read)
    {
        active_wires_.append("P3_to_DM_Memread");
        active_wires_.append("DMMux_to_DM");
    }
    if (ex_mem_reg_.mem_write)
    {
        active_wires_.append("P3_TO_DM_control_memwrite");
        active_wires_.append("P3ALUres_to_DMup");
    }

    // Register writeback paths (MEM/WB stage)
    if (mem_wb_reg_.prev_reg_write || mem_wb_reg_.reg_write)
    {
        active_wires_.append("P4_wbcontrol_to_WBmux");
        active_wires_.append("RdP4_to_FU");
        active_wires_.append("WBMux_to_RF");
    }

    // Forwarding unit activity: show FWD wires when forwarding is relevant
    if (ex_mem_reg_.reg_write && ex_mem_reg_.rd != 0)
    {
        if (ex_mem_reg_.rd == id_ex_reg_.rs1)
        {
            active_wires_.append("P2_to_FDU_rs1");
            active_wires_.append("P4EB_to_FWD_unit");
            active_wires_.append("FDU_to_FMUX1_cntrl");
            active_wires_.append("FDU_to_fMUX2");
        }
        if (ex_mem_reg_.rd == id_ex_reg_.rs2)
        {
            active_wires_.append("P2_to_FDU_RS2");
            active_wires_.append("P4EB_to_FWD_unit");
            active_wires_.append("FDU_to_FMUX3");
        }
    }

    if (mem_wb_reg_.prev_reg_write && mem_wb_reg_.prev_rd != 0)
    {
        if (mem_wb_reg_.prev_rd == id_ex_reg_.rs1)
        {
            active_wires_.append("P4EB_to_FWD_unit");
            active_wires_.append("FDU_to_FMUX1_cntrl");
        }
        if (mem_wb_reg_.prev_rd == id_ex_reg_.rs2)
        {
            active_wires_.append("P4EB_to_FWD_unit");
            active_wires_.append("FDU_to_FMUX3");
        }
    }

    // Branch / PC selection wires
    if (ex_mem_reg_.prev_branch_taken || ex_mem_reg_.branch_taken)
    {
        active_wires_.append("ALU_zerores_to_P3");
        active_wires_.append("ANDGate_lower_entry");
        active_wires_.append("ANDGATE_to_PCMUX");
        active_wires_.append("P3_to_PCMux");
    }
    else
    {
        active_wires_.append("ALU1_to_PCMuxUp");
    }

    // Hazard Detection Unit (HDU) / stall indicators
    if (stall_fetch_and_decode_ || stall_cycles_ > 0)
    {
        active_wires_.append("HDU_to_P1");
        active_wires_.append("HDUMux_to_P2");
        active_wires_.append("HDU_mux_lowest_toP2");
        active_wires_.append("HDU_to_HDU_mUX");
    }

    markVectorWires();

    // Preserve ordering while removing duplicates
    QList<QString> uniqueList;
    QSet<QString> seen;
    for (const auto &w : active_wires_)
    {
        if (!seen.contains(w))
        {
            uniqueList.append(w);
            seen.insert(w);
        }
    }
    active_wires_ = uniqueList;

    // The scene will read `active_wires_` when `updateCircuitStateSignal` is emitted by callers.
}

void RV5StageProcessorHF::markVectorWires()
{
    // Wires of each stage, as the scalar highlighting above assigns them: IF = IF/ID, ID = ID/EX,
    // EX = EX/MEM, MEM = MEM/WB, WB = the instruction written back this cycle.
    static const QStringList kIf = {"PC_to_IM", "IM_to_P1", "PCMux_to_PC", "ALU1_to_PCMuxUp"};
    static const QStringList kId = {"P1_to_Control_RF_andall", "RF_to_P2_UP", "RFdown_to_P2",
                                    "P1_to_P2_PCcarry", "Control_ControlMux",
                                    "HDUMux_to_P2_upper"};
    static const QStringList kEx = {"P2_to_FMUX",          "P2_to_FMUX2_UP",
                                    "Fmux1_to_ALU",        "Fmux_to_ALUMux_up",
                                    "AKUMux_to_ALU",       "P2_to_ALUMux",
                                    "P2_to_ALUControl",    "P2_to_ALUcontrol_Control",
                                    "ALUcontrol_to_ALU",   "ALU_to_P3",
                                    "rd_P2_to_P3",         "P2_to_P3_WBcontrol"};
    static const QStringList kExMemory = {"P2_to_P3_MEMControl", "FMUX3_bottommostinput"};
    static const QStringList kForward = {"P2_to_FDU_rs1",      "P2_to_FDU_RS2",
                                         "P4EB_to_FWD_unit",   "FDU_to_FMUX1_cntrl",
                                         "FDU_to_fMUX2",       "FDU_to_FMUX3",
                                         "P3ALUres_to_DMup"};
    static const QStringList kMem = {"rd_P3_to_P4", "P3_to_P4_WBcontrol", "P3ALUres_to_DMup"};
    static const QStringList kMemLoad = {"DMMux_to_DM", "P3_to_DM_Memread", "DM_to_P4"};
    static const QStringList kMemStore = {"DMMux_to_DM", "P3_TO_DM_control_memwrite"};
    static const QStringList kWb = {"P4_wbcontrol_to_WBmux", "WBMux_to_RF",
                                    "P4_to_RF_regwritecontrol", "RdP4_to_FU"};
    static const QStringList kHdu = {"HDU_to_P1", "HDUMux_to_P2", "HDU_mux_lowest_toP2",
                                     "HDU_to_HDU_mUX", "HDU_to_PCWrite"};

    QSet<QString> green;
    const auto add = [&green](const QStringList &wires)
    {
        for (const QString &wire : wires)
            green.insert(wire);
    };

    if (rvv::IsVectorInstruction(if_id_reg_.instruction))
        add(kIf);
    if (id_ex_reg_.is_vector)
        add(kId);
    if (ex_mem_reg_.is_vector)
    {
        add(kEx);
        if (ex_mem_reg_.vmem_read || ex_mem_reg_.vmem_write)
            add(kExMemory);
        if (vector_forwarded_)
            add(kForward);
    }
    if (mem_wb_reg_.is_vector)
    {
        add(kMem);
        const uint32_t opcode = mem_wb_reg_.instruction & 0x7F;
        if (opcode == rvv::OPCODE_LOAD_FP)
            add(kMemLoad);
        else if (opcode == rvv::OPCODE_STORE_FP)
            add(kMemStore);
    }
    if (wb_vector_)
    {
        add(kWb);
        // a loaded group comes from the memory side of the WB mux, anything else from the ALU side
        add({wb_vector_load_ ? QStringLiteral("P4DM_to_lastMux")
                             : QStringLiteral("P4ALU_to_WBMuxDown")});
    }
    if (vector_stall_)
        add(kHdu);

    if (green.isEmpty())
        return;
    // A green wire replaces its yellow entry.
    QList<QString> marked;
    for (const QString &wire : active_wires_)
    {
        if (!green.contains(wire))
            marked.append(wire);
    }
    for (const QString &wire : green)
        marked.append(QStringLiteral("V:") + wire);
    active_wires_ = marked;
}

void RV5StageProcessorHF::Reset()
{
    RV5StageVM_Base::Reset();
    stall_fetch_and_decode_ = false;
}

void RV5StageProcessorHF::Step()
{
    begin_step_delta();

    // 1. Execute back stages (WB -> MEM -> EX)
    wb_vector_ = mem_wb_reg_.is_vector;
    wb_vector_load_ = wb_vector_ && (mem_wb_reg_.instruction & 0x7F) == rvv::OPCODE_LOAD_FP;
    vector_stall_ = false;
    pipeline_writeback();
    pipeline_memory();
    pipeline_execute();

    // --- Hazard Detection and Stall Logic (The core of Mode 4) ---

    int stalls_needed = 0;
    bool currently_stalling = (stall_cycles_ > 0);

    if (currently_stalling)
    {
        // Stall is already active. Decrement counter and continue stall.
        stall_cycles_--;
        stalls_needed = 1; // Indicate we are continuing the stall (1 cycle consumed)
    }
    else
    {
        // Pipeline is free. Check for a NEW hazard using the external HDU function.
        // A vector consumer is checked by hdu_vector alone: the generic HDU would treat its
        // vs1/vs2 fields as GPRs. hdu_vector also covers the CSR stalls of scalar consumers.
        if (!rvv::IsVectorInstruction(if_id_reg_.instruction))
        {
            stalls_needed = check_data_hazard(if_id_reg_, id_ex_reg_, ex_mem_reg_,
                                              true /* is_forwarding_enabled */);
        }
        const int vector_stalls =
            check_vector_hazard(if_id_reg_, ex_mem_reg_, mem_wb_reg_, registers_);
        vector_stall_ = vector_stalls > stalls_needed;
        stalls_needed = std::max(stalls_needed, vector_stalls);
        if (stalls_needed > 0)
        {
            // Start the stall: stalls_needed cycles total. 1 cycle is handled now.
            stall_cycles_ = stalls_needed - 1;
        }
    }

    // 2. Process Decode/Fetch based on stall status
    if (stalls_needed > 0)
    {
        // STALL: Freeze IF/ID (by preventing its update) and inject NOP into ID/EX
        id_ex_reg_.reset();
        // Skip pipeline_decode() - The instruction in IF/ID stays put.
        stall_fetch_and_decode_ = true;
    }
    else
    {
        // NO STALL: Advance pipeline normally
        pipeline_decode();
        stall_fetch_and_decode_ = false;
    }

    // 3. Fetch and PC update
    fetch_and_advance_pc(stall_fetch_and_decode_);

    cycle_s_++; // One clock cycle has passed

    finalize_step_delta();
}

// --- Pipeline Stage Implementations (Mode 4: H_F) ---

void RV5StageProcessorHF::pipeline_fetch()
{
    // If a stall is active, we prevent IF/ID from updating.
    if (stall_fetch_and_decode_)
    {
        // IF/ID register is intentionally NOT updated, holding the stalled instruction.
        return;
    }

    if (program_counter_ < program_size_)
    {
        if_id_reg_.instruction = memory_controller_.readInstruction(program_counter_);
        if_id_reg_.pc = program_counter_;
    }
    else
    {
        if_id_reg_.reset();
    }
}

void RV5StageProcessorHF::pipeline_execute()
{
    uint32_t instruction = id_ex_reg_.instruction;
    bool overflow;
    uint64_t alu_result;
    ex_vector_ = ExResultVector{};
    vector_forwarded_ = false;

    bool is_f_instruction = instruction_set::isFInstruction(instruction);
    bool is_d_instruction = instruction_set::isDInstruction(instruction);

    // --- GPR forwarding: runs first, for every instruction ---
    // Integer ops use alu_in1/alu_in2 below; FP ops get the forwarded x[rs1] (the base address
    // of FP loads/stores, the integer source of fcvt.s.w / fmv.w.x).
    uint64_t alu_in1 = id_ex_reg_.reg1_data;
    uint64_t alu_in2 = id_ex_reg_.reg2_data;

    // --- GPR FORWARDING LOGIC ---
    uint8_t forward_a = 0; // 10 = EX/MEM, 01 = MEM/WB
    uint8_t forward_b = 0;

    // **Priority 1: EX/MEM Forward** (R-R, R-Store hazard solved)
    if (ex_mem_reg_.reg_write && (ex_mem_reg_.rd != 0))
    {
        if (ex_mem_reg_.rd == id_ex_reg_.rs1)
            forward_a = 2;
        if (ex_mem_reg_.rd == id_ex_reg_.rs2)
            forward_b = 2;
    }

    // **Priority 2: MEM/WB Forward** (R-R, L-Store hazard solved)
    if (mem_wb_reg_.prev_reg_write && (mem_wb_reg_.prev_rd != 0))
    {
        // Forward A from MEM/WB unless EX/MEM is already forwarding
        if (mem_wb_reg_.prev_rd == id_ex_reg_.rs1 && forward_a != 2)
            forward_a = 1;
        // Forward B from MEM/WB unless EX/MEM is already forwarding
        if (mem_wb_reg_.prev_rd == id_ex_reg_.rs2 && forward_b != 2)
            forward_b = 1;
    }

    // --- FORWARDING APPLICATION ---

    if (forward_a == 2)
    {
        alu_in1 = ex_mem_reg_.alu_result;
    }
    else if (forward_a == 1)
    {
        alu_in1 = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_memory_data
                                              : mem_wb_reg_.prev_alu_result;
    }

    if (forward_b == 2)
    {
        alu_in2 = ex_mem_reg_.alu_result;
    }
    else if (forward_b == 1)
    {
        alu_in2 = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_memory_data
                                              : mem_wb_reg_.prev_alu_result;
    }

    if (is_f_instruction)
    {
        alu_result = pipeline_execute_float(alu_in1);
    }
    else if (is_d_instruction)
    {
        alu_result = pipeline_execute_double(alu_in1);
    }
    else
    {
        // Re-apply immediate value check (must happen AFTER forwarding application)
        if (id_ex_reg_.alu_src)
        {
            alu_in2 = static_cast<uint64_t>(id_ex_reg_.imm);
        }

        // --- EXECUTION ---
        alu::AluOp alu_operation = control_unit_.GetAluSignal(instruction, id_ex_reg_.alu_op > 0);
        std::tie(alu_result, overflow) = alu::Alu::execute(alu_operation, alu_in1, alu_in2);
        if (id_ex_reg_.is_vector)
        {
            // alu_in1/alu_in2 hold x[rs1]/x[rs2] after forwarding: the AVL/vtype of vset*, the
            // scalar of .vx, or the base address/stride of a vector load/store.
            alu_result = pipeline_execute_vector(alu_in1, alu_in2);
        }

        // CSR instructions produce their result outside the ALU. The CSR itself is written
        // in WB.
        if ((instruction & 0x7F) == 0b1110011 && ((instruction >> 12) & 0x7) != 0)
        {
            alu_result = pipeline_execute_csr(instruction, alu_in1);
        }

        // temporary fix for lui instruction
        if ((id_ex_reg_.instruction & 0b1111111) == 0b0110111) // lui
        {
            alu_result = static_cast<uint64_t>(id_ex_reg_.imm << 12);
        }
        // auipc value adds the immediate to the current value of PC. This requires it to be done in the execute stage.
        if ((id_ex_reg_.instruction & 0b1111111) == 0b0010111)
        {
            const int32_t upper = static_cast<int32_t>(static_cast<uint32_t>(id_ex_reg_.imm) << 12);
            alu_result = id_ex_reg_.pc + static_cast<uint64_t>(static_cast<int64_t>(upper));
        }

        // Latch data for EX/MEM Register

        // Store Data: CRITICAL FORWARDING - Store data must also be forwarded!
        uint64_t store_data = id_ex_reg_.reg2_data;
        if (forward_b == 2)
        {
            store_data = ex_mem_reg_.alu_result;
        }
        else if (forward_b == 1)
        {
            store_data = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_memory_data
                                                     : mem_wb_reg_.prev_alu_result;
        }

        ex_mem_reg_.reg2_data = store_data;
    }

    ex_mem_reg_.prev_reg_write = ex_mem_reg_.reg_write;
    ex_mem_reg_.prev_rd = ex_mem_reg_.rd;
    ex_mem_reg_.prev_freg_write = ex_mem_reg_.freg_write;
    ex_mem_reg_.prev_frd = ex_mem_reg_.frd;
    ex_mem_reg_.pc = id_ex_reg_.pc;
    ex_mem_reg_.instruction = id_ex_reg_.instruction;
    ex_mem_reg_.alu_result = alu_result;
    ex_mem_reg_.f_alu_result = (is_f_instruction || is_d_instruction) ? alu_result : 0;
    ex_mem_reg_.rd = id_ex_reg_.rd;
    ex_mem_reg_.frd = id_ex_reg_.frd;
    // NOTE: reg2_data and freg2_data are set inside the integer/FP execute
    // branches above with proper forwarding applied — do NOT overwrite here.
    ex_mem_reg_.reg_write = id_ex_reg_.reg_write;
    ex_mem_reg_.freg_write = id_ex_reg_.freg_write;
    ex_mem_reg_.mem_to_reg = id_ex_reg_.mem_to_reg;
    ex_mem_reg_.prev_mem_read = ex_mem_reg_.mem_read;
    ex_mem_reg_.prev_mem_write = ex_mem_reg_.mem_write;
    ex_mem_reg_.mem_read = id_ex_reg_.mem_read;
    ex_mem_reg_.mem_write = id_ex_reg_.mem_write;
    ex_mem_reg_.prev_branch_taken = ex_mem_reg_.branch_taken;
    ex_mem_reg_.branch_taken = false;
    ex_mem_reg_.branch_target_pc = 0;

    // Vector and Zicsr fields
    ex_mem_reg_.is_vector = id_ex_reg_.is_vector;
    ex_mem_reg_.vmem_read = id_ex_reg_.vmem_read;
    ex_mem_reg_.vmem_write = id_ex_reg_.vmem_write;
    ex_mem_reg_.vcsr_write = id_ex_reg_.vcsr_write;
    ex_mem_reg_.vreg_write = id_ex_reg_.is_vector && !id_ex_reg_.vmem_write && ex_vector_.count > 0;
    ex_mem_reg_.v_wcount = ex_vector_.count;
    ex_mem_reg_.v_data = std::move(ex_vector_.data);
    ex_mem_reg_.v_vtype = ex_vector_.vtype;
    if (ex_mem_reg_.vreg_write)
    {
        ex_mem_reg_.rd = ex_vector_.base; // vd (rd's bits); vector ops never also write x[rd]
    }
    ex_mem_reg_.csr_write = ex_vector_.csr_write;
    ex_mem_reg_.csr_wdata = ex_vector_.csr_wdata;

    uint8_t opcode = instruction & 0b1111111;

    // --- Control Hazard Logic (Automated) ---

    // Conditional Branch Check (B-type)
    if (id_ex_reg_.branch && opcode == 0b1100011)
    {
        bool condition_met = false;
        uint8_t funct3 = (instruction >> 12) & 0x7;

        switch (funct3)
        {
        case 0b000:
            condition_met = (alu_result == 0);
            break; // BEQ
        case 0b001:
            condition_met = (alu_result != 0);
            break; // BNE
        case 0b100:
            condition_met = (alu_result == 1);
            break; // BLT
        case 0b101:
            condition_met = (alu_result == 0);
            break; // BGE
        case 0b110:
            condition_met = (alu_result == 1);
            break; // BLTU
        case 0b111:
            condition_met = (alu_result == 0);
            break; // BGEU
        }

        if (condition_met)
        {
            ex_mem_reg_.branch_taken = true;
            ex_mem_reg_.branch_target_pc = id_ex_reg_.pc + id_ex_reg_.imm;
        }
    }
    // Unconditional Jump Check (JAL/JALR: 1-cycle penalty)
    else if (opcode == 0b1101111 || opcode == 0b1100111)
    {
        uint64_t jump_target;
        if (opcode == 0b1101111)
        {
            jump_target = id_ex_reg_.pc + id_ex_reg_.imm; // JAL
        }
        else
        {
            jump_target = alu_result & ~1;              // JALR (ALU result is Reg + Imm)
            ex_mem_reg_.alu_result = id_ex_reg_.pc + 4; // Set link address (PC+4)
        }

        redirect_pc(jump_target);
        if_id_reg_.reset();
        ex_mem_reg_.branch_taken = true;
    }
}

namespace
{
bool IsVsetWord(uint32_t instruction)
{
    return (instruction & 0x7F) == rvv::OPCODE_OP_V &&
           ((instruction >> 12) & 0x7) == rvv::FUNCT3_OPCFG;
}

uint32_t CsrNumberOf(uint32_t instruction)
{
    return (instruction >> 20) & 0xFFF;
}
} // namespace

uint64_t RV5StageProcessorHF::pipeline_execute_vector(uint64_t rs1, uint64_t rs2)
{
    const uint32_t instruction = id_ex_reg_.instruction;

    // --- Vector CSRs, through the bypass. The instruction in EX/MEM (one ahead) has not
    // written back yet; the one two ahead already has (WB runs first in Step()).
    uint64_t vl = registers_.ReadCsr(RegisterFile::CSR_VL);
    uint64_t vtype = registers_.ReadCsr(RegisterFile::CSR_VTYPE);
    uint64_t vstart = registers_.ReadCsr(RegisterFile::CSR_VSTART);
    if (ex_mem_reg_.is_vector)
    {
        vstart = 0; // every vector instruction leaves vstart = 0
        if (ex_mem_reg_.vcsr_write && IsVsetWord(ex_mem_reg_.instruction))
        {
            vl = ex_mem_reg_.alu_result;
            vtype = ex_mem_reg_.v_vtype;
        }
        // (a vle*ff's vl is known only after MEM: hdu_vector stalls its readers instead)
    }
    else if (ex_mem_reg_.csr_write && CsrNumberOf(ex_mem_reg_.instruction) == RegisterFile::CSR_VSTART)
    {
        vstart = ex_mem_reg_.csr_wdata; // csrw vstart one ahead
    }

    const rvv::VectorRegisterUse use = rvv::DecodeVectorRegisterUse(instruction, vtype);

    if (use.isMemory)
    {
        // Address generation and the accesses happen in MEM. EX only passes on the forwarded
        // base (alu_result) and stride (reg2_data, latched by the caller), and the destination
        // group for the forwarding key / hazard unit.
        ex_vector_.base = use.writeBase;
        ex_vector_.count = use.writeCount;
        return rs1;
    }

    // --- Vector operand read: the register file, overlaid with the EX/MEM bypass.
    ex_vector_scratch_.SyncFrom(registers_);
    RegisterFile &operands = ex_vector_scratch_.File();
    operands.WriteCsr(RegisterFile::CSR_VL, vl);
    operands.WriteCsr(RegisterFile::CSR_VTYPE, vtype);
    operands.WriteCsr(RegisterFile::CSR_VSTART, vstart);

    const size_t vlen_bytes = registers_.GetVlenBytes();
    if (ex_mem_reg_.vreg_write && ex_mem_reg_.v_data.size() == ex_mem_reg_.v_wcount * vlen_bytes)
    {
        // Forwarding unit: the bypass is selected for the registers the producer writes.
        const uint32_t produced = rvv::VectorGroupMask(ex_mem_reg_.rd, ex_mem_reg_.v_wcount);
        vector_forwarded_ = (use.exReads & produced) != 0;
        for (size_t i = 0; i < ex_mem_reg_.v_wcount; ++i)
        {
            const auto begin = ex_mem_reg_.v_data.begin() + i * vlen_bytes;
            operands.WriteVectorRegister(ex_mem_reg_.rd + i,
                                         std::vector<uint8_t>(begin, begin + vlen_bytes));
        }
    }

    // --- Execute (the result lands in the operand file).
    std::vector<RegisterChange> changes;
    const rvv::VectorExecResult result =
        rvv::ExecuteVectorCompute(instruction, rs1, rs2, operands, changes);

    if (use.isVset)
    {
        ex_vector_.vtype = operands.ReadCsr(RegisterFile::CSR_VTYPE);
        return operands.ReadCsr(RegisterFile::CSR_VL); // the new vl (also rd's value)
    }

    // --- The destination group: the decoded one, widened to every register that changed.
    uint32_t first = use.writeBase;
    uint32_t last = use.writeBase + use.writeCount; // exclusive
    if (use.writeCount == 0)
    {
        first = 32;
        last = 0;
    }
    for (const RegisterChange &change : changes)
    {
        if (change.reg_type == 3)
        {
            first = std::min<uint32_t>(first, change.reg_index);
            last = std::max<uint32_t>(last, change.reg_index + 1);
        }
    }
    if (first < last)
    {
        ex_vector_.base = static_cast<uint8_t>(first);
        ex_vector_.count = static_cast<uint8_t>(last - first);
        ex_vector_.data.reserve(ex_vector_.count * vlen_bytes);
        for (uint32_t reg = first; reg < last; ++reg)
        {
            const std::vector<uint8_t> bytes = operands.ReadVectorRegister(reg);
            ex_vector_.data.insert(ex_vector_.data.end(), bytes.begin(), bytes.end());
        }
    }
    return result.rdValue;
}

uint64_t RV5StageProcessorHF::pipeline_execute_csr(uint32_t instruction, uint64_t rs1_value)
{
    const uint8_t funct3 = (instruction >> 12) & 0x7;
    const uint8_t rs1 = (instruction >> 15) & 0x1F;
    const uint32_t csr = CsrNumberOf(instruction);

    // --- CSR read through the bypass (the instruction one ahead writes back next cycle).
    uint64_t old_value = registers_.ReadCsr(csr);
    if (ex_mem_reg_.csr_write && CsrNumberOf(ex_mem_reg_.instruction) == csr)
    {
        old_value = ex_mem_reg_.csr_wdata;
    }
    else if (ex_mem_reg_.is_vector)
    {
        if (csr == RegisterFile::CSR_VSTART)
        {
            old_value = 0;
        }
        else if (ex_mem_reg_.vcsr_write && IsVsetWord(ex_mem_reg_.instruction))
        {
            if (csr == RegisterFile::CSR_VL)
                old_value = ex_mem_reg_.alu_result;
            else if (csr == RegisterFile::CSR_VTYPE)
                old_value = ex_mem_reg_.v_vtype;
        }
    }

    const uint64_t operand = (funct3 & 0b100) ? rs1 : rs1_value; // csrr*i: zero-extended uimm
    uint64_t new_value = old_value;
    bool writes = false;
    switch (funct3 & 0b011)
    {
    case 0b01: // csrrw / csrrwi: always write
        new_value = operand;
        writes = true;
        break;
    case 0b10: // csrrs / csrrsi: set bits, write only if there are any
        new_value = old_value | operand;
        writes = operand != 0;
        break;
    case 0b11: // csrrc / csrrci: clear bits
        new_value = old_value & ~operand;
        writes = operand != 0;
        break;
    default:
        return old_value;
    }
    if (writes && IsReadOnlyCsr(csr))
    {
        std::ostringstream message;
        message << "Illegal instruction: write to read-only CSR 0x" << std::hex << csr;
        throw std::runtime_error(message.str());
    }
    ex_vector_.csr_write = writes;
    ex_vector_.csr_wdata = new_value;
    return old_value;
}

void RV5StageProcessorHF::handle_syscall()
{
    if ((id_ex_reg_.instruction & 0x7F) == 0b1110011 &&
        ((id_ex_reg_.instruction >> 12) & 0x7) == 0b000)
    {
        RequestStop();
        output_status_ = "ECALL_EXIT";
        // DumpState("vm_state.json");
    }
}

uint64_t RV5StageProcessorHF::pipeline_execute_float(uint64_t int_rs1)
{
    uint32_t instruction = id_ex_reg_.instruction;
    uint8_t opcode = instruction & 0b1111111;
    uint8_t funct3 = (instruction >> 12) & 0b111;
    uint8_t funct7 = (instruction >> 25) & 0b1111111;
    uint8_t rm = funct3;

    uint8_t fcsr_status = 0;
    uint64_t alu_result = 0;

    if (rm == 0b111)
    {
        rm = registers_.ReadCsr(0x002);
    }

    uint64_t reg1_value = id_ex_reg_.freg1_data;
    uint64_t reg2_value = id_ex_reg_.freg2_data;
    uint64_t reg3_value = id_ex_reg_.freg3_data;

    if (funct7 == 0b1101000 || funct7 == 0b1111000 || opcode == 0b0000111 || opcode == 0b0100111)
    {
        reg1_value = int_rs1; // GPR source (base address / int operand), already forwarded
    }

    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    // --- FORWARDING for FPR sources (frs1/frs2/frs3) ---
    uint8_t fwd_a = 0; // 2 = EX/MEM, 1 = MEM/WB
    uint8_t fwd_b = 0;
    uint8_t fwd_c = 0;

    // EX/MEM forwarding (highest priority)
    if (ex_mem_reg_.freg_write && (ex_mem_reg_.frd != 0))
    {
        if (ex_mem_reg_.frd == id_ex_reg_.frs1)
            fwd_a = 2;
        if (ex_mem_reg_.frd == id_ex_reg_.frs2)
            fwd_b = 2;
        if (ex_mem_reg_.frd == id_ex_reg_.frs3)
            fwd_c = 2;
    }

    // MEM/WB forwarding (lower priority)
    if (mem_wb_reg_.prev_freg_write && (mem_wb_reg_.prev_frd != 0))
    {
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs1 && fwd_a != 2)
            fwd_a = 1;
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs2 && fwd_b != 2)
            fwd_b = 1;
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs3 && fwd_c != 2)
            fwd_c = 1;
    }

    // Apply forwarding to source values
    if (fwd_a == 2)
        reg1_value = ex_mem_reg_.f_alu_result;
    else if (fwd_a == 1)
        reg1_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    if (fwd_b == 2)
        reg2_value = ex_mem_reg_.f_alu_result;
    else if (fwd_b == 1)
        reg2_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    if (fwd_c == 2)
        reg3_value = ex_mem_reg_.f_alu_result;
    else if (fwd_c == 1)
        reg3_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    // Re-apply GPR-read override (for conversion ops) after forwarding decision: if instruction
    // requires GPR, use it.
    if (funct7 == 0b1101000 || funct7 == 0b1111000 || opcode == 0b0000111 || opcode == 0b0100111)
    {
        reg1_value = int_rs1; // GPR source (base address / int operand), already forwarded
    }

    // Re-apply immediate if ALUSrc was set (immediates take precedence for reg2)
    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    // Forward store data for FPR stores (frs2)
    uint64_t fstore_data = id_ex_reg_.freg2_data;
    if (fwd_b == 2)
        fstore_data = ex_mem_reg_.f_alu_result;
    else if (fwd_b == 1)
        fstore_data = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                  : mem_wb_reg_.prev_f_alu_result;

    ex_mem_reg_.freg2_data = fstore_data;
    // Execute FP operation
    alu::AluOp aluOperation = control_unit_.GetAluSignal(instruction, id_ex_reg_.alu_op > 0);
    std::tie(alu_result, fcsr_status) =
        alu::Alu::fpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);

    registers_.WriteCsr(0x003, fcsr_status);

    return alu_result;
}

uint64_t RV5StageProcessorHF::pipeline_execute_double(uint64_t int_rs1)
{
    uint32_t instruction = id_ex_reg_.instruction;
    uint8_t opcode = instruction & 0b1111111;
    uint8_t funct3 = (instruction >> 12) & 0b111;
    uint8_t funct7 = (instruction >> 25) & 0b1111111;
    uint8_t rm = funct3;

    uint8_t fcsr_status = 0;
    uint64_t alu_result = 0;

    if (rm == 0b111)
    {
        rm = registers_.ReadCsr(0x002);
    }

    uint64_t reg1_value = id_ex_reg_.freg1_data;
    uint64_t reg2_value = id_ex_reg_.freg2_data;
    uint64_t reg3_value = id_ex_reg_.freg3_data;

    if (funct7 == 0b1101001 || funct7 == 0b1111001 || opcode == 0b0000111 || opcode == 0b0100111)
    {
        reg1_value = int_rs1; // GPR source (base address / int operand), already forwarded
    }

    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    // --- FORWARDING for FPR sources (frs1/frs2/frs3) ---
    uint8_t fwd_a = 0; // 2 = EX/MEM, 1 = MEM/WB
    uint8_t fwd_b = 0;
    uint8_t fwd_c = 0;

    if (ex_mem_reg_.freg_write && (ex_mem_reg_.frd != 0))
    {
        if (ex_mem_reg_.frd == id_ex_reg_.frs1)
            fwd_a = 2;
        if (ex_mem_reg_.frd == id_ex_reg_.frs2)
            fwd_b = 2;
        if (ex_mem_reg_.frd == id_ex_reg_.frs3)
            fwd_c = 2;
    }

    if (mem_wb_reg_.prev_freg_write && (mem_wb_reg_.prev_frd != 0))
    {
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs1 && fwd_a != 2)
            fwd_a = 1;
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs2 && fwd_b != 2)
            fwd_b = 1;
        if (mem_wb_reg_.prev_frd == id_ex_reg_.frs3 && fwd_c != 2)
            fwd_c = 1;
    }

    if (fwd_a == 2)
        reg1_value = ex_mem_reg_.f_alu_result;
    else if (fwd_a == 1)
        reg1_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    if (fwd_b == 2)
        reg2_value = ex_mem_reg_.f_alu_result;
    else if (fwd_b == 1)
        reg2_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    if (fwd_c == 2)
        reg3_value = ex_mem_reg_.f_alu_result;
    else if (fwd_c == 1)
        reg3_value = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                 : mem_wb_reg_.prev_f_alu_result;

    // Re-apply GPR-read override for specific instructions
    if (funct7 == 0b1101001 || funct7 == 0b1111001 || opcode == 0b0000111 || opcode == 0b0100111)
    {
        reg1_value = int_rs1; // GPR source (base address / int operand), already forwarded
    }

    // Re-apply immediate for reg2
    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    // Forward store data for FPR stores
    uint64_t fstore_data = id_ex_reg_.freg2_data;
    if (fwd_b == 2)
        fstore_data = ex_mem_reg_.f_alu_result;
    else if (fwd_b == 1)
        fstore_data = mem_wb_reg_.prev_mem_to_reg ? mem_wb_reg_.prev_f_memory_data
                                                  : mem_wb_reg_.prev_f_alu_result;

    ex_mem_reg_.freg2_data = fstore_data;

    alu::AluOp alu_operation = control_unit_.GetAluSignal(instruction, id_ex_reg_.alu_op > 0);
    std::tie(alu_result, fcsr_status) =
        alu::Alu::dfpexecute(alu_operation, reg1_value, reg2_value, reg3_value, rm);

    registers_.WriteCsr(0x003, fcsr_status);

    return alu_result;
}
}//namespace Kites
