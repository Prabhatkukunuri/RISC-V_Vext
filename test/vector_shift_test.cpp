// Tests for the RVV single-width bit shifts (11.6): vsll, vsrl, vsra in .vv/.vx/.vi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: only the low log2(SEW) bits of the shift amount are used, .vi takes an
// unsigned 5-bit immediate, and vsra fills with the sign bit.

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
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorShiftAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vsll.vv v1, v2, v3", 0x962180d7},
        {"vsll.vx v4, v5, a0, v0.t", 0x94554257},
        {"vsll.vi v6, v7, 31", 0x967fb357},
        {"vsrl.vv v8, v9, v10, v0.t", 0xa0950457},
        {"vsrl.vx v11, v12, t0", 0xa2c2c5d7},
        {"vsrl.vi v13, v14, 0", 0xa2e036d7},
        {"vsra.vv v15, v16, v17", 0xa70887d7},
        {"vsra.vx v18, v19, s1", 0xa734c957},
        {"vsra.vi v3, v1, 2", 0xa61131d7},
        {"vsra.vi v20, v21, 17, v0.t", 0xa558ba57},
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

TEST(VectorShiftAssemblerTest, ImmediateIsUnsignedInDisassembly)
{
    using Kites::instruction_set::disassemble;
    // uimm 31 and 17 have bit 4 set: as a signed simm5 they would read -1 and -15.
    EXPECT_EQ(disassemble(0x967fb357), "vsll.vi v6, v7, 31");
    EXPECT_EQ(disassemble(0xa558ba57), "vsra.vi v20, v21, 17, v0.t");
    EXPECT_EQ(disassemble(0xa2c2c5d7), "vsrl.vx v11, v12, x5");
    // The signed instructions are unchanged.
    EXPECT_EQ(disassemble(0x2e2fb0d7), "vxor.vi v1, v2, -1");
}

TEST(VectorShiftAssemblerTest, ImmediateRangeIsZeroTo31)
{
    for (const std::string &line : {"vsll.vi v1, v2, 32\n", "vsrl.vi v1, v2, -1\n",
                                    "vsra.vi v1, v2, 100\n", "vsra.vi v0, v1, 3, v0.t\n",
                                    "vsll.vv v1, v2, a0\n", "vsra.vx v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    EXPECT_NO_THROW(assembleSource("vsll.vi v1, v2, 0\nvsra.vi v1, v2, 31\n"));
    // vadd.vi still uses the signed range.
    EXPECT_THROW(assembleSource("vadd.vi v1, v2, 16\n"), std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorShiftTest, LogicalVersusArithmeticRightShift)
{
    // 0x81 has its sign bit set: vsrl fills with 0, vsra with 1.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 1\n"
                                          "vsrl.vi v3, v1, 1\n"
                                          "vsra.vi v4, v1, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 8, {0x81, 0x7E}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 2), (Elements{0x02, 0xFC}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 2), (Elements{0x40, 0x3F}));
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 2), (Elements{0xC0, 0x3F}));
}

TEST(VectorShiftTest, VvUsesEachElementsOwnShiftAmountModuloSew)
{
    // e16: amounts 0, 1, 15 and 16. 16 & 15 = 0, so the last element is not shifted.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e16, m1, ta, ma\n"
                                          "vsra.vv v4, v1, v3\n"
                                          "vsrl.vv v5, v1, v3\n"
                                          "vsll.vv v6, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 16, Elements(4, 0x8000));
                                              writeElements(rf, 2, 16, Elements(4, 1));
                                              writeElements(rf, 3, 16, {0, 1, 15, 16});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x8000, 0xC000, 0xFFFF, 0x8000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 16, 4), (Elements{0x8000, 0x4000, 0x0001, 0x8000}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{0x0001, 0x0002, 0x8000, 0x0001}));
}

TEST(VectorShiftTest, VxScalarAmountIsMaskedToLog2Sew)
{
    // At e8 only 3 bits count: 9 shifts by 1. At e64, 6 bits: 68 shifts by 4.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 9\n"
                                          "li x11, 68\n"
                                          "vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vsll.vx v2, v1, x10\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsrl.vx v4, v3, x11\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 8, {0x01, 0x40});
                                              writeElements(rf, 3, 64, {0xF0, 0x100});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 2), (Elements{0x02, 0x80}));
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 2), (Elements{0x0F, 0x10}));
}

TEST(VectorShiftTest, ViImmediateIsUnsignedEvenAt64Bits)
{
    // uimm 31 must shift by 31. A sign-extended immediate would be -1, i.e. a shift of 63.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsra.vi v2, v1, 31\n"
                                          "vsll.vi v3, v1, 31\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 64, {0x8000000000000000, 1}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 64, 2), (Elements{0xFFFFFFFF00000000, 0}));
    EXPECT_EQ(readElements(vm->registers_, 3, 64, 2), (Elements{0, 0x80000000}));
}

TEST(VectorShiftTest, MaskedAndTailElementsAreUndisturbed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b011});
                                              writeElements(rf, 1, 32, {1, 2, 3, 4});
                                              writeElements(rf, 2, 32, {9, 9, 9, 9});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x10, 0x20, 9, 9}));
}

TEST(VectorShiftTest, UndoRestoresDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 8\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 32, {1, 2, 3, 4}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x100, 0x200, 0x300, 0x400}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x100, 0x200, 0x300, 0x400}));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kShiftProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "li x10, 3\n"
                            "nop\nnop\nnop\n"
                            "vadd.vi v1, v0, -8\n"   // 0xFFFFFFF8
                            "vsra.vi v2, v1, 2\n"    // -2 = 0xFFFFFFFE
                            "vsrl.vi v3, v1, 28\n"   // 0xF
                            "vsll.vx v4, v3, x10\n"  // 0x78
                            "vsra.vv v5, v4, v3\n"   // 0x78 >> 15 = 0
                            "vsll.vv v6, v3, v3\n"   // 0xF << 15 = 0x78000
                            "nop\nnop\nnop\n";

template <typename Vm> void expectShiftResult()
{
    auto vm = runOn<Vm>(kShiftProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 2, 32, 4), Elements(4, 0xFFFFFFFE));
    EXPECT_EQ(readElements(rf, 3, 32, 4), Elements(4, 0xF));
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 0x78));
    EXPECT_EQ(readElements(rf, 5, 32, 4), Elements(4, 0));
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 0x78000));
}
} // namespace

TEST(VectorShiftPipelineTest, ShiftProgramOnEveryVariant)
{
    expectShiftResult<Kites::RVSSProcessor>();
    expectShiftResult<Kites::RV5StageProcessorHF>();
    expectShiftResult<Kites::RV5StageProcessorHNF>();
    expectShiftResult<Kites::RV5StageProcessorNHF>();
    expectShiftResult<Kites::RV5StageProcessorNHNF>();
}
