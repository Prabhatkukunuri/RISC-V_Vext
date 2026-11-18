// Tests for the RVV 1.0 permutation instructions (spec section 16): vmv.x.s, vmv.s.x, the slides,
// vrgather / vrgatherei16, vcompress and the whole-register moves vmv<nr>r.v.
// Encodings are built from the MATCH constants of the riscv-opcodes tables (encoding.h).
// Rules under test: vmv.x.s sign-extends and runs even with vl = 0; vmv.s.x writes element 0
// only when vstart < vl; slideup leaves elements below OFFSET alone; slidedown and vrgather read 0
// past VLMAX; OFFSET and the .vx index are unsigned XLEN values (not cut to SEW); slide1up/down
// insert the low SEW bits of x[rs1]; vcompress needs vstart = 0; vmv<nr>r.v ignores vl and
// vtype; and slide-ups, vrgather and vcompress may not overlap their sources.

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
using Rvss = Kites::RVSSProcessor;

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
    EXPECT_THROW(runOn<Rvss>(source, setup), std::runtime_error) << source;
}

void expectRejected(const std::string &line)
{
    EXPECT_ANY_THROW(assembleSource(line + "\n")) << line;
}

// OP-V fields: funct6 | vm | vs2 | vs1/rs1/imm | funct3 | vd | opcode.
uint32_t fields(uint32_t vd, uint32_t vs2, uint32_t field19_15, bool vm = true)
{
    return (vm ? 1u << 25 : 0u) | (vs2 << 20) | (field19_15 << 15) | (vd << 7);
}

// MATCH_* values from riscv-opcodes (the bits fixed by the opcode, vm excluded unless fixed).
constexpr uint32_t MATCH_VMV_X_S = 0x42002057;
constexpr uint32_t MATCH_VMV_S_X = 0x42006057;
constexpr uint32_t MATCH_VSLIDEUP_VX = 0x38004057;
constexpr uint32_t MATCH_VSLIDEUP_VI = 0x38003057;
constexpr uint32_t MATCH_VSLIDEDOWN_VX = 0x3c004057;
constexpr uint32_t MATCH_VSLIDEDOWN_VI = 0x3c003057;
constexpr uint32_t MATCH_VSLIDE1UP_VX = 0x38006057;
constexpr uint32_t MATCH_VSLIDE1DOWN_VX = 0x3c006057;
constexpr uint32_t MATCH_VRGATHER_VV = 0x30000057;
constexpr uint32_t MATCH_VRGATHER_VX = 0x30004057;
constexpr uint32_t MATCH_VRGATHER_VI = 0x30003057;
constexpr uint32_t MATCH_VRGATHEREI16_VV = 0x38000057;
constexpr uint32_t MATCH_VCOMPRESS_VM = 0x5e002057;
constexpr uint32_t MATCH_VMV1R_V = 0x9e003057;
constexpr uint32_t MATCH_VMV2R_V = 0x9e00b057;
constexpr uint32_t MATCH_VMV4R_V = 0x9e01b057;
constexpr uint32_t MATCH_VMV8R_V = 0x9e03b057;
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorPermuteAssemblerTest, EncodingsMatchRiscvOpcodes)
{
    struct Case
    {
        const char *text;
        uint32_t word;
        const char *disassembly;
    };
    const std::vector<Case> cases = {
        {"vmv.x.s a0, v2", MATCH_VMV_X_S | fields(10, 2, 0), "vmv.x.s x10, v2"},
        {"vmv.s.x v1, a0", MATCH_VMV_S_X | fields(1, 0, 10), "vmv.s.x v1, x10"},
        {"vslideup.vx v1, v2, a0", MATCH_VSLIDEUP_VX | fields(1, 2, 10),
         "vslideup.vx v1, v2, x10"},
        {"vslideup.vi v1, v2, 31, v0.t", MATCH_VSLIDEUP_VI | fields(1, 2, 31, false),
         "vslideup.vi v1, v2, 31, v0.t"},
        {"vslidedown.vx v1, v2, a0, v0.t", MATCH_VSLIDEDOWN_VX | fields(1, 2, 10, false),
         "vslidedown.vx v1, v2, x10, v0.t"},
        {"vslidedown.vi v1, v2, 3", MATCH_VSLIDEDOWN_VI | fields(1, 2, 3),
         "vslidedown.vi v1, v2, 3"},
        {"vslide1up.vx v1, v2, a0", MATCH_VSLIDE1UP_VX | fields(1, 2, 10),
         "vslide1up.vx v1, v2, x10"},
        {"vslide1down.vx v1, v2, a0", MATCH_VSLIDE1DOWN_VX | fields(1, 2, 10),
         "vslide1down.vx v1, v2, x10"},
        {"vrgather.vv v1, v2, v3", MATCH_VRGATHER_VV | fields(1, 2, 3), "vrgather.vv v1, v2, v3"},
        {"vrgather.vx v1, v2, a0, v0.t", MATCH_VRGATHER_VX | fields(1, 2, 10, false),
         "vrgather.vx v1, v2, x10, v0.t"},
        {"vrgather.vi v1, v2, 17", MATCH_VRGATHER_VI | fields(1, 2, 17), "vrgather.vi v1, v2, 17"},
        {"vrgatherei16.vv v1, v2, v3", MATCH_VRGATHEREI16_VV | fields(1, 2, 3),
         "vrgatherei16.vv v1, v2, v3"},
        {"vcompress.vm v1, v2, v3", MATCH_VCOMPRESS_VM | fields(1, 2, 3),
         "vcompress.vm v1, v2, v3"},
        {"vmv1r.v v1, v2", MATCH_VMV1R_V | fields(1, 2, 0), "vmv1r.v v1, v2"},
        {"vmv2r.v v2, v4", MATCH_VMV2R_V | fields(2, 4, 0), "vmv2r.v v2, v4"},
        {"vmv4r.v v4, v8", MATCH_VMV4R_V | fields(4, 8, 0), "vmv4r.v v4, v8"},
        {"vmv8r.v v8, v16", MATCH_VMV8R_V | fields(8, 16, 0), "vmv8r.v v8, v16"},
    };
    std::string source;
    for (const Case &c : cases)
    {
        source += std::string(c.text) + "\n";
    }
    const Kites::AssembledProgram program = assembleSource(source);
    ASSERT_EQ(program.text_buffer.size(), cases.size());
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const uint32_t word = program.text_buffer[i];
        EXPECT_EQ(word, cases[i].word) << cases[i].text;
        EXPECT_TRUE(Kites::rvv::IsSupportedVectorInstruction(word)) << cases[i].text;
        EXPECT_EQ(Kites::instruction_set::disassemble(word), cases[i].disassembly);
    }
    // Only vmv.x.s writes a scalar register.
    EXPECT_TRUE(Kites::rvv::VectorInstructionWritesGpr(MATCH_VMV_X_S | fields(10, 2, 0)));
    EXPECT_FALSE(Kites::rvv::VectorInstructionWritesGpr(MATCH_VMV_S_X | fields(1, 0, 10)));
    // vmv<nr>r.v with a vs1 field other than 0, 1, 3 or 7 is reserved.
    EXPECT_FALSE(Kites::rvv::IsSupportedVectorInstruction(MATCH_VMV1R_V | fields(1, 2, 2)));
}

TEST(VectorPermuteAssemblerTest, OperandRules)
{
    expectRejected("vmv.x.s a0, v2, v0.t");      // never masked
    expectRejected("vmv.x.s v1, v2");            // rd is a scalar register
    expectRejected("vmv.s.x v1, a0, v0.t");
    expectRejected("vcompress.vm v1, v2, v3, v0.t");
    expectRejected("vmv2r.v v2, v4, v0.t");
    expectRejected("vslideup.vi v1, v2, 32");    // uimm is 0..31
    expectRejected("vslideup.vi v1, v2, -1");
    expectRejected("vrgather.vi v1, v2, 32");
    expectRejected("vslideup.vx v1, v1, a0");    // vd may not overlap vs2
    expectRejected("vslide1up.vx v1, v1, a0");
    expectRejected("vrgather.vv v1, v2, v1");    // ... nor vs1
    expectRejected("vrgather.vv v2, v2, v1");
    expectRejected("vcompress.vm v3, v2, v3");
    expectRejected("vslideup.vx v0, v2, a0, v0.t"); // masked op writing v0
    // Slide-downs may overlap their source.
    EXPECT_NO_THROW(assembleSource("vslidedown.vx v1, v1, a0\nvslide1down.vx v1, v1, a0\n"));
}

// ---------------------------------------------------------------------------------------------
// Scalar moves
// ---------------------------------------------------------------------------------------------

TEST(VectorPermuteTest, MoveXSSignExtendsAndIgnoresVl)
{
    auto vm = runOn<Rvss>("vsetivli x0, 0, e8, m1, ta, ma\n" // vl = 0: still runs
                          "vmv.x.s a0, v2\n"
                          "vsetivli x0, 1, e32, m1, ta, ma\n"
                          "vmv.x.s a1, v2\n"
                          "vsetivli x0, 1, e64, m1, ta, ma\n"
                          "vmv.x.s a2, v2\n",
                          [](RegisterFile &rf) { writeElements(rf, 2, 64, {0x123456787FFFFF80}); });
    EXPECT_EQ(vm->registers_.ReadGpr(10), static_cast<uint64_t>(-128));    // 0x80
    EXPECT_EQ(vm->registers_.ReadGpr(11), 0x000000007FFFFF80u);            // positive at e32
    EXPECT_EQ(vm->registers_.ReadGpr(12), 0x123456787FFFFF80u);
}

TEST(VectorPermuteTest, MoveSXWritesOnlyElementZero)
{
    auto vm = runOn<Rvss>("li a0, -2\n"
                          "vsetivli x0, 4, e16, m2, tu, mu\n"
                          "vmv.s.x v2, a0\n"                // LMUL is ignored
                          "vsetivli x0, 0, e16, m1, tu, mu\n"
                          "vmv.s.x v4, a0\n",               // vl = 0: nothing written
                          [](RegisterFile &rf)
                          {
                              writeElements(rf, 2, 16, {1, 2, 3, 4});
                              writeElements(rf, 4, 16, {7, 7});
                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 4), (Elements{0xFFFE, 2, 3, 4}));
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 2), (Elements{7, 7}));
}

// ---------------------------------------------------------------------------------------------
// Slides
// ---------------------------------------------------------------------------------------------

namespace
{
void slideData(RegisterFile &rf)
{
    writeElements(rf, 2, 32, {10, 20, 30, 40});
    writeElements(rf, 4, 32, {9, 9, 9, 9}); // destination's old contents
}
} // namespace

TEST(VectorPermuteTest, SlideUpLeavesElementsBelowOffset)
{
    auto vm = runOn<Rvss>("vsetivli x0, 4, e32, m1, tu, mu\n"
                          "vslideup.vi v4, v2, 2\n",
                          slideData);
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{9, 9, 10, 20}));

    // Masked, and OFFSET from x[rs1]; an OFFSET >= vl writes nothing.
    vm = runOn<Rvss>("li a0, 1\n"
                     "li a1, 100\n"
                     "vsetivli x0, 4, e32, m1, tu, mu\n"
                     "vslideup.vx v4, v2, a0, v0.t\n"
                     "vslideup.vx v6, v2, a1\n",
                     [](RegisterFile &rf)
                     {
                         slideData(rf);
                         writeElements(rf, 0, 8, {0b1010});
                         writeElements(rf, 6, 32, {5, 5, 5, 5});
                     });
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{9, 10, 9, 30}));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 4), (Elements{5, 5, 5, 5}));
}

TEST(VectorPermuteTest, SlideDownReadsZeroPastVlmax)
{
    // e32, m1, VLEN = 128: VLMAX = 4. vl = 2, but sources up to VLMAX are still read.
    auto vm = runOn<Rvss>("vsetivli x0, 2, e32, m1, tu, mu\n"
                          "vslidedown.vi v4, v2, 2\n"   // [30, 40]
                          "vslidedown.vi v6, v2, 3\n",  // [40, 0]
                          [](RegisterFile &rf)
                          {
                              slideData(rf);
                              writeElements(rf, 6, 32, {9, 9, 9, 9});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{30, 40, 9, 9}));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 4), (Elements{40, 0, 9, 9}));

    // A group may slide down onto itself.
    vm = runOn<Rvss>("vsetivli x0, 4, e32, m1, ta, ma\n"
                     "vslidedown.vi v2, v2, 1\n",
                     slideData);
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{20, 30, 40, 0}));
}

TEST(VectorPermuteTest, SlideDownOffsetIsNotTruncated)
{
    auto vm = runOn<Rvss>("li a0, 1\n"
                          "slli a0, a0, 32\n"
                          "addi a0, a0, 1\n" // 2^32 + 1: as a 32-bit value it would be 1
                          "vsetivli x0, 4, e32, m1, tu, mu\n"
                          "vslidedown.vx v4, v2, a0\n",
                          slideData);
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{0, 0, 0, 0}));
}

TEST(VectorPermuteTest, Slide1UpAndSlide1Down)
{
    auto vm = runOn<Rvss>("li a0, -1\n"
                          "vsetivli x0, 3, e16, m1, tu, mu\n"
                          "vslide1up.vx v4, v2, a0\n"    // [ffff, 1, 2] then tail
                          "vslide1down.vx v6, v2, a0\n", // [2, 3, ffff] then tail
                          [](RegisterFile &rf)
                          {
                              writeElements(rf, 2, 16, {1, 2, 3, 4});
                              writeElements(rf, 4, 16, {9, 9, 9, 9});
                              writeElements(rf, 6, 16, {9, 9, 9, 9});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0xFFFF, 1, 2, 9}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{2, 3, 0xFFFF, 9}));

    // Masked: an inactive element 0 / vl-1 does not receive x[rs1].
    vm = runOn<Rvss>("li a0, 7\n"
                     "vsetivli x0, 4, e16, m1, tu, mu\n"
                     "vslide1up.vx v4, v2, a0, v0.t\n"
                     "vslide1down.vx v6, v2, a0, v0.t\n",
                     [](RegisterFile &rf)
                     {
                         writeElements(rf, 0, 8, {0b0110});
                         writeElements(rf, 2, 16, {1, 2, 3, 4});
                         writeElements(rf, 4, 16, {9, 9, 9, 9});
                         writeElements(rf, 6, 16, {9, 9, 9, 9});
                     });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{9, 1, 2, 9}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{9, 3, 4, 9}));
}

// ---------------------------------------------------------------------------------------------
// Gather and compress
// ---------------------------------------------------------------------------------------------

TEST(VectorPermuteTest, GatherForms)
{
    auto vm = runOn<Rvss>("li a0, 2\n"
                          "li a1, 4\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vrgather.vv v4, v2, v3\n"   // indices [3, 0, 9, 1]: 9 >= VLMAX -> 0
                          "vrgather.vx v5, v2, a0\n"   // splat of element 2
                          "vrgather.vx v6, v2, a1\n"   // index 4 = VLMAX -> 0
                          "vrgather.vi v7, v2, 1\n",
                          [](RegisterFile &rf)
                          {
                              slideData(rf);
                              writeElements(rf, 3, 32, {3, 0, 9, 1});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{40, 10, 0, 20}));
    EXPECT_EQ(readElements(vm->registers_, 5, 32, 4), (Elements{30, 30, 30, 30}));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 4), (Elements{0, 0, 0, 0}));
    EXPECT_EQ(readElements(vm->registers_, 7, 32, 4), (Elements{20, 20, 20, 20}));
}

TEST(VectorPermuteTest, GatherReadsTheWholeGroupAndEi16Indices)
{
    // e8, m2: VLMAX = 32, so index 20 reaches into v3 even though vl = 4.
    auto vm = runOn<Rvss>("vsetivli x0, 4, e8, m2, ta, ma\n"
                          "vrgather.vv v4, v2, v6\n"
                          // e8 indices stop at 255; 16-bit indices (EMUL = 2*LMUL = 4) don't.
                          "vrgatherei16.vv v8, v2, v12\n",
                          [](RegisterFile &rf)
                          {
                              for (uint64_t i = 0; i < 32; ++i)
                              {
                                  rf.WriteVectorElement(2, i, 8, 100 + i);
                              }
                              writeElements(rf, 6, 8, {20, 0, 31, 255});
                              writeElements(rf, 12, 16, {31, 256, 1, 5});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{120, 100, 131, 0}));
    EXPECT_EQ(readElements(vm->registers_, 8, 8, 4), (Elements{131, 0, 101, 105}));
}

TEST(VectorPermuteTest, CompressPacksSelectedElements)
{
    auto vm = runOn<Rvss>("vsetivli x0, 4, e32, m1, tu, mu\n"
                          "vcompress.vm v4, v2, v1\n",
                          [](RegisterFile &rf)
                          {
                              slideData(rf);
                              writeElements(rf, 1, 8, {0b1101});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{10, 30, 40, 9}));
}

// ---------------------------------------------------------------------------------------------
// Whole-register moves
// ---------------------------------------------------------------------------------------------

TEST(VectorPermuteTest, WholeRegisterMoveIgnoresVlAndVtype)
{
    auto vm = runOn<Rvss>("vsetivli x0, 1, e8, mf8, ta, ma\n" // vl = 1, LMUL = 1/8
                          "vmv2r.v v4, v2\n",
                          [](RegisterFile &rf)
                          {
                              writeElements(rf, 2, 64, {1, 2});
                              writeElements(rf, 3, 64, {3, 4});
                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 2), (Elements{1, 2}));
    EXPECT_EQ(readElements(vm->registers_, 5, 64, 2), (Elements{3, 4}));

    // Works before any vsetvli (vtype.vill) as well.
    vm = runOn<Rvss>("vmv1r.v v7, v2\n",
                     [](RegisterFile &rf) { writeElements(rf, 2, 64, {5, 6}); });
    EXPECT_EQ(readElements(vm->registers_, 7, 64, 2), (Elements{5, 6}));
}

// ---------------------------------------------------------------------------------------------
// Reserved encodings, undo
// ---------------------------------------------------------------------------------------------

TEST(VectorPermuteTest, IllegalConfigurationsTrap)
{
    expectIllegal("vslideup.vi v4, v2, 1\n");                            // vtype.vill
    expectIllegal("vsetivli x0, 4, e32, m2, ta, ma\nvslideup.vi v3, v4, 1\n");  // misaligned vd
    expectIllegal("vsetivli x0, 4, e32, m2, ta, ma\nvrgather.vv v4, v6, v5\n"); // misaligned vs1
    expectIllegal("vmv2r.v v3, v4\n");                                   // NREG alignment
    expectIllegal("vmv4r.v v4, v6\n");
    // e8, m1: the ei16 index group is v2-v3, and vd = v3 sits inside it.
    expectIllegal("vsetivli x0, 4, e8, m1, ta, ma\nvrgatherei16.vv v3, v4, v2\n");
    // e8, m8: 16-bit indices would need EMUL = 16, which is reserved.
    expectIllegal("vsetvli x0, x0, e8, m8, ta, ma\nvrgatherei16.vv v8, v16, v0\n");
    // m2: the vcompress mask v3 lies inside vd's group v2-v3.
    expectIllegal("vsetivli x0, 4, e32, m2, ta, ma\nvcompress.vm v2, v4, v3\n");
    // vcompress with vstart != 0 (set directly: vsetvli would clear vstart).
    expectIllegal("vcompress.vm v4, v2, v1\n",
                  [](RegisterFile &rf)
                  {
                      rf.WriteCsr(RegisterFile::CSR_VTYPE,
                                  Kites::rvv::EncodeVtype(32, 0, false, false));
                      rf.WriteCsr(RegisterFile::CSR_VL, 4);
                      rf.WriteCsr(RegisterFile::CSR_VSTART, 1);
                  });
    // ... and the same instruction with vstart = 0 is fine.
    EXPECT_NO_THROW(runOn<Rvss>("vcompress.vm v4, v2, v1\n",
                                [](RegisterFile &rf)
                                {
                                    rf.WriteCsr(RegisterFile::CSR_VTYPE,
                                                Kites::rvv::EncodeVtype(32, 0, false, false));
                                    rf.WriteCsr(RegisterFile::CSR_VL, 4);
                                }));

    // vmv.s.x with a vs2 field other than 0 is reserved.
    auto vm = runOn<Rvss>("vsetivli x0, 4, e32, m1, ta, ma\n");
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(MATCH_VMV_S_X | fields(1, 2, 10), 0, 0,
                                                      vm->registers_, vm->memory_controller_,
                                                      changes, memoryChanges),
                 std::runtime_error);
}

TEST(VectorPermuteTest, UndoAndRedo)
{
    auto vm = runOn<Rvss>("vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vslideup.vi v4, v2, 1\n",
                          slideData);
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{9, 10, 20, 30}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{9, 9, 9, 9}));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), (Elements{9, 10, 20, 30}));
}

// ---------------------------------------------------------------------------------------------
// All VMs: vmv.x.s feeds a scalar instruction straight away (forwarding / stall on rd)
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kPermuteProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                              "vmv.x.s a1, v2\n"            // a1 = -5
                              "addi a2, a1, 1\n"            // a2 = -4 (uses a1 at once)
                              "vmv.s.x v5, a2\n"            // v5[0] = 0xFFFFFFFC
                              "vslide1down.vx v6, v2, a2\n" // [2, 3, 4, 0xFFFFFFFC]
                              "vslideup.vi v7, v2, 1\n"     // [old, -5, 2, 3]
                              "vrgather.vi v8, v6, 3\n"     // splat 0xFFFFFFFC
                              "nop\nnop\nnop\nnop\n";

// The no-hazard, no-forwarding pipeline needs the dependent instructions spaced out by hand.
const char *kPermuteProgramScheduled = "vsetivli x0, 4, e32, m1, ta, ma\n"
                                       "vmv.x.s a1, v2\n"
                                       "nop\nnop\nnop\n"
                                       "addi a2, a1, 1\n"
                                       "nop\nnop\nnop\n"
                                       "vmv.s.x v5, a2\n"
                                       "vslide1down.vx v6, v2, a2\n"
                                       "vslideup.vi v7, v2, 1\n"
                                       "vrgather.vi v8, v6, 3\n"
                                       "nop\nnop\nnop\nnop\n";

template <typename Vm> void expectPermuteResult(const char *program)
{
    auto vm = runOn<Vm>(program,
                        [](RegisterFile &rf)
                        {
                            writeElements(rf, 2, 32, {0xFFFFFFFB, 2, 3, 4});
                            writeElements(rf, 7, 32, {9, 9, 9, 9});
                        });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(rf.ReadGpr(11), static_cast<uint64_t>(-5));
    EXPECT_EQ(rf.ReadGpr(12), static_cast<uint64_t>(-4));
    EXPECT_EQ(rf.ReadVectorElement(5, 0, 32), 0xFFFFFFFCu);
    EXPECT_EQ(readElements(rf, 6, 32, 4), (Elements{2, 3, 4, 0xFFFFFFFC}));
    EXPECT_EQ(readElements(rf, 7, 32, 4), (Elements{9, 0xFFFFFFFB, 2, 3}));
    EXPECT_EQ(readElements(rf, 8, 32, 4), Elements(4, 0xFFFFFFFC));
}
} // namespace

TEST(VectorPermutePipelineTest, ProgramOnEveryVariant)
{
    expectPermuteResult<Kites::RVSSProcessor>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorHF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorHNF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorNHF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorNHNF>(kPermuteProgramScheduled);
}
