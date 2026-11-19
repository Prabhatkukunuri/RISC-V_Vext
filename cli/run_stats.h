/**
 * @file run_stats.h
 * @brief Dynamic instruction statistics for a batch run of kites-cli (--stats).
 *
 * The collector listens to ProcessorManager::processorClockedSignal. On the single-cycle VM
 * (rvss) every clock retires exactly one instruction, whose address is
 * ProcessorState::lastExecutedPC, so the instruction word is decoded from the program and
 * classified. The vector CSRs are read right after the instruction ran, which gives the vl and
 * SEW it used (vset* is counted as configuration, so its new vl does not matter).
 *
 * On the pipelined VMs the clock does not line up with one retired instruction, so only the
 * cycle and retired-instruction totals are reported there (instruction_mix is null).
 */
#pragma once

#include "common/assembled_program.h"
#include "processor/processor_state.h"
#include "processor/registers.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace Kites::cli
{

struct RunStats
{
    // Instruction mix (rvss only).
    uint64_t scalarAlu = 0;   ///< OP, OP-IMM (+32-bit forms), LUI, AUIPC; includes M (mul/div)
    uint64_t scalarLoad = 0;  ///< integer and FP loads
    uint64_t scalarStore = 0; ///< integer and FP stores
    uint64_t branch = 0;
    uint64_t branchTaken = 0;
    uint64_t jump = 0;        ///< jal, jalr
    uint64_t scalarOther = 0; ///< system (ecall, csr*), FP compute, fences
    uint64_t vectorConfig = 0;
    uint64_t vectorLoad = 0;
    uint64_t vectorStore = 0;
    uint64_t vectorCompute = 0; ///< every other OP-V instruction

    // Memory traffic (rvss only).
    uint64_t scalarLoadBytes = 0;
    uint64_t scalarStoreBytes = 0;
    uint64_t vectorLoadBytes = 0;
    uint64_t vectorStoreBytes = 0;

    // Vector work (rvss only).
    uint64_t vectorElements = 0; ///< elements processed by vector loads, stores and compute
    std::map<std::string, uint64_t> vectorByMnemonic;

    uint64_t retiredSeen = 0; ///< instructions classified (should equal instructions_retired)
};

class RunStatsCollector
{
  public:
    RunStatsCollector(const AssembledProgram &program, const RegisterFile &registers,
                      uint64_t vlenBits);

    /// Classifies the instruction that the clock just executed (single-cycle VM only).
    void onClock(const ProcessorState &state);

    const RunStats &stats() const { return m_stats; }

    /**
     * @brief Writes the statistics as JSON and returns a short human-readable summary.
     * @param mixAvailable false on the pipelined VMs (instruction_mix etc. are written as null).
     */
    std::string write(const std::filesystem::path &path, const std::string &vmName,
                      uint64_t cycles, uint64_t instructionsRetired, bool mixAvailable) const;

  private:
    void classifyVectorMemory(uint32_t instruction, bool isStore);
    void classifyVectorArith(uint32_t instruction);

    const AssembledProgram &m_program;
    const RegisterFile &m_registers;
    uint64_t m_vlenBits;
    RunStats m_stats;
};

} // namespace Kites::cli
