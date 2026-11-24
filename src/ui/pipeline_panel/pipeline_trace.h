/**
 * @file pipeline_trace.h
 * @brief Turns the per-cycle contents of the pipeline registers into the rows of a pipeline
 * space-time diagram: one row per instruction (or bubble), one column per cycle, each cell the
 * stage (IF, ID, EX, MEM, WB) the row is in during that cycle.
 *
 * Plain C++ (no widgets), so it can be tested without a GUI.
 *
 * Reading a cycle c (the registers at the end of cycle c):
 *   IF  = what was fetched into IF/ID in c        ID  = what moved into ID/EX in c
 *   EX  = what moved into EX/MEM in c             MEM = what moved into MEM/WB in c
 *   WB  = what was in MEM/WB at the end of c-1
 * A load-use stall keeps the consumer in IF/ID (it repeats ID) and puts a bubble in ID/EX; a
 * taken branch or jump empties IF/ID and/or ID/EX, which turns what was there into bubbles. All
 * bubbles are shown as "nop" rows.
 */
#pragma once

#include "processor/pipeline_snapshot.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kites
{
enum class PipeStage : uint8_t
{
    None = 0,
    IF,
    ID,
    EX,
    MEM,
    WB,
};

const char *PipeStageName(PipeStage stage);

struct PipelineRow
{
    std::string text;        ///< the instruction's disassembly ("" for a bubble)
    bool bubble = false;     ///< a nop the pipeline made: stall bubble or empty fetch slot
    bool flushed = false;    ///< a real instruction squashed by a branch/jump (now a nop)
    uint64_t firstCycle = 0; ///< cycle of stages[0]
    std::vector<PipeStage> stages;

    bool IsNop() const { return bubble || flushed; }
    PipeStage StageAt(uint64_t cycle) const;
    uint64_t LastCycle() const { return stages.empty() ? firstCycle : firstCycle + stages.size() - 1; }
};

class PipelineTrace
{
  public:
    void Clear();

    /// Adds one cycle. A snapshot that is not newer than the last one (after an undo) first
    /// rewinds the trace to just before its cycle; cycle 0 clears it.
    void Push(const PipelineSnapshot &snapshot);

    /// Rows in diagram order. Bubbles after the last real instruction (an empty fetch slot while
    /// the pipeline drains) are not counted.
    const std::vector<PipelineRow> &Rows() const { return m_rows; }
    size_t VisibleRowCount() const;

    uint64_t FirstCycle() const { return m_history.empty() ? 0 : m_history.front().cycle; }
    uint64_t LastCycle() const { return m_history.empty() ? 0 : m_history.back().cycle; }
    size_t StallCount() const { return m_stalls; }

  private:
    struct Cycle
    {
        uint64_t cycle;
        std::array<uint64_t, 4> pcs;
    };

    void Apply(const Cycle &cycle);
    void Rebuild();
    int NewRow(PipelineRow row);
    int InsertRowBefore(int index, PipelineRow row);
    void Mark(int row, uint64_t cycle, PipeStage stage);
    std::string TextOf(uint64_t pc) const;

    std::vector<Cycle> m_history;
    std::unordered_map<uint64_t, std::string> m_texts; ///< pc -> disassembly
    std::vector<PipelineRow> m_rows;
    std::array<int, 4> m_slots{-1, -1, -1, -1}; ///< row held by IF/ID, ID/EX, EX/MEM, MEM/WB
    size_t m_stalls = 0;
};
} // namespace Kites
