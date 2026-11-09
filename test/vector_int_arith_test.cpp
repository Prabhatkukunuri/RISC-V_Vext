// Tests for the RVV single-width integer add/subtract instructions:
// vadd.vv/.vx/.vi, vsub.vv/.vx, vrsub.vx/.vi.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding);
// element behaviour follows RVV 1.0 section 11.1 and Spike (tail and masked-off elements are
// left undisturbed).

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
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

using Setup = std::function<void(RegisterFile &)>;

// Loads the program, lets @p setup seed registers, then runs it to the end.
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

void writeElements(RegisterFile &rf, size_t reg, size_t sew, const std::vector<uint64_t> &values)
{
    for (size_t i = 0; i < values.size(); ++i)
    {
        rf.WriteVectorElement(reg, i, sew, values[i]);
    }
}

std::vector<uint64_t> readElements(const RegisterFile &rf, size_t reg, size_t sew, size_t count)
{
    std::vector<uint64_t> values;
    for (size_t i = 0; i < count; ++i)
    {
        values.push_back(rf.ReadVectorElement(reg, i, sew));
    }
    return values;
}

using Elements = std::vector<uint64_t>;

} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorIntArithAssemblerTest, EncodingsMatchLlvmMc)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vadd.vv v1, v2, v3", 0x022180d7},
        {"vadd.vv v8, v16, v24, v0.t", 0x010c0457},
        {"vadd.vx v4, v5, x10", 0x02554257},
        {"vadd.vx v31, v30, t6, v0.t", 0x01efcfd7},
        {"vadd.vi v6, v7, -16", 0x02783357},
        {"vadd.vi v6, v7, 15, v0.t", 0x0077b357},
        {"vsub.vv v1, v2, v3", 0x0a2180d7},
        {"vsub.vx v9, v10, a1, v0.t", 0x08a5c4d7},
        {"vrsub.vx v12, v13, t0", 0x0ed2c657},
        {"vrsub.vi v14, v15, -1", 0x0effb757},
        {"vrsub.vi v0, v1, 0", 0x0e103057},
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
        EXPECT_EQ(program.text_buffer[i], cases[i].second) << cases[i].first;
    }
}

TEST(VectorIntArithAssemblerTest, IllegalFormsAreRejected)
{
    const std::vector<std::string> bad = {
        "vsub.vi v1, v2, 3\n",           // no such form
        "vrsub.vv v1, v2, v3\n",         // no such form
        "vadd.vi v1, v2, 16\n",          // simm5 is -16..15
        "vadd.vi v1, v2, -17\n",
        "vadd.vv v0, v1, v2, v0.t\n",    // masked op can't write the mask register
        "vadd.vv v1, v2, x3\n",          // wrong operand kinds
        "vadd.vx v1, v2, v3\n",
        "vadd.vv v1, v2, v3, v1.t\n",    // only v0 can be a mask
        "vadd.vv v1, v2, v3, v0\n",
        "vadd.vv v1, v2\n",
        "vadd.vv v1, v2, v0.t\n",
    };
    for (const std::string &line : bad)
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

TEST(VectorIntArithAssemblerTest, DisassemblyRoundTrips)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x022180d7), "vadd.vv v1, v2, v3");
    EXPECT_EQ(disassemble(0x010c0457), "vadd.vv v8, v16, v24, v0.t");
    EXPECT_EQ(disassemble(0x02554257), "vadd.vx v4, v5, x10");
    EXPECT_EQ(disassemble(0x02783357), "vadd.vi v6, v7, -16");
    EXPECT_EQ(disassemble(0x0077b357), "vadd.vi v6, v7, 15, v0.t");
    EXPECT_EQ(disassemble(0x0ed2c657), "vrsub.vx v12, v13, x5");
    EXPECT_EQ(disassemble(0x0effb757), "vrsub.vi v14, v15, -1");
}

// ---------------------------------------------------------------------------------------------
// Element results (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorIntArithTest, VaddVvWrapsAtSew32)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vadd.vv v1, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 32, {1, 2, 3, 0xFFFFFFFF});
                                              writeElements(rf, 3, 32, {10, 20, 30, 1});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{11, 22, 33, 0}));
}

TEST(VectorIntArithTest, VsubVvAtSew64)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsub.vv v1, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 64, {100, 0});
                                              writeElements(rf, 3, 64, {1, 1});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 64, 2), (Elements{99, ~0ULL}));
}

TEST(VectorIntArithTest, ScalarOperandIsTruncatedToSew)
{
    // x10 = 0x1_0000_0105: only the low 8 bits (0x05) take part at e8.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0x10000\n"
                                          "slli x10, x10, 16\n"
                                          "addi x10, x10, 0x105\n"
                                          "vsetivli x0, 16, e8, m1, ta, ma\n"
                                          "vadd.vx v1, v2, x10\n"
                                          "vsub.vx v3, v2, x10\n"
                                          "vrsub.vx v4, v2, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 8, {0, 1, 0xFF, 0xFB}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 4), (Elements{0x05, 0x06, 0x04, 0x00}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 4), (Elements{0xFB, 0xFC, 0xFA, 0xF6}));
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x05, 0x04, 0x06, 0x0A}));
}

TEST(VectorIntArithTest, ImmediateIsSignExtendedToSew)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e16, m1, ta, ma\n"
                                          "vadd.vi v1, v2, -16\n"
                                          "vadd.vi v3, v2, 15\n"
                                          "vrsub.vi v4, v2, -1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 16, {0, 20, 0xFFFF}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 16, 3), (Elements{0xFFF0, 4, 0xFFEF}));
    EXPECT_EQ(readElements(vm->registers_, 3, 16, 3), (Elements{15, 35, 14}));
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 3), (Elements{0xFFFF, 0xFFEB, 0}));
}

TEST(VectorIntArithTest, MaskedOffAndTailElementsAreUndisturbed)
{
    // vl = 3 of VLMAX 4; v0 = 0b0101 enables elements 0 and 2. Element 1 is masked off,
    // element 3 is tail: both keep vd's old value.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vadd.vi v1, v2, 1, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 1, 32, {7, 7, 7, 7});
                                              writeElements(rf, 2, 32, {10, 20, 30, 40});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{11, 7, 31, 7}));
}

TEST(VectorIntArithTest, StartsAtVstartAndClearsIt)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "csrrwi x0, vstart, 2\n"
                                          "vadd.vi v1, v2, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 32, {10, 20, 30, 40}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{0, 0, 31, 41}));
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VectorIntArithTest, Lmul2GroupSpansTwoRegisters)
{
    // e32, m2: VLMAX 8, so v2/v3 and v4/v5 act as 8-element groups.
    auto vm = runOn<Kites::RVSSProcessor>("vsetvli x5, x0, e32, m2, ta, ma\n"
                                          "vadd.vi v2, v4, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 4, 32, {0, 1, 2, 3, 4, 5, 6, 7}); });
    EXPECT_EQ(vm->registers_.ReadGpr(5), 8u);
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{1, 2, 3, 4}));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4), (Elements{5, 6, 7, 8}));
}

TEST(VectorIntArithTest, IllegalInstructionsTrap)
{
    // No vsetvli: vtype is still vill.
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vadd.vv v1, v2, v3\n"), std::runtime_error);
    // LMUL = 2 needs even register numbers.
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e32, m2, ta, ma\n"
                                             "vadd.vv v3, v2, v4\n"),
                 std::runtime_error);
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e32, m2, ta, ma\n"
                                             "vadd.vx v2, v5, x1\n"),
                 std::runtime_error);

    // A masked op writing v0 can't be assembled, so patch vd to 0 in the machine code.
    Kites::AssembledProgram program = assembleSource("vsetivli x0, 4, e32, m1, ta, ma\n"
                                                     "vadd.vv v1, v2, v3, v0.t\n");
    program.text_buffer[1] &= ~(0x1Fu << 7);
    Kites::setupVmStateDirectory();
    Kites::RVSSProcessor vm;
    vm.LoadProgram(program);
    EXPECT_THROW(vm.DebugRun(), std::runtime_error);
}

TEST(VectorIntArithTest, UndoAndRedoRestoreDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vadd.vi v1, v2, 5\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 32, {9, 9, 9, 9});
                                              writeElements(rf, 2, 32, {1, 2, 3, 4});
                                          });
    const Elements after{6, 7, 8, 9};
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), after);

    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{9, 9, 9, 9}));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), after);
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
// Vector-to-vector dependencies are back to back: vector instructions execute and commit in
// EX in program order, so even the variants without hazard detection see each result. The only
// scalar dependency (x10) is spaced out.
const char *kChainProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "li x10, 7\n"
                            "nop\nnop\nnop\n"
                            "vadd.vi v1, v0, 3\n"   // 3
                            "vadd.vx v2, v1, x10\n" // 10
                            "vsub.vv v3, v2, v1\n"  // 7
                            "vrsub.vi v4, v3, -1\n" // -8
                            "vrsub.vx v5, v1, x10\n" // 4
                            "vsub.vx v6, v5, x10\n" // -3
                            "nop\nnop\nnop\n";

template <typename Vm> void expectChainResult()
{
    auto vm = runOn<Vm>(kChainProgram);
    const auto all = [](uint64_t v) { return Elements(4, v & 0xFFFFFFFF); };
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), all(3));
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), all(10));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4), all(7));
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), all(-8));
    EXPECT_EQ(readElements(vm->registers_, 5, 32, 4), all(4));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 4), all(-3));
    // vd is a vector register: x1..x6 must not have been written.
    for (size_t reg = 1; reg <= 6; ++reg)
    {
        EXPECT_EQ(vm->registers_.ReadGpr(reg), 0u) << "x" << reg;
    }
}
} // namespace

TEST(VectorIntArithPipelineTest, ChainOnEveryVariant)
{
    expectChainResult<Kites::RVSSProcessor>();
    expectChainResult<Kites::RV5StageProcessorHF>();
    expectChainResult<Kites::RV5StageProcessorHNF>();
    expectChainResult<Kites::RV5StageProcessorNHF>();
    expectChainResult<Kites::RV5StageProcessorNHNF>();
}

TEST(VectorIntArithPipelineTest, ScalarOperandIsForwarded)
{
    const char *program = "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "li x11, 100\n"
                          "vadd.vx v7, v0, x11\n";
    for (const auto &vm : {std::unique_ptr<Kites::ProcessorBase>(
                               runOn<Kites::RV5StageProcessorHF>(program)),
                           std::unique_ptr<Kites::ProcessorBase>(
                               runOn<Kites::RV5StageProcessorHNF>(program))})
    {
        EXPECT_EQ(readElements(vm->registers_, 7, 32, 4), Elements(4, 100));
    }
}

TEST(VectorIntArithPipelineTest, UndoToStartClearsVectorResults)
{
    auto vm = runOn<Kites::RV5StageProcessorHF>(kChainProgram);
    for (uint64_t cycles = vm->cycle_s_; cycles > 0; --cycles)
    {
        vm->Undo();
    }
    for (size_t reg = 1; reg <= 6; ++reg)
    {
        EXPECT_EQ(readElements(vm->registers_, reg, 32, 4), Elements(4, 0)) << "v" << reg;
    }
}
