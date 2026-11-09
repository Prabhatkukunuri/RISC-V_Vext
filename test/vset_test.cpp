// Tests for the RVV configuration-setting instructions: vsetvli, vsetivli and vsetvl.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding);
// the vl/vtype rules follow RVV 1.0 section 6 and Spike's set_vl().

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "common/instructions.h"
#include "config/config.h"
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
namespace rvv = Kites::rvv;

constexpr uint64_t VLEN = 128; // vm_config default

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

template <typename Vm> std::unique_ptr<Vm> runOn(const std::string &source)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(assembleSource(source));
    vm->DebugRun();
    return vm;
}

uint64_t vl(const Kites::ProcessorBase &vm)
{
    return vm.registers_.ReadCsr(RegisterFile::CSR_VL);
}

uint64_t vtype(const Kites::ProcessorBase &vm)
{
    return vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VsetAssemblerTest, EncodingsMatchLlvmMc)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vsetvli x5, x10, e32, m1, ta, ma", 0x0d0572d7},
        {"vsetvli x0, x0, e8, mf8, tu, mu", 0x00507057},
        {"vsetvli x1, x0, e64, m8, ta, mu", 0x05b070d7},
        {"vsetvli x31, x2, e16, mf2, tu, ma", 0x08f17fd7},
        {"vsetvli x3, x4, 0x7ff", 0x7ff271d7},
        {"vsetivli x6, 31, e32, m4, ta, ma", 0xcd2ff357},
        {"vsetivli x0, 0, e8, m1, tu, mu", 0xc0007057},
        {"vsetivli x7, 5, 0x3ff", 0xfff2f3d7},
        {"vsetvl x8, x9, x10", 0x80a4f457},
        {"vsetvl x0, x0, x31", 0x81f07057},
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

TEST(VsetAssemblerTest, AbiRegisterNamesAndDefaults)
{
    // Omitted fields default to m1, tu, mu (vtypei 0x010 for e32).
    const Kites::AssembledProgram program = assembleSource("vsetvli t0, a0, e32, m1, ta, ma\n"
                                                           "vsetvli t0, a0, e32\n"
                                                           "vsetvli t0, a0, e32, m2\n"
                                                           "vsetvli t0, a0, e32, ta\n"
                                                           "vsetvli t0, a0, e32, ma\n");
    ASSERT_EQ(program.text_buffer.size(), 5u);
    EXPECT_EQ(program.text_buffer[0], 0x0d0572d7u);
    EXPECT_EQ(rvv::VsetImmediateVtype(program.text_buffer[1]), 0x010u);
    EXPECT_EQ(rvv::VsetImmediateVtype(program.text_buffer[2]), 0x011u);
    EXPECT_EQ(rvv::VsetImmediateVtype(program.text_buffer[3]), 0x050u);
    EXPECT_EQ(rvv::VsetImmediateVtype(program.text_buffer[4]), 0x090u);
}

TEST(VsetAssemblerTest, MalformedOperandsAreRejected)
{
    const std::vector<std::string> bad = {
        "vsetvli x1, x2, e128\n",               // SEW not in Zve64x's set
        "vsetvli x1, x2, m1, e32\n",            // SEW must come first
        "vsetvli x1, x2, e32, m1, ma, ta\n",    // policies out of order
        "vsetvli x1, x2, e32, m3\n",            // no such LMUL
        "vsetvli x1, x2, e32 m1\n",             // missing comma
        "vsetvli x1, x2, e32,\n",               // trailing comma
        "vsetvli x1, x2\n",                     // no vtype
        "vsetvli x1, x2, 2048\n",               // vtypei is 11 bits
        "vsetivli x1, 32, e8\n",                // uimm is 5 bits
        "vsetivli x1, 4, 1024\n",               // vtypei is 10 bits
        "vsetivli x1, x2, e8\n",                // AVL must be an immediate
        "vsetvl x1, x2, e8\n",                  // vsetvl takes rs2, not a vtype
    };
    for (const std::string &line : bad)
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

TEST(VsetAssemblerTest, DisassemblyRoundTrips)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x0d0572d7), "vsetvli x5, x10, e32, m1, ta, ma");
    EXPECT_EQ(disassemble(0x00507057), "vsetvli x0, x0, e8, mf8, tu, mu");
    EXPECT_EQ(disassemble(0x7ff271d7), "vsetvli x3, x4, 0x7ff");
    EXPECT_EQ(disassemble(0xcd2ff357), "vsetivli x6, 31, e32, m4, ta, ma");
    EXPECT_EQ(disassemble(0x80a4f457), "vsetvl x8, x9, x10");
}

// ---------------------------------------------------------------------------------------------
// vtype / vl rules
// ---------------------------------------------------------------------------------------------

TEST(VsetRulesTest, VlmaxForEveryLegalSewLmul)
{
    // VLMAX = LMUL * VLEN / SEW, VLEN = 128.
    struct Case
    {
        uint64_t sew;
        int lmulLog2;
        uint64_t vlmax;
    };
    const Case cases[] = {
        {8, 0, 16},  {8, 3, 128}, {8, -3, 2},  {16, 1, 16}, {16, -2, 2},
        {32, 0, 4},  {32, 2, 16}, {32, -1, 2}, {64, 0, 2},  {64, 3, 16},
    };
    for (const Case &c : cases)
    {
        const uint64_t type = rvv::EncodeVtype(c.sew, c.lmulLog2, true, true);
        const auto result = rvv::ComputeVset(type, ~0ULL, false, false, 0, VLEN);
        EXPECT_EQ(result.vtype, type) << "e" << c.sew << " lmulLog2=" << c.lmulLog2;
        EXPECT_EQ(result.vl, c.vlmax) << "e" << c.sew << " lmulLog2=" << c.lmulLog2;
    }
}

TEST(VsetRulesTest, UnsupportedVtypeSetsVill)
{
    const uint64_t illegal[] = {
        0x04,                     // vlmul = 100 is reserved
        0x20,                     // vsew = 100 (e128) > ELEN
        rvv::EncodeVtype(64, -1, false, false), // e64 needs LMUL >= 1 when ELEN = 64
        rvv::EncodeVtype(16, -3, false, false), // e16, mf8: SEW > LMUL * ELEN
        0x100,                    // reserved bit 8
        RegisterFile::VTYPE_VILL, // vill itself is a reserved bit for a requested vtype
    };
    for (uint64_t type : illegal)
    {
        const auto result = rvv::ComputeVset(type, 5, false, false, 3, VLEN);
        EXPECT_EQ(result.vtype, RegisterFile::VTYPE_VILL) << std::hex << type;
        EXPECT_EQ(result.vl, 0u) << std::hex << type;
    }
    // The smallest legal fractional cases.
    rvv::Vtype decoded;
    EXPECT_TRUE(rvv::DecodeVtype(rvv::EncodeVtype(8, -3, false, false), decoded));
    EXPECT_TRUE(rvv::DecodeVtype(rvv::EncodeVtype(32, -1, false, false), decoded));
}

TEST(VsetRulesTest, AvlSelection)
{
    const uint64_t e32m1 = rvv::EncodeVtype(32, 0, true, true); // VLMAX 4

    // rs1 != x0: vl = min(AVL, VLMAX).
    EXPECT_EQ(rvv::ComputeVset(e32m1, 3, false, false, 0, VLEN).vl, 3u);
    EXPECT_EQ(rvv::ComputeVset(e32m1, 4, false, false, 0, VLEN).vl, 4u);
    EXPECT_EQ(rvv::ComputeVset(e32m1, 100, false, false, 0, VLEN).vl, 4u);
    EXPECT_EQ(rvv::ComputeVset(e32m1, 0, false, false, 2, VLEN).vl, 0u);

    // rs1 = x0, rd != x0: vl = VLMAX.
    EXPECT_EQ(rvv::ComputeVset(e32m1, 1, false, true, 0, VLEN).vl, 4u);

    // rs1 = rd = x0: keep vl, capped to the new VLMAX.
    EXPECT_EQ(rvv::ComputeVset(e32m1, 0, true, true, 3, VLEN).vl, 3u);
    EXPECT_EQ(rvv::ComputeVset(e32m1, 0, true, true, 9, VLEN).vl, 4u);
}

// ---------------------------------------------------------------------------------------------
// Single-cycle VM
// ---------------------------------------------------------------------------------------------

TEST(VsetRvssTest, AllThreeFormsUpdateRdVlAndVtype)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 10\n"
                                          "vsetvli x5, x10, e32, m1, ta, ma\n" // VLMAX 4
                                          "csrrs x20, vl, x0\n"
                                          "csrrs x21, vtype, x0\n"
                                          "vsetivli x6, 3, e8, mf2, tu, mu\n"  // VLMAX 8
                                          "li x11, 0x58\n"                     // e64, m1, ta, mu
                                          "vsetvl x7, x0, x11\n"               // VLMAX 2
                                          "vsetvli x0, x0, e32, m1, ta, ma\n"); // keep vl = 2
    EXPECT_EQ(vm->registers_.ReadGpr(5), 4u);
    EXPECT_EQ(vm->registers_.ReadGpr(20), 4u);
    EXPECT_EQ(vm->registers_.ReadGpr(21), 0xd0u);
    EXPECT_EQ(vm->registers_.ReadGpr(6), 3u);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 2u);
    EXPECT_EQ(vl(*vm), 2u);
    EXPECT_EQ(vtype(*vm), 0xd0u);
}

TEST(VsetRvssTest, IllegalVtypeSetsVillAndZeroesVl)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x5, 4, e8, m1, ta, ma\n"
                                          "li x10, 5\n"
                                          "li x11, 4\n" // vlmul = 100, reserved
                                          "vsetvl x6, x10, x11\n");
    EXPECT_EQ(vm->registers_.ReadGpr(5), 4u);
    EXPECT_EQ(vm->registers_.ReadGpr(6), 0u);
    EXPECT_EQ(vl(*vm), 0u);
    EXPECT_EQ(vtype(*vm), RegisterFile::VTYPE_VILL);
}

TEST(VsetRvssTest, VsetClearsVstart)
{
    auto vm = runOn<Kites::RVSSProcessor>("csrrwi x0, vstart, 5\n"
                                          "vsetivli x0, 1, e8, m1, ta, ma\n");
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VsetRvssTest, VsetvlWithX0RdAndRs1KeepsVl)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "li x11, 0xd0\n"
                                          "vsetvl x0, x0, x11\n");
    EXPECT_EQ(vl(*vm), 3u);
    EXPECT_EQ(vtype(*vm), 0xd0u);
}

TEST(VsetRvssTest, UndoRestoresRdVlVtypeAndVstart)
{
    auto vm = runOn<Kites::RVSSProcessor>("csrrwi x0, vstart, 2\n"
                                          "li x10, 7\n"
                                          "vsetvli x5, x10, e8, m1, ta, ma\n");
    EXPECT_EQ(vl(*vm), 7u);

    vm->Undo();
    EXPECT_EQ(vm->registers_.ReadGpr(5), 0u);
    EXPECT_EQ(vl(*vm), 0u);
    EXPECT_EQ(vtype(*vm), RegisterFile::VTYPE_VILL);
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 2u);

    vm->Redo();
    EXPECT_EQ(vm->registers_.ReadGpr(5), 7u);
    EXPECT_EQ(vl(*vm), 7u);
    EXPECT_EQ(vtype(*vm), 0xc0u);
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VsetRvssTest, VlmaxFollowsConfiguredVlen)
{
    const uint64_t saved = Kites::vm_config::config.getVlen();
    Kites::vm_config::config.setVlen(512);
    auto vm = runOn<Kites::RVSSProcessor>("vsetvli x5, x0, e8, m8, ta, ma\n");
    Kites::vm_config::config.setVlen(saved);
    EXPECT_EQ(vm->registers_.ReadGpr(5), 512u); // 8 * 512 / 8
}

// ---------------------------------------------------------------------------------------------
// Pipelined VMs
// ---------------------------------------------------------------------------------------------

namespace
{
// Every dependency is three instructions apart, so even the variants without hazard
// detection read up-to-date registers.
const char *kSpacedProgram = "li x10, 10\n"
                             "li x11, 0x58\n"
                             "nop\nnop\nnop\n"
                             "vsetvli x5, x10, e32, m1, ta, ma\n"
                             "vsetivli x6, 3, e8, mf2, tu, mu\n"
                             "vsetvl x7, x0, x11\n"
                             "vsetvli x0, x0, e32, m1, ta, ma\n"
                             "nop\nnop\nnop\n";

template <typename Vm> void expectSpacedProgramResult()
{
    auto vm = runOn<Vm>(kSpacedProgram);
    EXPECT_EQ(vm->registers_.ReadGpr(5), 4u);
    EXPECT_EQ(vm->registers_.ReadGpr(6), 3u);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 2u);
    EXPECT_EQ(vl(*vm), 2u);
    EXPECT_EQ(vtype(*vm), 0xd0u);
}
} // namespace

TEST(VsetPipelineTest, SpacedProgramOnEveryVariant)
{
    expectSpacedProgramResult<Kites::RVSSProcessor>();
    expectSpacedProgramResult<Kites::RV5StageProcessorHF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorHNF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorNHF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorNHNF>();
}

namespace
{
// Back-to-back dependencies through rs1, through rd, and through vl itself, plus a load-use.
const char *kDependentProgram = ".data\n"
                                "avl: .dword 9\n"
                                ".text\n"
                                "li x10, 7\n"
                                "vsetvli x5, x10, e8, m1, ta, ma\n" // x10 forwarded: vl = 7
                                "addi x6, x5, 1\n"                  // vl forwarded: 8
                                "vsetvli x0, x0, e16, m1, ta, ma\n" // VLMAX 8, keeps vl 7
                                "vsetvli x7, x0, e64, m1, ta, ma\n" // VLMAX 2
                                "add x8, x7, x7\n"                  // 4
                                // .data starts at 0x10000000. lui, not la: the pipelines
                                // don't execute auipc correctly yet.
                                "lui x13, 0x10000\n"
                                "ld x12, 0(x13)\n"
                                "vsetvli x9, x12, e8, m1, ta, ma\n"; // load-use: 9

template <typename Vm> void expectDependentProgramResult()
{
    auto vm = runOn<Vm>(kDependentProgram);
    EXPECT_EQ(vm->registers_.ReadGpr(5), 7u);
    EXPECT_EQ(vm->registers_.ReadGpr(6), 8u);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 2u);
    EXPECT_EQ(vm->registers_.ReadGpr(8), 4u);
    EXPECT_EQ(vm->registers_.ReadGpr(9), 9u);
    EXPECT_EQ(vl(*vm), 9u);
    EXPECT_EQ(vtype(*vm), 0xc0u);
}
} // namespace

TEST(VsetPipelineTest, DependentProgramWithHazardDetection)
{
    expectDependentProgramResult<Kites::RVSSProcessor>();
    expectDependentProgramResult<Kites::RV5StageProcessorHF>();
    expectDependentProgramResult<Kites::RV5StageProcessorHNF>();
}

TEST(VsetPipelineTest, UndoToStartRestoresVectorCsrs)
{
    auto vm = runOn<Kites::RV5StageProcessorHF>(kDependentProgram);
    ASSERT_EQ(vl(*vm), 9u);
    // One undo per clock cycle walks back to the start.
    for (uint64_t cycles = vm->cycle_s_; cycles > 0; --cycles)
    {
        vm->Undo();
    }
    EXPECT_EQ(vm->cycle_s_, 0u);
    EXPECT_EQ(vl(*vm), 0u);
    EXPECT_EQ(vtype(*vm), RegisterFile::VTYPE_VILL);
    EXPECT_EQ(vm->registers_.ReadGpr(5), 0u);
}

