/**
 * @file pipeline_trace.cpp
 * @brief Pipeline space-time diagram model. See pipeline_trace.h.
 */
#include "ui/pipeline_panel/pipeline_trace.h"

#include "processor/processor_constants.h"

namespace Kites
{
const char *PipeStageName(PipeStage stage)
{
    switch (stage)
    {
    case PipeStage::IF:
        return "IF";
    case PipeStage::ID:
        return "ID";
    case PipeStage::EX:
        return "EX";
    case PipeStage::MEM:
        return "MEM";
    case PipeStage::WB:
        return "WB";
    default:
        return "";
    }
}

PipeStage PipelineRow::StageAt(uint64_t cycle) const
{
    if (cycle < firstCycle || cycle - firstCycle >= stages.size())
    {
        return PipeStage::None;
    }
    return stages[cycle - firstCycle];
}

void PipelineTrace::Clear()
{
    m_history.clear();
    m_texts.clear();
    m_rows.clear();
    m_slots = {-1, -1, -1, -1};
    m_stalls = 0;
}

void PipelineTrace::Push(const PipelineSnapshot &snapshot)
{
    for (size_t i = 0; i < 4; ++i)
    {
        if (snapshot.pcs[i] != INVALID_PC && !snapshot.texts[i].empty())
        {
            m_texts[snapshot.pcs[i]] = snapshot.texts[i];
        }
    }
    if (snapshot.cycle == 0)
    {
        Clear();
        return;
    }
    if (!m_history.empty() && snapshot.cycle <= m_history.back().cycle)
    {
        // Undo: forget this cycle and everything after it, then replay what is left.
        while (!m_history.empty() && m_history.back().cycle >= snapshot.cycle)
        {
            m_history.pop_back();
        }
        Rebuild();
    }
    const Cycle cycle{snapshot.cycle, snapshot.pcs};
    m_history.push_back(cycle);
    Apply(cycle);
}

void PipelineTrace::Rebuild()
{
    m_rows.clear();
    m_slots = {-1, -1, -1, -1};
    m_stalls = 0;
    std::vector<Cycle> history;
    history.swap(m_history);
    for (const Cycle &cycle : history)
    {
        m_history.push_back(cycle);
        Apply(cycle);
    }
}

std::string PipelineTrace::TextOf(uint64_t pc) const
{
    const auto it = m_texts.find(pc);
    return it == m_texts.end() ? std::string() : it->second;
}

int PipelineTrace::NewRow(PipelineRow row)
{
    m_rows.push_back(std::move(row));
    return static_cast<int>(m_rows.size()) - 1;
}

int PipelineTrace::InsertRowBefore(int index, PipelineRow row)
{
    m_rows.insert(m_rows.begin() + index, std::move(row));
    return index;
}

void PipelineTrace::Mark(int row, uint64_t cycle, PipeStage stage)
{
    PipelineRow &r = m_rows[row];
    if (r.stages.empty())
    {
        r.firstCycle = cycle;
    }
    if (cycle < r.firstCycle)
    {
        return;
    }
    const size_t at = cycle - r.firstCycle;
    if (r.stages.size() <= at)
    {
        r.stages.resize(at + 1, PipeStage::None);
    }
    r.stages[at] = stage;
}

void PipelineTrace::Apply(const Cycle &cycle)
{
    const auto valid = [&](size_t i) { return cycle.pcs[i] != INVALID_PC; };
    const uint64_t c = cycle.cycle;
    const bool hasPrevious = m_history.size() >= 2;
    const Cycle *previous = hasPrevious ? &m_history[m_history.size() - 2] : nullptr;

    // A load-use stall holds the instruction in IF/ID and puts a bubble in ID/EX.
    const bool stalled = previous != nullptr && valid(0) && cycle.pcs[0] == previous->pcs[0] &&
                         !valid(1);

    const std::array<int, 4> old = m_slots;
    std::array<int, 4> next{-1, -1, -1, -1};

    // WB: what was in MEM/WB.
    if (old[3] >= 0)
    {
        Mark(old[3], c, PipeStage::WB);
    }
    // MEM: EX/MEM always moves on.
    next[3] = old[2];
    if (next[3] >= 0)
    {
        Mark(next[3], c, PipeStage::MEM);
    }
    // EX: ID/EX moves on, unless a branch resolved in MEM squashed it first.
    next[2] = old[1];
    if (next[2] >= 0)
    {
        if (!m_rows[next[2]].IsNop() && !valid(2))
        {
            m_rows[next[2]].flushed = true;
        }
        Mark(next[2], c, PipeStage::EX);
    }

    if (stalled)
    {
        ++m_stalls;
        // The consumer repeats ID; the bubble enters EX next cycle. It goes above the
        // consumer so the diagram keeps its staircase.
        const int consumer = old[0];
        Mark(consumer, c, PipeStage::ID);
        PipelineRow bubble;
        bubble.bubble = true;
        InsertRowBefore(consumer, std::move(bubble));
        for (size_t i = 2; i < 4; ++i) // rows the insertion shifted down
        {
            if (next[i] >= consumer)
            {
                ++next[i];
            }
        }
        next[1] = consumer;     // the bubble
        next[0] = consumer + 1; // the consumer, still in IF/ID
        m_slots = next;
        return;
    }

    // ID: IF/ID moves on, unless a jump or branch squashed it.
    next[1] = old[0];
    if (next[1] >= 0)
    {
        if (!m_rows[next[1]].IsNop() && !valid(1))
        {
            m_rows[next[1]].flushed = true;
        }
        Mark(next[1], c, PipeStage::ID);
    }

    // IF: a new instruction, or an empty fetch slot.
    if (valid(0))
    {
        PipelineRow row;
        row.text = TextOf(cycle.pcs[0]);
        next[0] = NewRow(std::move(row));
        Mark(next[0], c, PipeStage::IF);
    }
    else if (hasPrevious)
    {
        PipelineRow bubble;
        bubble.bubble = true;
        next[0] = NewRow(std::move(bubble));
    }
    m_slots = next;
}

size_t PipelineTrace::VisibleRowCount() const
{
    size_t count = m_rows.size();
    while (count > 0 && m_rows[count - 1].bubble)
    {
        --count;
    }
    return count;
}
} // namespace Kites
