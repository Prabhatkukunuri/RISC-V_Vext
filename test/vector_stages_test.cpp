// The EX / MEM / WB split of vector instructions (vector_stages.h), as the single-cycle VM uses it:
// EX changes no state, MEM touches only memory (and only for loads/stores), WB commits the
// registers. The composed path (ExecuteVectorInstruction) is covered by every other vector test.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/registers.h"
#include "processor/rvss/rvss_processor.h"
#include "processor/vector/vector_stages.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;
using Kites::RVSSProcessor;
namespace rvv = Kites::rvv;

constexpr uint64_t kBase = 0x10000000;

uint32_t assembleOne(const std::string &line)
{
    std::istringstream stream(line + "\n");
    const Kites::AssembledProgram program = Kites::assemble(stream);
    if (program.text_buffer.size() != 1)
    {
        throw std::runtime_error("expected one instruction from: " + line);
    }
    return program.text_buffer[0];
}

/// Everything a vector instruction may change.
struct Snapshot
{
    std::vector<std::vector<uint8_t>> vregs;
    uint64_t vl = 0, vtype = 0, vstart = 0;
    std::vector<uint8_t> memory;

    bool operator==(const Snapshot &) const = default;
};

Snapshot Take(RVSSProcessor &vm)
{
    Snapshot s;
    for (size_t r = 0; r < RegisterFile::NUM_VR; ++r)
    {
        s.vregs.push_back(vm.registers_.ReadVectorRegister(r));
    }
    s.vl = vm.registers_.ReadCsr(RegisterFile::CSR_VL);
    s.vtype = vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE);
    s.vstart = vm.registers_.ReadCsr(RegisterFile::CSR_VSTART);
    for (uint64_t i = 0; i < 32; ++i)
    {
        s.memory.push_back(vm.memory_controller_.peekByte(kBase + i));
    }
    return s;
}

/// A VM with vl = 4, e32/m1, v1 = {1,2,3,4}, v2 = {10,20,30,40}, mem[kBase] = {5,6,7,8}.
class VectorStagesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Kites::setupVmStateDirectory();
        std::vector<Kites::RegisterChange> changes;
        std::vector<Kites::MemoryChange> memoryChanges;
        rvv::ExecuteVectorInstruction(assembleOne("vsetivli x0, 4, e32, m1, ta, ma"), 0, 0,
                                      vm.registers_, vm.memory_controller_, changes,
                                      memoryChanges);
        for (uint64_t i = 0; i < 4; ++i)
        {
            vm.registers_.WriteVectorElement(1, i, 32, i + 1);
            vm.registers_.WriteVectorElement(2, i, 32, 10 * (i + 1));
            vm.memory_controller_.writeWord(kBase + 4 * i, static_cast<uint32_t>(i + 5));
        }
    }

    std::vector<uint64_t> Elements(size_t reg)
    {
        std::vector<uint64_t> out;
        for (uint64_t i = 0; i < 4; ++i)
        {
            out.push_back(vm.registers_.ReadVectorElement(reg, i, 32));
        }
        return out;
    }

    RVSSProcessor vm;
    rvv::VectorScratch scratch;
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
};
} // namespace

TEST_F(VectorStagesTest, ArithmeticWritesVdOnlyInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vadd.vv v3, v1, v2"), 0, 0,
                                                       vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";
    ASSERT_EQ(stage.pendingWrites.size(), 1u);
    EXPECT_EQ(stage.pendingWrites[0].reg_index, 3u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(Take(vm), before) << "MEM changed state for an arithmetic instruction";
    EXPECT_TRUE(memoryChanges.empty());

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(Elements(3), (std::vector<uint64_t>{11, 22, 33, 44}));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reg_type, 3u);
    EXPECT_EQ(changes[0].old_bytes, before.vregs[3]);
}

TEST_F(VectorStagesTest, VsetWritesVlAndVtypeOnlyInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(
        assembleOne("vsetvli t0, a0, e16, m2, ta, ma"), 3, 0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed vl/vtype";
    EXPECT_TRUE(stage.result.writesRd);
    EXPECT_EQ(stage.result.rdValue, 3u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    const rvv::VectorExecResult rd = rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(rd.rdValue, 3u);
    EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VL), 3u);
    EXPECT_NE(vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE), before.vtype);
}

TEST_F(VectorStagesTest, StoreWritesMemoryInMemoryStage)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vse32.v v2, (a0)"), kBase,
                                                       0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";
    ASSERT_EQ(stage.memPlan.accesses.size(), 4u);
    EXPECT_EQ(stage.memPlan.accesses[3].address, kBase + 12);
    EXPECT_EQ(stage.memPlan.accesses[3].storeValue, 40u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(vm.memory_controller_.peekByte(kBase), 10u);
    EXPECT_EQ(vm.memory_controller_.peekByte(kBase + 12), 40u);
    EXPECT_EQ(memoryChanges.size(), 4u);
    Snapshot afterMem = Take(vm);
    afterMem.memory = before.memory;
    EXPECT_EQ(afterMem, before) << "MEM changed a register";

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_TRUE(changes.empty());
}

TEST_F(VectorStagesTest, LoadReadsInMemoryStageAndWritesVdInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vle32.v v4, (a0)"), kBase,
                                                       0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(stage.memData.loadedValues, (std::vector<uint64_t>{5, 6, 7, 8}));
    EXPECT_EQ(Take(vm), before) << "MEM wrote vd";

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(Elements(4), (std::vector<uint64_t>{5, 6, 7, 8}));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reg_index, 4u);
}

TEST_F(VectorStagesTest, IllegalInstructionTrapsInExecuteStage)
{
    // At e32/m1, vle64.v has EMUL = 2, so v1 is a misaligned group: illegal, caught in EX.
    EXPECT_THROW(rvv::VectorExecuteStage(assembleOne("vle64.v v1, (a0)"), kBase, 0,
                                         vm.registers_, scratch),
                 std::runtime_error);
}

TEST(VectorStagesRvssTest, VectorAddExampleRunsAndUndoesThroughThePhases)
{
    Kites::setupVmStateDirectory();
    std::ifstream file("../examples/vector_add.s");
    ASSERT_TRUE(file.good());
    std::stringstream source;
    source << file.rdbuf();
    std::istringstream stream(source.str());

    RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.DebugRun();
    const uint64_t c = kBase + 80; // after a[10] and b[10]
    for (uint64_t i = 0; i < 10; ++i)
    {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; ++b)
        {
            word |= static_cast<uint32_t>(vm.memory_controller_.peekByte(c + 4 * i + b)) << (8 * b);
        }
        EXPECT_EQ(word, 11 * (i + 1)) << "c[" << i << "]";
    }

    for (int guard = 0; guard < 1000 && vm.program_counter_ != 0; ++guard)
    {
        vm.Undo();
    }
    ASSERT_EQ(vm.program_counter_, 0u);
    for (uint64_t i = 0; i < 40; ++i)
    {
        EXPECT_EQ(vm.memory_controller_.peekByte(c + i), 0u) << "byte " << i << " of c";
    }
    EXPECT_EQ(vm.registers_.ReadVectorRegister(3),
              std::vector<uint8_t>(vm.registers_.GetVlenBytes(), 0));
}
