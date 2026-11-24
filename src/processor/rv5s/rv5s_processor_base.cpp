#include "common/debug_colors.h"
#include "common/instructions.h"
#include "processor/rv5s/rv5s_processor_base.h"
#include "processor/vector/vector_operands.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Kites
{
void RV5StageVM_Base::Run()
{
    ClearStop();
    while (!stop_requested_ && (program_counter_ < program_size_ || !is_pipeline_drained()))
    {
        if (last_breakpoint_pc_ && program_counter_ != *last_breakpoint_pc_)
        {
            last_breakpoint_pc_.reset();
        }

        // if we hit a break point (and it's a new PC), we pause execution
        // This prevents firing the same breakpoint multiple times during stalls
        if (std::find(breakpoints_.begin(), breakpoints_.end(), program_counter_) !=
                breakpoints_.end() && program_counter_ != last_breakpoint_pc_)
        {
            pause_requested_ = true;
            last_breakpoint_pc_ = program_counter_;
            emit processorPausedAtBreakpointSignal();
        }

        {
            QMutexLocker locker(&pause_mutex_);
            // using while because thie wait can be interrupted by spurious wakeups
            while (pause_requested_ && !stop_requested_)
            {
                pause_wait_condition_.wait(&pause_mutex_);
            }

            if (stop_requested_) // if we were requested to stop while paused
                break;
        }
        last_executed_pc_ = id_ex_reg_.pc; // this pc will be executed in this step, 
        //so we set it as the last executed pc before we step
        Step();
        SetActiveWireNames();
        cpi_ = instructions_retired_
                   ? static_cast<float>(cycle_s_) / static_cast<float>(instructions_retired_)
                   : 0.0f;
        ipc_ = cycle_s_ ? static_cast<float>(instructions_retired_) / static_cast<float>(cycle_s_)
                        : 0.0f;
        
        setProcessorState();
        emit updateCircuitStateSignal(active_wires_);
        emit processorClockedSignal(processor_state_);

        // handling the delay
        {
            QMutexLocker locker(&pause_mutex_);
            // A zero delay (the CLI) must not wait at all: on Windows even a 0 ms timed
            // wait sleeps for a scheduler tick (~16 ms), i.e. ~16 ms per instruction.
            if (stop_requested_ || pause_requested_ || step_delay_ == 0)
                continue;
            pause_wait_condition_.wait(&pause_mutex_, step_delay_);
            // we wait for step_delay_ milliseconds or until notified to wake up
        }
    }
    if (stop_requested_)
    {
        emit processorClockedSignal(processor_state_);
        // we emit the processor clocked signal once more to update the ui
        // this will get rid of any highlights as they pause when we stop
        // before execution is complete
    }
}

void RV5StageVM_Base::DebugRun()
{
    ClearStop();
    while (!stop_requested_ && (program_counter_ < program_size_ || !is_pipeline_drained()))
    {
#ifdef VM_DEBUG_PRINTS
        print_pipeline_registers_debug();
#endif
        Step();
        std::cout << "Cycle: " << cycle_s_ << " | PC: 0x" << std::hex << program_counter_
                  << std::dec << std::endl;
    }
#ifdef VM_DEBUG_PRINTS
    print_pipeline_registers_debug();
#endif
}

void RV5StageVM_Base::Reset()
{
    program_counter_      = 0;
    instructions_retired_ = 0;
    cycle_s_              = 0;
    stall_cycles_         = 0;
    pc_redirected_        = false;
    last_breakpoint_pc_.reset(); // Clear breakpoint tracking on reset

    registers_.Reset();
    memory_controller_.reset();
    control_unit_.Reset();

    if_id_reg_.reset();
    id_ex_reg_.reset();
    ex_mem_reg_.reset();
    mem_wb_reg_.reset();
    current_delta_ = RV5StageStepDelta{};
    undo_stack_ = std::stack<RV5StageStepDelta>();
    redo_stack_ = std::stack<RV5StageStepDelta>();
    processor_state_.reset();
}

void RV5StageVM_Base::begin_step_delta()
{
    current_delta_                           = RV5StageStepDelta{};
    current_delta_.old_pc                    = program_counter_;
    current_delta_.old_cycle                 = cycle_s_;
    current_delta_.old_stall_cycles          = stall_cycles_;
    current_delta_.old_instructions_retired  = instructions_retired_;
    current_delta_.old_branch_mispredictions = branch_mispredictions_;

    current_delta_.pipeline_register_change.old_if_id_reg  = if_id_reg_;
    current_delta_.pipeline_register_change.old_id_ex_reg  = id_ex_reg_;
    current_delta_.pipeline_register_change.old_ex_mem_reg = ex_mem_reg_;
    current_delta_.pipeline_register_change.old_mem_wb_reg = mem_wb_reg_;
}

void RV5StageVM_Base::finalize_step_delta()
{
    current_delta_.new_pc                    = program_counter_;
    current_delta_.new_cycle                 = cycle_s_;
    current_delta_.new_stall_cycles          = stall_cycles_;
    current_delta_.new_instructions_retired  = instructions_retired_;
    current_delta_.new_branch_mispredictions = branch_mispredictions_;

    current_delta_.pipeline_register_change.new_if_id_reg  = if_id_reg_;
    current_delta_.pipeline_register_change.new_id_ex_reg  = id_ex_reg_;
    current_delta_.pipeline_register_change.new_ex_mem_reg = ex_mem_reg_;
    current_delta_.pipeline_register_change.new_mem_wb_reg = mem_wb_reg_;

    undo_stack_.push(current_delta_);
    while (!redo_stack_.empty())
    {
        redo_stack_.pop();
    }
}

void RV5StageVM_Base::setProcessorState()
{
    // processor_state_.programCounters[toIndex(PipelineStage::IF)]  = program_counter_;
    // processor_state_.programCounters[toIndex(PipelineStage::ID)]  = if_id_reg_.pc;
    // processor_state_.programCounters[toIndex(PipelineStage::EX)]  = id_ex_reg_.pc;
    // processor_state_.programCounters[toIndex(PipelineStage::MEM)] = ex_mem_reg_.pc;
    // processor_state_.programCounters[toIndex(PipelineStage::WB)]  = mem_wb_reg_.pc;
    processor_state_.programCounters = 
    {program_counter_, if_id_reg_.pc, id_ex_reg_.pc, ex_mem_reg_.pc, mem_wb_reg_.pc};
}

namespace
{
/// Whether an F/D instruction writes its result to an integer register (x[rd]) instead of an
/// FPR: compares, fcvt to an integer, fmv.x and fclass. Decode uses it to set reg_write/freg_write.
bool FpResultGoesToGpr(uint32_t instruction)
{
    const uint8_t funct7 = ((instruction >> 25) & 0x7F) & ~1u; // without the .s/.d bit
    return (instruction & 0x7F) == 0b1010011 &&                  // OP-FP (not FMA, not FP loads)
           (funct7 == 0b1010000     // feq / flt / fle
            || funct7 == 0b1100000  // fcvt.w / wu / l / lu
            || funct7 == 0b1110000); // fmv.x.w / fmv.x.d, fclass
}
} // namespace

void RV5StageVM_Base::pipeline_decode()
{
    uint32_t instruction = if_id_reg_.instruction;
    if (instruction == NOP)
    {
        // Pass through fields as needed
        id_ex_reg_.pc          = if_id_reg_.pc;
        id_ex_reg_.instruction = instruction;
        id_ex_reg_.imm         = 0;
        id_ex_reg_.rs1         = id_ex_reg_.rs2 = id_ex_reg_.rd = 0;
        id_ex_reg_.reg1_data   = 0;
        id_ex_reg_.reg2_data   = 0;

        // Clear FPR fields so stale values don't poison forwarding
        id_ex_reg_.frs1 = id_ex_reg_.frs2 = id_ex_reg_.frs3 = id_ex_reg_.frd = 0;
        id_ex_reg_.freg1_data = 0;
        id_ex_reg_.freg2_data = 0;
        id_ex_reg_.freg3_data = 0;

        // Critically: zero *all* control signals so downstream stages are idle
        id_ex_reg_.reg_write  = false;
        id_ex_reg_.freg_write = false;
        id_ex_reg_.branch     = false;
        id_ex_reg_.alu_src    = false;
        id_ex_reg_.mem_read   = false;
        id_ex_reg_.mem_write  = false;
        id_ex_reg_.mem_to_reg = false;
        id_ex_reg_.alu_op     = 0;

        id_ex_reg_.is_vector  = false;
        id_ex_reg_.vreg_write = false;
        id_ex_reg_.vcsr_write = false;
        id_ex_reg_.vmem_read  = false;
        id_ex_reg_.vmem_write = false;
        return;
    }
    // Control Unit: Generate signals based on the instruction
    control_unit_.SetControlSignals(instruction);

    // Latch data for the ID/EX register
    id_ex_reg_.pc = if_id_reg_.pc;
    id_ex_reg_.instruction = instruction;
    id_ex_reg_.imm = ImmGenerator(instruction);

    // Extract register numbers
    id_ex_reg_.rs1 = (instruction >> 15) & 0x1F;
    id_ex_reg_.rs2 = (instruction >> 20) & 0x1F;
    id_ex_reg_.rd = (instruction >> 7) & 0x1F;

    // Extract FP register indices for F/D instructions
    if (instruction_set::isFInstruction(instruction) ||
        instruction_set::isDInstruction(instruction))
    {
        id_ex_reg_.frs1 = (instruction >> 15) & 0x1F;
        id_ex_reg_.frs2 = (instruction >> 20) & 0x1F;
        id_ex_reg_.frs3 = (instruction >> 27) & 0x1F;
        id_ex_reg_.frd = (instruction >> 7) & 0x1F;

        id_ex_reg_.freg1_data = registers_.ReadFpr(id_ex_reg_.frs1);
        id_ex_reg_.freg2_data = registers_.ReadFpr(id_ex_reg_.frs2);
        id_ex_reg_.freg3_data = registers_.ReadFpr(id_ex_reg_.frs3);


    }
    // Always read the GPR operands: FP loads/stores and int->FP ops use x[rs1] too (base address
    // or integer source), and EX forwards these values for every instruction.
    id_ex_reg_.reg1_data = registers_.ReadGpr(id_ex_reg_.rs1);
    id_ex_reg_.reg2_data = registers_.ReadGpr(id_ex_reg_.rs2);

    // Pass all control signals to the next stage. reg_write means "writes x[rd]" and freg_write
    // "writes f[rd]", so forwarding, the HDU and WB can trust them: an FP instruction writes an FPR
    // unless its result goes to an integer register (feq/flt/fle, fcvt.w*/l*, fmv.x.*, fclass).
    const bool is_fp = instruction_set::isFInstruction(instruction) ||
                       instruction_set::isDInstruction(instruction);
    const bool writes_rd = control_unit_.GetRegWrite();
    const bool fp_to_gpr = is_fp && FpResultGoesToGpr(instruction);
    id_ex_reg_.reg_write  = writes_rd && (!is_fp || fp_to_gpr);
    id_ex_reg_.freg_write = writes_rd && is_fp && !fp_to_gpr;
    id_ex_reg_.branch     = control_unit_.GetBranch();
    id_ex_reg_.alu_src    = control_unit_.GetAluSrc();
    id_ex_reg_.mem_read   = control_unit_.GetMemRead();
    id_ex_reg_.mem_write  = control_unit_.GetMemWrite();
    id_ex_reg_.mem_to_reg = control_unit_.GetMemToReg();
    id_ex_reg_.alu_op     = control_unit_.GetAluOp();

    // Vector control. Only H_F carries these past EX; the other variants run a vector
    // instruction whole in EX and ignore them.
    id_ex_reg_.is_vector = rvv::IsVectorInstruction(instruction);
    if (id_ex_reg_.is_vector)
    {
        const rvv::VectorRegisterUse use = rvv::DecodeVectorRegisterUse(
            instruction, registers_.ReadCsr(RegisterFile::CSR_VTYPE));
        id_ex_reg_.vreg_write = use.writeCount > 0;
        id_ex_reg_.vcsr_write = use.writesVlVtype;
        id_ex_reg_.vmem_read  = use.isLoad;
        id_ex_reg_.vmem_write = use.isStore;
    }
    else
    {
        id_ex_reg_.vreg_write = id_ex_reg_.vcsr_write = false;
        id_ex_reg_.vmem_read = id_ex_reg_.vmem_write = false;
    }
}

void RV5StageVM_Base::memory_writeback()
{
    auto read_bytes = [&](uint64_t addr, size_t byte_count)
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(byte_count);
        for (size_t i = 0; i < byte_count; ++i)
        {
            bytes.push_back(memory_controller_.peekByte(addr + i));
        }
        return bytes;
    };

    switch ((mem_wb_reg_.instruction >> 12) & 0b111)
    {
    case 0b000:
    { // SB
        auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 1);
        memory_controller_.writeByte(ex_mem_reg_.alu_result, ex_mem_reg_.reg2_data & 0xFF);
        auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 1);
        if (old_bytes_vec != new_bytes_vec)
        {
            current_delta_.memory_changes.push_back(
                {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
        }
        break;
    }
    case 0b001:
    { // SH
        auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 2);
        memory_controller_.writeHalfWord(ex_mem_reg_.alu_result, ex_mem_reg_.reg2_data & 0xFFFF);
        auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 2);
        if (old_bytes_vec != new_bytes_vec)
        {
            current_delta_.memory_changes.push_back(
                {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
        }
        break;
    }
    case 0b010:
    { // SW
        auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 4);
        memory_controller_.writeWord(ex_mem_reg_.alu_result, ex_mem_reg_.reg2_data & 0xFFFFFFFF);
        auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 4);
        if (old_bytes_vec != new_bytes_vec)
        {
            current_delta_.memory_changes.push_back(
                {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
        }
        break;
    }
    case 0b011:
    { // SD
        auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 8);
        memory_controller_.writeDoubleWord(ex_mem_reg_.alu_result,
                                           ex_mem_reg_.reg2_data & 0xFFFFFFFFFFFFFFFF);
        auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 8);
        if (old_bytes_vec != new_bytes_vec)
        {
            current_delta_.memory_changes.push_back(
                {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
        }
        break;
    }
    }
}

/** Ignore the next two fucntion they i Ill refactor thosee i promise :'( */

void RV5StageVM_Base::memory_read()
{
    switch ((mem_wb_reg_.instruction >> 12) & 0b111)
    {
    case 0b000:
    { // LB
        mem_wb_reg_.memory_data =
            static_cast<int8_t>(memory_controller_.readByte(ex_mem_reg_.alu_result));
        break;
    }
    case 0b001:
    { // LH
        mem_wb_reg_.memory_data =
            static_cast<int16_t>(memory_controller_.readHalfWord(ex_mem_reg_.alu_result));
        break;
    }
    case 0b010:
    { // LW
        mem_wb_reg_.memory_data =
            static_cast<int32_t>(memory_controller_.readWord(ex_mem_reg_.alu_result));
        break;
    }
    case 0b011:
    { // LD
        mem_wb_reg_.memory_data = memory_controller_.readDoubleWord(ex_mem_reg_.alu_result);
        break;
    }
    case 0b100:
    { // LBU
        mem_wb_reg_.memory_data =
            static_cast<uint8_t>(memory_controller_.readByte(ex_mem_reg_.alu_result));
        break;
    }
    case 0b101:
    { // LHU
        mem_wb_reg_.memory_data =
            static_cast<uint16_t>(memory_controller_.readHalfWord(ex_mem_reg_.alu_result));
        break;
    }
    case 0b110:
    { // LWU
        mem_wb_reg_.memory_data =
            static_cast<uint32_t>(memory_controller_.readWord(ex_mem_reg_.alu_result));
        break;
    }
    }
}

void RV5StageVM_Base::register_write_back(const uint64_t &write_data)
{
    switch (mem_wb_reg_.instruction & 0b1111111)
    {
    case 0b0110011: // R-Type
    case 0b0010011: // I-Type
    case 0b0010111:
    { // AUIPC
        registers_.WriteGpr(mem_wb_reg_.rd, write_data);
        break;
    }
    case 0b0000011:
    { // Load
        registers_.WriteGpr(mem_wb_reg_.rd, write_data);
        break;
    }
    case 0b1100111: // JALR
    case 0b1101111:
    { // JAL
        registers_.WriteGpr(mem_wb_reg_.rd, mem_wb_reg_.pc + 4);
        break;
    }
    case 0b0110111:
    { // LUI
        registers_.WriteGpr(mem_wb_reg_.rd, write_data << 12);
        break;
    }
    default:
        break;
    }
}

//////////////////////

void RV5StageVM_Base::redirect_pc(uint64_t target)
{
    program_counter_ = target;
    pc_redirected_ = true;
}

// IF stage plus PC update. Runs after WB/MEM/EX/ID so a redirect resolved this cycle
// is visible. The target is fetched next cycle, leaving a bubble in IF/ID now, the same
// as hardware that only learns the target at the end of the resolving stage.
void RV5StageVM_Base::fetch_and_advance_pc(bool stalled)
{
    if (pc_redirected_)
    {
        if_id_reg_.reset();
        pc_redirected_ = false;
        return;
    }

    pipeline_fetch();
    // A stall skipped the fetch, so hold the PC on the instruction still to be fetched.
    if (!stalled)
    {
        program_counter_ += 4;
    }
}

void RV5StageVM_Base::pipeline_memory()
{

    // --- B-Type Conditional Branch Resolution (3-Cycle Penalty) ---
    if (ex_mem_reg_.branch_taken && (ex_mem_reg_.instruction & 0b1111111) == 0b1100011)
    {
        // B-Type misprediction confirmed in MEM stage. Hardware flushes the pipeline.

        redirect_pc(ex_mem_reg_.branch_target_pc);
        if_id_reg_.reset();
        id_ex_reg_.reset();
        branch_mispredictions_++;
    }

    // since we are running cycle backward
    // by the time execture check this register forwarding previous results are gone
    // so we store these seperately
    mem_wb_reg_.prev_rd          = mem_wb_reg_.rd;
    mem_wb_reg_.prev_alu_result  = mem_wb_reg_.alu_result;
    mem_wb_reg_.prev_mem_to_reg  = mem_wb_reg_.mem_to_reg;
    mem_wb_reg_.prev_reg_write   = mem_wb_reg_.reg_write;
    mem_wb_reg_.prev_memory_data = mem_wb_reg_.memory_data;
    // Save FPR prev fields for forwarding (mirrors GPR prev fields above)
    mem_wb_reg_.prev_frd           = mem_wb_reg_.frd;
    mem_wb_reg_.prev_f_alu_result  = mem_wb_reg_.f_alu_result;
    mem_wb_reg_.prev_f_memory_data = mem_wb_reg_.f_memory_data;
    mem_wb_reg_.prev_freg_write    = mem_wb_reg_.freg_write;
    // --- Standard MEM Operations ---
    mem_wb_reg_.pc          = ex_mem_reg_.pc;
    mem_wb_reg_.instruction = ex_mem_reg_.instruction;
    mem_wb_reg_.alu_result  = ex_mem_reg_.alu_result;
    mem_wb_reg_.rd          = ex_mem_reg_.rd;
    mem_wb_reg_.reg_write   = ex_mem_reg_.reg_write;
    mem_wb_reg_.mem_to_reg  = ex_mem_reg_.mem_to_reg;
    // Latch FPR fields from EX/MEM
    mem_wb_reg_.frd          = ex_mem_reg_.frd;
    mem_wb_reg_.f_alu_result = ex_mem_reg_.f_alu_result;
    mem_wb_reg_.freg_write   = ex_mem_reg_.freg_write;
    // Vector and Zicsr fields (only H_F sets them). v_data is copied, not moved: EX still
    // reads EX/MEM for its bypass later in this step.
    mem_wb_reg_.is_vector  = ex_mem_reg_.is_vector;
    mem_wb_reg_.vreg_write = ex_mem_reg_.vreg_write;
    mem_wb_reg_.vcsr_write = ex_mem_reg_.vcsr_write;
    mem_wb_reg_.v_wcount   = ex_mem_reg_.v_wcount;
    mem_wb_reg_.v_data     = ex_mem_reg_.v_data;
    mem_wb_reg_.v_vtype    = ex_mem_reg_.v_vtype;
    mem_wb_reg_.csr_write  = ex_mem_reg_.csr_write;
    mem_wb_reg_.csr_wdata  = ex_mem_reg_.csr_wdata;
    if (ex_mem_reg_.is_vector && (ex_mem_reg_.vmem_read || ex_mem_reg_.vmem_write))
    {
        pipeline_memory_vector();
    }

    bool is_F_Instruction = instruction_set::isFInstruction(mem_wb_reg_.instruction);
    bool is_D_Instruction = instruction_set::isDInstruction(mem_wb_reg_.instruction);

    // Memory Access
    if (ex_mem_reg_.mem_read)
    {
        // Load instruction: Result available at end of this stage (Load-Use still needs 1 NOP
        // stall)
        if (is_F_Instruction)
        { // FLW

            mem_wb_reg_.memory_data =
                static_cast<int32_t>(memory_controller_.readWord(ex_mem_reg_.alu_result));
            mem_wb_reg_.f_memory_data = mem_wb_reg_.memory_data;
        }
        else if (is_D_Instruction)
        { // FLD
            mem_wb_reg_.memory_data = memory_controller_.readDoubleWord(ex_mem_reg_.alu_result);
            mem_wb_reg_.f_memory_data = mem_wb_reg_.memory_data;
        }
        else
        {
            switch ((mem_wb_reg_.instruction >> 12) & 0b111)
            {
            case 0b000:
            { // LB
                mem_wb_reg_.memory_data =
                    static_cast<int8_t>(memory_controller_.readByte(ex_mem_reg_.alu_result));
                break;
            }
            case 0b001:
            { // LH
                mem_wb_reg_.memory_data =
                    static_cast<int16_t>(memory_controller_.readHalfWord(ex_mem_reg_.alu_result));
                break;
            }
            case 0b010:
            { // LW
                mem_wb_reg_.memory_data =
                    static_cast<int32_t>(memory_controller_.readWord(ex_mem_reg_.alu_result));
                break;
            }
            case 0b011:
            { // LD
                mem_wb_reg_.memory_data = memory_controller_.readDoubleWord(ex_mem_reg_.alu_result);
                break;
            }
            case 0b100:
            { // LBU
                mem_wb_reg_.memory_data =
                    static_cast<uint8_t>(memory_controller_.readByte(ex_mem_reg_.alu_result));
                break;
            }
            case 0b101:
            { // LHU
                mem_wb_reg_.memory_data =
                    static_cast<uint16_t>(memory_controller_.readHalfWord(ex_mem_reg_.alu_result));
                break;
            }
            case 0b110:
            { // LWU
                mem_wb_reg_.memory_data =
                    static_cast<uint32_t>(memory_controller_.readWord(ex_mem_reg_.alu_result));
                break;
            }
            }
        }
    }
    else if (ex_mem_reg_.mem_write)
    {
        auto read_bytes = [&](uint64_t addr, size_t byte_count)
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(byte_count);
            for (size_t i = 0; i < byte_count; ++i)
            {
                bytes.push_back(memory_controller_.peekByte(addr + i));
            }
            return bytes;
        };

        if (is_F_Instruction)
        { // FSW
            auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 4);
            memory_controller_.writeWord(ex_mem_reg_.alu_result,
                                         ex_mem_reg_.freg2_data & 0xFFFFFFFF);
            auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 4);
            if (old_bytes_vec != new_bytes_vec)
            {
                current_delta_.memory_changes.push_back(
                    {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
            }
        }
        else if (is_D_Instruction)
        { // FSD
            auto old_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 8);
            memory_controller_.writeDoubleWord(ex_mem_reg_.alu_result,
                                               ex_mem_reg_.freg2_data & 0xFFFFFFFFFFFFFFFF);
            auto new_bytes_vec = read_bytes(ex_mem_reg_.alu_result, 8);
            if (old_bytes_vec != new_bytes_vec)
            {
                current_delta_.memory_changes.push_back(
                    {ex_mem_reg_.alu_result, old_bytes_vec, new_bytes_vec});
            }
        }
        else
        {
            memory_writeback();
        }
    }
}

void RV5StageVM_Base::pipeline_writeback()
{
    // Every real instruction reaching WB retires, whether or not it writes a register
    // (stores, branches, jumps to x0, nops). Bubbles carry INVALID_PC.
    if (mem_wb_reg_.pc != INVALID_PC)
    {
        instructions_retired_++;
    }

    if (mem_wb_reg_.is_vector)
    {
        pipeline_writeback_vector();
    }
    if (mem_wb_reg_.csr_write)
    {
        pipeline_writeback_csr();
    }

    if (!mem_wb_reg_.reg_write && !mem_wb_reg_.freg_write)
    {
        return;
    }

    // --- FP Writeback Delegation ---
    // Before the rd != 0 check: for FP ops rd names an FPR, and f0 is writable.
    if (instruction_set::isFInstruction(mem_wb_reg_.instruction))
    {
        pipeline_writeback_float();
        return;
    }
    else if (instruction_set::isDInstruction(mem_wb_reg_.instruction))
    {
        pipeline_writeback_double();
        return;
    }

    if (mem_wb_reg_.rd != 0)
    {
        uint64_t write_data =
            mem_wb_reg_.mem_to_reg ? mem_wb_reg_.memory_data : mem_wb_reg_.alu_result;
        // JAL/JALR write the link address, not the ALU result.
        uint8_t opcode = mem_wb_reg_.instruction & 0b1111111;
        if (opcode == 0b1100111 || opcode == 0b1101111)
        {
            write_data = mem_wb_reg_.pc + 4;
        }

        // Record state for Undo/Redo
        uint64_t old_value = registers_.ReadGpr(mem_wb_reg_.rd);
        if (old_value != write_data)
        {
            current_delta_.register_changes.push_back({mem_wb_reg_.rd,
                                                       0, // GPR type
                                                       old_value, write_data});
        }

        switch (opcode)
        {
        case 0b0110011: // R-Type
        case 0b0010011: // I-Type
        case 0b0010111: // AUIPC
        case 0b0111011: // OP-32 R-type (ADDW, SUBW, MULW, DIVW, DIVUW, REMW, REMUW, SLLW, SRLW, SRAW)
        case 0b0011011: // OP-32 I-type (ADDIW, SLLIW, SRLIW, SRAIW)
        case rvv::OPCODE_OP_V: // vset*: the new vl
        case 0b1110011:        // csrr*: the old CSR value
        {
            registers_.WriteGpr(mem_wb_reg_.rd, write_data);
            break;
        }
        case 0b0000011:
        { // Load
            registers_.WriteGpr(mem_wb_reg_.rd, write_data);
            break;
        }
        case 0b1100111: // JALR
        case 0b1101111:
        { // JAL
            registers_.WriteGpr(mem_wb_reg_.rd, mem_wb_reg_.pc + 4);
            break;
        }
        case 0b0110111:
        { // LUI
            registers_.WriteGpr(mem_wb_reg_.rd, write_data);
            break;
        }
        default:
            break;
        }
    }
}

void RV5StageVM_Base::pipeline_writeback_float()
{
    // yes i just copy pasted this from rvss
    uint64_t current_instruction_ = mem_wb_reg_.instruction;
    uint8_t opcode = current_instruction_ & 0b1111111;
    uint8_t rd = (current_instruction_ >> 7) & 0b11111;

    uint64_t old_reg = 0;
    unsigned int reg_index = rd;
    unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
    uint64_t new_reg = 0;

    // write to GPR: decode set reg_write only for FP ops whose result is an integer
    if (mem_wb_reg_.reg_write)
    { // f(eq|lt|le).s, fcvt.(w|wu|l|lu).s, fmv.x.w, fclass.s
        old_reg = registers_.ReadGpr(rd);
        registers_.WriteGpr(rd, mem_wb_reg_.alu_result);
        new_reg = mem_wb_reg_.alu_result;
        reg_type = 0; // GPR
    }
    // write to FPR
    else if (opcode == 0b0000111)
    {
        old_reg = registers_.ReadFpr(rd);
        registers_.WriteFpr(rd, mem_wb_reg_.memory_data);
        new_reg = mem_wb_reg_.memory_data;
        reg_type = 2; // FPR
    }
    else
    {
        old_reg = registers_.ReadFpr(rd);
        registers_.WriteFpr(rd, mem_wb_reg_.alu_result);
        new_reg = mem_wb_reg_.alu_result;
        reg_type = 2; // FPR
    }

    if (old_reg != new_reg)
    {
        current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
    }
}

void RV5StageVM_Base::pipeline_writeback_double()
{
    uint64_t current_instruction_ = mem_wb_reg_.instruction;
    uint8_t opcode = current_instruction_ & 0b1111111;
    uint8_t rd = (current_instruction_ >> 7) & 0b11111;

    uint64_t old_reg = 0;
    unsigned int reg_index = rd;
    unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
    uint64_t new_reg = 0;

    // write to GPR: decode set reg_write only for FP ops whose result is an integer
    if (mem_wb_reg_.reg_write)
    { // f(eq|lt|le).d, fcvt.(w|wu|l|lu).d, fmv.x.d, fclass.d
        old_reg = registers_.ReadGpr(rd);
        registers_.WriteGpr(rd, mem_wb_reg_.alu_result);
        new_reg = mem_wb_reg_.alu_result;
        reg_type = 0; // GPR
    }
    // write to FPR
    else if (opcode == 0b0000111)
    {
        old_reg = registers_.ReadFpr(rd);
        registers_.WriteFpr(rd, mem_wb_reg_.memory_data);
        new_reg = mem_wb_reg_.memory_data;
        reg_type = 2; // FPR
    }
    else
    {
        old_reg = registers_.ReadFpr(rd);
        registers_.WriteFpr(rd, mem_wb_reg_.alu_result);
        new_reg = mem_wb_reg_.alu_result;
        reg_type = 2; // FPR
    }

    if (old_reg != new_reg)
    {
        current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
    }
}


uint64_t RV5StageVM_Base::execute_csr(uint32_t instruction, uint64_t rs1_value)
{
    const uint8_t funct3 = (instruction >> 12) & 0x7;
    const uint8_t rs1 = (instruction >> 15) & 0x1F;
    const auto csr = static_cast<uint16_t>((instruction >> 20) & 0xFFF);
    const uint64_t old_value = registers_.ReadCsr(csr);
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
    if (writes)
    {
        if (IsReadOnlyCsr(csr))
        {
            std::ostringstream message;
            message << "Illegal instruction: write to read-only CSR 0x" << std::hex << csr;
            throw std::runtime_error(message.str());
        }
        registers_.WriteCsr(csr, new_value);
        const uint64_t written = registers_.ReadCsr(csr);
        if (written != old_value)
        {
            current_delta_.register_changes.push_back({csr, 1, old_value, written});
        }
    }
    return old_value;
}

uint64_t RV5StageVM_Base::special_execute_result(uint32_t instruction, uint64_t alu_result, uint64_t rs1_value)
{
    const uint8_t opcode = instruction & 0x7F;
    if (opcode == 0b1110011 && ((instruction >> 12) & 0x7) != 0)
    {
        return execute_csr(instruction, rs1_value);
    }
    return alu_result;
}

uint64_t RV5StageVM_Base::execute_vector(uint32_t instruction, uint64_t rs1_value,
                                         uint64_t rs2_value)
{
    return rvv::ExecuteVectorInstruction(instruction, rs1_value, rs2_value, registers_,
                                         memory_controller_, current_delta_.register_changes,
                                         current_delta_.memory_changes)
        .rdValue;
}

void RV5StageVM_Base::pipeline_memory_vector()
{
    const uint32_t instruction = ex_mem_reg_.instruction;
    // EX latched the forwarded base address in alu_result and x[rs2] (the stride) in reg2_data.
    const rvv::VectorMemPlan plan = rvv::PlanVectorMemory(instruction, ex_mem_reg_.alu_result,
                                                          ex_mem_reg_.reg2_data, registers_);
    const rvv::VectorMemData data =
        rvv::AccessVectorMemory(plan, memory_controller_, current_delta_.memory_changes);
    if (!ex_mem_reg_.vmem_read)
    {
        return; // a store is complete: memory was written above
    }

    // Build the destination group as it will look after this load: the old vd (tail and
    // masked-off elements stay undisturbed) with the loaded elements merged in.
    mem_vector_scratch_.SyncFrom(registers_);
    std::vector<RegisterChange> loaded;
    rvv::WriteBackVectorMemory(plan, data, mem_vector_scratch_.File(), loaded);

    // The group WB writes: the decoded one, widened to every register the load changed.
    uint32_t first = ex_mem_reg_.rd;
    uint32_t last = first + ex_mem_reg_.v_wcount; // exclusive
    for (const RegisterChange &change : loaded)
    {
        if (change.reg_type == 3)
        {
            first = std::min<uint32_t>(first, change.reg_index);
            last = std::max<uint32_t>(last, change.reg_index + 1);
        }
    }
    mem_wb_reg_.rd = static_cast<uint8_t>(first);
    mem_wb_reg_.v_wcount = static_cast<uint8_t>(last - first);
    mem_wb_reg_.v_data.clear();
    for (uint32_t reg = first; reg < last; ++reg)
    {
        const std::vector<uint8_t> bytes = mem_vector_scratch_.File().ReadVectorRegister(reg);
        mem_wb_reg_.v_data.insert(mem_wb_reg_.v_data.end(), bytes.begin(), bytes.end());
    }
    mem_wb_reg_.memory_data = data.newVl; // vl after a fault-only-first load trimmed it
}

namespace
{
void WriteCsrRecorded(RegisterFile &registers, size_t csr, uint64_t value,
                      std::vector<RegisterChange> &changes)
{
    const uint64_t old_value = registers.ReadCsr(csr);
    registers.WriteCsr(csr, value);
    const uint64_t written = registers.ReadCsr(csr);
    if (written != old_value)
    {
        changes.push_back({static_cast<unsigned int>(csr), 1, old_value, written});
    }
}
} // namespace

void RV5StageVM_Base::pipeline_writeback_vector()
{
    auto &changes = current_delta_.register_changes;
    if (mem_wb_reg_.vreg_write)
    {
        const size_t vlen_bytes = registers_.GetVlenBytes();
        if (mem_wb_reg_.v_data.size() == mem_wb_reg_.v_wcount * vlen_bytes)
        {
            for (size_t i = 0; i < mem_wb_reg_.v_wcount; ++i)
            {
                const size_t reg = mem_wb_reg_.rd + i;
                const auto begin = mem_wb_reg_.v_data.begin() + i * vlen_bytes;
                std::vector<uint8_t> new_bytes(begin, begin + vlen_bytes);
                std::vector<uint8_t> old_bytes = registers_.ReadVectorRegister(reg);
                if (new_bytes != old_bytes)
                {
                    registers_.WriteVectorRegister(reg, new_bytes);
                    RegisterChange change{static_cast<unsigned int>(reg), 3, 0, 0};
                    change.old_bytes = std::move(old_bytes);
                    change.new_bytes = std::move(new_bytes);
                    changes.push_back(std::move(change));
                }
            }
        }
    }
    if (mem_wb_reg_.vcsr_write)
    {
        if ((mem_wb_reg_.instruction & 0x7F) == rvv::OPCODE_OP_V) // vset*: vl rides in alu_result
        {
            WriteCsrRecorded(registers_, RegisterFile::CSR_VL, mem_wb_reg_.alu_result, changes);
            WriteCsrRecorded(registers_, RegisterFile::CSR_VTYPE, mem_wb_reg_.v_vtype, changes);
        }
        else // vle*ff: the (possibly trimmed) vl rides in memory_data
        {
            WriteCsrRecorded(registers_, RegisterFile::CSR_VL, mem_wb_reg_.memory_data, changes);
        }
    }
    // Every vector instruction leaves vstart = 0.
    WriteCsrRecorded(registers_, RegisterFile::CSR_VSTART, 0, changes);
}

void RV5StageVM_Base::pipeline_writeback_csr()
{
    const size_t csr = (mem_wb_reg_.instruction >> 20) & 0xFFF;
    WriteCsrRecorded(registers_, csr, mem_wb_reg_.csr_wdata, current_delta_.register_changes);
}

uint64_t RV5StageVM_Base::execute_float()
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
        reg1_value = registers_.ReadGpr(id_ex_reg_.rs1);
    }

    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    alu::AluOp aluOperation = control_unit_.GetAluSignal(instruction, id_ex_reg_.alu_op > 0);
    std::tie(alu_result, fcsr_status) =
        alu::Alu::fpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);

    registers_.WriteCsr(0x003, fcsr_status);
    return alu_result;
}

uint64_t RV5StageVM_Base::execute_double()
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
        reg1_value = registers_.ReadGpr(id_ex_reg_.rs1);
    }

    if (id_ex_reg_.alu_src)
    {
        reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_reg_.imm));
    }

    alu::AluOp alu_operation = control_unit_.GetAluSignal(instruction, id_ex_reg_.alu_op > 0);
    std::tie(alu_result, fcsr_status) =
        alu::Alu::dfpexecute(alu_operation, reg1_value, reg2_value, reg3_value, rm);

    registers_.WriteCsr(0x003, fcsr_status);
    return alu_result;
}

bool RV5StageVM_Base::is_pipeline_drained() const
{
    // A stage holds a real instruction (possibly an actual `nop`) unless its PC is
    // INVALID_PC, which is what flushes, stalls and fetches past the end leave behind.
    if (if_id_reg_.pc != INVALID_PC || id_ex_reg_.pc != INVALID_PC ||
        ex_mem_reg_.pc != INVALID_PC || mem_wb_reg_.pc != INVALID_PC)
        return false;
    if (if_id_reg_.instruction != NOP)
        return false;
    if (id_ex_reg_.instruction != NOP)
        return false;
    if (ex_mem_reg_.instruction != NOP)
        return false;
    if (mem_wb_reg_.instruction != NOP)
        return false;
    // EX/MEM: Check for architectural side effects (Reg Write, Mem Read, Mem Write).
    // A NOP will have all these control signals disabled (false).
    if (ex_mem_reg_.reg_write || ex_mem_reg_.mem_read || ex_mem_reg_.mem_write)
        return false;

    // MEM/WB: Check for final architectural side effect (Reg Write).
    if (mem_wb_reg_.reg_write)
        return false;

    return true;
}

void RV5StageVM_Base::Undo()
{
    if (undo_stack_.empty())
    {
        return;
    }

    RV5StageStepDelta last = undo_stack_.top();
    undo_stack_.pop();

    for (auto it = last.register_changes.rbegin(); it != last.register_changes.rend(); ++it)
    {
        const auto &change = *it;
        switch (change.reg_type)
        {
        case 0:
            registers_.WriteGpr(change.reg_index, change.old_value);
            break;
        case 1:
            registers_.WriteCsr(change.reg_index, change.old_value);
            break;
        case 2:
            registers_.WriteFpr(change.reg_index, change.old_value);
            break;
        case 3:
            registers_.WriteVectorRegister(change.reg_index, change.old_bytes);
            break;
        default:
            break;
        }
    }

    for (auto it = last.memory_changes.rbegin(); it != last.memory_changes.rend(); ++it)
    {
        const auto &change = *it;
        for (size_t i = 0; i < change.old_bytes_vec.size(); ++i)
        {
            // Through the cache, like the store being undone: bypassing it would leave the
            // new value in a dirty L1 line, where every later read still sees it.
            memory_controller_.writeByte(change.address + i, change.old_bytes_vec[i]);
        }
    }

    program_counter_ = last.old_pc;
    cycle_s_ = last.old_cycle;
    instructions_retired_ = last.old_instructions_retired;
    stall_cycles_ = last.old_stall_cycles;
    branch_mispredictions_ = last.old_branch_mispredictions;

    if_id_reg_ = last.pipeline_register_change.old_if_id_reg;
    id_ex_reg_ = last.pipeline_register_change.old_id_ex_reg;
    ex_mem_reg_ = last.pipeline_register_change.old_ex_mem_reg;
    mem_wb_reg_ = last.pipeline_register_change.old_mem_wb_reg;

    redo_stack_.push(last);

    cpi_ = instructions_retired_
               ? static_cast<float>(cycle_s_) / static_cast<float>(instructions_retired_)
               : 0.0f;
    ipc_ =
        cycle_s_ ? static_cast<float>(instructions_retired_) / static_cast<float>(cycle_s_) : 0.0f;
    setProcessorState();
    SetActiveWireNames();
    emit updateCircuitStateSignal(active_wires_);
    emit processorClockedSignal(processor_state_);
}

void RV5StageVM_Base::Redo()
{
    if (redo_stack_.empty())
    {
        return;
    }

    RV5StageStepDelta next = redo_stack_.top();
    redo_stack_.pop();

    for (const auto &change : next.register_changes)
    {
        switch (change.reg_type)
        {
        case 0:
            registers_.WriteGpr(change.reg_index, change.new_value);
            break;
        case 1:
            registers_.WriteCsr(change.reg_index, change.new_value);
            break;
        case 2:
            registers_.WriteFpr(change.reg_index, change.new_value);
            break;
        case 3:
            registers_.WriteVectorRegister(change.reg_index, change.new_bytes);
            break;
        default:
            break;
        }
    }

    for (const auto &change : next.memory_changes)
    {
        for (size_t i = 0; i < change.new_bytes_vec.size(); ++i)
        {
            memory_controller_.writeByte(change.address + i, change.new_bytes_vec[i]);
        }
    }

    program_counter_ = next.new_pc;
    cycle_s_ = next.new_cycle;
    instructions_retired_ = next.new_instructions_retired;
    stall_cycles_ = next.new_stall_cycles;
    branch_mispredictions_ = next.new_branch_mispredictions;

    if_id_reg_ = next.pipeline_register_change.new_if_id_reg;
    id_ex_reg_ = next.pipeline_register_change.new_id_ex_reg;
    ex_mem_reg_ = next.pipeline_register_change.new_ex_mem_reg;
    mem_wb_reg_ = next.pipeline_register_change.new_mem_wb_reg;

    undo_stack_.push(next);

    cpi_ = instructions_retired_
               ? static_cast<float>(cycle_s_) / static_cast<float>(instructions_retired_)
               : 0.0f;
    ipc_ =
        cycle_s_ ? static_cast<float>(instructions_retired_) / static_cast<float>(cycle_s_) : 0.0f;
    setProcessorState();
    SetActiveWireNames();
    emit updateCircuitStateSignal(active_wires_);
    emit processorClockedSignal(processor_state_);
}

void RV5StageVM_Base::print_pipeline_registers_debug()
{
    std::cout << "\xEF\xBB\xBF"; // UTF-8 BOM (optional but helps old terminals)

    std::cout << "\n┌──────────────────────────────────────────────────────────┐\n";
    std::cout << "│ Pipeline Debug (Cycle " << cycle_s_ << ")"
              << std::string(34 - std::to_string(cycle_s_).size(), ' ') << "│\n";
    std::cout << "├──────────────────────────────────────────────────────────┤\n";

    std::cout << "│ PC: 0x" << std::hex << std::setw(8) << std::setfill('0') << program_counter_
              << std::dec << "                                           │\n";

    auto inst_to_mnemonic = [](uint32_t inst) -> const char *
    {
        if (inst == NOP)
            return "NOP";
        uint8_t opc = inst & 0x7F;
        switch (opc)
        {
        case 0b1101111:
            return "JAL";
        case 0b1100111:
            return "JALR";
        case 0b1100011:
            return "BR";
        case 0b0000011:
            return "LOAD";
        case 0b0100011:
            return "STORE";
        case 0b0010011:
            return "ALU_IMM";
        case 0b0110011:
            return "ALU_REG";
        case 0b1110011:
            return "SYSTEM";
        default:
            return "OTHER";
        }
    };

    auto print_row = [&](const char *stage, uint32_t inst)
    {
        std::stringstream ss;
        ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << inst << std::dec;

        std::cout << "│ " << std::left << std::setw(6) << stage << "│ " << std::setw(12) << ss.str()
                  << "│ " << std::setw(10) << inst_to_mnemonic(inst) << "│\n";
    };

    std::cout << "├──────────────────────────────────────────────────────────┤\n";
    std::cout << "│ Stage  │ Instruction  │ Mnemonic                         │\n";
    std::cout << "├──────────────────────────────────────────────────────────┤\n";

    print_row("IF/ID", if_id_reg_.instruction);
    print_row("ID/EX", id_ex_reg_.instruction);
    print_row("EX/MEM", ex_mem_reg_.instruction);
    print_row("MEM/WB", mem_wb_reg_.instruction);

    // You had not printed MEM/WB, so I didn’t add it
    // Same logic preserved exactly

    std::cout << "└──────────────────────────────────────────────────────────┘\n";
}
}//namespace Kites
