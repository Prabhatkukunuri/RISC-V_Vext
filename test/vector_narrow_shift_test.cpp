// Tests for the RVV narrowing right shifts (11.7): vnsrl and vnsra in .wv/.wx/.wi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: vs2 is 2*SEW wide, the result is truncated to SEW, the shift amount uses
// log2(2*SEW) bits, .wi takes an unsigned 5-bit immediate, and vd may overlap only the start
// of the wide vs2 group.

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "common/instructions.h"
#include "processor/registers.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "processor/vector/vector_config.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;
using Elements = std::vector<uint64_t>;
using Setup = std::function<void(RegisterFile &)>;

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

template <typename Vm>
std::unique_ptr<Vm> runOn(const std::string &source, const Setup &setup = {})
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(assembleSource(source));
    if (setup)
    {
        setup(vm->registers_);
    }
    vm->DebugRun();
    return vm;
}

void writeElements(RegisterFile &rf, size_t reg, size_t sew, const Elements &values)
{
    for (size_t i = 0; i < values.size(); ++i)
    {
        rf.WriteVectorElement(reg, i, sew, values[i]);
    }
}

Elements readElements(const RegisterFile &rf, size_t reg, size_t sew, size_t count)
{
    Elements values;
    for (size_t i = 0; i < count; ++i)
    {
        values.push_back(rf.ReadVectorElement(reg, i, sew));
    }
    return values;
}

void expectIllegal(const std::string &source, const Setup &setup = {})
{
    // Must assemble: the error has to come from the run-time check, not the assembler.
    ASSERT_NO_THROW(assembleSource(source)) << source;
    EXPECT_THROW(runOn<Kites::RVSSProcessor>(source, setup), std::runtime_error) << source;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorNarrowShiftAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vnsrl.wv v1, v2, v3", 0xb22180d7},
        {"vnsrl.wx v4, v6, a0, v0.t", 0xb0654257},
        {"vnsrl.wi v8, v10, 31", 0xb2afb457},
        {"vnsra.wv v12, v14, v16, v0.t", 0xb4e80657},
        {"vnsra.wx v2, v2, t0", 0xb622c157},
        {"vnsra.wi v3, v4, 0", 0xb64031d7},
        {"vnsra.wi v5, v6, 17, v0.t", 0xb468b2d7},
    };
    std::string source;
    for (const auto &[text, word] : cases)
    {
        source += text + "\n";
    }
    const Kites::AssembledProgram program = assembleSource(source);
    ASSERT_EQ(program.text_buffer.size(), cases.size());
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const std::string &text = cases[i].first;
        EXPECT_EQ(program.text_buffer[i], cases[i].second) << text;
        const Kites::rvv::VectorArithInfo *info =
            Kites::rvv::DecodeVectorArith(program.text_buffer[i]);
        ASSERT_NE(info, nullptr) << text;
        EXPECT_EQ(text.substr(0, text.find(' ')), info->mnemonic);
    }
}

TEST(VectorNarrowShiftAssemblerTest, DisassemblyAndImmediateRange)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0xb2afb457), "vnsrl.wi v8, v10, 31");
    EXPECT_EQ(disassemble(0xb468b2d7), "vnsra.wi v5, v6, 17, v0.t");
    EXPECT_EQ(disassemble(0xb622c157), "vnsra.wx v2, v2, x5");

    for (const std::string &line : {"vnsrl.wi v1, v2, 32\n", "vnsra.wi v1, v2, -1\n",
                                    "vnsrl.wi v0, v2, 3, v0.t\n", "vnsra.wv v1, v2, a0\n",
                                    "vnsrl.wx v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // vd may be vs2 itself: the lowest-numbered part of the wide group.
    EXPECT_NO_THROW(assembleSource("vnsrl.wi v2, v2, 0\nvnsra.wi v1, v2, 31\n"));
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorNarrowShiftTest, ShiftsWideElementAndTruncates)
{
    // e16 results from e32 sources in the v2-v3 group.
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 4, e16, m1, ta, ma\n"
        "vnsrl.wi v4, v2, 16\n"
        "vnsrl.wi v5, v2, 20\n"
        "vnsra.wi v6, v2, 20\n",
        [](RegisterFile &rf)
        { writeElements(rf, 2, 32, {0x80000000, 0x12345678, 0xFFFF0000, 0x0000FFFF}); });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x8000, 0x1234, 0xFFFF, 0x0000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 16, 4), (Elements{0x0800, 0x0123, 0x0FFF, 0x0000}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{0xF800, 0x0123, 0xFFFF, 0x0000}));
}

TEST(VectorNarrowShiftTest, AmountIsMaskedToLog2OfTwiceSew)
{
    // e8 results from e16 sources: 4 bits of the amount count, not 3. A shift by 8 (or 24)
    // must really move the high byte down; masking to 3 bits would make it a shift by 0.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 24\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vnsrl.wx v2, v4, x10\n"
                                          "vnsrl.wi v3, v4, 8\n"
                                          "vnsra.wv v7, v4, v6\n"
                                          "vnsrl.wv v8, v4, v6\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 16,
                                                            {0x1234, 0x8000, 0xABCD, 0x00FF});
                                              writeElements(rf, 6, 8, {8, 15, 16, 0});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 4), (Elements{0x12, 0x80, 0xAB, 0x00}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 4), (Elements{0x12, 0x80, 0xAB, 0x00}));
    // Per-element amounts 8, 15, 16 (& 15 = 0) and 0.
    EXPECT_EQ(readElements(vm->registers_, 7, 8, 4), (Elements{0x12, 0xFF, 0xCD, 0xFF}));
    EXPECT_EQ(readElements(vm->registers_, 8, 8, 4), (Elements{0x12, 0x01, 0xCD, 0xFF}));
}

TEST(VectorNarrowShiftTest, InPlaceOverTheWideSourceKeepsTheTail)
{
    // vd == vs2: each narrow result lands below the wide elements still to be read.
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 4, e16, m1, ta, ma\n"
        "vnsrl.wi v2, v2, 16\n",
        [](RegisterFile &rf)
        { writeElements(rf, 2, 32, {0x00010002, 0x00030004, 0x00050006, 0x00070008}); });
    // Elements 4..7 are the tail: the untouched upper half of the old v2.
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 8), (Elements{1, 3, 5, 7, 6, 5, 8, 7}));
}

TEST(VectorNarrowShiftTest, MaskedOffElementsAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e16, m1, ta, ma\n"
                                          "vnsrl.wi v4, v2, 4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 32, {0x10, 0x20, 0x30, 0x40});
                                              writeElements(rf, 4, 16, Elements(4, 9));
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x1, 9, 0x3, 9}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), Elements(4, 9));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x1, 9, 0x3, 9}));
}

TEST(VectorNarrowShiftTest, IllegalConfigurationsTrap)
{
    // 2*SEW = 128 exceeds ELEN.
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvnsrl.wi v1, v2, 0\n");
    // LMUL = 8 would make vs2 a 16-register group.
    expectIllegal("vsetvli x0, x0, e8, m8, ta, ma\nvnsrl.wi v0, v16, 0\n");
    // vs2 is a 2-register group at LMUL = 1, so it must be even.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvnsrl.wi v1, v3, 0\n");
    // vd may not overlap the upper half of vs2.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvnsra.wi v3, v2, 0\n");
    // vd at LMUL = 2 must be even.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvnsrl.wi v1, v4, 0\n");
}

TEST(VectorNarrowShiftTest, FractionalLmulAllowsVdNextToVs2)
{
    // At LMUL = 1/2 vs2 has EMUL = 1, so v3 is outside it.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e16, mf2, ta, ma\n"
                                          "vnsrl.wi v3, v2, 8\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 32, {0x123400, 0xABCD00}); });
    EXPECT_EQ(readElements(vm->registers_, 3, 16, 2), (Elements{0x1234, 0xABCD}));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kNarrowProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                             "li x10, 4\n"
                             "nop\nnop\nnop\n"
                             "vadd.vi v2, v0, -8\n"        // e32 0xFFFFFFF8
                             "vsetivli x0, 4, e16, m1, ta, ma\n"
                             "vnsra.wi v4, v2, 2\n"        // -2 = 0xFFFE
                             "vnsrl.wx v5, v2, x10\n"      // 0x0FFFFFFF -> 0xFFFF
                             "vnsrl.wi v6, v2, 28\n"       // 0xF
                             "vnsra.wv v7, v2, v6\n"       // -8 >> 15 = -1 -> 0xFFFF
                             "vnsrl.wv v8, v2, v6\n"       // 0xFFFFFFF8 >> 15 = 0x1FFFF -> 0xFFFF
                             "nop\nnop\nnop\n";

template <typename Vm> void expectNarrowResult()
{
    auto vm = runOn<Vm>(kNarrowProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 4), Elements(4, 0xFFFE));
    EXPECT_EQ(readElements(rf, 5, 16, 4), Elements(4, 0xFFFF));
    EXPECT_EQ(readElements(rf, 6, 16, 4), Elements(4, 0xF));
    EXPECT_EQ(readElements(rf, 7, 16, 4), Elements(4, 0xFFFF));
    EXPECT_EQ(readElements(rf, 8, 16, 4), Elements(4, 0xFFFF));
}
} // namespace

TEST(VectorNarrowShiftPipelineTest, NarrowProgramOnEveryVariant)
{
    expectNarrowResult<Kites::RVSSProcessor>();
    expectNarrowResult<Kites::RV5StageProcessorHF>();
    expectNarrowResult<Kites::RV5StageProcessorHNF>();
    expectNarrowResult<Kites::RV5StageProcessorNHF>();
    expectNarrowResult<Kites::RV5StageProcessorNHNF>();
}
