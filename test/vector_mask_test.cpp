// Tests for the RVV mask instructions (section 15): the vm*.mm logical ops and their pseudos
// (vmmv.m, vmnot.m, vmclr.m, vmset.m), vcpop.m, vfirst.m, vmsbf.m, vmsif.m, vmsof.m, viota.m
// and vid.v. Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x).
// Rules under test: mask bits are one per element; tail and masked-off bits/elements are left
// undisturbed; vcpop/vfirst write x[rd]; vstart != 0 is illegal for vcpop, vfirst, vms*f and
// viota; vms*f and viota may not overlap their source mask; masked vms*f/viota/vid cannot write v0.

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
uint64_t maskByte(const RegisterFile &rf, size_t reg, size_t byte = 0)
{
    return rf.ReadVectorElement(reg, byte, 8);
}

void executeRaw(Kites::RVSSProcessor &vm, uint32_t word)
{
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    Kites::rvv::ExecuteVectorInstruction(word, 0, 0, vm.registers_, vm.memory_controller_,
                                         changes, memoryChanges);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------

TEST(VectorMaskAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmandn.mm v1, v2, v3", 0x6221a0d7}, {"vmand.mm v1, v2, v3", 0x6621a0d7},
        {"vmor.mm v1, v2, v3", 0x6a21a0d7},   {"vmxor.mm v1, v2, v3", 0x6e21a0d7},
        {"vmorn.mm v1, v2, v3", 0x7221a0d7},  {"vmnand.mm v1, v2, v3", 0x7621a0d7},
        {"vmnor.mm v1, v2, v3", 0x7a21a0d7},  {"vmxnor.mm v1, v2, v3", 0x7e21a0d7},
        {"vcpop.m x10, v2", 0x42282557},      {"vcpop.m x10, v2, v0.t", 0x40282557},
        {"vfirst.m x10, v2", 0x4228a557},     {"vfirst.m x10, v2, v0.t", 0x4028a557},
        {"vmsbf.m v1, v2", 0x5220a0d7},       {"vmsbf.m v1, v2, v0.t", 0x5020a0d7},
        {"vmsif.m v1, v2", 0x5221a0d7},       {"vmsof.m v1, v2, v0.t", 0x502120d7},
        {"viota.m v4, v2", 0x52282257},       {"viota.m v4, v2, v0.t", 0x50282257},
        {"vid.v v4", 0x5208a257},             {"vid.v v4, v0.t", 0x5008a257},
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
    // The ABI name assembles to the same word.
    EXPECT_EQ(assembleSource("vcpop.m a0, v2\n").text_buffer.at(0), 0x42282557u);
}

TEST(VectorMaskAssemblerTest, PseudosMatchLlvmMc)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmmv.m v1, v2", 0x662120d7},  // vmand.mm v1, v2, v2
        {"vmnot.m v1, v2", 0x762120d7}, // vmnand.mm v1, v2, v2
        {"vmclr.m v1", 0x6e10a0d7},     // vmxor.mm v1, v1, v1
        {"vmset.m v1", 0x7e10a0d7},     // vmxnor.mm v1, v1, v1
    };
    for (const auto &[text, word] : cases)
    {
        const Kites::AssembledProgram program = assembleSource(text + "\n");
        ASSERT_EQ(program.text_buffer.size(), 1u) << text;
        EXPECT_EQ(program.text_buffer[0], word) << text;
    }
}

TEST(VectorMaskAssemblerTest, OperandRules)
{
    for (const std::string &line : {
             "vmand.mm v1, v2, v3, v0.t\n", // the .mm ops are never masked
             "vmsbf.m v2, v2\n",            // vd overlaps the source mask
             "viota.m v2, v2\n",
             "vmsbf.m v0, v2, v0.t\n", // a masked vd cannot be v0
             "viota.m v0, v2, v0.t\n",
             "vid.v v0, v0.t\n",
             "vcpop.m v1, v2\n",        // the destination is an x register
             "vid.v v4, v5\n",          // vid.v has no source
             "vmmv.m v1, v2, v0.t\n",   // pseudos are never masked
             "vmclr.m v1, v2\n",
         })
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------

TEST(VectorMaskTest, LogicalOpsWriteOnlyBitsBelowVl)
{
    // vs2 = 0xCC, vs1 = 0xAA; vl = 6, so bits 6 and 7 of each vd (0xC0) are tail.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 6, e8, m1, ta, ma\n"
                                          "vmandn.mm v10, v2, v3\n"
                                          "vmand.mm v11, v2, v3\n"
                                          "vmor.mm v12, v2, v3\n"
                                          "vmxor.mm v13, v2, v3\n"
                                          "vmorn.mm v14, v2, v3\n"
                                          "vmnand.mm v15, v2, v3\n"
                                          "vmnor.mm v16, v2, v3\n"
                                          "vmxnor.mm v17, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xCC});
                                              writeElements(rf, 3, 8, {0xAA});
                                              for (size_t r = 10; r <= 17; ++r)
                                              {
                                                  writeElements(rf, r, 8, {0xC0, 0xFF});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    const std::vector<std::pair<size_t, uint64_t>> expected = {
        {10, 0x04}, {11, 0x08}, {12, 0x2E}, {13, 0x26},
        {14, 0x1D}, {15, 0x37}, {16, 0x11}, {17, 0x19},
    };
    for (const auto &[reg, low6] : expected)
    {
        EXPECT_EQ(maskByte(rf, reg), 0xC0 | low6) << "v" << reg;
        EXPECT_EQ(maskByte(rf, reg, 1), 0xFFu) << "v" << reg;
    }
}

TEST(VectorMaskTest, Pseudoinstructions)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmmv.m v20, v2\n"
                                          "vmnot.m v21, v2\n"
                                          "vmclr.m v22\n"
                                          "vmset.m v23\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xCC});
                                              for (size_t r = 20; r <= 23; ++r)
                                              {
                                                  writeElements(rf, r, 8, {0x5A, 0x5A});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 20), 0xCCu);
    EXPECT_EQ(maskByte(rf, 21), 0x33u);
    EXPECT_EQ(maskByte(rf, 22), 0x00u);
    EXPECT_EQ(maskByte(rf, 23), 0xFFu);
    EXPECT_EQ(maskByte(rf, 22, 1), 0x5Au); // beyond vl = 8: tail
}

TEST(VectorMaskTest, CpopAndFirstWriteAnXRegister)
{
    // v2 = 0b10010100: set bits at 2, 4 and 7. v0 = 0b11110000.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vcpop.m a0, v2\n"
                                          "vfirst.m a1, v2\n"
                                          "vcpop.m a2, v2, v0.t\n"
                                          "vfirst.m a3, v2, v0.t\n"
                                          "vfirst.m a4, v5\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vcpop.m a5, v2\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0x94});
                                              writeElements(rf, 0, 8, {0xF0});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(rf.ReadGpr(10), 3u);
    EXPECT_EQ(rf.ReadGpr(11), 2u);
    EXPECT_EQ(rf.ReadGpr(12), 2u);                  // only bits 4 and 7 are active
    EXPECT_EQ(rf.ReadGpr(13), 4u);
    EXPECT_EQ(rf.ReadGpr(14), 0xFFFFFFFFFFFFFFFFu); // no set bit: -1
    EXPECT_EQ(rf.ReadGpr(15), 1u);                  // vl = 4: only bit 2 counts
}

TEST(VectorMaskTest, SetBeforeIncludingOnlyFirst)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmsbf.m v10, v3\n"
                                          "vmsif.m v11, v3\n"
                                          "vmsof.m v12, v3\n"
                                          "vmsbf.m v13, v4\n"         // no set bit at all
                                          "vmsbf.m v14, v3, v0.t\n",  // spec example
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 3, 8, {0x94}); // 1001 0100
                                              writeElements(rf, 0, 8, {0xC3}); // 1100 0011
                                              writeElements(rf, 14, 8, {0x3C});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 10), 0x03u);
    EXPECT_EQ(maskByte(rf, 11), 0x07u);
    EXPECT_EQ(maskByte(rf, 12), 0x04u);
    EXPECT_EQ(maskByte(rf, 13), 0xFFu);
    // Active elements 0, 1, 6, 7 give 1, 1, 1, 0; elements 2..5 keep 0x3C's bits.
    EXPECT_EQ(maskByte(rf, 14), 0x7Fu);
}

TEST(VectorMaskTest, IotaIsAPrefixSumOfActiveBits)
{
    // The spec's examples. v2 = 1 0 0 1 0 0 0 1 (elements 7..0).
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "viota.m v4, v2\n"
                                          "viota.m v5, v2, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0x91});
                                              writeElements(rf, 0, 8, {0xEB}); // 1110 1011
                                              writeElements(rf, 5, 8, {9, 8, 7, 6, 5, 4, 3, 2});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 8), (Elements{0, 1, 1, 1, 1, 2, 2, 2}));
    EXPECT_EQ(readElements(vm->registers_, 5, 8, 8), (Elements{0, 1, 7, 1, 5, 1, 1, 1}));
}

TEST(VectorMaskTest, IdWritesTheElementIndex)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 5, e16, m1, ta, ma\n"
                                          "vid.v v4\n"
                                          "vid.v v5, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b10101});
                                              writeElements(rf, 5, 16, Elements(6, 9));
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 5), (Elements{0, 1, 2, 3, 4}));
    EXPECT_EQ(readElements(vm->registers_, 5, 16, 6), (Elements{0, 9, 2, 9, 4, 9}));
}

TEST(VectorMaskTest, IllegalCasesTrap)
{
    expectIllegal("vcpop.m a0, v2\n");                                      // vtype.vill
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nviota.m v4, v5\n");     // vd group holds vs2
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nviota.m v5, v2\n");     // misaligned vd

    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n");
    EXPECT_THROW(executeRaw(*vm, 0x5208a257 | (3u << 20)), std::runtime_error); // vid, vs2 != 0
    EXPECT_THROW(executeRaw(*vm, 0x6621a0d7 & ~(1u << 25)), std::runtime_error); // vmand, vm = 0

    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 1); // vstart != 0
    EXPECT_THROW(executeRaw(*vm, 0x42282557), std::runtime_error); // vcpop.m
    EXPECT_THROW(executeRaw(*vm, 0x4228a557), std::runtime_error); // vfirst.m
    EXPECT_THROW(executeRaw(*vm, 0x5220a0d7), std::runtime_error); // vmsbf.m
    EXPECT_THROW(executeRaw(*vm, 0x52282257), std::runtime_error); // viota.m
    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 0);
    EXPECT_NO_THROW(executeRaw(*vm, 0x52282257));
}

TEST(VectorMaskTest, UndoRestoresDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "viota.m v4, v2\n",
                                          [](RegisterFile &rf) { writeElements(rf, 2, 8, {0x91}); });
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 8), (Elements{0, 1, 1, 1, 1, 2, 2, 2}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 8), Elements(8, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 8), (Elements{0, 1, 1, 1, 1, 2, 2, 2}));
}

// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
// The NH_NF pipeline has neither hazard detection nor forwarding, so (as in its scalar tests)
// nops separate vcpop.m from the instruction that reads its rd.
const char *kMaskProgram = "vsetivli x0, 8, e8, m1, ta, ma\n"
                           "vid.v v2\n"                  // [0, 1, ..., 7]
                           "vmsgtu.vi v3, v2, 4\n"       // elements 5, 6, 7: 0xE0
                           "vcpop.m a0, v3\n"            // 3
                           "nop\nnop\nnop\n"
                           "addi a1, a0, 10\n"           // 13
                           "vfirst.m a2, v3\n"           // 5
                           "viota.m v4, v3\n"            // [0,0,0,0,0,0,1,2]
                           "vmnot.m v5, v3\n"            // 0x1F
                           "vmmv.m v0, v3\n"
                           "vid.v v6, v0.t\n"            // [0,0,0,0,0,5,6,7]
                           "nop\nnop\nnop\n";

template <typename Vm> void expectMaskResult(const char *name)
{
    SCOPED_TRACE(name);
    auto vm = runOn<Vm>(kMaskProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 3), 0xE0u);
    EXPECT_EQ(rf.ReadGpr(10), 3u);
    EXPECT_EQ(rf.ReadGpr(11), 13u);
    EXPECT_EQ(rf.ReadGpr(12), 5u);
    EXPECT_EQ(readElements(rf, 4, 8, 8), (Elements{0, 0, 0, 0, 0, 0, 1, 2}));
    EXPECT_EQ(maskByte(rf, 5), 0x1Fu);
    EXPECT_EQ(readElements(rf, 6, 8, 8), (Elements{0, 0, 0, 0, 0, 5, 6, 7}));
}
} // namespace

TEST(VectorMaskPipelineTest, ProgramOnEveryVariant)
{
    expectMaskResult<Kites::RVSSProcessor>("RVSSProcessor");
    expectMaskResult<Kites::RV5StageProcessorHF>("RV5StageProcessorHF");
    expectMaskResult<Kites::RV5StageProcessorHNF>("RV5StageProcessorHNF");
    expectMaskResult<Kites::RV5StageProcessorNHF>("RV5StageProcessorNHF");
    expectMaskResult<Kites::RV5StageProcessorNHNF>("RV5StageProcessorNHNF");
}

namespace
{
// vcpop.m's rd read by the very next instruction: forwarded (F) or stalled for (H).
template <typename Vm> void expectBackToBackUse(const char *name)
{
    SCOPED_TRACE(name);
    auto vm = runOn<Vm>("vsetivli x0, 8, e8, m1, ta, ma\n"
                        "vid.v v2\n"
                        "vmsgtu.vi v3, v2, 4\n"
                        "vcpop.m a0, v3\n"
                        "addi a1, a0, 10\n"
                        "vfirst.m a2, v3\n"
                        "add a3, a2, a2\n"
                        "nop\nnop\nnop\n");
    EXPECT_EQ(vm->registers_.ReadGpr(11), 13u);
    EXPECT_EQ(vm->registers_.ReadGpr(13), 10u);
}
} // namespace

TEST(VectorMaskPipelineTest, RdIsForwardedOrStalledFor)
{
    expectBackToBackUse<Kites::RVSSProcessor>("RVSSProcessor");
    expectBackToBackUse<Kites::RV5StageProcessorHF>("RV5StageProcessorHF");
    expectBackToBackUse<Kites::RV5StageProcessorHNF>("RV5StageProcessorHNF");
    expectBackToBackUse<Kites::RV5StageProcessorNHF>("RV5StageProcessorNHF");
}
