// Pipelined vector instructions on the hazard-detecting, forwarding 5-stage VM (H_F).
//
// A vector instruction goes through IF -> ID -> EX -> MEM -> WB like any other: EX computes the
// result (reading vector operands from the register file plus the EX/MEM bypass), MEM performs
// vector loads/stores, WB writes the vector registers and vl/vtype/vstart. Zicsr instructions
// write their CSR in WB too. The Hn test names refer to the hazard matrix of the design:
//   H1  vector arith -> vector reader             bypass, no stall
//   H2  vector load  -> vector reader in EX       1 stall
//   H3  vector load  -> vector store data          read in MEM, no stall
//   H4  vset*        -> vector instruction         vl/vtype bypass, no stall
//   H5  vle*ff       -> reader of vl in EX         1 stall
//   H7  vector       -> vector (vstart = 0)        bypass, no stall
//   H8  vset*/vector -> csrr* of vl/vtype/vstart   CSR bypass, no stall (H8b: vle*ff, 1 stall)
//   H9  vector       -> csrw vstart                in-order WB writes, no stall
//   H11 scalar load  -> vector x[rs1]/x[rs2]       1 stall
//   H13 scalar load  -> vector vs1/vs2 field       no dependence, no stall
//   H17 csrw         -> csrr of the same CSR       CSR bypass, no stall
//   H18 csrw f-CSR   -> F/D instruction            1 stall
// Cycle counts are compared with an equally long program without the dependency, so each test
// measures exactly the stalls the hazard costs.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/registers.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rvss/rvss_processor.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_operands.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;
using Kites::RV5StageProcessorHF;

constexpr uint64_t kData = 0x10000000;
constexpr uint64_t kOut = kData + 64;

// Three instructions: s0 = &arr, s1 = &out, vl = 4 at e32/m1.
const std::string kPrelude = ".data\n"
                             "arr: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16\n"
                             "out: .zero 64\n"
                             ".text\n"
                             "    lui s0, 0x10000\n"
                             "    addi s1, s0, 64\n"
                             "    vsetivli t0, 4, e32, m1, ta, ma\n";

template <typename Vm> std::unique_ptr<Vm> Load(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(Kites::assemble(stream));
    return vm;
}

template <typename Vm> std::unique_ptr<Vm> RunBody(const std::string &body)
{
    auto vm = Load<Vm>(kPrelude + body);
    vm->DebugRun();
    return vm;
}

unsigned Cycles(const std::string &body)
{
    return RunBody<RV5StageProcessorHF>(body)->cycle_s_;
}

uint64_t Element32(const Kites::ProcessorBase &vm, size_t reg, size_t index)
{
    return vm.registers_.ReadVectorElement(reg, index, 32);
}

uint32_t Word(const Kites::ProcessorBase &vm, uint64_t address)
{
    uint32_t word = 0;
    for (int i = 3; i >= 0; --i)
    {
        word = (word << 8) | const_cast<Kites::ProcessorBase &>(vm).memory_controller_.peekByte(
                                 address + i);
    }
    return word;
}

struct State
{
    std::vector<uint64_t> gprs;
    std::vector<std::vector<uint8_t>> vregs;
    uint64_t vl = 0, vtype = 0, vstart = 0, fcsr = 0;
    std::vector<uint8_t> data;

    bool operator==(const State &other) const
    {
        return gprs == other.gprs && vregs == other.vregs && vl == other.vl &&
               vtype == other.vtype && vstart == other.vstart && fcsr == other.fcsr &&
               data == other.data;
    }
};

State Capture(Kites::ProcessorBase &vm)
{
    State state;
    for (size_t r = 0; r < 32; ++r)
    {
        state.gprs.push_back(vm.registers_.ReadGpr(r));
        state.vregs.push_back(vm.registers_.ReadVectorRegister(r));
    }
    state.vl = vm.registers_.ReadCsr(RegisterFile::CSR_VL);
    state.vtype = vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE);
    state.vstart = vm.registers_.ReadCsr(RegisterFile::CSR_VSTART);
    state.fcsr = vm.registers_.ReadCsr(0x003);
    for (uint64_t i = 0; i < 128; ++i)
    {
        state.data.push_back(vm.memory_controller_.peekByte(kData + i));
    }
    return state;
}

void ExpectSameAsSingleCycle(const std::string &body)
{
    auto reference = RunBody<Kites::RVSSProcessor>(body);
    auto pipelined = RunBody<RV5StageProcessorHF>(body);
    const State expected = Capture(*reference);
    const State actual = Capture(*pipelined);
    for (size_t r = 1; r < 32; ++r)
    {
        EXPECT_EQ(actual.gprs[r], expected.gprs[r]) << "x" << r;
    }
    for (size_t r = 0; r < 32; ++r)
    {
        EXPECT_EQ(actual.vregs[r], expected.vregs[r]) << "v" << r;
    }
    EXPECT_EQ(actual.vl, expected.vl);
    EXPECT_EQ(actual.vtype, expected.vtype);
    EXPECT_EQ(actual.vstart, expected.vstart);
    EXPECT_EQ(actual.fcsr, expected.fcsr);
    EXPECT_EQ(actual.data, expected.data);
}

/// A program exercising most of the forwarding and hazard paths back to back.
const std::string kMixedProgram = "    vsetivli t1, 8, e32, m2, ta, ma\n"
                                  "    vle32.v v2, (s0)\n"           // v2-v3
                                  "    vadd.vv v4, v2, v2\n"         // H2: load-use
                                  "    vadd.vv v6, v4, v2\n"         // H1: v4 group bypassed
                                  "    vmul.vv v8, v6, v4\n"         // H1
                                  "    vse32.v v8, (s1)\n"           // H3: store data in MEM
                                  "    vsetivli t1, 4, e32, m1, tu, mu\n"
                                  "    vadd.vi v10, v8, 1\n"         // H4: new vtype bypassed
                                  "    vmseq.vi v0, v10, 7\n"
                                  "    vadd.vv v12, v10, v10, v0.t\n" // H1 through the mask
                                  "    vredsum.vs v14, v12, v10\n"
                                  "    vmv.x.s a0, v14\n"
                                  "    addi a1, a0, 1\n"             // vector rd -> scalar
                                  "    csrrwi x0, vstart, 1\n"
                                  "    vadd.vv v16, v12, v12\n"      // H10: vstart = 1
                                  "    vadd.vv v18, v16, v16\n"      // H7: vstart = 0
                                  "    csrrs a2, vstart, x0\n"       // H8
                                  "    vsetivli t1, 4, e16, m1, ta, ma\n"
                                  "    vwadd.vv v20, v10, v10\n"     // writes v20-v21
                                  "    vsetivli t1, 4, e32, m2, ta, ma\n"
                                  "    vadd.vv v22, v20, v20\n"      // H1 with H4
                                  "    vsetvli a3, x0, e64, m1, ta, ma\n"
                                  "    csrrs a4, vl, x0\n"           // H8
                                  "    csrrs a5, vtype, x0\n"
                                  "    vle32ff.v v24, (s0)\n"
                                  "    csrrs a6, vl, x0\n"           // H8b
                                  "    lw t2, 0(s0)\n"
                                  "    vadd.vx v26, v24, t2\n"       // H11
                                  "    vse64.v v26, (s1)\n"
                                  "    ld a7, 0(s1)\n";
} // namespace

// --- Timing -------------------------------------------------------------------------------

TEST(Rv5sVectorPipelineTest, IndependentVectorInstructionsTakeNPlusFourCycles)
{
    // 3 prelude + 4 vector instructions, no hazards: one per cycle plus the 4-cycle fill.
    EXPECT_EQ(Cycles("    vmv.v.i v1, 1\n    vmv.v.i v2, 2\n"
                     "    vmv.v.i v3, 3\n    vmv.v.i v4, 4\n"),
              7u + 4u);
}

TEST(Rv5sVectorPipelineTest, VectorRegisterIsWrittenInWbNotEx)
{
    // vmv.v.i is the 4th instruction (pc 12): IF in cycle 4, EX in 6, MEM in 7, WB in 8.
    auto vm = Load<RV5StageProcessorHF>(kPrelude + "    vmv.v.i v1, 7\n");
    for (int cycle = 1; cycle <= 7; ++cycle)
    {
        vm->Step();
    }
    EXPECT_EQ(Element32(*vm, 1, 0), 0u) << "written before WB";
    vm->Step();
    EXPECT_EQ(Element32(*vm, 1, 0), 7u);
}

TEST(Rv5sVectorPipelineTest, VectorStoreWritesMemoryInMem)
{
    // vse32.v is the 5th instruction (pc 16): MEM in cycle 8.
    auto vm = Load<RV5StageProcessorHF>(kPrelude + "    vmv.v.i v1, 9\n    vse32.v v1, (s1)\n");
    for (int cycle = 1; cycle <= 7; ++cycle)
    {
        vm->Step();
    }
    EXPECT_EQ(Word(*vm, kOut), 0u);
    vm->Step();
    EXPECT_EQ(Word(*vm, kOut), 9u);
}

TEST(Rv5sVectorPipelineTest, CsrIsWrittenInWbNotEx)
{
    auto vm = Load<RV5StageProcessorHF>(kPrelude + "    csrrwi x0, vstart, 5\n");
    for (int cycle = 1; cycle <= 7; ++cycle)
    {
        vm->Step();
    }
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u) << "written before WB";
    vm->Step();
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 5u);
}

// --- Forwarding (no stalls) ---------------------------------------------------------------

TEST(Rv5sVectorPipelineTest, H1_VectorResultIsForwardedFromExMem)
{
    const std::string dependent = "    vmv.v.i v1, 3\n    vadd.vv v2, v1, v1\n";
    EXPECT_EQ(Cycles(dependent), Cycles("    vmv.v.i v1, 3\n    vadd.vv v2, v3, v3\n"));

    auto vm = Load<RV5StageProcessorHF>(kPrelude + dependent);
    bool forwarded = false;
    for (int cycle = 0; cycle < 12; ++cycle)
    {
        vm->Step();
        forwarded = forwarded || vm->VectorForwardSelected();
    }
    EXPECT_TRUE(forwarded);
    for (size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(Element32(*vm, 2, i), 6u);
    }
}

TEST(Rv5sVectorPipelineTest, H1_PartialGroupOverlapIsForwarded)
{
    // vwadd writes v4-v5 (2*SEW); the next instruction reads only v5, the second half.
    const std::string body = "    vle32.v v1, (s0)\n"
                             "    nop\n"
                             "    vwadd.vv v4, v1, v1\n"
                             "    vadd.vv v6, v5, v5\n";
    EXPECT_EQ(Cycles(body), Cycles("    vle32.v v1, (s0)\n    nop\n    vwadd.vv v4, v1, v1\n"
                                   "    vadd.vv v6, v7, v7\n"));
    ExpectSameAsSingleCycle(body);
}

TEST(Rv5sVectorPipelineTest, H3_LoadedRegisterStoredRightAwayNeedsNoStall)
{
    const std::string dependent = "    vle32.v v1, (s0)\n    vse32.v v1, (s1)\n";
    EXPECT_EQ(Cycles(dependent), Cycles("    vle32.v v1, (s0)\n    vse32.v v3, (s1)\n"));
    auto vm = RunBody<RV5StageProcessorHF>(dependent);
    for (uint64_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(Word(*vm, kOut + 4 * i), i + 1);
    }
}

TEST(Rv5sVectorPipelineTest, H4_NewVlIsForwardedToTheNextVectorInstruction)
{
    const std::string body = "    vmv.v.i v2, 7\n"
                             "    vsetivli t1, 2, e32, m1, tu, mu\n"
                             "    vmv.v.i v2, 1\n";
    EXPECT_EQ(Cycles(body), Cycles("    vmv.v.i v2, 7\n    addi t1, x0, 2\n    vmv.v.i v2, 1\n"));
    auto vm = RunBody<RV5StageProcessorHF>(body);
    EXPECT_EQ(Element32(*vm, 2, 0), 1u);
    EXPECT_EQ(Element32(*vm, 2, 1), 1u);
    EXPECT_EQ(Element32(*vm, 2, 2), 7u) << "vl = 2 must reach the vmv through the bypass";
    EXPECT_EQ(Element32(*vm, 2, 3), 7u);
}

TEST(Rv5sVectorPipelineTest, H7_H10_VstartReachesTheNextInstructionAndIsThenCleared)
{
    const std::string body = "    vmv.v.i v1, 5\n"
                             "    csrrwi x0, vstart, 2\n"
                             "    vadd.vv v2, v1, v1\n"  // H10: starts at element 2
                             "    vadd.vv v3, v1, v1\n"  // H7: vstart is 0 again
                             "    csrrs t2, vstart, x0\n"; // H8: 0
    ExpectSameAsSingleCycle(body);
    auto vm = RunBody<RV5StageProcessorHF>(body);
    EXPECT_EQ(Element32(*vm, 2, 0), 0u);
    EXPECT_EQ(Element32(*vm, 2, 2), 10u);
    EXPECT_EQ(Element32(*vm, 3, 0), 10u);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 0u);
}

TEST(Rv5sVectorPipelineTest, H8_VsetCsrsAreForwardedToCsrReads)
{
    const std::string body = "    vsetivli t1, 2, e32, m2, ta, ma\n"
                             "    csrrs t2, vl, x0\n"
                             "    csrrs t3, vtype, x0\n";
    EXPECT_EQ(Cycles(body), Cycles("    addi t1, x0, 2\n    csrrs t2, vl, x0\n"
                                   "    csrrs t3, vtype, x0\n"));
    ExpectSameAsSingleCycle(body);
    auto vm = RunBody<RV5StageProcessorHF>(body);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 2u);
    EXPECT_EQ(vm->registers_.ReadGpr(28), Kites::rvv::EncodeVtype(32, 1, true, true));
}

TEST(Rv5sVectorPipelineTest, H9_CsrWriteAfterAVectorInstructionSurvives)
{
    const std::string body = "    vadd.vv v2, v1, v1\n    csrrwi x0, vstart, 3\n";
    EXPECT_EQ(Cycles(body), Cycles("    vadd.vv v2, v1, v1\n    csrrwi x0, fflags, 3\n"));
    auto vm = RunBody<RV5StageProcessorHF>(body);
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 3u)
        << "the older vector instruction's vstart = 0 must not land after the csrw";
}

TEST(Rv5sVectorPipelineTest, H17_CsrWriteIsForwardedToTheNextCsrRead)
{
    const std::string body = "    csrrwi x0, fflags, 5\n    csrrs t2, fflags, x0\n";
    EXPECT_EQ(Cycles(body), Cycles("    csrrwi x0, fflags, 5\n    csrrs t2, frm, x0\n"));
    EXPECT_EQ(RunBody<RV5StageProcessorHF>(body)->registers_.ReadGpr(7), 5u);
}

TEST(Rv5sVectorPipelineTest, H13_ScalarLoadDoesNotStallAVectorRegisterField)
{
    // vs1/vs2 = 3/4 name vector registers; the load writes x3.
    EXPECT_EQ(Cycles("    lw x3, 0(s0)\n    vadd.vv v1, v3, v4\n"),
              Cycles("    lw x5, 0(s0)\n    vadd.vv v1, v3, v4\n"));
}

// --- Stalls --------------------------------------------------------------------------------

TEST(Rv5sVectorPipelineTest, H2_VectorLoadUseStallsOneCycle)
{
    const std::string dependent = "    vle32.v v1, (s0)\n    vadd.vv v2, v1, v1\n";
    EXPECT_EQ(Cycles(dependent),
              Cycles("    vle32.v v1, (s0)\n    vadd.vv v2, v3, v3\n") + 1);
    auto vm = RunBody<RV5StageProcessorHF>(dependent);
    for (size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(Element32(*vm, 2, i), 2 * (i + 1));
    }
}

TEST(Rv5sVectorPipelineTest, H2_OnlyAnOverlappingGroupStalls)
{
    // m4: the load writes v4-v7; a reader of v8-v11 is independent, a reader of v6 is not.
    const std::string setup = "    vsetivli t1, 16, e32, m4, ta, ma\n    vle32.v v4, (s0)\n";
    EXPECT_EQ(Cycles(setup + "    vadd.vv v12, v4, v4\n"),
              Cycles(setup + "    vadd.vv v12, v8, v8\n") + 1);
    // v6 is inside the v4 group, but not aligned to m4: use m2 for the reader.
    EXPECT_EQ(Cycles(setup + "    vsetivli t1, 4, e32, m2, ta, ma\n    vadd.vv v12, v6, v6\n"),
              Cycles(setup + "    vsetivli t1, 4, e32, m2, ta, ma\n    vadd.vv v12, v8, v8\n"));
}

TEST(Rv5sVectorPipelineTest, H5_FaultOnlyFirstLoadStallsAVlReader)
{
    EXPECT_EQ(Cycles("    vle32ff.v v1, (s0)\n    vadd.vv v2, v3, v3\n"),
              Cycles("    vle32.v v1, (s0)\n    vadd.vv v2, v3, v3\n") + 1);
}

TEST(Rv5sVectorPipelineTest, H8b_FaultOnlyFirstLoadStallsACsrReadOfVl)
{
    const std::string body = "    vle32ff.v v1, (s0)\n    csrrs t2, vl, x0\n";
    EXPECT_EQ(Cycles(body), Cycles("    vle32.v v1, (s0)\n    csrrs t2, vl, x0\n") + 1);
    EXPECT_EQ(RunBody<RV5StageProcessorHF>(body)->registers_.ReadGpr(7), 4u);
}

TEST(Rv5sVectorPipelineTest, H11_ScalarLoadIntoAVectorScalarOperandStalls)
{
    const std::string dependent = "    lw t1, 0(s0)\n    vadd.vx v1, v2, t1\n";
    EXPECT_EQ(Cycles(dependent), Cycles("    lw t1, 0(s0)\n    vadd.vx v1, v2, t2\n") + 1);
    EXPECT_EQ(Element32(*RunBody<RV5StageProcessorHF>(dependent), 1, 3), 1u);
    // The base address of a vector load is x[rs1] too.
    EXPECT_EQ(Cycles("    lw t1, 0(s0)\n    vle32.v v1, (t1)\n"),
              Cycles("    lw t1, 0(s0)\n    vle32.v v1, (s0)\n") + 1);
}

TEST(Rv5sVectorPipelineTest, H18_FpCsrWriteStallsTheNextFpInstruction)
{
    EXPECT_EQ(Cycles("    csrrwi x0, frm, 1\n    fadd.s f3, f1, f2\n"),
              Cycles("    csrrwi x0, vstart, 0\n    fadd.s f3, f1, f2\n") + 1);
    // Without the stall, the older csrw would land in WB after the fdiv wrote its flags in EX.
    const std::string body = "    addi t1, x0, 1\n"
                             "    fcvt.s.w f1, t1\n"
                             "    csrrwi x0, fcsr, 0\n"
                             "    fdiv.s f3, f1, f0\n";
    ExpectSameAsSingleCycle(body);
    EXPECT_NE(RunBody<RV5StageProcessorHF>(body)->registers_.ReadCsr(0x003), 0u);
}

TEST(Rv5sVectorPipelineTest, HduSizesGroupsWithThePendingVtypeInMemWb)
{
    // The register file still says m8 when vadd is checked: with it, vadd's v0-v7 group would
    // overlap the load's v4 and stall. With the m1 vsetivli waiting in MEM/WB it does not.
    const std::string setup = "    vsetivli t1, 8, e32, m8, ta, ma\n    nop\n    nop\n    nop\n"
                              "    vsetivli t1, 4, e32, m1, ta, ma\n";
    EXPECT_EQ(Cycles(setup + "    vle32.v v4, (s0)\n    vadd.vv v0, v1, v1\n"),
              Cycles(setup + "    vle32.v v20, (s0)\n    vadd.vv v0, v1, v1\n"));
}

// --- Whole programs -------------------------------------------------------------------------

TEST(Rv5sVectorPipelineTest, MixedProgramMatchesTheSingleCycleVm)
{
    ExpectSameAsSingleCycle(kMixedProgram);
}

TEST(Rv5sVectorPipelineTest, UndoToTheStartAndRedoToTheEnd)
{
    auto vm = Load<RV5StageProcessorHF>(kPrelude + kMixedProgram);
    const State start = Capture(*vm);
    vm->DebugRun();
    const State end = Capture(*vm);
    const unsigned cycles = vm->cycle_s_;
    ASSERT_GT(cycles, 0u);

    for (unsigned i = 0; i < cycles; ++i)
    {
        vm->Undo();
    }
    EXPECT_EQ(vm->cycle_s_, 0u);
    EXPECT_TRUE(Capture(*vm) == start);

    for (unsigned i = 0; i < cycles; ++i)
    {
        vm->Redo();
    }
    EXPECT_EQ(vm->cycle_s_, cycles);
    EXPECT_TRUE(Capture(*vm) == end);
}

// --- The operand decoder --------------------------------------------------------------------

namespace
{
uint32_t Encode(const std::string &instruction)
{
    std::istringstream stream(".text\n    " + instruction + "\n");
    return Kites::assemble(stream).text_buffer.at(0);
}

uint32_t Group(uint32_t base, uint32_t count)
{
    return Kites::rvv::VectorGroupMask(base, count);
}
} // namespace

TEST(VectorOperandDecoderTest, ArithmeticGroupsFollowLmulAndShape)
{
    using Kites::rvv::DecodeVectorRegisterUse;
    using Kites::rvv::EncodeVtype;

    auto use = DecodeVectorRegisterUse(Encode("vadd.vv v4, v8, v12"), EncodeVtype(32, 1, 1, 1));
    EXPECT_EQ(use.exReads, Group(4, 2) | Group(8, 2) | Group(12, 2));
    EXPECT_EQ(use.writeBase, 4);
    EXPECT_EQ(use.writeCount, 2);
    EXPECT_TRUE(use.readsVlInEx);

    use = DecodeVectorRegisterUse(Encode("vwadd.vv v8, v2, v4"), EncodeVtype(16, 1, 1, 1));
    EXPECT_EQ(use.writeCount, 4);
    EXPECT_EQ(use.exReads, Group(2, 2) | Group(4, 2) | Group(8, 4));

    use = DecodeVectorRegisterUse(Encode("vnsrl.wv v2, v4, v8"), EncodeVtype(16, 0, 1, 1));
    EXPECT_EQ(use.exReads, Group(4, 2) | Group(8, 1) | Group(2, 1));
    EXPECT_EQ(use.writeCount, 1);

    use = DecodeVectorRegisterUse(Encode("vredsum.vs v1, v8, v2"), EncodeVtype(32, 2, 1, 1));
    EXPECT_EQ(use.exReads, Group(8, 4) | Group(2, 1) | Group(1, 1));
    EXPECT_EQ(use.writeBase, 1);
    EXPECT_EQ(use.writeCount, 1);

    use = DecodeVectorRegisterUse(Encode("vadd.vv v2, v4, v6, v0.t"), EncodeVtype(32, 0, 1, 1));
    EXPECT_EQ(use.exReads, Group(0, 1) | Group(2, 1) | Group(4, 1) | Group(6, 1));

    use = DecodeVectorRegisterUse(Encode("vadd.vx v2, v4, a0"), EncodeVtype(32, 0, 1, 1));
    EXPECT_TRUE(use.readsRs1);
    EXPECT_EQ(use.exReads, Group(2, 1) | Group(4, 1)) << "x10 is not v10";

    use = DecodeVectorRegisterUse(Encode("vmv.x.s a0, v4"), EncodeVtype(32, 0, 1, 1));
    EXPECT_EQ(use.writeCount, 0);
    EXPECT_EQ(use.exReads, Group(4, 1));
}

TEST(VectorOperandDecoderTest, LoadsAndStoresReadNothingInEx)
{
    using Kites::rvv::DecodeVectorRegisterUse;
    using Kites::rvv::EncodeVtype;

    // EEW 32 at SEW 8, m1: EMUL = 4.
    auto use = DecodeVectorRegisterUse(Encode("vle32.v v4, (a0)"), EncodeVtype(8, 0, 1, 1));
    EXPECT_TRUE(use.isLoad);
    EXPECT_EQ(use.exReads, 0u);
    EXPECT_EQ(use.writeBase, 4);
    EXPECT_EQ(use.writeCount, 4);
    EXPECT_TRUE(use.readsRs1);
    EXPECT_FALSE(use.readsRs2);

    use = DecodeVectorRegisterUse(Encode("vlsseg2e32.v v4, (a0), a1"), EncodeVtype(32, 0, 1, 1));
    EXPECT_EQ(use.writeCount, 2);
    EXPECT_TRUE(use.readsRs2);

    use = DecodeVectorRegisterUse(Encode("vl2re32.v v4, (a0)"), EncodeVtype(32, 0, 1, 1));
    EXPECT_EQ(use.writeCount, 2);

    use = DecodeVectorRegisterUse(Encode("vle32ff.v v4, (a0)"), EncodeVtype(32, 0, 1, 1));
    EXPECT_TRUE(use.isFaultOnlyFirst);
    EXPECT_TRUE(use.writesVlVtype);

    use = DecodeVectorRegisterUse(Encode("vse32.v v4, (a0)"), EncodeVtype(32, 0, 1, 1));
    EXPECT_TRUE(use.isStore);
    EXPECT_EQ(use.writeCount, 0);
}

TEST(VectorOperandDecoderTest, VsetReadsGprsAndWritesVlVtype)
{
    auto use = Kites::rvv::DecodeVectorRegisterUse(Encode("vsetvl t0, a0, a1"), 0);
    EXPECT_TRUE(use.isVset);
    EXPECT_TRUE(use.writesVlVtype);
    EXPECT_TRUE(use.readsRs1);
    EXPECT_TRUE(use.readsRs2);
    EXPECT_EQ(use.writeCount, 0);

    use = Kites::rvv::DecodeVectorRegisterUse(Encode("vsetivli t0, 4, e32, m1, ta, ma"), 0);
    EXPECT_FALSE(use.readsRs1);
}

// --- Benchmarks -------------------------------------------------------------------------------

namespace
{
class VectorBenchmarkDifferentialTest : public ::testing::TestWithParam<const char *>
{
};

template <typename Vm> std::unique_ptr<Vm> RunFile(const std::string &path)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(Kites::assemble(path));
    vm->DebugRun();
    return vm;
}
} // namespace

TEST_P(VectorBenchmarkDifferentialTest, PipelinedRunMatchesTheSingleCycleVm)
{
    const std::string path = GetParam();
    if (!std::ifstream(path))
    {
        GTEST_SKIP() << path << " not found";
    }
    auto reference = RunFile<Kites::RVSSProcessor>(path);
    auto pipelined = RunFile<RV5StageProcessorHF>(path);
    for (size_t r = 1; r < 32; ++r)
    {
        EXPECT_EQ(pipelined->registers_.ReadGpr(r), reference->registers_.ReadGpr(r)) << "x" << r;
    }
    for (size_t r = 0; r < 32; ++r)
    {
        EXPECT_EQ(pipelined->registers_.ReadVectorRegister(r),
                  reference->registers_.ReadVectorRegister(r))
            << "v" << r;
    }
    EXPECT_EQ(pipelined->registers_.ReadCsr(RegisterFile::CSR_VL),
              reference->registers_.ReadCsr(RegisterFile::CSR_VL));
    EXPECT_EQ(pipelined->registers_.ReadCsr(RegisterFile::CSR_VTYPE),
              reference->registers_.ReadCsr(RegisterFile::CSR_VTYPE));
    size_t mismatches = 0;
    for (uint64_t address = kData; address < kData + 48 * 1024; ++address)
    {
        mismatches += pipelined->memory_controller_.peekByte(address) !=
                      reference->memory_controller_.peekByte(address);
    }
    EXPECT_EQ(mismatches, 0u);
    std::cout << "[ cycles ] " << path << ": RVSS " << reference->cycle_s_ << ", H_F "
              << pipelined->cycle_s_ << " (" << pipelined->instructions_retired_
              << " instructions)\n";
}

INSTANTIATE_TEST_SUITE_P(Programs, VectorBenchmarkDifferentialTest,
                         ::testing::Values("../benchmarks/programs/dot_vector.s",
                                           "../benchmarks/programs/matmul_vector.s",
                                           "../benchmarks/programs/max_vector.s",
                                           "../benchmarks/programs/saxpy_vector.s",
                                           "../benchmarks/programs/vadd_vector.s",
                                           "../examples/bicameral_fir_bank.s"));

// --- Datapath view ------------------------------------------------------------------------------

TEST(Rv5sVectorPipelineTest, VectorInstructionWiresAreMarkedGreen)
{
    // vmv.v.i (pc 12) is fetched in cycle 4, in EX/MEM after cycle 6 and written back in 8.
    auto vm = Load<RV5StageProcessorHF>(kPrelude + "    vmv.v.i v1, 7\n    addi x5, x0, 1\n");
    const auto wiresAfter = [&vm](int cycle)
    {
        while (static_cast<int>(vm->cycle_s_) < cycle)
        {
            vm->Step();
        }
        vm->SetActiveWireNames();
        return vm->active_wires_;
    };
    QList<QString> wires = wiresAfter(4);
    EXPECT_TRUE(wires.contains("V:IM_to_P1")) << "IF of the vector instruction";
    EXPECT_FALSE(wires.contains("IM_to_P1")) << "green replaces yellow";

    wires = wiresAfter(5);
    EXPECT_FALSE(wires.contains("V:IM_to_P1")) << "the scalar addi is in IF now";
    EXPECT_TRUE(wires.contains("IM_to_P1"));
    EXPECT_TRUE(wires.contains("V:RF_to_P2_UP")) << "ID";

    EXPECT_TRUE(wiresAfter(6).contains("V:ALU_to_P3")) << "EX";
    EXPECT_TRUE(wiresAfter(7).contains("V:rd_P3_to_P4")) << "MEM";
    EXPECT_TRUE(wiresAfter(8).contains("V:WBMux_to_RF")) << "WB";

    // A vector load in MEM lights the data-memory read path green.
    auto load = Load<RV5StageProcessorHF>(kPrelude + "    vle32.v v1, (s0)\n");
    for (int cycle = 0; cycle < 7; ++cycle)
    {
        load->Step();
    }
    load->SetActiveWireNames();
    EXPECT_TRUE(load->active_wires_.contains("V:P3_to_DM_Memread"));
}
