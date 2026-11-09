// Tests for RVV widening integer add/subtract (11.2), integer extension (11.3) and bitwise
// logical (11.5) instructions. Encodings are checked against llvm-mc
// (-triple=riscv64 -mattr=+zve64x -show-encoding); legality checks follow Spike.

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

// Runs a single-cycle program that is expected to trap as an illegal instruction.
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

TEST(VectorWidenExtLogicAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vwaddu.vv v2, v4, v5", 0xc242a157},
        {"vwaddu.vx v2, v4, a0", 0xc2456157},
        {"vwsubu.vv v2, v4, v5, v0.t", 0xc842a157},
        {"vwsubu.vx v2, v4, a0", 0xca456157},
        {"vwadd.vv v8, v10, v11", 0xc6a5a457},
        {"vwadd.vx v8, v10, t1, v0.t", 0xc4a36457},
        {"vwsub.vv v8, v10, v11", 0xcea5a457},
        {"vwsub.vx v8, v10, t1", 0xcea36457},
        {"vwaddu.wv v2, v4, v6", 0xd2432157},
        {"vwaddu.wx v2, v4, a0", 0xd2456157},
        {"vwsubu.wv v2, v4, v6", 0xda432157},
        {"vwsubu.wx v2, v4, a0, v0.t", 0xd8456157},
        {"vwadd.wv v2, v4, v6", 0xd6432157},
        {"vwadd.wx v2, v4, a0", 0xd6456157},
        {"vwsub.wv v2, v4, v6, v0.t", 0xdc432157},
        {"vwsub.wx v2, v4, a0", 0xde456157},
        {"vzext.vf2 v2, v4", 0x4a432157},
        {"vzext.vf4 v2, v4, v0.t", 0x48422157},
        {"vzext.vf8 v2, v4", 0x4a412157},
        {"vsext.vf2 v2, v4", 0x4a43a157},
        {"vsext.vf4 v2, v4", 0x4a42a157},
        {"vsext.vf8 v2, v4, v0.t", 0x4841a157},
        {"vand.vv v1, v2, v3", 0x262180d7},
        {"vand.vx v1, v2, a0", 0x262540d7},
        {"vand.vi v1, v2, -16", 0x262830d7},
        {"vor.vv v1, v2, v3, v0.t", 0x282180d7},
        {"vor.vx v1, v2, a0", 0x2a2540d7},
        {"vor.vi v1, v2, 15", 0x2a27b0d7},
        {"vxor.vv v1, v2, v3", 0x2e2180d7},
        {"vxor.vx v1, v2, a0, v0.t", 0x2c2540d7},
        {"vxor.vi v1, v2, -1", 0x2e2fb0d7}, // llvm-mc prints this as its alias vnot.v
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
        // The assembler's encoding table and the executor's decode table must agree.
        const Kites::rvv::VectorArithInfo *info =
            Kites::rvv::DecodeVectorArith(program.text_buffer[i]);
        ASSERT_NE(info, nullptr) << text;
        EXPECT_EQ(text.substr(0, text.find(' ')), info->mnemonic);
    }
}

TEST(VectorWidenExtLogicAssemblerTest, DisassemblyRoundTrips)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x4a432157), "vzext.vf2 v2, v4");
    EXPECT_EQ(disassemble(0x4841a157), "vsext.vf8 v2, v4, v0.t");
    EXPECT_EQ(disassemble(0xc4a36457), "vwadd.vx v8, v10, x6, v0.t");
    EXPECT_EQ(disassemble(0xd2432157), "vwaddu.wv v2, v4, v6");
    EXPECT_EQ(disassemble(0x262830d7), "vand.vi v1, v2, -16");
    EXPECT_EQ(disassemble(0x2e2fb0d7), "vxor.vi v1, v2, -1");
}

TEST(VectorWidenExtLogicAssemblerTest, IllegalFormsAreRejected)
{
    const std::vector<std::string> bad = {
        "vwadd.vv v2, v2, v5\n",       // vd is a narrower source (any LMUL)
        "vwadd.vv v2, v4, v2\n",
        "vwaddu.vx v4, v4, a0\n",
        "vwadd.wv v2, v4, v2\n",       // vs1 is narrower than vd
        "vzext.vf2 v2, v2\n",          // vd == vs2 is always illegal for extensions
        "vzext.vf2 v0, v2, v0.t\n",    // masked op writing v0
        "vwadd.vv v0, v2, v4, v0.t\n",
        "vzext.vf2 v2, v4, v6\n",      // extensions take two operands
        "vzext.vf2 v2, a0\n",
        "vwadd.vi v2, v4, 3\n",        // no .vi form of widening add
        "vwadd.wi v2, v4, 3\n",
        "vand.vi v1, v2, 16\n",        // simm5 range
        "vxor.vi v1, v2, -17\n",
    };
    for (const std::string &line : bad)
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // .wv may write its wide source: same element width.
    EXPECT_NO_THROW(assembleSource("vwadd.wv v2, v2, v6\n"));
}

// ---------------------------------------------------------------------------------------------
// Widening add/subtract
// ---------------------------------------------------------------------------------------------

TEST(VectorWideningTest, VvUnsignedAndSignedAtSew8)
{
    // e8 sources, e16 results in the v2/v3 group.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vwaddu.vv v2, v4, v5\n"
                                          "vwadd.vv v6, v4, v5\n"
                                          "vwsubu.vv v8, v5, v4\n"
                                          "vwsub.vv v10, v5, v4\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 8, {0xFF, 0x80});
                                              writeElements(rf, 5, 8, {0xFF, 0x02});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 2), (Elements{0x01FE, 0x0082}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 2), (Elements{0xFFFE, 0xFF82})); // -2, -126
    EXPECT_EQ(readElements(vm->registers_, 8, 16, 2), (Elements{0x0000, 0xFF82}));  // 2-128
    EXPECT_EQ(readElements(vm->registers_, 10, 16, 2), (Elements{0x0000, 0x0082})); // 2-(-128)
}

TEST(VectorWideningTest, VxTruncatesThenExtendsTheScalar)
{
    // x10 = 0x1FF: at e8 it is 0xFF, i.e. 255 unsigned or -1 signed.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0x1FF\n"
                                          "vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vwaddu.vx v2, v4, x10\n"
                                          "vwadd.vx v6, v4, x10\n"
                                          "vwsubu.vx v8, v4, x10\n"
                                          "vwsub.vx v10, v4, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 4, 8, {0x01, 0x80}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 2), (Elements{0x0100, 0x017F}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 2), (Elements{0x0000, 0xFF7F}));
    EXPECT_EQ(readElements(vm->registers_, 8, 16, 2), (Elements{0xFF02, 0xFF81}));
    EXPECT_EQ(readElements(vm->registers_, 10, 16, 2), (Elements{0x0002, 0xFF81}));
}

TEST(VectorWideningTest, WvAndWxUseAWideVs2)
{
    // vs2 is already e16; only vs1 / rs1 is extended from e8.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0xFF\n"
                                          "vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vwaddu.wv v2, v4, v6\n"
                                          "vwadd.wv v8, v4, v6\n"
                                          "vwsubu.wx v10, v4, x10\n"
                                          "vwsub.wx v12, v4, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 16, {0xFFFF, 0x0100});
                                              writeElements(rf, 6, 8, {0x01, 0xFF});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 2), (Elements{0x0000, 0x01FF}));
    EXPECT_EQ(readElements(vm->registers_, 8, 16, 2), (Elements{0x0000, 0x00FF}));
    EXPECT_EQ(readElements(vm->registers_, 10, 16, 2), (Elements{0xFF00, 0x0001}));
    EXPECT_EQ(readElements(vm->registers_, 12, 16, 2), (Elements{0x0000, 0x0101}));
}

TEST(VectorWideningTest, Sew32WidensTo64)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 2, e32, m1, ta, ma\n"
                                          "vwadd.vx v2, v4, x10\n"
                                          "vwaddu.vx v6, v4, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 4, 32, {0x80000000, 5}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 64, 2), (Elements{0xFFFFFFFF7FFFFFFF, 4}));
    EXPECT_EQ(readElements(vm->registers_, 6, 64, 2), (Elements{0x17FFFFFFF, 0x100000004}));
}

TEST(VectorWideningTest, Lmul2DestinationSpansFourRegisters)
{
    // e16, m2: VLMAX 16. Sources are v8/v9, the e32 result is the v4..v7 group.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 1\n"
                                          "vsetvli x5, x0, e16, m2, ta, ma\n"
                                          "vwaddu.vx v4, v8, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              Elements values;
                                              for (uint64_t i = 0; i < 16; ++i)
                                              {
                                                  values.push_back(i * 0x1000);
                                              }
                                              writeElements(rf, 8, 16, values);
                                          });
    EXPECT_EQ(vm->registers_.ReadGpr(5), 16u);
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{1, 0x1001, 0x2001, 0x3001}));
    EXPECT_EQ(readElements(vm->registers_, 7, 32, 4), (Elements{0xC001, 0xD001, 0xE001, 0xF001}));
}

TEST(VectorWideningTest, MaskedOffAndTailElementsAreUndisturbed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwaddu.vv v2, v4, v5, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b101});
                                              writeElements(rf, 2, 16, {7, 7, 7, 7});
                                              writeElements(rf, 4, 8, {1, 2, 3, 4});
                                              writeElements(rf, 5, 8, {10, 20, 30, 40});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 4), (Elements{11, 7, 33, 7}));
}

TEST(VectorWideningTest, IllegalConfigurationsTrap)
{
    // 2*SEW > ELEN, and EMUL = 2*LMUL > 8.
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwadd.vv v2, v4, v5\n");
    expectIllegal("vsetvli x0, x0, e8, m8, ta, ma\nvwadd.vv v16, v8, v0\n");
    // The destination group (2 registers at LMUL=1) must be even.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvwadd.vv v3, v4, v5\n");
    // .wv: vs2 is also a 2-register group.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvwadd.wv v2, v5, v6\n");
}

TEST(VectorWideningTest, SourceMayOverlapTheTopOfTheDestination)
{
    // At LMUL >= 1 a narrow source may sit in the highest-numbered part of the destination group.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vwaddu.vv v2, v3, v5\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 3, 8, {1, 2});
                                              writeElements(rf, 5, 8, {10, 20});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 2), (Elements{11, 22}));
}

TEST(VectorWideningTest, UndoRestoresTheWholeDestinationGroup)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 16, e8, m1, ta, ma\n"
                                          "vwaddu.vv v2, v4, v5\n",
                                          [](RegisterFile &rf)
                                          {
                                              for (uint64_t i = 0; i < 16; ++i)
                                              {
                                                  rf.WriteVectorElement(4, i, 8, i + 1);
                                              }
                                          });
    EXPECT_EQ(vm->registers_.ReadVectorElement(3, 7, 16), 16u); // last element, in v3

    vm->Undo();
    EXPECT_EQ(vm->registers_.ReadVectorRegister(2),
              std::vector<uint8_t>(vm->registers_.GetVlenBytes(), 0));
    EXPECT_EQ(vm->registers_.ReadVectorRegister(3),
              std::vector<uint8_t>(vm->registers_.GetVlenBytes(), 0));
    vm->Redo();
    EXPECT_EQ(vm->registers_.ReadVectorElement(3, 7, 16), 16u);
}

// ---------------------------------------------------------------------------------------------
// Integer extension
// ---------------------------------------------------------------------------------------------

TEST(VectorExtensionTest, ZeroAndSignExtendByEachFactor)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e16, m1, ta, ma\n"
                                          "vzext.vf2 v2, v1\n"
                                          "vsext.vf2 v3, v1\n"
                                          "vsetivli x0, 2, e32, m1, ta, ma\n"
                                          "vzext.vf4 v4, v1\n"
                                          "vsext.vf4 v5, v1\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vzext.vf8 v6, v1\n"
                                          "vsext.vf8 v7, v1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 8, {0x80, 0x7F}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 2), (Elements{0x0080, 0x007F}));
    EXPECT_EQ(readElements(vm->registers_, 3, 16, 2), (Elements{0xFF80, 0x007F}));
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 2), (Elements{0x80, 0x7F}));
    EXPECT_EQ(readElements(vm->registers_, 5, 32, 2), (Elements{0xFFFFFF80, 0x7F}));
    EXPECT_EQ(readElements(vm->registers_, 6, 64, 2), (Elements{0x80, 0x7F}));
    EXPECT_EQ(readElements(vm->registers_, 7, 64, 2), (Elements{0xFFFFFFFFFFFFFF80, 0x7F}));
}

TEST(VectorExtensionTest, MaskedAndTailElementsAreUndisturbed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vsext.vf2 v2, v4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b110});
                                              writeElements(rf, 2, 32, {9, 9, 9, 9});
                                              writeElements(rf, 4, 16, {0x8000, 0x8000, 0x0001});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{9, 0xFFFF8000, 1, 9}));
}

TEST(VectorExtensionTest, IllegalConfigurationsTrap)
{
    // The source EEW SEW/factor must be at least 8.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvzext.vf2 v2, v4\n");
    expectIllegal("vsetvli x0, x0, e32, m1, ta, ma\nvsext.vf8 v2, v4\n");
    // e32, m2: vd is v2/v3; a vf4 source (EMUL 1/2) can't overlap it at all.
    expectIllegal("vsetvli x0, x0, e32, m2, ta, ma\nvzext.vf4 v2, v3\n");
    // vd must be aligned to LMUL.
    expectIllegal("vsetvli x0, x0, e32, m2, ta, ma\nvzext.vf2 v3, v4\n");
}

TEST(VectorExtensionTest, WholeRegisterSourceMayOverlapTheTop)
{
    // e32, m2, vf2: the source EMUL is 1 and v3 is the top half of the v2/v3 destination.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m2, ta, ma\n"
                                          "vsext.vf2 v2, v3\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 3, 16, {1, 0xFFFF, 3, 0x8000}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{1, 0xFFFFFFFF, 3, 0xFFFF8000}));
}

// ---------------------------------------------------------------------------------------------
// Bitwise logical
// ---------------------------------------------------------------------------------------------

TEST(VectorLogicalTest, AllFormsAtSew32)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0xFF\n"
                                          "vsetivli x0, 2, e32, m1, ta, ma\n"
                                          "vand.vv v1, v2, v3\n"
                                          "vand.vx v4, v2, x10\n"
                                          "vand.vi v5, v2, -16\n"
                                          "vor.vv v6, v2, v3\n"
                                          "vor.vx v7, v2, x10\n"
                                          "vor.vi v8, v2, 15\n"
                                          "vxor.vv v9, v2, v3\n"
                                          "vxor.vx v10, v2, x10\n"
                                          "vxor.vi v11, v2, -1\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 32, {0x12345678, 0xF0F0F0F0});
                                              writeElements(rf, 3, 32, {0x0000FFFF, 0x0F0F0F0F});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 1, 32, 2), (Elements{0x00005678, 0x00000000}));
    EXPECT_EQ(readElements(rf, 4, 32, 2), (Elements{0x00000078, 0x000000F0}));
    EXPECT_EQ(readElements(rf, 5, 32, 2), (Elements{0x12345670, 0xF0F0F0F0}));
    EXPECT_EQ(readElements(rf, 6, 32, 2), (Elements{0x1234FFFF, 0xFFFFFFFF}));
    EXPECT_EQ(readElements(rf, 7, 32, 2), (Elements{0x123456FF, 0xF0F0F0FF}));
    EXPECT_EQ(readElements(rf, 8, 32, 2), (Elements{0x1234567F, 0xF0F0F0FF}));
    EXPECT_EQ(readElements(rf, 9, 32, 2), (Elements{0x1234A987, 0xFFFFFFFF}));
    EXPECT_EQ(readElements(rf, 10, 32, 2), (Elements{0x12345687, 0xF0F0F00F}));
    EXPECT_EQ(readElements(rf, 11, 32, 2), (Elements{0xEDCBA987, 0x0F0F0F0F})); // vnot
}

TEST(VectorLogicalTest, ScalarAndImmediateFollowSew)
{
    // At e8, x10 = 0x1F0 acts as 0xF0 and -16 as 0xF0.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0x1F0\n"
                                          "vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vand.vx v1, v2, x10\n"
                                          "vxor.vi v3, v2, -16\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 8, {0xAB, 0x0F}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 3), (Elements{0xA0, 0x00, 0}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 3), (Elements{0x5B, 0xFF, 0}));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
// Vector dependencies are back to back (vector instructions commit in EX, in order); the
// scalar x10 is spaced out so the variants without hazard detection also see it.
const char *kMixedProgram = "vsetivli x0, 4, e8, m1, ta, ma\n"
                            "li x10, -3\n"
                            "nop\nnop\nnop\n"
                            "vadd.vi v1, v0, -2\n"    // e8: 0xFE
                            "vor.vi v4, v1, 1\n"      // 0xFF
                            "vxor.vx v5, v4, x10\n"   // 0xFF ^ 0xFD = 0x02
                            "vwadd.vv v2, v1, v5\n"   // e16: -2 + 2 = 0
                            "vwaddu.vv v6, v1, v5\n"  // e16: 254 + 2 = 0x100
                            "vwsub.wx v8, v6, x10\n"  // e16: 0x100 - (-3) = 0x103
                            "vsetivli x0, 4, e16, m1, ta, ma\n"
                            "vsext.vf2 v10, v1\n"     // e16: 0xFFFE
                            "vand.vv v11, v10, v8\n"  // 0x0102
                            "nop\nnop\nnop\n";

template <typename Vm> void expectMixedResult()
{
    auto vm = runOn<Vm>(kMixedProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 5, 8, 4), Elements(4, 0x02));
    EXPECT_EQ(readElements(rf, 2, 16, 4), Elements(4, 0x0000));
    EXPECT_EQ(readElements(rf, 6, 16, 4), Elements(4, 0x0100));
    EXPECT_EQ(readElements(rf, 8, 16, 4), Elements(4, 0x0103));
    EXPECT_EQ(readElements(rf, 10, 16, 4), Elements(4, 0xFFFE));
    EXPECT_EQ(readElements(rf, 11, 16, 4), Elements(4, 0x0102));
}
} // namespace

TEST(VectorWidenExtLogicPipelineTest, MixedProgramOnEveryVariant)
{
    expectMixedResult<Kites::RVSSProcessor>();
    expectMixedResult<Kites::RV5StageProcessorHF>();
    expectMixedResult<Kites::RV5StageProcessorHNF>();
    expectMixedResult<Kites::RV5StageProcessorNHF>();
    expectMixedResult<Kites::RV5StageProcessorNHNF>();
}
