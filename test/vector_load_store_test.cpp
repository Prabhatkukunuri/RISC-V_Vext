// Tests for the RVV 1.0 vector loads and stores (spec section 7): unit-stride, strided, indexed
// (ordered/unordered), mask, fault-only-first, segment and whole-register forms.
//
// Encodings are checked against the MATCH/MASK table of riscv-opcodes (extensions/rv_v), the
// table Spike and the GNU/LLVM toolchains are generated from. Element behaviour follows the spec
// and Spike: tail and masked-off elements are left undisturbed. VLEN is 128 (16 bytes/register).

#include <gtest/gtest.h>

#include <cstdint>
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
#include "processor/vector/vector_memory.h"
#include "utils/utils.h"

namespace
{
using Kites::RegisterFile;
using Elements = std::vector<uint64_t>;
using Bytes = std::vector<uint8_t>;

constexpr uint64_t kBase = 0x10000000; // "lui a0, 0x10000"

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

uint32_t assembleOne(const std::string &line)
{
    const Kites::AssembledProgram program = assembleSource(line + "\n");
    if (program.text_buffer.size() != 1)
    {
        throw std::runtime_error("expected one instruction from: " + line);
    }
    return program.text_buffer[0];
}

template <typename Vm> using Setup = std::function<void(Vm &)>;

// Loads the program, lets @p setup seed registers and memory, then runs it to the end.
template <typename Vm>
std::unique_ptr<Vm> runOn(const std::string &source, const Setup<Vm> &setup = {})
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(assembleSource(source));
    if (setup)
    {
        setup(*vm);
    }
    vm->DebugRun();
    return vm;
}

template <typename Vm> void writeBytes(Vm &vm, uint64_t address, const Bytes &bytes)
{
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        vm.memory_controller_.writeByte(address + i, bytes[i]);
    }
}

template <typename Vm> void writeWords(Vm &vm, uint64_t address, const Elements &words)
{
    for (size_t i = 0; i < words.size(); ++i)
    {
        vm.memory_controller_.writeWord(address + 4 * i, static_cast<uint32_t>(words[i]));
    }
}

template <typename Vm> Bytes readBytes(Vm &vm, uint64_t address, size_t count)
{
    Bytes bytes;
    for (size_t i = 0; i < count; ++i)
    {
        bytes.push_back(vm.memory_controller_.peekByte(address + i));
    }
    return bytes;
}

template <typename Vm> Elements readWords(Vm &vm, uint64_t address, size_t count)
{
    Elements words;
    for (size_t i = 0; i < count; ++i)
    {
        uint64_t word = 0;
        for (int b = 3; b >= 0; --b)
        {
            word = (word << 8) | vm.memory_controller_.peekByte(address + 4 * i + b);
        }
        words.push_back(word);
    }
    return words;
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

using Rvss = Kites::RVSSProcessor;

} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler and disassembler
// ---------------------------------------------------------------------------------------------

namespace
{
struct OpcodeRef
{
    const char *name;
    uint32_t match;
    uint32_t mask;
};

// riscv-opcodes extensions/rv_v, every vector load/store (fixed bits only; operand fields,
// nf and vm are free where the table leaves them free).
const OpcodeRef kOpcodes[] = {
    {"vlm.v", 0x02b00007, 0xfff0707f},      {"vsm.v", 0x02b00027, 0xfff0707f},
    {"vle8.v", 0x00000007, 0x1df0707f},     {"vle16.v", 0x00005007, 0x1df0707f},
    {"vle32.v", 0x00006007, 0x1df0707f},    {"vle64.v", 0x00007007, 0x1df0707f},
    {"vse8.v", 0x00000027, 0x1df0707f},     {"vse16.v", 0x00005027, 0x1df0707f},
    {"vse32.v", 0x00006027, 0x1df0707f},    {"vse64.v", 0x00007027, 0x1df0707f},
    {"vluxei8.v", 0x04000007, 0x1c00707f},  {"vluxei16.v", 0x04005007, 0x1c00707f},
    {"vluxei32.v", 0x04006007, 0x1c00707f}, {"vluxei64.v", 0x04007007, 0x1c00707f},
    {"vsuxei8.v", 0x04000027, 0x1c00707f},  {"vsuxei16.v", 0x04005027, 0x1c00707f},
    {"vsuxei32.v", 0x04006027, 0x1c00707f}, {"vsuxei64.v", 0x04007027, 0x1c00707f},
    {"vlse8.v", 0x08000007, 0x1c00707f},    {"vlse16.v", 0x08005007, 0x1c00707f},
    {"vlse32.v", 0x08006007, 0x1c00707f},   {"vlse64.v", 0x08007007, 0x1c00707f},
    {"vsse8.v", 0x08000027, 0x1c00707f},    {"vsse16.v", 0x08005027, 0x1c00707f},
    {"vsse32.v", 0x08006027, 0x1c00707f},   {"vsse64.v", 0x08007027, 0x1c00707f},
    {"vloxei8.v", 0x0c000007, 0x1c00707f},  {"vloxei16.v", 0x0c005007, 0x1c00707f},
    {"vloxei32.v", 0x0c006007, 0x1c00707f}, {"vloxei64.v", 0x0c007007, 0x1c00707f},
    {"vsoxei8.v", 0x0c000027, 0x1c00707f},  {"vsoxei16.v", 0x0c005027, 0x1c00707f},
    {"vsoxei32.v", 0x0c006027, 0x1c00707f}, {"vsoxei64.v", 0x0c007027, 0x1c00707f},
    {"vle8ff.v", 0x01000007, 0x1df0707f},   {"vle16ff.v", 0x01005007, 0x1df0707f},
    {"vle32ff.v", 0x01006007, 0x1df0707f},  {"vle64ff.v", 0x01007007, 0x1df0707f},
    {"vl1re8.v", 0x02800007, 0xfff0707f},   {"vl1re16.v", 0x02805007, 0xfff0707f},
    {"vl1re32.v", 0x02806007, 0xfff0707f},  {"vl1re64.v", 0x02807007, 0xfff0707f},
    {"vl2re8.v", 0x22800007, 0xfff0707f},   {"vl2re16.v", 0x22805007, 0xfff0707f},
    {"vl2re32.v", 0x22806007, 0xfff0707f},  {"vl2re64.v", 0x22807007, 0xfff0707f},
    {"vl4re8.v", 0x62800007, 0xfff0707f},   {"vl4re16.v", 0x62805007, 0xfff0707f},
    {"vl4re32.v", 0x62806007, 0xfff0707f},  {"vl4re64.v", 0x62807007, 0xfff0707f},
    {"vl8re8.v", 0xe2800007, 0xfff0707f},   {"vl8re16.v", 0xe2805007, 0xfff0707f},
    {"vl8re32.v", 0xe2806007, 0xfff0707f},  {"vl8re64.v", 0xe2807007, 0xfff0707f},
    {"vs1r.v", 0x02800027, 0xfff0707f},     {"vs2r.v", 0x22800027, 0xfff0707f},
    {"vs4r.v", 0x62800027, 0xfff0707f},     {"vs8r.v", 0xe2800027, 0xfff0707f},
};

// Operands for @p name: v8, (x10)[, x11 | v4]. v8 is aligned for every whole-register nf.
std::string operandsFor(const std::string &name)
{
    const Kites::rvv::VectorMemFields *fields = Kites::rvv::LookupVectorMemMnemonic(name);
    std::string text = name + " v8, (x10)";
    if (fields->mode == Kites::rvv::VectorMemMode::Strided)
    {
        text += ", x11";
    }
    else if (fields->mode == Kites::rvv::VectorMemMode::IndexedUnordered ||
             fields->mode == Kites::rvv::VectorMemMode::IndexedOrdered)
    {
        text += ", v4";
    }
    return text;
}
} // namespace

TEST(VectorLoadStoreAssemblerTest, EncodingsMatchRiscvOpcodes)
{
    for (const OpcodeRef &ref : kOpcodes)
    {
        const std::string line = operandsFor(ref.name);
        const uint32_t word = assembleOne(line);
        EXPECT_EQ(word & ref.mask, ref.match) << line;
        EXPECT_EQ((word >> 7) & 0x1F, 8u) << line;   // vd / vs3
        EXPECT_EQ((word >> 15) & 0x1F, 10u) << line; // rs1
        EXPECT_EQ((word >> 25) & 1, 1u) << line;     // unmasked
        if ((ref.mask >> 29) == 0) // nf is free in the table: a plain access has nf = 0
        {
            EXPECT_EQ(word >> 29, 0u) << line;
        }
        if (ref.mask == 0x1c00707f) // strided/indexed: rs2/vs2 is an operand
        {
            EXPECT_EQ((word >> 20) & 0x1F, line.find("x11") != std::string::npos ? 11u : 4u)
                << line;
        }
    }
}

TEST(VectorLoadStoreAssemblerTest, SegmentAndMaskedEncodings)
{
    // nf = NFIELDS - 1 in bits 31:29; vm (bit 25) = 1 unless v0.t. Built from the
    // vle/vlse/vsuxei rows.
    EXPECT_EQ(assembleOne("vlseg3e16.v v4, (a0)"),
              0x00005007u | 2u << 29 | 1u << 25 | 10u << 15 | 4u << 7);
    EXPECT_EQ(assembleOne("vlsseg2e32.v v4, (a0), a1"),
              0x08006007u | 1u << 29 | 1u << 25 | 11u << 20 | 10u << 15 | 4u << 7);
    EXPECT_EQ(assembleOne("vsuxseg8ei8.v v8, (x5), v16, v0.t"),
              (0x04000027u | 7u << 29 | 16u << 20 | 5u << 15 | 8u << 7) & ~(1u << 25));
    EXPECT_EQ(assembleOne("vlseg2e8ff.v v2, (a0)"),
              0x01000007u | 1u << 29 | 1u << 25 | 10u << 15 | 2u << 7);
    EXPECT_EQ(assembleOne("vle32.v v1, (a0), v0.t"), 0x00006007u | 10u << 15 | 1u << 7);
    // A masked store may use v0 as its data register.
    EXPECT_EQ(assembleOne("vse8.v v0, (a0), v0.t"), 0x00000027u | 10u << 15);
}

TEST(VectorLoadStoreAssemblerTest, AliasesAndZeroOffset)
{
    EXPECT_EQ(assembleOne("vl1r.v v3, (a0)"), assembleOne("vl1re8.v v3, (a0)"));
    EXPECT_EQ(assembleOne("vl8r.v v8, (a0)"), assembleOne("vl8re8.v v8, (a0)"));
    EXPECT_EQ(assembleOne("vle1.v v3, (a0)"), assembleOne("vlm.v v3, (a0)"));
    EXPECT_EQ(assembleOne("vse1.v v3, (a0)"), assembleOne("vsm.v v3, (a0)"));
    EXPECT_EQ(assembleOne("vle32.v v1, 0(a0)"), assembleOne("vle32.v v1, (a0)"));
}

TEST(VectorLoadStoreAssemblerTest, EveryMnemonicRoundTripsThroughDisassembly)
{
    size_t count = 0;
    for (const std::string &name : Kites::rvv::VectorMemMnemonics())
    {
        const uint32_t word = assembleOne(operandsFor(name));
        const std::string text = Kites::instruction_set::disassemble(word);
        EXPECT_EQ(assembleOne(text), word) << name << " -> " << text;
        ++count;
    }
    // 4 EEWs x 8 NFIELDS x (unit, strided, 2 indexed) x 2 + ff loads + whole + mask + aliases
    EXPECT_EQ(count, 4u * 8 * 4 * 2 + 4 * 8 + 16 + 4 + 2 + 4 + 2);
}

TEST(VectorLoadStoreAssemblerTest, DisassemblyText)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(assembleOne("vle32.v v1, (a0)")), "vle32.v v1, (x10)");
    EXPECT_EQ(disassemble(assembleOne("vlse64.v v2, (a0), t1, v0.t")),
              "vlse64.v v2, (x10), x6, v0.t");
    EXPECT_EQ(disassemble(assembleOne("vloxei16.v v3, (sp), v9")), "vloxei16.v v3, (x2), v9");
    EXPECT_EQ(disassemble(assembleOne("vssseg4e8.v v4, (a0), a1")),
              "vssseg4e8.v v4, (x10), x11");
    EXPECT_EQ(disassemble(assembleOne("vl1r.v v3, (a0)")), "vl1re8.v v3, (x10)");
    EXPECT_EQ(disassemble(assembleOne("vs4r.v v4, (a0)")), "vs4r.v v4, (x10)");
    EXPECT_EQ(disassemble(assembleOne("vlm.v v0, (a0)")), "vlm.v v0, (x10)");
    // flw/fld still disassemble as scalar FP loads.
    EXPECT_EQ(disassemble(assembleOne("flw f1, 4(a0)")).rfind("flw", 0), 0u);
    // Reserved: mew = 1.
    EXPECT_EQ(disassemble(0x10006007u).rfind("unknown", 0), 0u);
}

TEST(VectorLoadStoreAssemblerTest, IllegalFormsAreRejected)
{
    const std::vector<std::string> bad = {
        "vle32.v v1, 4(a0)\n",          // only a 0 offset is allowed
        "vle32.v v1, a0\n",             // missing parentheses
        "vle32.v v0, (a0), v0.t\n",     // masked load can't write v0
        "vlm.v v1, (a0), v0.t\n",       // always unmasked
        "vl1re8.v v1, (a0), v0.t\n",    // always unmasked
        "vl2re8.v v3, (a0)\n",          // whole-register group must be aligned
        "vs4r.v v2, (a0)\n",
        "vlse32.v v1, (a0)\n",          // missing stride
        "vlse32.v v1, (a0), v2\n",      // stride is an x register
        "vluxei32.v v1, (a0), a1\n",    // index is a v register
        "vle32.v v1, (a0), a1\n",       // unit-stride has no third operand
        "vle32.v v1, (a0), v1.t\n",     // only v0 can be a mask
        "vsseg9e8.v v1, (a0)\n",        // NFIELDS is 2..8
        "vs1re32.v v1, (a0)\n",         // whole-register stores are EEW=8 only
        "vse32ff.v v1, (a0)\n",         // no fault-only-first stores
    };
    for (const std::string &line : bad)
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

// ---------------------------------------------------------------------------------------------
// Unit-stride, EEW and masking
// ---------------------------------------------------------------------------------------------

TEST(VectorLoadStoreTest, UnitStrideLoadLeavesTailUndisturbed)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 3, e32, m1, tu, mu\n"
                          "vle32.v v1, (a0)\n",
                          [](Rvss &m)
                          {
                              writeWords(m, kBase, {11, 22, 33, 44});
                              writeElements(m.registers_, 1, 32, {7, 7, 7, 7});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{11, 22, 33, 7}));
}

TEST(VectorLoadStoreTest, UnitStrideStoreWritesOnlyVlElements)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 3, e16, m1, ta, ma\n"
                          "vse16.v v2, (a0)\n",
                          [](Rvss &m)
                          {
                              writeElements(m.registers_, 2, 16, {0x1111, 0x2222, 0x3333, 0x4444});
                              writeBytes(m, kBase, Bytes(8, 0xEE));
                          });
    EXPECT_EQ(readBytes(*vm, kBase, 8),
              (Bytes{0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0xEE, 0xEE}));
}

TEST(VectorLoadStoreTest, EewDiffersFromSew)
{
    // SEW=32, LMUL=1, vl=4: vle8 has EMUL=1/4, vle64 has EMUL=2 (an even register group).
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vle8.v v1, (a0)\n"
                          "vle64.v v2, (a0)\n",
                          [](Rvss &m)
                          {
                              writeBytes(m, kBase, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                                    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                                                    25, 26, 27, 28, 29, 30, 31, 32});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 5), (Elements{1, 2, 3, 4, 0}));
    EXPECT_EQ(readElements(vm->registers_, 2, 64, 4),
              (Elements{0x0807060504030201, 0x100F0E0D0C0B0A09, 0x1817161514131211,
                        0x201F1E1D1C1B1A19}));
}

TEST(VectorLoadStoreTest, MaskedLoadAndStoreSkipInactiveElements)
{
    // v0 = 0b1010: elements 1 and 3 active.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "addi a1, a0, 64\n"
                          "vsetivli x0, 4, e32, m1, tu, mu\n"
                          "vle32.v v1, (a0), v0.t\n"
                          "vse32.v v2, (a1), v0.t\n",
                          [](Rvss &m)
                          {
                              writeElements(m.registers_, 0, 8, {0b1010});
                              writeWords(m, kBase, {1, 2, 3, 4});
                              writeWords(m, kBase + 64, {9, 9, 9, 9});
                              writeElements(m.registers_, 1, 32, {7, 7, 7, 7});
                              writeElements(m.registers_, 2, 32, {50, 60, 70, 80});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{7, 2, 7, 4}));
    EXPECT_EQ(readWords(*vm, kBase + 64, 4), (Elements{9, 60, 9, 80}));
}

TEST(VectorLoadStoreTest, StartsAtVstartAndClearsIt)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "csrrwi x0, vstart, 2\n"
                          "vle32.v v1, (a0)\n",
                          [](Rvss &m) { writeWords(m, kBase, {1, 2, 3, 4}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{0, 0, 3, 4}));
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VectorLoadStoreTest, IllegalConfigurationsTrap)
{
    const auto expectTrap = [](const std::string &source)
    { EXPECT_THROW(runOn<Rvss>("lui a0, 0x10000\n" + source), std::runtime_error) << source; };
    expectTrap("vle32.v v1, (a0)\n");                                     // vtype.vill
    expectTrap("vsetivli x0, 4, e32, m1, ta, ma\nvle64.v v1, (a0)\n");    // EMUL=2, odd vd
    expectTrap("vsetvli x0, x0, e8, m8, ta, ma\nvle64.v v8, (a0)\n");     // EMUL=64
    expectTrap("vsetvli x0, x0, e64, mf2, ta, ma\nvle8.v v1, (a0)\n");    // vill: SEW > LMUL*ELEN
}

TEST(VectorLoadStoreTest, SmallestEmulIsLegal)
{
    // e64/m1 with EEW=8 gives EMUL=1/8, the smallest legal EMUL.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetvli t0, x0, e64, m1, ta, ma\n" // vl = VLMAX = 2
                          "vle8.v v1, (a0)\n",
                          [](Rvss &m) { writeBytes(m, kBase, {5, 6, 7}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 3), (Elements{5, 6, 0}));
}

// ---------------------------------------------------------------------------------------------
// Strided and indexed
// ---------------------------------------------------------------------------------------------

TEST(VectorLoadStoreTest, StridedLoadWithPositiveNegativeAndZeroStride)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "li a1, 8\n"
                          "li a2, -4\n"
                          "addi a3, a0, 12\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vlse32.v v1, (a0), a1\n"
                          "vlse32.v v2, (a3), a2\n"
                          "vlse32.v v3, (a0), x0\n",
                          [](Rvss &m) { writeWords(m, kBase, {10, 11, 12, 13, 14, 15, 16, 17}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{10, 12, 14, 16}));
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{13, 12, 11, 10}));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4), (Elements{10, 10, 10, 10}));
}

TEST(VectorLoadStoreTest, StridedStore)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "li a1, 3\n"
                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                          "vsse8.v v1, (a0), a1\n",
                          [](Rvss &m)
                          {
                              writeElements(m.registers_, 1, 8, {0xA1, 0xA2, 0xA3});
                              writeBytes(m, kBase, Bytes(9, 0));
                          });
    EXPECT_EQ(readBytes(*vm, kBase, 9), (Bytes{0xA1, 0, 0, 0xA2, 0, 0, 0xA3, 0, 0}));
}

TEST(VectorLoadStoreTest, IndexedLoadUsesZeroExtendedByteOffsets)
{
    // Data SEW=32 in v1; 8-bit indices in v4. Index 0xFC is +252, not -4.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vluxei8.v v1, (a0), v4\n"
                          "vloxei8.v v2, (a0), v4\n",
                          [](Rvss &m)
                          {
                              writeWords(m, kBase, {100, 101, 102, 103});
                              writeWords(m, kBase + 252, {999});
                              writeElements(m.registers_, 4, 8, {12, 0, 0xFC, 4});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{103, 100, 999, 101}));
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{103, 100, 999, 101}));
}

TEST(VectorLoadStoreTest, IndexedStoreScatters)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 3, e16, m1, ta, ma\n"
                          "vsoxei16.v v1, (a0), v2\n",
                          [](Rvss &m)
                          {
                              writeElements(m.registers_, 1, 16, {0xAAAA, 0xBBBB, 0xCCCC});
                              writeElements(m.registers_, 2, 16, {4, 0, 8});
                              writeBytes(m, kBase, Bytes(10, 0));
                          });
    EXPECT_EQ(readBytes(*vm, kBase, 10),
              (Bytes{0xBB, 0xBB, 0, 0, 0xAA, 0xAA, 0, 0, 0xCC, 0xCC}));
}

TEST(VectorLoadStoreTest, IndexedOverlapRules)
{
    const std::string prefix = "lui a0, 0x10000\n";
    // Data e8 (1 reg) narrower than index e32 (EMUL 4, v4..v7): only vd == vs2 may overlap.
    EXPECT_NO_THROW(runOn<Rvss>(prefix + "vsetivli x0, 4, e8, m1, ta, ma\n"
                                         "vluxei32.v v4, (a0), v4\n"));
    EXPECT_THROW(runOn<Rvss>(prefix + "vsetivli x0, 4, e8, m1, ta, ma\n"
                                      "vluxei32.v v5, (a0), v4\n"),
                 std::runtime_error);
    // Data e32 wider than index e8 with index EMUL 1/4: no overlap allowed.
    EXPECT_THROW(runOn<Rvss>(prefix + "vsetivli x0, 4, e32, m1, ta, ma\n"
                                      "vluxei8.v v4, (a0), v4\n"),
                 std::runtime_error);
    // Data e32 m4 (v4..v7) over index e16 m2 in its highest part (v6..v7): allowed.
    EXPECT_NO_THROW(runOn<Rvss>(prefix + "vsetvli t0, x0, e32, m4, ta, ma\n"
                                         "vluxei16.v v4, (a0), v6\n"));
    // ...but not in its lowest part.
    EXPECT_THROW(runOn<Rvss>(prefix + "vsetvli t0, x0, e32, m4, ta, ma\n"
                                      "vluxei16.v v4, (a0), v4\n"),
                 std::runtime_error);
    // A store has no destination, so data and index may share registers.
    EXPECT_NO_THROW(runOn<Rvss>(prefix + "vsetivli x0, 4, e32, m1, ta, ma\n"
                                         "vsuxei8.v v4, (a0), v4\n"));
}

// ---------------------------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------------------------

TEST(VectorLoadStoreTest, UnitStrideSegmentLoadDeinterleaves)
{
    // RGB triples: field 0 -> v1, field 1 -> v2, field 2 -> v3.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                          "vlseg3e8.v v1, (a0)\n",
                          [](Rvss &m) { writeBytes(m, kBase, {1, 2, 3, 4, 5, 6, 7, 8, 9}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 3), (Elements{1, 4, 7}));
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 3), (Elements{2, 5, 8}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 3), (Elements{3, 6, 9}));
}

TEST(VectorLoadStoreTest, SegmentStoreInterleavesAndFieldsUseWholeGroups)
{
    // e16, m2: each field is a 2-register group, so fields live in v2 and v4.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "vsetivli x0, 2, e16, m2, ta, ma\n"
                          "vsseg2e16.v v2, (a0)\n",
                          [](Rvss &m)
                          {
                              writeElements(m.registers_, 2, 16, {0x0A0A, 0x0B0B});
                              writeElements(m.registers_, 4, 16, {0x1A1A, 0x1B1B});
                          });
    EXPECT_EQ(readBytes(*vm, kBase, 8),
              (Bytes{0x0A, 0x0A, 0x1A, 0x1A, 0x0B, 0x0B, 0x1B, 0x1B}));
}

TEST(VectorLoadStoreTest, StridedAndIndexedSegments)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "li a1, 16\n"
                          "vsetivli x0, 2, e32, m1, ta, ma\n"
                          "vlsseg2e32.v v1, (a0), a1\n"
                          "vluxseg2ei8.v v5, (a0), v8\n",
                          [](Rvss &m)
                          {
                              writeWords(m, kBase, {1, 2, 3, 4, 5, 6, 7, 8});
                              writeElements(m.registers_, 8, 8, {20, 4});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 2), (Elements{1, 5}));
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 2), (Elements{2, 6}));
    // Segment i at base + index[i]; field f at +f*4.
    EXPECT_EQ(readElements(vm->registers_, 5, 32, 2), (Elements{6, 2}));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 2), (Elements{7, 3}));
}

TEST(VectorLoadStoreTest, IllegalSegmentsTrap)
{
    const std::string prefix = "lui a0, 0x10000\n";
    // vd + NFIELDS * EMUL > 32
    EXPECT_THROW(runOn<Rvss>(prefix + "vsetivli x0, 2, e8, m1, ta, ma\nvlseg4e8.v v30, (a0)\n"),
                 std::runtime_error);
    // NFIELDS * EMUL > 8
    EXPECT_THROW(runOn<Rvss>(prefix + "vsetvli x0, x0, e8, m4, ta, ma\nvlseg3e8.v v0, (a0)\n"),
                 std::runtime_error);
    // An indexed segment load's destination may never overlap the index.
    EXPECT_THROW(runOn<Rvss>(prefix +
                             "vsetivli x0, 2, e8, m1, ta, ma\nvluxseg2ei8.v v4, (a0), v5\n"),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// Whole-register and mask
// ---------------------------------------------------------------------------------------------

TEST(VectorLoadStoreTest, WholeRegisterIgnoresVtypeAndVl)
{
    // No vsetvli at all: vtype is vill and vl = 0, yet whole-register moves still work.
    Bytes data;
    for (int i = 0; i < 32; ++i)
    {
        data.push_back(static_cast<uint8_t>(i + 1));
    }
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "addi a1, a0, 64\n"
                          "vl2re32.v v2, (a0)\n"
                          "vs2r.v v2, (a1)\n",
                          [&](Rvss &m) { writeBytes(m, kBase, data); });
    EXPECT_EQ(vm->registers_.ReadVectorRegister(2), Bytes(data.begin(), data.begin() + 16));
    EXPECT_EQ(vm->registers_.ReadVectorRegister(3), Bytes(data.begin() + 16, data.end()));
    EXPECT_EQ(readBytes(*vm, kBase + 64, 32), data);
}

TEST(VectorLoadStoreTest, MaskLoadAndStoreMoveCeilVlOver8Bytes)
{
    // vl = 10 -> evl = 2 bytes.
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "addi a1, a0, 16\n"
                          "vsetivli x0, 10, e8, m1, ta, ma\n"
                          "vlm.v v1, (a0)\n"
                          "vsm.v v2, (a1)\n",
                          [](Rvss &m)
                          {
                              writeBytes(m, kBase, {0xAB, 0xCD, 0xEF});
                              writeBytes(m, kBase + 16, {0, 0, 0});
                              writeElements(m.registers_, 1, 8, {0, 0, 0x77});
                              writeElements(m.registers_, 2, 8, {0x12, 0x34, 0x56});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 3), (Elements{0xAB, 0xCD, 0x77}));
    EXPECT_EQ(readBytes(*vm, kBase + 16, 3), (Bytes{0x12, 0x34, 0}));
}

// ---------------------------------------------------------------------------------------------
// Fault-only-first and faults
// ---------------------------------------------------------------------------------------------

namespace
{
// The last byte of the 64-bit address space is past the end of memory, so an access to it
// faults. a0 = 2^64 - 4: elements 0..2 of a byte load are fine, element 3 faults.
const char *kNearTop = "li a0, -4\n";
} // namespace

TEST(VectorLoadStoreTest, FaultOnlyFirstTrimsVl)
{
    auto vm = runOn<Rvss>(std::string(kNearTop) + "vsetivli x0, 8, e8, m1, ta, ma\n"
                                                   "vle8ff.v v1, (a0)\n"
                                                   "csrrs a1, vl, x0\n",
                          [](Rvss &m) { writeBytes(m, ~0ULL - 3, {1, 2, 3}); });
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VL), 3u);
    EXPECT_EQ(vm->registers_.ReadGpr(11), 3u);
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 4), (Elements{1, 2, 3, 0}));
}

TEST(VectorLoadStoreTest, FaultOnElementZeroTraps)
{
    // Element 0 of a fault-only-first load, and any element of an ordinary load, trap.
    EXPECT_THROW(runOn<Rvss>("li a0, -1\nvsetivli x0, 4, e8, m1, ta, ma\nvle8ff.v v1, (a0)\n"),
                 std::runtime_error);
    EXPECT_THROW(runOn<Rvss>(std::string(kNearTop) +
                             "vsetivli x0, 8, e8, m1, ta, ma\nvle8.v v1, (a0)\n"),
                 std::runtime_error);
    EXPECT_THROW(runOn<Rvss>(std::string(kNearTop) +
                             "vsetivli x0, 8, e8, m1, ta, ma\nvse8.v v1, (a0)\n"),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------------------------

TEST(VectorLoadStoreTest, UndoAndRedoRestoreMemoryRegistersAndVl)
{
    auto vm = runOn<Rvss>(std::string(kNearTop) + "lui a1, 0x10000\n"
                                                   "vsetivli x0, 8, e8, m1, ta, ma\n"
                                                   "vle8ff.v v1, (a0)\n"
                                                   "vse8.v v1, (a1)\n",
                          [](Rvss &m)
                          {
                              writeBytes(m, ~0ULL - 3, {1, 2, 3});
                              writeBytes(m, kBase, {9, 9, 9, 9});
                          });
    EXPECT_EQ(readBytes(*vm, kBase, 4), (Bytes{1, 2, 3, 9}));

    vm->Undo(); // vse8.v
    EXPECT_EQ(readBytes(*vm, kBase, 4), (Bytes{9, 9, 9, 9}));
    vm->Undo(); // vle8ff.v
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 3), (Elements{0, 0, 0}));
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VL), 8u);

    vm->Redo();
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VL), 3u);
    vm->Redo();
    EXPECT_EQ(readBytes(*vm, kBase, 4), (Bytes{1, 2, 3, 9}));
}

// ---------------------------------------------------------------------------------------------
// All VMs: ordering against scalar memory ops, and forwarded base/stride registers
// ---------------------------------------------------------------------------------------------

namespace
{
// Scalar stores feed a vector load back to back; the vector store is read back by a scalar
// load right after it. The base (a0) and stride (a2) are produced just before their use, so
// the forwarding variants must forward them (the others stall).
const char *kMemoryChain = "lui a0, 0x10000\n"
                           "li t0, 5\n"
                           "sw t0, 0(a0)\n"
                           "li t0, 6\n"
                           "sw t0, 8(a0)\n"
                           "li a2, 8\n"
                           "vsetivli x0, 2, e32, m1, ta, ma\n"
                           "vlse32.v v1, (a0), a2\n"   // {5, 6}
                           "vadd.vi v2, v1, 10\n"      // {15, 16}
                           "addi a1, a0, 32\n"
                           "vse32.v v2, (a1)\n"
                           "lw t1, 36(a0)\n"           // 16
                           "nop\nnop\nnop\n";

// The same chain scheduled by hand for the variant with neither hazard detection nor
// forwarding: three nops between every scalar producer and its consumer. Memory ordering
// between the scalar and vector accesses is still back to back.
const char *kMemoryChainScheduled = "lui a0, 0x10000\n"
                                    "li t0, 5\n"
                                    "li t2, 6\n"
                                    "li a2, 8\n"
                                    "nop\nnop\nnop\n"
                                    "addi a1, a0, 32\n"
                                    "sw t0, 0(a0)\n"
                                    "sw t2, 8(a0)\n"
                                    "vsetivli x0, 2, e32, m1, ta, ma\n"
                                    "vlse32.v v1, (a0), a2\n"
                                    "vadd.vi v2, v1, 10\n"
                                    "vse32.v v2, (a1)\n"
                                    "lw t1, 36(a0)\n"
                                    "nop\nnop\nnop\n";

template <typename Vm> void expectMemoryChain(const char *name, const char *program = kMemoryChain)
{
    SCOPED_TRACE(name);
    auto vm = runOn<Vm>(program);
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 2), (Elements{5, 6}));
    EXPECT_EQ(readWords(*vm, kBase + 32, 2), (Elements{15, 16}));
    EXPECT_EQ(vm->registers_.ReadGpr(6), 16u); // t1
}
} // namespace

TEST(VectorLoadStorePipelineTest, MemoryChainOnEveryVariant)
{
    expectMemoryChain<Kites::RVSSProcessor>("RVSS");
    expectMemoryChain<Kites::RV5StageProcessorHF>("HF");
    expectMemoryChain<Kites::RV5StageProcessorHNF>("HNF");
    expectMemoryChain<Kites::RV5StageProcessorNHF>("NHF");
    expectMemoryChain<Kites::RV5StageProcessorNHNF>("NHNF", kMemoryChainScheduled);
}

TEST(VectorLoadStorePipelineTest, UndoToStartRestoresMemory)
{
    auto vm = runOn<Kites::RV5StageProcessorHF>(kMemoryChain);
    for (uint64_t cycles = vm->cycle_s_; cycles > 0; --cycles)
    {
        vm->Undo();
    }
    EXPECT_EQ(readWords(*vm, kBase + 32, 2), (Elements{0, 0}));
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 2), (Elements{0, 0}));
}
