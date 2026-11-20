// Tests for the RVV integer reductions (14.1 / 14.2): vredsum, vredand, vredor, vredxor,
// vredminu, vredmin, vredmaxu, vredmax and the widening vwredsumu / vwredsum.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: vd[0] = vs1[0] op vs2[active elements < vl]; vs2 is an LMUL group while vd
// and vs1 are single registers; only vd[0] is written; vl = 0 writes nothing; vstart != 0 is
// illegal; vd may overlap any source, v0 included.

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
// e8 data: vs2 = v2 [0xF1, 0xF3, 0x7F, 0xF5]  (signed: -15, -13, 127, -11)
//          v3[0] = 0x05, v4[0] = 0xFF (-1 signed, 255 unsigned)
void reductionData(RegisterFile &rf)
{
    writeElements(rf, 2, 8, {0xF1, 0xF3, 0x7F, 0xF5});
    writeElements(rf, 3, 8, {0x05});
    writeElements(rf, 4, 8, {0xFF});
    for (size_t r = 10; r <= 17; ++r)
    {
        writeElements(rf, r, 8, {0x99, 0x99, 0x99, 0x99}); // element 0 is replaced, 1.. are tail
    }
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorReductionAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vredsum.vs v1, v2, v3", 0x0221a0d7},       {"vredand.vs v1, v2, v3, v0.t", 0x0421a0d7},
        {"vredor.vs v1, v2, v3", 0x0a21a0d7},        {"vredxor.vs v1, v2, v3", 0x0e21a0d7},
        {"vredminu.vs v1, v2, v3", 0x1221a0d7},      {"vredmin.vs v1, v2, v3", 0x1621a0d7},
        {"vredmaxu.vs v1, v2, v3", 0x1a21a0d7},      {"vredmax.vs v1, v2, v3", 0x1e21a0d7},
        {"vwredsumu.vs v1, v2, v3", 0xc22180d7},     {"vwredsum.vs v1, v2, v3, v0.t", 0xc42180d7},
        {"vredsum.vs v0, v2, v3, v0.t", 0x0021a057}, {"vredsum.vs v2, v2, v2", 0x02212157},
        {"vwredsum.vs v2, v2, v3", 0xc6218157},
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
        EXPECT_EQ(Kites::instruction_set::disassemble(program.text_buffer[i]), text);
    }
}

TEST(VectorReductionAssemblerTest, OperandRules)
{
    for (const std::string &line : {"vredsum.vs v1, v2\n", "vredsum.vs v1, v2, a0\n",
                                    "vredsum.vs v1, v2, 3\n", "vwredsum.vs v1, v2, v3, v0\n",
                                    "vredsum.vv v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // Unlike ordinary arithmetic, a reduction may write v0 under v0.t and overlap any source.
    EXPECT_NO_THROW(assembleSource("vredsum.vs v0, v2, v3, v0.t\nvwredsum.vs v2, v2, v3\n"));
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorReductionTest, EveryOperationSignedAndUnsigned)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n"
                                          "vredand.vs v11, v2, v4\n"
                                          "vredor.vs v12, v2, v3\n"
                                          "vredxor.vs v13, v2, v3\n"
                                          "vredminu.vs v14, v2, v4\n"
                                          "vredmin.vs v15, v2, v4\n"
                                          "vredmaxu.vs v16, v2, v3\n"
                                          "vredmax.vs v17, v2, v3\n",
                                          reductionData);
    const RegisterFile &rf = vm->registers_;
    // 5 + 241 + 243 + 127 + 245 = 861 = 0x35D, truncated to 8 bits.
    EXPECT_EQ(readElements(rf, 10, 8, 4), (Elements{0x5D, 0x99, 0x99, 0x99}));
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0x71}));
    EXPECT_EQ(readElements(rf, 12, 8, 1), (Elements{0xFF}));
    EXPECT_EQ(readElements(rf, 13, 8, 1), (Elements{0x8D}));
    // vs1 = 0xFF: the largest unsigned value but -1 signed.
    EXPECT_EQ(readElements(rf, 14, 8, 1), (Elements{0x7F})); // unsigned min of 255, 241, .., 127
    EXPECT_EQ(readElements(rf, 15, 8, 1), (Elements{0xF1})); // signed min: -15
    EXPECT_EQ(readElements(rf, 16, 8, 1), (Elements{0xF5})); // unsigned max: 245
    EXPECT_EQ(readElements(rf, 17, 8, 1), (Elements{0x7F})); // signed max: 127
    // Only element 0 is written; the rest of vd is tail.
    EXPECT_EQ(readElements(rf, 17, 8, 4), (Elements{0x7F, 0x99, 0x99, 0x99}));
}

TEST(VectorReductionTest, AllOnesVs1ForMax)
{
    // vs1[0] = 0xFF: the largest unsigned value, but -1 signed.
    // v2 = [0xF1, 0xF3, 0x7F, 0xF5] has a positive element (127); v5 is all negative.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredmaxu.vs v10, v2, v4\n"
                                          "vredmax.vs  v11, v2, v4\n"
                                          "vredmax.vs  v12, v5, v4\n",
                                          [](RegisterFile &rf)
                                          {
                                              reductionData(rf);
                                              writeElements(rf, 5, 8, {0xF1, 0xF3, 0x80, 0xF5});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 1), (Elements{0xFF})); // 255 beats every element
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0x7F})); // -1 loses to 127
    EXPECT_EQ(readElements(rf, 12, 8, 1), (Elements{0xFF})); // -1 beats all-negative elements
}

TEST(VectorReductionTest, MaskedOffElementsAreLeftOut)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3, v0.t\n"
                                          "vredmaxu.vs v11, v2, v3, v0.t\n"
                                          "vredsum.vs v0, v2, v3, v0.t\n", // v0 may be vd
                                          [](RegisterFile &rf)
                                          {
                                              reductionData(rf);
                                              writeElements(rf, 0, 8, {0b0101}); // 0 and 2
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 1), (Elements{0x75})); // 5 + 241 + 127 = 373
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0xF1}));
    EXPECT_EQ(readElements(rf, 0, 8, 1), (Elements{0x75}));
}

TEST(VectorReductionTest, Vs2IsAnLmulGroup)
{
    Elements data;
    for (uint64_t i = 1; i <= 20; ++i)
    {
        data.push_back(i);
    }
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 20\n"
                                          "vsetvli x0, x5, e8, m2, ta, ma\n"
                                          "vredsum.vs v8, v4, v1\n"
                                          "vredmaxu.vs v9, v4, v1\n",
                                          [&](RegisterFile &rf) { writeElements(rf, 4, 8, data); });
    EXPECT_EQ(readElements(vm->registers_, 8, 8, 1), (Elements{210})); // 1 + 2 + ... + 20
    EXPECT_EQ(readElements(vm->registers_, 9, 8, 1), (Elements{20}));
}

TEST(VectorReductionTest, WideningSumsAreTwiceSew)
{
    // vs2 e8 [0xFF, 0xFF, 0x80, 0x01]; vs1[0] e16 = 1000.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vwredsumu.vs v10, v2, v3\n"
                                          "vwredsum.vs v11, v2, v3\n"
                                          "vwredsum.vs v2, v2, v3\n", // vd may overlap vs2
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 0xFF, 0x80, 0x01});
                                              writeElements(rf, 3, 16, {1000});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 16, 1), (Elements{1639})); // 1000 + 255 + 255 + 128 + 1
    EXPECT_EQ(readElements(rf, 11, 16, 1), (Elements{871}));  // 1000 - 1 - 1 - 128 + 1
    EXPECT_EQ(readElements(rf, 2, 16, 1), (Elements{871}));
}

TEST(VectorReductionTest, ZeroVlWritesNothing)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 0\n"
                                          "vsetvli x0, x5, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n",
                                          reductionData);
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 4), Elements(4, 0x99));
}

TEST(VectorReductionTest, IllegalConfigurationsTrap)
{
    expectIllegal("vredsum.vs v1, v2, v3\n");                                  // vtype.vill
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwredsum.vs v1, v2, v3\n"); // 2*SEW > ELEN
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvredsum.vs v1, v3, v4\n");  // misaligned vs2

    // A non-zero vstart is illegal for a reduction.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n", reductionData);
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 1);
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x0221a0d7, 0, 0, vm->registers_,
                                                      vm->memory_controller_, changes,
                                                      memoryChanges),
                 std::runtime_error);
    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 0);
    EXPECT_NO_THROW(Kites::rvv::ExecuteVectorInstruction(0x0221a0d7, 0, 0, vm->registers_,
                                                         vm->memory_controller_, changes,
                                                         memoryChanges));
}

TEST(VectorReductionTest, UndoRestoresDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n",
                                          reductionData);
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x5D}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x99}));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x5D}));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kReductionProgram = "li x10, 3\n"
                                "nop\nnop\nnop\n"
                                "vsetivli x0, 4, e32, m1, ta, ma\n"
                                "vmv.v.x v2, x10\n"             // [3, 3, 3, 3]
                                "vmv.v.i v3, 1\n"               // vs1[0] = 1
                                "vredsum.vs v4, v2, v3\n"       // 1 + 12 = 13
                                "vredmax.vs v5, v2, v3\n"       // 3
                                "vmv.v.i v6, -1\n"
                                "vredand.vs v7, v6, v6\n"       // 0xFFFFFFFF
                                "vwredsumu.vs v8, v2, v9\n"     // e64: 0 + 12
                                "vredsum.vs v0, v2, v3\n"       // 13, into v0
                                "nop\nnop\nnop\n";

template <typename Vm> void expectReductionResult()
{
    auto vm = runOn<Vm>(kReductionProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 32, 1), (Elements{13}));
    EXPECT_EQ(readElements(rf, 5, 32, 1), (Elements{3}));
    EXPECT_EQ(readElements(rf, 7, 32, 1), (Elements{0xFFFFFFFF}));
    EXPECT_EQ(readElements(rf, 8, 64, 1), (Elements{12}));
    EXPECT_EQ(readElements(rf, 0, 32, 1), (Elements{13}));
}
} // namespace

TEST(VectorReductionPipelineTest, ProgramOnEveryVariant)
{
    expectReductionResult<Kites::RVSSProcessor>();
    expectReductionResult<Kites::RV5StageProcessorHF>();
    expectReductionResult<Kites::RV5StageProcessorHNF>();
    expectReductionResult<Kites::RV5StageProcessorNHF>();
    expectReductionResult<Kites::RV5StageProcessorNHNF>();
}
