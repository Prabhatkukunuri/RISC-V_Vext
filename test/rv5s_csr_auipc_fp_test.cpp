// Regression tests for three 5-stage pipeline fixes, each checked against the single-cycle VM:
//   1. Zicsr instructions (csrrw/csrrs/csrrc and the immediate forms) execute in EX: they used to
//      be no-ops on every pipeline (execute_csr() was an empty stub).
//   2. auipc adds the PC (it used to give 0, which broke every `la`).
//   3. On the forwarding pipelines, FP loads/stores and int-to-FP moves get their GPR operand
//      forwarded from the instruction just ahead (it used to be read stale from the register file).

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
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

struct Snapshot
{
    std::vector<uint64_t> gprs, fprs;
    std::vector<std::vector<uint8_t>> vregs;
    std::vector<uint64_t> csrs;
    std::vector<uint8_t> data;
};

const size_t kCsrs[] = {0x001, 0x002, 0x003, RegisterFile::CSR_VSTART, RegisterFile::CSR_VL,
                        RegisterFile::CSR_VTYPE};

template <typename Vm> Snapshot RunProgram(const std::string &source, Vm *keep = nullptr)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    Vm local;
    Vm &vm = keep ? *keep : local;
    vm.LoadProgram(Kites::assemble(stream));
    vm.DebugRun();
    Snapshot s;
    for (size_t r = 0; r < 32; ++r)
    {
        s.gprs.push_back(vm.registers_.ReadGpr(r));
        s.fprs.push_back(vm.registers_.ReadFpr(r));
        s.vregs.push_back(vm.registers_.ReadVectorRegister(r));
    }
    for (size_t csr : kCsrs)
    {
        s.csrs.push_back(vm.registers_.ReadCsr(csr));
    }
    for (uint64_t i = 0; i < 64; ++i)
    {
        s.data.push_back(vm.memory_controller_.peekByte(0x10000000 + i));
    }
    return s;
}

void ExpectSame(const Snapshot &expected, const Snapshot &actual)
{
    for (size_t r = 1; r < 32; ++r)
    {
        EXPECT_EQ(actual.gprs[r], expected.gprs[r]) << "x" << r;
    }
    for (size_t r = 0; r < 32; ++r)
    {
        EXPECT_EQ(actual.fprs[r], expected.fprs[r]) << "f" << r;
        EXPECT_EQ(actual.vregs[r], expected.vregs[r]) << "v" << r;
    }
    for (size_t i = 0; i < std::size(kCsrs); ++i)
    {
        EXPECT_EQ(actual.csrs[i], expected.csrs[i]) << "CSR 0x" << std::hex << kCsrs[i];
    }
    EXPECT_EQ(actual.data, expected.data);
}

// Every Zicsr form, with back-to-back use of both the CSR result (rd) and the CSR operand (rs1).
const char *kCsrProgram = "    li     t0, 3\n"
                          "    csrrw  t1, frm, t0\n"     // rs1 from the instruction just ahead
                          "    addi   t2, t1, 10\n"      // rd used right away: old frm = 0
                          "    csrrs  t3, frm, x0\n"     // read only: 3
                          "    li     t0, 0x1f\n"
                          "    csrrs  t4, fflags, t0\n"  // set all flag bits
                          "    csrrc  t5, fflags, t0\n"  // read 0x1f, clear them again
                          "    csrrwi a0, fflags, 2\n"   // fflags is 0 again here
                          "    csrrsi a1, fflags, 1\n"   // 2 | 1
                          "    csrrci a2, fflags, 2\n"   // 3 & ~2
                          "    csrrs  a3, fflags, x0\n"  // 1
                          "    csrrsi a4, fflags, 4\n"   // 1 | 4
                          "    csrrci a5, fflags, 0\n"   // uimm 0: read only, no write
                          "    csrrs  a6, fcsr, x0\n"    // plain read (see the reference test)
                          "    vsetivli s0, 4, e32, m1, ta, ma\n"
                          "    csrrs  s1, vl, x0\n"      // vl right after vsetivli: 4
                          "    csrrs  s2, vtype, x0\n"   // 0xd0
                          "    csrrs  s3, vlenb, x0\n"   // 16
                          "    vmv.v.i v1, 7\n"
                          "    csrrwi x0, vstart, 2\n"   // a vector op right after: starts at 2
                          "    vadd.vi v2, v1, 1\n"
                          "    csrrs  s4, vstart, x0\n"  // the vector op cleared vstart
                          "    nop\n    nop\n    nop\n";
} // namespace

TEST(Rv5sCsrTest, ReferenceValuesOnTheSingleCycleVm)
{
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(kCsrProgram);
    EXPECT_EQ(ref.gprs[6], 0u);     // t1: old frm
    EXPECT_EQ(ref.gprs[7], 10u);    // t2
    EXPECT_EQ(ref.gprs[28], 3u);    // t3: frm
    EXPECT_EQ(ref.gprs[30], 0x1fu); // t5: fflags before the clear
    EXPECT_EQ(ref.gprs[11], 2u);    // a1: fflags before |= 1
    EXPECT_EQ(ref.gprs[13], 1u);    // a3: fflags after the clear
    EXPECT_EQ(ref.gprs[15], 5u);    // a5: fflags after |= 4; uimm 0 leaves it
    // a6 reads fcsr. The spec defines it as frm << 5 | fflags, but Kites stores fcsr, frm and
    // fflags as separate CSRs, so it is not pinned here; the pipelines only have to match.
    EXPECT_EQ(ref.gprs[9], 4u);     // s1: vl
    EXPECT_EQ(ref.gprs[18], 0xd0u); // s2: vtype
    EXPECT_EQ(ref.gprs[19], 16u);   // s3: vlenb
    EXPECT_EQ(ref.gprs[20], 0u);    // s4: vstart after the vector op
}

TEST(Rv5sCsrTest, PipelinesWithForwardingOrStallsMatchTheSingleCycleVm)
{
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(kCsrProgram);
    {
        SCOPED_TRACE("H_F");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHF>(kCsrProgram));
    }
    {
        SCOPED_TRACE("H_NF");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHNF>(kCsrProgram));
    }
    {
        SCOPED_TRACE("NH_F");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorNHF>(kCsrProgram));
    }
}

TEST(Rv5sCsrTest, PipelineWithoutForwardingMatchesWhenSpaced)
{
    // NH_NF has neither forwarding nor stalls: dependent instructions need 3 slots in between.
    std::string spaced;
    std::istringstream lines(kCsrProgram);
    for (std::string line; std::getline(lines, line);)
    {
        spaced += line + "\n    nop\n    nop\n    nop\n";
    }
    SCOPED_TRACE("NH_NF");
    ExpectSame(RunProgram<Kites::RVSSProcessor>(spaced), RunProgram<Kites::RV5StageProcessorNHNF>(spaced));
}

TEST(Rv5sCsrTest, WritingAReadOnlyCsrIsIllegalOnEveryPipeline)
{
    const std::string program = "    li t0, 1\n    nop\n    nop\n    nop\n"
                                "    csrrw x0, vlenb, t0\n    nop\n    nop\n    nop\n";
    EXPECT_THROW(RunProgram<Kites::RVSSProcessor>(program), std::runtime_error);
    EXPECT_THROW(RunProgram<Kites::RV5StageProcessorHF>(program), std::runtime_error);
    EXPECT_THROW(RunProgram<Kites::RV5StageProcessorHNF>(program), std::runtime_error);
    EXPECT_THROW(RunProgram<Kites::RV5StageProcessorNHF>(program), std::runtime_error);
    EXPECT_THROW(RunProgram<Kites::RV5StageProcessorNHNF>(program), std::runtime_error);
    // Reading one is fine.
    EXPECT_NO_THROW(RunProgram<Kites::RV5StageProcessorHF>("csrrs t1, vlenb, x0\nnop\nnop\nnop\n"));
}

TEST(Rv5sCsrTest, UndoRestoresCsrsWrittenInThePipeline)
{
    Kites::RV5StageProcessorHF vm;
    RunProgram<Kites::RV5StageProcessorHF>(kCsrProgram, &vm);
    ASSERT_EQ(vm.registers_.ReadCsr(0x001), 5u); // fflags
    ASSERT_EQ(vm.registers_.ReadCsr(0x002), 3u);
    for (uint64_t cycles = vm.cycle_s_; cycles > 0; --cycles)
    {
        vm.Undo();
    }
    EXPECT_EQ(vm.registers_.ReadCsr(0x001), 0u); // fflags
    EXPECT_EQ(vm.registers_.ReadCsr(0x002), 0u);
    EXPECT_EQ(vm.registers_.ReadGpr(6), 0u);
}

namespace
{
// `la` (auipc + addi) feeding loads, and FP loads/stores right after their base is computed.
const char *kAuipcFpProgram = ".data\n"
                              "w:    .dword 0x4045000000000000, 7\n" // 42.0 as a double
                              "f32:  .word 0x40490fdb\n"             // 3.14159 as a float
                              "out:  .dword 0, 0\n"
                              ".text\n"
                              "    la    t0, w\n"
                              "    ld    t1, 8(t0)\n"            // la -> load: 7
                              "    auipc t2, 0\n"                // this instruction's PC
                              "    la    t3, f32\n"
                              "    flw   f1, 0(t3)\n"            // FP load right after its base
                              "    addi  t4, t0, 0\n"
                              "    fld   f2, 0(t4)\n"
                              "    la    t5, out\n"
                              "    fsd   f2, 0(t5)\n"            // FP store right after its base
                              "    addi  t6, t5, 8\n"
                              "    fsw   f1, 0(t6)\n"
                              "    nop\n    nop\n    nop\n";
} // namespace

TEST(Rv5sAuipcFpTest, ReferenceValuesOnTheSingleCycleVm)
{
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(kAuipcFpProgram);
    EXPECT_EQ(ref.gprs[5], 0x10000000u); // t0 = &w
    EXPECT_EQ(ref.gprs[6], 7u);          // t1
    EXPECT_EQ(ref.gprs[7], 12u);         // t2 = PC of the auipc (after la's two instructions + ld)
    EXPECT_EQ(ref.fprs[2], 0x4045000000000000u);
    EXPECT_EQ(ref.data[0x18], 0x00u);    // out[0] = 42.0: low byte 0
    EXPECT_EQ(ref.data[0x1F], 0x40u);    //               high byte 0x40
}

TEST(Rv5sAuipcFpTest, PipelinesMatchTheSingleCycleVm)
{
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(kAuipcFpProgram);
    {
        SCOPED_TRACE("H_F");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHF>(kAuipcFpProgram));
    }
    {
        SCOPED_TRACE("H_NF");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHNF>(kAuipcFpProgram));
    }
    {
        SCOPED_TRACE("NH_F");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorNHF>(kAuipcFpProgram));
    }
}

namespace
{
// Integer <-> FP register dependencies through the forwarding paths:
//  - an integer load feeding the base address of an FP load (load-use);
//  - an FP result in f9 must not be taken for x9 by an integer instruction right after;
//  - FP compare / convert / move results go to x registers and are forwarded as such;
//  - an integer result feeding fcvt.s.w / fmv.w.x right away;
//  - an FMA whose rs3 field makes bits 31:25 look like feq's funct7 still writes an FPR.
// "<LOAD_GAP>" marks where NH_F (no hazard detection) needs a free slot after a load.
const char *kIntFpProgram = ".data\n"
                            "ptr:  .dword 0x10000010\n"           // address of val
                            "pad:  .dword 0\n"
                            "val:  .word 0x40490fdb\n"            // 3.14159f
                            ".text\n"
                            "    lui    a0, 0x10000\n"
                            "    li     s1, 100\n"                // x9 = 100
                            "    li     t1, 3\n"
                            "    fcvt.s.w f1, t1\n"               // f1 = 3.0 (t1 one ahead)
                            "    ld     a1, 0(a0)\n<LOAD_GAP>"    // a1 = &val
                            "    flw    f2, 0(a1)\n<LOAD_GAP>"    // base from the load: 3.14159
                            "    fadd.s f9, f1, f1\n"             // writes f9, not x9
                            "    addi   s2, s1, 1\n"              // x18 = x9 + 1 = 101
                            "    feq.s  a2, f1, f1\n"             // a2 = 1 (an x register)
                            "    addi   a3, a2, 10\n"             // 11, forwarded from feq
                            "    fcvt.s.w f6, a3\n"               // f6 = 11.0, a3 one ahead
                            "    fmv.x.w a4, f6\n"                // a4 = bits of 11.0
                            "    addi   a5, a4, 0\n"              // forwarded from fmv.x.w
                            "    fmadd.s f19, f1, f1, f20\n"      // rs3 = 20: bits 31:25 = 1010000
                            "    addi   s4, s3, 0\n"              // x19 (s3) must still be 0
                            "    nop\n    nop\n    nop\n";

std::string WithLoadGap(const std::string &program, const std::string &gap)
{
    std::string out = program;
    for (size_t at = out.find("<LOAD_GAP>"); at != std::string::npos;
         at = out.find("<LOAD_GAP>", at))
    {
        out.replace(at, 10, gap);
    }
    return out;
}
} // namespace

TEST(Rv5sIntFpForwardingTest, ReferenceValuesOnTheSingleCycleVm)
{
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(WithLoadGap(kIntFpProgram, ""));
    EXPECT_EQ(ref.fprs[2] & 0xFFFFFFFFu, 0x40490fdbu); // f2 = 3.14159f
    EXPECT_EQ(ref.gprs[18], 101u);                    // s2 = x9 + 1, not the bits of f9
    EXPECT_EQ(ref.gprs[13], 11u);                     // a3 = feq + 10
    EXPECT_EQ(ref.gprs[15], 0x41300000u);             // a5 = bits of 11.0f
    EXPECT_EQ(ref.gprs[19], 0u);                      // s3 untouched by the FMA (f19 only)
    EXPECT_EQ(ref.gprs[20], 0u);                      // s4 = s3
}

TEST(Rv5sIntFpForwardingTest, PipelinesMatchTheSingleCycleVm)
{
    const std::string program = WithLoadGap(kIntFpProgram, "");
    const Snapshot ref = RunProgram<Kites::RVSSProcessor>(program);
    {
        SCOPED_TRACE("H_F");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHF>(program));
    }
    {
        SCOPED_TRACE("H_NF");
        ExpectSame(ref, RunProgram<Kites::RV5StageProcessorHNF>(program));
    }
    {
        SCOPED_TRACE("NH_F, with a free slot after each load");
        const std::string spaced = WithLoadGap(kIntFpProgram, "    nop\n");
        ExpectSame(RunProgram<Kites::RVSSProcessor>(spaced),
                   RunProgram<Kites::RV5StageProcessorNHF>(spaced));
    }
}
