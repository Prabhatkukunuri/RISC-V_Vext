// Tests for the RVV integer compares (11.8): vmseq, vmsne, vmslt{u}, vmsle{u}, vmsgt{u} and the
// assembler aliases vmsgt{u}.vv, vmsge{u}.vv, vmslt{u}.vi, vmsge{u}.vi.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: the result is a mask (one bit per element, a single register), a masked
// compare may write v0, unsigned .vi compares use the sign-extended simm5 as an unsigned SEW
// value, and the mask may overlap only the first register of a source group.

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

namespace
{
uint64_t maskByte(const RegisterFile &rf, size_t reg, size_t byte = 0)
{
    return rf.ReadVectorElement(reg, byte, 8);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorCompareAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmseq.vv v1, v2, v3", 0x622180d7},        {"vmseq.vx v1, v2, a0, v0.t", 0x602540d7},
        {"vmseq.vi v1, v2, -16", 0x622830d7},       {"vmsne.vv v0, v2, v3, v0.t", 0x64218057},
        {"vmsne.vx v4, v8, t0", 0x6682c257},        {"vmsne.vi v5, v6, 15", 0x6667b2d7},
        {"vmsltu.vv v1, v2, v3", 0x6a2180d7},       {"vmsltu.vx v1, v2, a1", 0x6a25c0d7},
        {"vmslt.vv v1, v2, v3, v0.t", 0x6c2180d7},  {"vmslt.vx v1, v2, a1", 0x6e25c0d7},
        {"vmsleu.vv v1, v2, v3", 0x722180d7},       {"vmsleu.vx v1, v2, a1", 0x7225c0d7},
        {"vmsleu.vi v1, v2, -1", 0x722fb0d7},       {"vmsle.vv v1, v2, v3", 0x762180d7},
        {"vmsle.vx v1, v2, a1", 0x7625c0d7},        {"vmsle.vi v1, v2, 7, v0.t", 0x7423b0d7},
        {"vmsgtu.vx v1, v2, a1", 0x7a25c0d7},       {"vmsgtu.vi v1, v2, 3", 0x7a21b0d7},
        {"vmsgt.vx v1, v2, a1", 0x7e25c0d7},        {"vmsgt.vi v1, v2, -3, v0.t", 0x7c2eb0d7},
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
    EXPECT_EQ(Kites::instruction_set::disassemble(0x722fb0d7), "vmsleu.vi v1, v2, -1");
}

TEST(VectorCompareAssemblerTest, AliasesMatchLlvmMc)
{
    // Each alias assembles to the same word llvm-mc emits for it.
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmsgt.vv v1, v2, v3", 0x6e3100d7},        // vmslt.vv v1, v3, v2
        {"vmsgtu.vv v1, v2, v3, v0.t", 0x683100d7}, // vmsltu.vv v1, v3, v2, v0.t
        {"vmsge.vv v1, v2, v3", 0x763100d7},        // vmsle.vv v1, v3, v2
        {"vmsgeu.vv v1, v2, v3", 0x723100d7},       // vmsleu.vv v1, v3, v2
        {"vmslt.vi v1, v2, 16", 0x7627b0d7},        // vmsle.vi v1, v2, 15
        {"vmslt.vi v1, v2, -15", 0x762830d7},       // vmsle.vi v1, v2, -16
        {"vmsltu.vi v1, v2, 16", 0x7227b0d7},       // vmsleu.vi v1, v2, 15
        {"vmsltu.vi v1, v2, 0", 0x662100d7},        // vmsne.vv v1, v2, v2
        {"vmsltu.vi v1, v2, -15", 0x722830d7},      // vmsleu.vi v1, v2, -16
        {"vmsge.vi v1, v2, 16", 0x7e27b0d7},        // vmsgt.vi v1, v2, 15
        {"vmsge.vi v1, v2, -15, v0.t", 0x7c2830d7}, // vmsgt.vi v1, v2, -16, v0.t
        {"vmsgeu.vi v1, v2, 0", 0x622100d7},        // vmseq.vv v1, v2, v2
        {"vmsgeu.vi v1, v2, 5", 0x7a2230d7},        // vmsgtu.vi v1, v2, 4
    };
    for (const auto &[text, word] : cases)
    {
        const Kites::AssembledProgram program = assembleSource(text + "\n");
        ASSERT_EQ(program.text_buffer.size(), 1u) << text;
        EXPECT_EQ(program.text_buffer[0], word) << text;
    }
}

TEST(VectorCompareAssemblerTest, OperandRules)
{
    for (const std::string &line :
         {"vmseq.vi v1, v2, 16\n", "vmsle.vi v1, v2, -17\n", "vmslt.vi v1, v2, -16\n",
          "vmslt.vi v1, v2, 17\n", "vmsltu.vi v1, v2, 17\n", "vmsgeu.vi v1, v2, -16\n",
          "vmslt.vi v1, v2, a0\n", "vmsltu.vv v1, v2, a0\n",
          // Not in the ISA, and the expansions need the mask-logical instructions.
          "vmsge.vx v1, v2, a0\n", "vmsltu.vi.x v1, v2, 3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // A compare may write its mask into v0 even when masked by v0.
    EXPECT_NO_THROW(assembleSource("vmslt.vv v0, v2, v3, v0.t\nvmslt.vi v0, v2, 3, v0.t\n"));
    // ...but other instructions still may not.
    EXPECT_THROW(assembleSource("vadd.vv v0, v2, v3, v0.t\n"), std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

namespace
{
// e8 operands: vs2 signed [0, 1, 5, 127, -128, -1, 5, 6], vs1 signed [0, 2, 5, -128, 127, -1, 4, 7].
void compareData(RegisterFile &rf)
{
    writeElements(rf, 2, 8, {0x00, 0x01, 0x05, 0x7F, 0x80, 0xFF, 0x05, 0x06});
    writeElements(rf, 3, 8, {0x00, 0x02, 0x05, 0x80, 0x7F, 0xFF, 0x04, 0x07});
}
} // namespace

TEST(VectorCompareTest, EveryCompareSignedAndUnsigned)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 5\n"
                                          "vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmseq.vv v4, v2, v3\n"
                                          "vmsne.vv v5, v2, v3\n"
                                          "vmsltu.vv v6, v2, v3\n"
                                          "vmslt.vv v7, v2, v3\n"
                                          "vmsleu.vv v8, v2, v3\n"
                                          "vmsle.vv v9, v2, v3\n"
                                          "vmseq.vx v10, v2, x10\n"
                                          "vmslt.vx v11, v2, x10\n"
                                          "vmsltu.vx v12, v2, x10\n"
                                          "vmsle.vi v13, v2, 5\n"
                                          "vmsleu.vi v14, v2, 5\n"
                                          "vmsgt.vx v15, v2, x10\n"
                                          "vmsgtu.vi v16, v2, 5\n"
                                          "vmsne.vi v17, v2, 5\n",
                                          compareData);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x25u);  // equal at 0, 2, 5
    EXPECT_EQ(maskByte(rf, 5), 0xDAu);
    EXPECT_EQ(maskByte(rf, 6), 0x8Au);  // unsigned: 127 < 128 at element 3
    EXPECT_EQ(maskByte(rf, 7), 0x92u);  // signed: -128 < 127 at element 4
    EXPECT_EQ(maskByte(rf, 8), 0xAFu);
    EXPECT_EQ(maskByte(rf, 9), 0xB7u);
    EXPECT_EQ(maskByte(rf, 10), 0x44u);
    EXPECT_EQ(maskByte(rf, 11), 0x33u); // -128 and -1 are below 5 signed...
    EXPECT_EQ(maskByte(rf, 12), 0x03u); // ...but not unsigned
    EXPECT_EQ(maskByte(rf, 13), 0x77u);
    EXPECT_EQ(maskByte(rf, 14), 0x47u);
    EXPECT_EQ(maskByte(rf, 15), 0x88u);
    EXPECT_EQ(maskByte(rf, 16), 0xB8u);
    EXPECT_EQ(maskByte(rf, 17), 0xBBu);
    // Only the 8 body bits are written; the rest of the register is untouched.
    EXPECT_EQ(maskByte(rf, 5, 1), 0u);
}

TEST(VectorCompareTest, AllOnesScalarIsMaxUnsignedAndMinusOneSigned)
{
    // x10 = -1 (all 64 bits set). At e8 it acts as 0xFF: 255 unsigned, -1 signed.
    // v2 = [0, 0x7F, 0x80, 0xFF] = unsigned [0, 127, 128, 255] = signed [0, 127, -128, -1].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmseq.vx  v4, v2, x10\n"
                                          "vmsne.vx  v5, v2, x10\n"
                                          "vmsltu.vx v6, v2, x10\n"
                                          "vmslt.vx  v7, v2, x10\n"
                                          "vmsleu.vx v8, v2, x10\n"
                                          "vmsle.vx  v9, v2, x10\n"
                                          "vmsgtu.vx v10, v2, x10\n"
                                          "vmsgt.vx  v11, v2, x10\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmseq.vx  v12, v3, x10\n"
                                          "vmsltu.vx v13, v3, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0x00, 0x7F, 0x80, 0xFF});
                                              writeElements(rf, 3, 64, {~0ULL, 5});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x08u);  // == : only 0xFF
    EXPECT_EQ(maskByte(rf, 5), 0x07u);  // !=
    EXPECT_EQ(maskByte(rf, 6), 0x07u);  // <u 255: everything but 0xFF
    EXPECT_EQ(maskByte(rf, 7), 0x04u);  // <s -1: only -128
    EXPECT_EQ(maskByte(rf, 8), 0x0Fu);  // <=u 255: always
    EXPECT_EQ(maskByte(rf, 9), 0x0Cu);  // <=s -1: -128 and -1
    EXPECT_EQ(maskByte(rf, 10), 0x00u); // >u 255: never
    EXPECT_EQ(maskByte(rf, 11), 0x03u); // >s -1: 0 and 127
    // At e64 the scalar is used whole: 2^64 - 1.
    EXPECT_EQ(maskByte(rf, 12), 0x01u);
    EXPECT_EQ(maskByte(rf, 13), 0x02u);
}

TEST(VectorCompareTest, UnsignedImmediateIsSignExtendedThenUnsigned)
{
    // -1 is 0xFF at e8 and 0xFFFFFFFFFFFFFFFF at e64; -2 is 0xFE / 0x...FE.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmsleu.vi v4, v2, -1\n"
                                          "vmsgtu.vi v5, v2, -2\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmsgtu.vi v6, v7, -2\n",
                                          [](RegisterFile &rf)
                                          {
                                              compareData(rf);
                                              writeElements(rf, 7, 64, {~0ULL, ~0ULL - 1});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 4), 0xFFu);
    EXPECT_EQ(maskByte(vm->registers_, 5), 0x20u); // only 0xFF (element 5)
    EXPECT_EQ(maskByte(vm->registers_, 6), 0x01u);
}

TEST(VectorCompareTest, AliasesBehaveAsNamed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmsgt.vv v4, v2, v3\n"
                                          "vmsge.vv v5, v2, v3\n"
                                          "vmslt.vi v6, v2, 6\n"
                                          "vmsltu.vi v7, v2, 0\n"
                                          "vmsgeu.vi v8, v2, 0\n"
                                          "vmsge.vi v9, v2, 6\n"
                                          "vmsltu.vi v10, v2, 16\n"
                                          "vmsgeu.vi v11, v2, 5\n",
                                          [](RegisterFile &rf)
                                          {
                                              compareData(rf);
                                              writeElements(rf, 7, 8, {0xFF});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x48u);  // signed va > vb at 3 and 6
    EXPECT_EQ(maskByte(rf, 5), 0x6Du);  // NOT (va < vb)
    EXPECT_EQ(maskByte(rf, 6), 0x77u);
    EXPECT_EQ(maskByte(rf, 7), 0x00u);  // va <u 0: never
    EXPECT_EQ(maskByte(rf, 8), 0xFFu);  // va >=u 0: always
    EXPECT_EQ(maskByte(rf, 9), 0x88u);
    EXPECT_EQ(maskByte(rf, 10), 0xC7u);
    EXPECT_EQ(maskByte(rf, 11), 0xFCu);
}

TEST(VectorCompareTest, MaskedCompareIntoV0AndsTheConditions)
{
    // (a < b) && (b < c), the spec's two-instruction idiom.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, mu\n"
                                          "vmslt.vv v0, v2, v3\n"
                                          "vmslt.vv v0, v3, v4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {1, 5, 2, 9});
                                              writeElements(rf, 3, 8, {3, 4, 6, 10});
                                              writeElements(rf, 4, 8, {5, 5, 5, 11});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 0), 0x09u);
}

TEST(VectorCompareTest, MaskedOffAndTailBitsUndisturbedAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmseq.vi v5, v2, 0, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0, 0, 1, 1});
                                              writeElements(rf, 5, 8, {0xFF, 0xFF});
                                          });
    // Bit 0 = 1, bit 2 = 0; masked-off bits 1, 3 and tail bits 4.. keep their 1s.
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFBu);
    EXPECT_EQ(maskByte(vm->registers_, 5, 1), 0xFFu);
    vm->Undo();
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFFu);
    vm->Redo();
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFBu);
}

TEST(VectorCompareTest, InPlaceOverAnLmul2Source)
{
    // vd == vs2 at LMUL = 2: the mask lands in v4 while the 32 elements of v4-v5 are read.
    Elements data;
    for (int i = 0; i < 32; ++i)
    {
        data.push_back(i % 3 == 0 ? 1 : 2);
    }
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 32\n"
                                          "vsetvli x0, x5, e8, m2, ta, ma\n"
                                          "vmseq.vi v4, v4, 1\n",
                                          [&](RegisterFile &rf) { writeElements(rf, 4, 8, data); });
    uint64_t expected = 0;
    for (int i = 0; i < 32; i += 3)
    {
        expected |= 1ULL << i;
    }
    EXPECT_EQ(vm->registers_.ReadVectorElement(4, 0, 32), expected);
    // Bytes 4..15 of v4 are past the 32 mask bits: the original data.
    for (size_t i = 4; i < 16; ++i)
    {
        EXPECT_EQ(maskByte(vm->registers_, 4, i), data[i]) << i;
    }
}

TEST(VectorCompareTest, IllegalOverlapsAndConfigurationsTrap)
{
    // vd inside the vs2 group but not at its start.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vv v5, v4, v8\n");
    // vd inside the vs1 group but not at its start.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vv v9, v2, v8\n");
    // vs2 must be aligned to its group.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vi v1, v3, 0\n");
    // vtype.vill (no vsetvli yet).
    expectIllegal("vmseq.vi v1, v2, 0\n");
    // vd at the start of a group, and anywhere at all outside it, is fine.
    EXPECT_NO_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e8, m2, ta, ma\n"
                                                "vmseq.vv v4, v4, v8\n"
                                                "vmseq.vv v8, v4, v8\n"
                                                "vmseq.vv v1, v4, v8\n"));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kCompareProgram = "li x10, 3\n"
                              "nop\nnop\nnop\n"
                              "vsetivli x0, 4, e32, m1, ta, ma\n"
                              "vadd.vi v2, v0, 1\n"           // [1, 1, 1, 1]
                              "vsetivli x0, 2, e32, m1, ta, ma\n"
                              "vadd.vi v2, v2, 5\n"           // [6, 6, 1, 1]
                              "vsetivli x0, 4, e32, m1, ta, ma\n"
                              "vmslt.vx v4, v2, x10\n"        // 0b1100
                              "vmsgt.vi v5, v2, 3\n"          // 0b0011
                              "vmsge.vv v6, v2, v2\n"         // 0b1111
                              "vmseq.vi v0, v2, 6\n"          // 0b0011
                              "vadd.vi v7, v2, 10, v0.t\n"    // [16, 16, 0, 0]
                              "nop\nnop\nnop\n";

template <typename Vm> void expectCompareResult()
{
    auto vm = runOn<Vm>(kCompareProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x0Cu);
    EXPECT_EQ(maskByte(rf, 5), 0x03u);
    EXPECT_EQ(maskByte(rf, 6), 0x0Fu);
    EXPECT_EQ(maskByte(rf, 0), 0x03u);
    EXPECT_EQ(readElements(rf, 7, 32, 4), (Elements{16, 16, 0, 0}));
}
} // namespace

TEST(VectorComparePipelineTest, CompareProgramOnEveryVariant)
{
    expectCompareResult<Kites::RVSSProcessor>();
    expectCompareResult<Kites::RV5StageProcessorHF>();
    expectCompareResult<Kites::RV5StageProcessorHNF>();
    expectCompareResult<Kites::RV5StageProcessorNHF>();
    expectCompareResult<Kites::RV5StageProcessorNHNF>();
}
