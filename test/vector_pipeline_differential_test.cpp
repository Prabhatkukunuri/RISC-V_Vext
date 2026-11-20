// Differential check of the 5-stage pipelines against the single-cycle VM for back-to-back
// dependencies between scalar and vector instructions: a scalar load feeding a vector .vx
// operand, a vector base address or vsetvl's AVL; vset*'s rd, vmv.x.s, vcpop.m and vfirst.m
// feeding scalar code; and a vector-derived value stored right away. The final x registers,
// vector registers, vl/vtype and .data must be identical.
//
// H_F (forwarding + hazard detection) and H_NF (stalls) must match with no help. NH_F has
// forwarding but no hazard detection, so a load-use pair needs one instruction in between; NH_NF
// (neither) is left out because it needs every dependency spaced by hand.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/registers.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;

// "<LOAD_GAP>" marks the spot after each scalar load where NH_F needs a free slot.
const char *kProgram = ".data\n"
                       "vals:  .dword 7, 0x10000020, 3, 0\n"
                       "arr:   .word 10, 20, 30, 40, 50, 60, 70, 80\n"
                       ".text\n"
                       "    lui   s0, 0x10000\n"
                       "    nop\n    nop\n    nop\n"
                       "    vsetivli t0, 4, e32, m1, ta, ma\n"
                       "    add   t4, t0, t0\n"         // vset rd -> scalar
                       "    vmv.v.i v2, 1\n"
                       "    ld    t1, 0(s0)\n<LOAD_GAP>"
                       "    vadd.vx v1, v2, t1\n"       // load -> vector .vx
                       "    ld    t2, 8(s0)\n<LOAD_GAP>"
                       "    vle32.v v3, (t2)\n"         // load -> vector base address
                       "    ld    t3, 16(s0)\n"
                       "    li    t6, 0xd0\n"
                       "    vsetvl t5, t3, t6\n"        // load (two back) -> AVL; vl = 3
                       "    vmv.x.s a2, v3\n"
                       "    add   a3, a2, a2\n"         // vmv.x.s -> scalar
                       "    li    s2, 20\n"
                       "    vmseq.vx v0, v3, s2\n"
                       "    vcpop.m a0, v0\n"
                       "    addi  a1, a0, 100\n"        // vcpop.m -> scalar
                       "    vfirst.m a4, v0\n"
                       "    slli  a5, a4, 2\n"          // vfirst.m -> scalar
                       "    vredsum.vs v4, v3, v2\n"
                       "    vmv.x.s a6, v4\n"
                       "    sw    a6, 0(s0)\n"          // vector-derived value -> store
                       "    lw    a7, 0(s0)\n<LOAD_GAP>"
                       "    vadd.vx v5, v3, a7\n"       // load -> vector .vx
                       "    vsetivli s1, 2, e64, m1, ta, ma\n"
                       "    vle64.v v6, (s0)\n"
                       "    sd    s1, 24(s0)\n"         // vset rd -> store
                       "    nop\n    nop\n    nop\n";

std::string WithLoadGap(bool gap)
{
    std::string text = kProgram;
    const std::string marker = "<LOAD_GAP>";
    for (size_t at; (at = text.find(marker)) != std::string::npos;)
    {
        text.replace(at, marker.size(), gap ? "    nop\n" : "");
    }
    return text;
}

struct FinalState
{
    std::vector<uint64_t> gprs;
    std::vector<std::vector<uint8_t>> vregs;
    uint64_t vl = 0, vtype = 0;
    std::vector<uint8_t> data;
};

template <typename Vm> FinalState RunProgram(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    Vm vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.DebugRun();
    FinalState state;
    for (size_t r = 0; r < 32; ++r)
    {
        state.gprs.push_back(vm.registers_.ReadGpr(r));
        state.vregs.push_back(vm.registers_.ReadVectorRegister(r));
    }
    state.vl = vm.registers_.ReadCsr(RegisterFile::CSR_VL);
    state.vtype = vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE);
    for (uint64_t i = 0; i < 64; ++i)
    {
        state.data.push_back(vm.memory_controller_.peekByte(0x10000000 + i));
    }
    return state;
}

void ExpectSameState(const FinalState &expected, const FinalState &actual)
{
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
    EXPECT_EQ(actual.data, expected.data);
}
} // namespace

TEST(VectorPipelineDifferentialTest, ReferenceValuesOnTheSingleCycleVm)
{
    // Pin the reference itself, so a single-cycle regression cannot hide behind equality.
    const FinalState ref = RunProgram<Kites::RVSSProcessor>(WithLoadGap(false));
    EXPECT_EQ(ref.gprs[29], 8u);   // t4 = vl * 2
    EXPECT_EQ(ref.gprs[30], 3u);   // t5 = vl from vsetvl with AVL 3
    EXPECT_EQ(ref.gprs[13], 20u);  // a3 = 2 * v3[0]
    EXPECT_EQ(ref.gprs[11], 101u); // a1 = vcpop + 100
    EXPECT_EQ(ref.gprs[15], 4u);   // a5 = vfirst << 2
    EXPECT_EQ(ref.gprs[17], 61u);  // a7 = 1 + 10 + 20 + 30 through memory
    EXPECT_EQ(ref.gprs[9], 2u);    // s1 = vl at e64
}

TEST(VectorPipelineDifferentialTest, HazardDetectingPipelinesMatchWithoutHelp)
{
    const std::string source = WithLoadGap(false);
    const FinalState ref = RunProgram<Kites::RVSSProcessor>(source);
    {
        SCOPED_TRACE("H_F");
        ExpectSameState(ref, RunProgram<Kites::RV5StageProcessorHF>(source));
    }
    {
        SCOPED_TRACE("H_NF");
        ExpectSameState(ref, RunProgram<Kites::RV5StageProcessorHNF>(source));
    }
}

TEST(VectorPipelineDifferentialTest, ForwardingWithoutHazardDetectionNeedsALoadUseGap)
{
    const std::string source = WithLoadGap(true);
    const FinalState ref = RunProgram<Kites::RVSSProcessor>(source);
    SCOPED_TRACE("NH_F");
    ExpectSameState(ref, RunProgram<Kites::RV5StageProcessorNHF>(source));
}
