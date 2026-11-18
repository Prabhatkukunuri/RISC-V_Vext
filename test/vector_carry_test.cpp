// Tests for RVV add-with-carry / subtract-with-borrow (11.4): vadc, vsbc, vmadc, vmsbc.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: v0 supplies the carry/borrow-in, every body element is written, vadc/vsbc
// cannot write v0 and are reserved with vm = 1, and vmadc/vmsbc write a mask (even into v0).

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

namespace
{
uint64_t maskByte(const RegisterFile &rf, size_t reg)
{
    return rf.ReadVectorElement(reg, 0, 8);
}

// e8, vl = 4: carry-in (v0) at elements 1 and 2.
void carryData(RegisterFile &rf)
{
    writeElements(rf, 0, 8, {0b0110});
    writeElements(rf, 2, 8, {0xFF, 0xFF, 0x10, 0x00});
    writeElements(rf, 3, 8, {0x01, 0x00, 0x20, 0x00});
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorCarryAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vadc.vvm v1, v2, v3, v0", 0x402180d7},  {"vadc.vxm v1, v2, a0, v0", 0x402540d7},
        {"vadc.vim v1, v2, -16, v0", 0x402830d7}, {"vmadc.vvm v1, v2, v3, v0", 0x442180d7},
        {"vmadc.vxm v0, v2, a0, v0", 0x44254057}, {"vmadc.vim v1, v2, 15, v0", 0x4427b0d7},
        {"vmadc.vv v1, v2, v3", 0x462180d7},      {"vmadc.vx v1, v2, a0", 0x462540d7},
        {"vmadc.vi v1, v2, -1", 0x462fb0d7},      {"vsbc.vvm v1, v2, v3, v0", 0x482180d7},
        {"vsbc.vxm v1, v2, a0, v0", 0x482540d7},  {"vmsbc.vvm v1, v2, v3, v0", 0x4c2180d7},
        {"vmsbc.vxm v1, v2, a0, v0", 0x4c2540d7}, {"vmsbc.vv v1, v2, v3", 0x4e2180d7},
        {"vmsbc.vx v1, v2, a0", 0x4e2540d7},
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

    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x402830d7), "vadc.vim v1, v2, -16, v0");
    EXPECT_EQ(disassemble(0x44254057), "vmadc.vxm v0, v2, x10, v0");
    EXPECT_EQ(disassemble(0x462180d7), "vmadc.vv v1, v2, v3");
}

TEST(VectorCarryAssemblerTest, OperandRules)
{
    for (const std::string &line : {
             "vadc.vvm v0, v2, v3, v0\n",     // v0 holds the carry-in
             "vsbc.vxm v0, v2, a0, v0\n",
             "vadc.vvm v1, v2, v3\n",         // the v0 operand is required
             "vadc.vvm v1, v2, v3, v0.t\n",   // ...and is written "v0"
             "vmadc.vv v1, v2, v3, v0.t\n",   // the no-carry forms take no mask
             "vmadc.vv v1, v2, v3, v0\n",
             "vadc.vim v1, v2, 16, v0\n",     // simm5
             "vsbc.vim v1, v2, 3, v0\n",      // no subtract-immediate forms
             "vmsbc.vi v1, v2, 3\n",
         })
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // The carry-out may go to v0.
    EXPECT_NO_THROW(assembleSource("vmadc.vvm v0, v2, v3, v0\nvmsbc.vv v0, v2, v3\n"));
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorCarryTest, VectorVectorForms)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vvm v4, v2, v3, v0\n"
                                          "vmadc.vvm v5, v2, v3, v0\n"
                                          "vmadc.vv v6, v2, v3\n"
                                          "vsbc.vvm v7, v2, v3, v0\n"
                                          "vmsbc.vvm v8, v2, v3, v0\n"
                                          "vmsbc.vv v9, v2, v3\n",
                                          carryData);
    const RegisterFile &rf = vm->registers_;
    // Every element is written, whatever its v0 bit.
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));
    EXPECT_EQ(maskByte(rf, 5), 0x03u); // 0xFF+1 and 0xFF+0+1 carry
    EXPECT_EQ(maskByte(rf, 6), 0x01u); // without the carry-in only 0xFF+1 does
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0xFE, 0xFE, 0xEF, 0x00}));
    EXPECT_EQ(maskByte(rf, 8), 0x04u); // 0x10 - 0x20 - 1 < 0
    EXPECT_EQ(maskByte(rf, 9), 0x04u);
}

TEST(VectorCarryTest, ScalarAndImmediateForms)
{
    // The immediate -1 is 0xFF at e8 (sign-extended, then an unsigned SEW value).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 1\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vxm v10, v2, x10, v0\n"
                                          "vmadc.vxm v11, v2, x10, v0\n"
                                          "vmsbc.vx v12, v2, x10\n"
                                          "vadc.vim v13, v2, -1, v0\n"
                                          "vmadc.vi v14, v2, -1\n"
                                          "vmadc.vim v15, v2, -1, v0\n",
                                          carryData);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 4), (Elements{0x00, 0x01, 0x12, 0x01}));
    EXPECT_EQ(maskByte(rf, 11), 0x03u);
    EXPECT_EQ(maskByte(rf, 12), 0x08u); // only 0 - 1 borrows
    EXPECT_EQ(readElements(rf, 13, 8, 4), (Elements{0xFE, 0xFF, 0x10, 0xFF}));
    EXPECT_EQ(maskByte(rf, 14), 0x07u);
    EXPECT_EQ(maskByte(rf, 15), 0x07u);
}

TEST(VectorCarryTest, CarryOutAt64Bits)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmadc.vv v4, v2, v3\n"
                                          "vmsbc.vv v5, v2, v3\n"
                                          "vmadc.vvm v6, v2, v8, v0\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b01});
                                              writeElements(rf, 2, 64, {~0ULL, 5});
                                              writeElements(rf, 3, 64, {1, 7});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 4), 0x01u); // 2^64 - 1 + 1 carries
    EXPECT_EQ(maskByte(vm->registers_, 5), 0x02u); // 5 - 7 borrows
    EXPECT_EQ(maskByte(vm->registers_, 6), 0x01u); // 2^64 - 1 + 0 + carry-in 1
}

TEST(VectorCarryTest, CarryOutCanReplaceTheCarryIn)
{
    // vmadc.vvm v0, ..., v0: bit i of v0 is read before it is replaced.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmadc.vvm v0, v2, v3, v0\n",
                                          carryData);
    EXPECT_EQ(maskByte(vm->registers_, 0), 0x03u);
}

TEST(VectorCarryTest, TwoWordAddition)
{
    // Two 128-bit sums, low words in v4/v8 and high words in v5/v9:
    //   (1 : 2^64-1) + (2 : 1) = (4 : 0)   and   (0 : 5) + (0 : 6) = (0 : 11)
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmadc.vv v0, v4, v8\n"       // carry out of the low words
                                          "vadd.vv v4, v4, v8\n"        // low sum
                                          "vadc.vvm v5, v5, v9, v0\n",  // high sum + carry
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 64, {~0ULL, 5});
                                              writeElements(rf, 5, 64, {1, 0});
                                              writeElements(rf, 8, 64, {1, 6});
                                              writeElements(rf, 9, 64, {2, 0});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 2), (Elements{0, 11}));
    EXPECT_EQ(readElements(vm->registers_, 5, 64, 2), (Elements{4, 0}));
}

TEST(VectorCarryTest, ReservedEncodingsAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vvm v4, v2, v3, v0\n",
                                          carryData);
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));

    std::vector<Kites::RegisterChange> changes;
    // vadc.vvm v1, v2, v3 with vm = 1 is reserved.
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x402180d7 | (1u << 25), 0, 0,
                                                      vm->registers_, changes),
                 std::runtime_error);
    // vadc.vvm v0, v2, v3, v0 is reserved (vd = v0).
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x40218057, 0, 0, vm->registers_, changes),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kCarryProgram = "li x10, -1\n"
                            "nop\nnop\nnop\n"
                            "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "vmv.v.x v2, x10\n"             // 0xFFFFFFFF
                            "vmv.v.i v3, 1\n"
                            "vmadc.vv v0, v2, v3\n"         // every element carries
                            "vadc.vvm v4, v3, v3, v0\n"     // 1 + 1 + 1 = 3
                            "vmsbc.vx v5, v3, x10\n"        // 1 < 0xFFFFFFFF: borrow
                            "vsbc.vxm v6, v3, x10, v0\n"    // 1 - (-1) - 1 = 1
                            "nop\nnop\nnop\n";

template <typename Vm> void expectCarryResult()
{
    auto vm = runOn<Vm>(kCarryProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 0), 0x0Fu);
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 3));
    EXPECT_EQ(maskByte(rf, 5), 0x0Fu);
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 1));
}
} // namespace

TEST(VectorCarryPipelineTest, ProgramOnEveryVariant)
{
    expectCarryResult<Kites::RVSSProcessor>();
    expectCarryResult<Kites::RV5StageProcessorHF>();
    expectCarryResult<Kites::RV5StageProcessorHNF>();
    expectCarryResult<Kites::RV5StageProcessorNHF>();
    expectCarryResult<Kites::RV5StageProcessorNHNF>();
}
