// The pipeline panel's space-time diagram model, fed cycle by cycle from a real H_F run.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/processor_constants.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "ui/pipeline_panel/pipeline_trace.h"
#include "utils/utils.h"

namespace
{
using Kites::PipelineRow;
using Kites::PipelineSnapshot;
using Kites::PipelineTrace;
using Kites::PipeStage;

/// Runs @p source on H_F to the end and returns one snapshot per cycle, labelling each PC
/// "i<n>" (its instruction number).
std::vector<PipelineSnapshot> Snapshots(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    Kites::RV5StageProcessorHF vm;
    vm.LoadProgram(Kites::assemble(stream));
    Kites::ProcessorBase &base = vm;
    std::vector<PipelineSnapshot> snapshots;
    for (int guard = 0; guard < 200; ++guard)
    {
        vm.Step();
        base.setProcessorState();
        PipelineSnapshot snapshot;
        snapshot.cycle = vm.cycle_s_;
        bool empty = true;
        for (size_t i = 0; i < 4; ++i)
        {
            snapshot.pcs[i] = vm.processor_state_.programCounters[i + 1];
            if (snapshot.pcs[i] != INVALID_PC)
            {
                snapshot.texts[i] = "i" + std::to_string(snapshot.pcs[i] / 4);
                empty = false;
            }
        }
        snapshots.push_back(snapshot);
        if (empty && vm.program_counter_ >= 4 * 64)
            break;
        if (empty && guard > 4)
            break;
    }
    return snapshots;
}

PipelineTrace Trace(const std::vector<PipelineSnapshot> &snapshots)
{
    PipelineTrace trace;
    for (const PipelineSnapshot &snapshot : snapshots)
    {
        trace.Push(snapshot);
    }
    return trace;
}

/// The row's stages as text, e.g. "IF ID EX MEM WB", starting at its first cycle.
std::string Stages(const PipelineRow &row)
{
    std::string text;
    for (PipeStage stage : row.stages)
    {
        if (!text.empty())
            text += ' ';
        text += stage == PipeStage::None ? "." : Kites::PipeStageName(stage);
    }
    return text;
}

std::vector<PipelineRow> VisibleRows(const PipelineTrace &trace)
{
    return {trace.Rows().begin(), trace.Rows().begin() + trace.VisibleRowCount()};
}
} // namespace

TEST(PipelineTraceTest, IndependentInstructionsFormAStaircase)
{
    const auto rows = VisibleRows(Trace(Snapshots(".text\n"
                                                  "    addi x1, x0, 1\n"
                                                  "    addi x2, x0, 2\n"
                                                  "    addi x3, x0, 3\n")));
    ASSERT_EQ(rows.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(rows[i].text, "i" + std::to_string(i));
        EXPECT_FALSE(rows[i].IsNop());
        EXPECT_EQ(rows[i].firstCycle, i + 1);
        EXPECT_EQ(Stages(rows[i]), "IF ID EX MEM WB");
    }
}

TEST(PipelineTraceTest, LoadUseStallRepeatsIdAndShowsANop)
{
    const PipelineTrace trace = Trace(Snapshots(".data\nv: .word 5\n.text\n"
                                                "    lui x5, 0x10000\n"
                                                "    lw x6, 0(x5)\n"
                                                "    add x7, x6, x6\n"
                                                "    addi x8, x0, 1\n"));
    const auto rows = VisibleRows(trace);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[1].text, "i1"); // lw
    EXPECT_EQ(Stages(rows[1]), "IF ID EX MEM WB");
    EXPECT_TRUE(rows[2].bubble); // the bubble, above the stalled add
    EXPECT_EQ(rows[2].firstCycle, 5u);
    EXPECT_EQ(Stages(rows[2]), "EX MEM WB");
    EXPECT_EQ(rows[3].text, "i2"); // add
    EXPECT_EQ(Stages(rows[3]), "IF ID ID EX MEM WB");
    EXPECT_EQ(rows[4].text, "i3");
    EXPECT_EQ(rows[4].firstCycle, 5u);
    EXPECT_EQ(trace.StallCount(), 1u);
}

TEST(PipelineTraceTest, TakenBranchFlushesTheWrongPathIntoNops)
{
    const auto rows = VisibleRows(Trace(Snapshots(".text\n"
                                                  "    beq x0, x0, target\n"
                                                  "    addi x1, x0, 1\n"
                                                  "    addi x2, x0, 2\n"
                                                  "    addi x3, x0, 3\n"
                                                  "target:\n"
                                                  "    addi x4, x0, 4\n")));
    // beq, its wrong-path instructions (flushed), the empty fetch slot, then the target.
    ASSERT_GE(rows.size(), 4u);
    EXPECT_EQ(rows[0].text, "i0");
    EXPECT_EQ(Stages(rows[0]), "IF ID EX MEM WB");
    size_t flushed = 0;
    for (const PipelineRow &row : rows)
    {
        flushed += row.flushed;
    }
    EXPECT_EQ(flushed, 2u) << "the instructions in IF/ID and ID/EX when MEM redirects";
    EXPECT_EQ(rows.back().text, "i4");
    EXPECT_FALSE(rows.back().IsNop());
    EXPECT_EQ(Stages(rows.back()), "IF ID EX MEM WB");
}

TEST(PipelineTraceTest, UndoRewindsToTheSameDiagram)
{
    const auto snapshots = Snapshots(".data\nv: .word 5\n.text\n"
                                     "    lui x5, 0x10000\n"
                                     "    lw x6, 0(x5)\n"
                                     "    add x7, x6, x6\n"
                                     "    beq x0, x0, end\n"
                                     "    addi x8, x0, 1\n"
                                     "end:\n"
                                     "    addi x9, x0, 1\n");
    ASSERT_GT(snapshots.size(), 6u);
    const std::vector<PipelineSnapshot> firstSix(snapshots.begin(), snapshots.begin() + 6);
    const PipelineTrace expected = Trace(firstSix);

    PipelineTrace trace = Trace(snapshots);
    trace.Push(snapshots[5]); // undo back to cycle 6: the VM announces cycle 6 again
    ASSERT_EQ(trace.LastCycle(), expected.LastCycle());
    ASSERT_EQ(trace.Rows().size(), expected.Rows().size());
    for (size_t i = 0; i < trace.Rows().size(); ++i)
    {
        EXPECT_EQ(trace.Rows()[i].text, expected.Rows()[i].text);
        EXPECT_EQ(Stages(trace.Rows()[i]), Stages(expected.Rows()[i]));
    }

    PipelineSnapshot reset;
    reset.cycle = 0;
    trace.Push(reset);
    EXPECT_EQ(trace.VisibleRowCount(), 0u);
}
