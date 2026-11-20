// Tests for RVV integer min/max (11.9), multiply (11.10), divide (11.11), widening multiply
// (11.12), multiply-add (11.13), widening multiply-add (11.14), merge (11.15) and move (11.16).
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).

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
#include "processor/vector/vector_execute.h"
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

TEST(VectorMulDivAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vminu.vv v1, v2, v3", 0x122180d7},
        {"vmul.vv v1, v2, v3", 0x9621a0d7},
        {"vmul.vx v1, v2, a0, v0.t", 0x942560d7},
        {"vmulh.vv v1, v2, v3", 0x9e21a0d7},
        {"vmulh.vx v1, v2, a0", 0x9e2560d7},
        {"vmulhu.vv v1, v2, v3", 0x9221a0d7},
        {"vmulhu.vx v1, v2, a0", 0x922560d7},
        {"vmulhsu.vv v1, v2, v3, v0.t", 0x9821a0d7},
        {"vmulhsu.vx v1, v2, a0", 0x9a2560d7},
        {"vdivu.vv v1, v2, v3", 0x8221a0d7},
        {"vdivu.vx v1, v2, a0", 0x822560d7},
        {"vdiv.vv v1, v2, v3", 0x8621a0d7},
        {"vdiv.vx v1, v2, a0, v0.t", 0x842560d7},
        {"vremu.vv v1, v2, v3", 0x8a21a0d7},
        {"vremu.vx v1, v2, a0", 0x8a2560d7},
        {"vrem.vv v1, v2, v3", 0x8e21a0d7},
        {"vrem.vx v1, v2, a0", 0x8e2560d7},
        {"vwmul.vv v2, v4, v5", 0xee42a157},
        {"vwmul.vx v2, v4, a0", 0xee456157},
        {"vwmulu.vv v2, v4, v5, v0.t", 0xe042a157},
        {"vwmulu.vx v2, v4, a0", 0xe2456157},
        {"vwmulsu.vv v2, v4, v5", 0xea42a157},
        {"vwmulsu.vx v2, v4, a0", 0xea456157},
        {"vmacc.vv v1, v2, v3", 0xb63120d7},
        {"vmacc.vx v1, a0, v3, v0.t", 0xb43560d7},
        {"vnmsac.vv v1, v2, v3", 0xbe3120d7},
        {"vnmsac.vx v1, a0, v3", 0xbe3560d7},
        {"vmadd.vv v1, v2, v3", 0xa63120d7},
        {"vmadd.vx v1, a0, v3", 0xa63560d7},
        {"vnmsub.vv v1, v2, v3", 0xae3120d7},
        {"vnmsub.vx v1, a0, v3", 0xae3560d7},
        {"vwmaccu.vv v2, v4, v5", 0xf2522157},
        {"vwmaccu.vx v2, a0, v5", 0xf2556157},
        {"vwmacc.vv v2, v4, v5, v0.t", 0xf4522157},
        {"vwmacc.vx v2, a0, v5", 0xf6556157},
        {"vwmaccsu.vv v2, v4, v5", 0xfe522157},
        {"vwmaccsu.vx v2, a0, v5", 0xfe556157},
        {"vwmaccus.vx v2, a0, v5", 0xfa556157},
        {"vmerge.vvm v1, v2, v3, v0", 0x5c2180d7},
        {"vmerge.vxm v1, v2, a0, v0", 0x5c2540d7},
        {"vmerge.vim v1, v2, -16, v0", 0x5c2830d7},
        {"vmv.v.v v1, v3", 0x5e0180d7},
        {"vmv.v.x v1, a0", 0x5e0540d7},
        {"vmv.v.i v1, 15", 0x5e07b0d7},
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

TEST(VectorMulDivAssemblerTest, MinMaxEncodings)
{
    // funct6 000100 vminu, 000101 vmin, 000110 vmaxu, 000111 vmax; OPIVV and OPIVX only.
    const Kites::AssembledProgram program = assembleSource("vminu.vv v1, v2, v3\n"
                                                           "vmin.vx v4, v5, a0, v0.t\n"
                                                           "vmaxu.vv v11, v12, v13\n"
                                                           "vmax.vx v19, v20, a7, v0.t\n");
    EXPECT_EQ(program.text_buffer, (std::vector<uint32_t>{0x122180d7, 0x14554257, 0x1ac685d7,
                                                          0x1d48c9d7}));
}

TEST(VectorMulDivAssemblerTest, DisassemblyUsesEachInstructionsOperandOrder)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0xb43560d7), "vmacc.vx v1, x10, v3, v0.t");
    EXPECT_EQ(disassemble(0xb63120d7), "vmacc.vv v1, v2, v3");
    EXPECT_EQ(disassemble(0xfa556157), "vwmaccus.vx v2, x10, v5");
    EXPECT_EQ(disassemble(0x5c2830d7), "vmerge.vim v1, v2, -16, v0");
    EXPECT_EQ(disassemble(0x5e0180d7), "vmv.v.v v1, v3");
    EXPECT_EQ(disassemble(0x5e0540d7), "vmv.v.x v1, x10");
    EXPECT_EQ(disassemble(0x5e07b0d7), "vmv.v.i v1, 15");
    EXPECT_EQ(disassemble(0x9821a0d7), "vmulhsu.vv v1, v2, v3, v0.t");
}

TEST(VectorMulDivAssemblerTest, OperandRules)
{
    for (const std::string &line : {
             "vmacc.vx v1, v2, a0\n",          // rs1 comes before vs2
             "vmerge.vvm v1, v2, v3\n",        // the v0 operand is required
             "vmerge.vvm v1, v2, v3, v0.t\n",  // ...and is written "v0"
             "vmerge.vvm v0, v2, v3, v0\n",    // cannot overwrite its own mask
             "vmerge.vim v1, v2, 16, v0\n",    // simm5
             "vmv.v.v v1, v2, v0.t\n",         // never masked
             "vmv.v.i v1, 16\n",               // simm5
             "vmv.v.x v1, v2\n",
             "vwmacc.vv v2, v2, v3\n",         // vd overlaps a narrower vs1
             "vwmul.vv v4, v4, v5\n",          // vd overlaps a narrower vs2
             "vwmaccus.vv v2, v4, v5\n",       // only a .vx form exists
             "vmin.vi v1, v2, 3\n",            // no .vi forms
             "vmul.vv v0, v1, v2, v0.t\n",
         })
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorMinMaxTest, SignedAndUnsigned)
{
    // vs2 signed [0, 1, 5, 127, -128, -1, 5, 6]; vs1 signed [0, 2, 5, -128, 127, -1, 4, 7].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vminu.vv v4, v2, v3\n"
                                          "vmin.vv v5, v2, v3\n"
                                          "vmaxu.vv v6, v2, v3\n"
                                          "vmax.vv v7, v2, v3\n"
                                          "vminu.vx v8, v2, x10\n"
                                          "vmin.vx v9, v2, x10\n"
                                          "vmaxu.vx v10, v2, x10\n"
                                          "vmax.vx v11, v2, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0, 1, 5, 0x7F, 0x80, 0xFF, 5, 6});
                                              writeElements(rf, 3, 8, {0, 2, 5, 0x80, 0x7F, 0xFF, 4, 7});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 8), (Elements{0, 1, 5, 0x7F, 0x7F, 0xFF, 4, 6}));
    EXPECT_EQ(readElements(rf, 5, 8, 8), (Elements{0, 1, 5, 0x80, 0x80, 0xFF, 4, 6}));
    EXPECT_EQ(readElements(rf, 6, 8, 8), (Elements{0, 2, 5, 0x80, 0x80, 0xFF, 5, 7}));
    EXPECT_EQ(readElements(rf, 7, 8, 8), (Elements{0, 2, 5, 0x7F, 0x7F, 0xFF, 5, 7}));
    // x10 = -1 is 0xFF at e8: the largest unsigned value, the smallest-but-one signed one.
    EXPECT_EQ(readElements(rf, 8, 8, 8), (Elements{0, 1, 5, 0x7F, 0x80, 0xFF, 5, 6}));
    EXPECT_EQ(readElements(rf, 9, 8, 8), (Elements{0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 10, 8, 8), Elements(8, 0xFF));
    EXPECT_EQ(readElements(rf, 11, 8, 8), (Elements{0, 1, 5, 0x7F, 0xFF, 0xFF, 5, 6}));
}

TEST(VectorMinMaxTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vminu.vv v4, v2, v3, v0.t\n"
                                          "vmin.vx  v5, v2, x10, v0.t\n"
                                          "vmaxu.vv v6, v2, v3, v0.t\n"
                                          "vmax.vx  v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x7F, 1, 0x80, 2});
                                              writeElements(rf, 3, 8, {0x80, 3, 0x7F, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x7F, 0x5A, 0x7F, 0x5A})); // unsigned min
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0xFF, 0x5A, 0x80, 0x5A})); // signed min vs -1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x80, 0x5A, 0x80, 0x5A})); // unsigned max
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x7F, 0x5A, 0xFF, 0x5A})); // signed max vs -1
}

TEST(VectorMultiplyTest, LowAndHighHalvesAtE8)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmul.vv v4, v2, v3\n"
                                          "vmulh.vv v5, v2, v3\n"
                                          "vmulhu.vv v6, v2, v3\n"
                                          "vmulhsu.vv v7, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {3, 0x80, 0xFF, 0x7F});
                                              writeElements(rf, 3, 8, {5, 0x80, 0xFF, 2});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x0F, 0x00, 0x01, 0xFE}));
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x00, 0x40, 0x00, 0x00})); // -128*-128, -1*-1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x00, 0x40, 0xFE, 0x00})); // 255*255 = 0xFE01
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x00, 0xC0, 0xFF, 0x00})); // -128*128, -1*255
}

TEST(VectorMultiplyTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it. x10 = -1 is 0xFF.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmul.vv    v4, v2, v3, v0.t\n"
                                          "vmulh.vv   v5, v2, v3, v0.t\n"
                                          "vmulhu.vx  v6, v2, x10, v0.t\n"
                                          "vmulhsu.vx v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x80, 1, 0xFF, 2});
                                              writeElements(rf, 3, 8, {0x80, 3, 0xFF, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // 0x80 x 0x80 = 0x4000, 0xFF x 0xFF = 0xFE01: low halves
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x00, 0x5A, 0x01, 0x5A}));
    // signed: -128 x -128 = 0x4000, -1 x -1 = 0x0001: high halves
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x40, 0x5A, 0x00, 0x5A}));
    // unsigned: 128 x 255 = 0x7F80, 255 x 255 = 0xFE01
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x7F, 0x5A, 0xFE, 0x5A}));
    // signed x unsigned: -128 x 255 = 0x8080, -1 x 255 = 0xFF01
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x80, 0x5A, 0xFF, 0x5A}));
}

TEST(VectorMultiplyTest, HighHalfAtE64Uses128BitProducts)
{
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 2, e64, m1, ta, ma\n"
        "vmul.vv v4, v2, v3\n"
        "vmulh.vv v5, v2, v3\n"
        "vmulhu.vv v6, v2, v3\n"
        "vmulhsu.vv v7, v2, v3\n",
        [](RegisterFile &rf)
        {
            writeElements(rf, 2, 64, {~0ULL, 0x8000000000000000});
            writeElements(rf, 3, 64, {~0ULL, 0x8000000000000000});
        });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 64, 2), (Elements{1, 0}));
    EXPECT_EQ(readElements(rf, 5, 64, 2), (Elements{0, 0x4000000000000000}));
    EXPECT_EQ(readElements(rf, 6, 64, 2), (Elements{0xFFFFFFFFFFFFFFFE, 0x4000000000000000}));
    // signed(-1) * unsigned(2^64 - 1) and signed(-2^63) * unsigned(2^63).
    EXPECT_EQ(readElements(rf, 7, 64, 2), (Elements{0xFFFFFFFFFFFFFFFF, 0xC000000000000000}));
}

TEST(VectorDivideTest, EdgeCasesMatchTheScalarMExtension)
{
    // vs2 signed [7, -128, -7, 9, -128] / vs1 signed [2, -1, 2, 0, 0]
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 5, e8, m1, ta, ma\n"
                                          "vdiv.vv v4, v2, v3\n"
                                          "vrem.vv v5, v2, v3\n"
                                          "vdivu.vv v6, v2, v3\n"
                                          "vremu.vv v7, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {7, 0x80, 0xF9, 9, 0x80});
                                              writeElements(rf, 3, 8, {2, 0xFF, 2, 0, 0});
                                          });
    const RegisterFile &rf = vm->registers_;
    // -128 / -1 overflows to -128 (remainder 0); x / 0 is all ones (remainder x).
    EXPECT_EQ(readElements(rf, 4, 8, 5), (Elements{3, 0x80, 0xFD, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 5, 8, 5), (Elements{1, 0, 0xFF, 9, 0x80}));
    EXPECT_EQ(readElements(rf, 6, 8, 5), (Elements{3, 0, 0x7C, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 7, 8, 5), (Elements{1, 0x80, 1, 9, 0x80}));
}

TEST(VectorDivideTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it. Element 0 divides by
    // zero (.vv) or is MIN / -1 (.vx, x10 = -1).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vdivu.vv v4, v2, v3, v0.t\n"
                                          "vdiv.vx  v5, v2, x10, v0.t\n"
                                          "vremu.vv v6, v2, v3, v0.t\n"
                                          "vrem.vx  v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x80, 1, 0xF9, 2});
                                              writeElements(rf, 3, 8, {0, 3, 2, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0xFF, 0x5A, 0x7C, 0x5A})); // 128/0, 249/2
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x80, 0x5A, 0x07, 0x5A})); // -128/-1, -7/-1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x80, 0x5A, 0x01, 0x5A})); // 128%0, 249%2
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x00, 0x5A, 0x00, 0x5A})); // -128%-1, -7%-1
}

TEST(VectorDivideTest, SignedOverflowAtE64)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 1, e64, m1, ta, ma\n"
                                          "vdiv.vx v4, v2, x10\n"
                                          "vrem.vx v5, v2, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 64, {0x8000000000000000}); });
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 1), (Elements{0x8000000000000000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 64, 1), (Elements{0}));
}

TEST(VectorMultiplyTest, WideningMultiplySignedness)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmulu.vv v4, v2, v3\n"
                                          "vwmul.vv v6, v2, v3\n"
                                          "vwmulsu.vv v8, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 0x80, 3});
                                              writeElements(rf, 3, 8, {0xFF, 2, 0xFE});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 3), (Elements{0xFE01, 0x0100, 0x02FA}));
    EXPECT_EQ(readElements(rf, 6, 16, 3), (Elements{0x0001, 0xFF00, 0xFFFA}));
    EXPECT_EQ(readElements(rf, 8, 16, 3), (Elements{0xFF01, 0xFF00, 0x02FA}));
}

TEST(VectorMultiplyTest, WideningMaskedOffAndTailElementsKeepTheirOldValues)
{
    // e8 sources, e16 results. vl = 3; v0 = 0b0101 enables elements 0 and 2. Every 16-bit
    // destination element starts as 0x5A5A, so element 1 (masked off) and element 3 (tail)
    // must visibly keep it. x10 = -1 is 0xFF.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmul.vv   v4, v2, v3, v0.t\n"
                                          "vwmulu.vx  v6, v2, x10, v0.t\n"
                                          "vwmulsu.vv v8, v2, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0xFF, 1, 0x80, 2});
                                              writeElements(rf, 3, 8, {0xFF, 3, 2, 4});
                                              for (size_t r = 4; r <= 8; r += 2)
                                              {
                                                  writeElements(rf, r, 16, Elements(4, 0x5A5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // signed: -1 x -1 = 1, -128 x 2 = -256
    EXPECT_EQ(readElements(rf, 4, 16, 4), (Elements{0x0001, 0x5A5A, 0xFF00, 0x5A5A}));
    // unsigned: 255 x 255 = 0xFE01, 128 x 255 = 0x7F80
    EXPECT_EQ(readElements(rf, 6, 16, 4), (Elements{0xFE01, 0x5A5A, 0x7F80, 0x5A5A}));
    // signed vs2 x unsigned vs1: -1 x 255 = -255, -128 x 2 = -256
    EXPECT_EQ(readElements(rf, 8, 16, 4), (Elements{0xFF01, 0x5A5A, 0xFF00, 0x5A5A}));
}

TEST(VectorMultiplyAddTest, SingleWidthForms)
{
    // vs1 = v2 [3, -1, 2], vs2 = v3 [4, 5, 128]; each destination starts as [10, 10, 10].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 2\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmacc.vv v4, v2, v3\n"
                                          "vnmsac.vv v5, v2, v3\n"
                                          "vmadd.vv v6, v2, v3\n"
                                          "vnmsub.vv v7, v2, v3\n"
                                          "vmacc.vx v8, x10, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {3, 0xFF, 2});
                                              writeElements(rf, 3, 8, {4, 5, 0x80});
                                              for (size_t r = 4; r <= 8; ++r)
                                              {
                                                  writeElements(rf, r, 8, {10, 10, 10});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 3), (Elements{22, 5, 10}));     // vd + vs1*vs2
    EXPECT_EQ(readElements(rf, 5, 8, 3), (Elements{0xFE, 15, 10}));  // vd - vs1*vs2
    EXPECT_EQ(readElements(rf, 6, 8, 3), (Elements{34, 0xFB, 0x94})); // vs1*vd + vs2
    EXPECT_EQ(readElements(rf, 7, 8, 3), (Elements{0xE6, 15, 0x6C})); // vs2 - vs1*vd
    EXPECT_EQ(readElements(rf, 8, 8, 3), (Elements{18, 20, 10}));
}

TEST(VectorMultiplyAddTest, MaskedOffAndTailElementsAreNotComputed)
{
    // vl = 3; v0 = 0b0101 enables elements 0 and 2. Every vd starts as 10. Elements 1 (masked
    // off) and 3 (tail) would change if they were computed, so they must stay exactly 10.
    // vs1 = v2 [3, 7, -1, 9], vs2 = v3 [4, 6, 5, 8], x10 = 2.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 2\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmacc.vv  v4, v2, v3, v0.t\n"
                                          "vnmsac.vx v5, x10, v3, v0.t\n"
                                          "vmadd.vv  v6, v2, v3, v0.t\n"
                                          "vnmsub.vx v7, x10, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {3, 7, 0xFF, 9});
                                              writeElements(rf, 3, 8, {4, 6, 5, 8});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 10));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{22, 10, 5, 10}));     // vd + vs1*vs2
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{2, 10, 0, 10}));      // vd - 2*vs2
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{34, 10, 0xFB, 10}));  // vs1*vd + vs2
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0xF0, 10, 0xF1, 10})); // vs2 - 2*vd
}

TEST(VectorMultiplyAddTest, WideningFormsAndSignedness)
{
    // vs1 = v2 [0xFF, 2, 0x80], vs2 = v3 [0xFF, 0xFE, 3]; each e16 destination starts at 1000.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmaccu.vv v4, v2, v3\n"
                                          "vwmacc.vv v6, v2, v3\n"
                                          "vwmaccsu.vv v8, v2, v3\n"
                                          "vwmaccus.vx v10, x10, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 2, 0x80});
                                              writeElements(rf, 3, 8, {0xFF, 0xFE, 3});
                                              for (size_t r = 4; r <= 10; r += 2)
                                              {
                                                  writeElements(rf, r, 16, {1000, 1000, 1000});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 3), (Elements{489, 1508, 1384})); // 1000+65025 wraps
    EXPECT_EQ(readElements(rf, 6, 16, 3), (Elements{1001, 996, 616}));
    EXPECT_EQ(readElements(rf, 8, 16, 3), (Elements{745, 1508, 616}));  // signed vs1
    EXPECT_EQ(readElements(rf, 10, 16, 3), (Elements{745, 490, 1765})); // unsigned rs1 = 255
}

TEST(VectorMergeMoveTest, MergeWritesEveryBodyElementAndMoveSplats)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -7\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmerge.vvm v5, v2, v3, v0\n"
                                          "vmerge.vxm v6, v2, x10, v0\n"
                                          "vmerge.vim v7, v2, -16, v0\n"
                                          "vmv.v.v v8, v3\n"
                                          "vmv.v.x v9, x10\n"
                                          "vmv.v.i v10, 15\n"
                                          // The spec's mask-to-elements idiom.
                                          "vmv.v.i v11, 0\n"
                                          "vmerge.vim v11, v11, 1, v0\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {1, 2, 3, 4});
                                              writeElements(rf, 3, 8, {10, 20, 30, 40});
                                              for (size_t r = 5; r <= 10; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(5, 9));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // Element 4 is the tail: undisturbed.
    EXPECT_EQ(readElements(rf, 5, 8, 5), (Elements{10, 2, 30, 4, 9}));
    EXPECT_EQ(readElements(rf, 6, 8, 5), (Elements{0xF9, 2, 0xF9, 4, 9}));
    EXPECT_EQ(readElements(rf, 7, 8, 5), (Elements{0xF0, 2, 0xF0, 4, 9}));
    EXPECT_EQ(readElements(rf, 8, 8, 5), (Elements{10, 20, 30, 40, 9}));
    EXPECT_EQ(readElements(rf, 9, 8, 5), (Elements{0xF9, 0xF9, 0xF9, 0xF9, 9}));
    EXPECT_EQ(readElements(rf, 10, 8, 5), (Elements{15, 15, 15, 15, 9}));
    EXPECT_EQ(readElements(rf, 11, 8, 4), (Elements{1, 0, 1, 0}));
}

TEST(VectorMergeMoveTest, MoveWithNonZeroVs2IsReservedAndUndoWorks)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vmv.v.i v1, -3\n");
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0xFFFFFFFD));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0xFFFFFFFD));

    // vmv.v.v v1, v3 with the vs2 field set to v2 instead of v0.
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x5e0180d7 | (2u << 20), 0, 0,
                                                      vm->registers_, vm->memory_controller_,
                                                      changes, memoryChanges),
                 std::runtime_error);
}

TEST(VectorMultiplyAddTest, WideningMaskedOffAndTailElementsAreNotComputed)
{
    // e8 sources, e16 accumulators all starting at 1000. vl = 3; v0 = 0b0101 enables elements 0
    // and 2. Elements 1 (masked off) and 3 (tail) would change if computed, so they must stay
    // 1000. vs1 = v2 [0xFF, 7, 0x80, 9], vs2 = v3 [0xFF, 6, 3, 8], x10 = -1 (0xFF).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmaccu.vv  v4, v2, v3, v0.t\n"
                                          "vwmacc.vx   v6, x10, v3, v0.t\n"
                                          "vwmaccsu.vv v8, v2, v3, v0.t\n"
                                          "vwmaccus.vx v10, x10, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0xFF, 7, 0x80, 9});
                                              writeElements(rf, 3, 8, {0xFF, 6, 3, 8});
                                              for (size_t r = 4; r <= 10; r += 2)
                                              {
                                                  writeElements(rf, r, 16, Elements(4, 1000));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // unsigned x unsigned: 1000 + 255*255 wraps to 489; 1000 + 128*3
    EXPECT_EQ(readElements(rf, 4, 16, 4), (Elements{489, 1000, 1384, 1000}));
    // signed -1 x signed vs2: 1000 + 1; 1000 - 3
    EXPECT_EQ(readElements(rf, 6, 16, 4), (Elements{1001, 1000, 997, 1000}));
    // signed vs1 x unsigned vs2: 1000 - 255; 1000 - 384
    EXPECT_EQ(readElements(rf, 8, 16, 4), (Elements{745, 1000, 616, 1000}));
    // unsigned 255 x signed vs2: 1000 - 255; 1000 + 765
    EXPECT_EQ(readElements(rf, 10, 16, 4), (Elements{745, 1000, 1765, 1000}));
}

TEST(VectorMultiplyAddTest, IllegalWideningConfigurationsTrap)
{
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwmacc.vv v2, v4, v5\n");
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwmul.vv v2, v4, v5\n");
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvwmaccu.vv v3, v4, v5\n"); // odd vd group
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kMulProgram = "li x10, 3\n"
                          "nop\nnop\nnop\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vmv.v.i v1, 7\n"               // 7
                          "vmv.v.x v2, x10\n"             // 3
                          "vmul.vv v3, v1, v2\n"          // 21
                          "vdiv.vx v4, v3, x10\n"         // 7
                          "vrem.vx v5, v1, x10\n"         // 1
                          "vmacc.vv v3, v1, v2\n"         // 21 + 21 = 42
                          "vmax.vx v6, v5, x10\n"         // 3
                          "vmv.v.i v0, 5\n"               // mask bits 0 and 2
                          "vmerge.vvm v7, v1, v3, v0\n"   // [42, 7, 42, 7]
                          "vwmulu.vx v8, v1, x10\n"       // e64 21
                          "nop\nnop\nnop\n";

template <typename Vm> void expectMulResult()
{
    auto vm = runOn<Vm>(kMulProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 3, 32, 4), Elements(4, 42));
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 7));
    EXPECT_EQ(readElements(rf, 5, 32, 4), Elements(4, 1));
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 3));
    EXPECT_EQ(readElements(rf, 7, 32, 4), (Elements{42, 7, 42, 7}));
    EXPECT_EQ(readElements(rf, 8, 64, 2), Elements(2, 21));
    EXPECT_EQ(readElements(rf, 9, 64, 2), Elements(2, 21));
}
} // namespace

TEST(VectorMulDivPipelineTest, ProgramOnEveryVariant)
{
    expectMulResult<Kites::RVSSProcessor>();
    expectMulResult<Kites::RV5StageProcessorHF>();
    expectMulResult<Kites::RV5StageProcessorHNF>();
    expectMulResult<Kites::RV5StageProcessorNHF>();
    expectMulResult<Kites::RV5StageProcessorNHNF>();
}
