// Shared helpers of the vector test suites (vector_rvss_tests.cpp, vector_rv5s_tests.cpp).
// Each former test file keeps its helpers in its own namespace (vtest_<topic>); the tests of that
// topic live in the same namespace in the two .cpp files. Helpers sit in anonymous namespaces, so
// a suite that does not use some of them gets unused-function warnings: silence those here.
#pragma once

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-template"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <utility>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <random>
#include <set>
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
#include "processor/vector/vector_execute.h"
#include "processor/vector/vector_memory.h"
#include "processor/vector/vector_operands.h"
#include "processor/vector/vector_stages.h"
#include "utils/utils.h"

// ===== vset_test =============================================================================
// Tests for the RVV configuration-setting instructions: vsetvli, vsetivli and vsetvl.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding);
// the vl/vtype rules follow RVV 1.0 section 6 and Spike's set_vl().
namespace vtest_vset
{
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





// ---------------------------------------------------------------------------------------------
// vtype / vl rules
// ---------------------------------------------------------------------------------------------




// ---------------------------------------------------------------------------------------------
// Single-cycle VM
// ---------------------------------------------------------------------------------------------







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
} // namespace vtest_vset

// ===== vector_register_file_test =============================================================
namespace vtest_register_file
{
namespace
{

using Kites::RegisterFile;

std::unique_ptr<Kites::RVSSProcessor> runSource(const std::string &source)
{
    Kites::setupVmStateDirectory();

    auto vm = std::make_unique<Kites::RVSSProcessor>();
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    vm->LoadProgram(program);
    vm->DebugRun();
    return vm;
}

// Restores the global VLEN so one test changing it can't leak into the others.
class VlenGuard
{
  public:
    VlenGuard() : m_saved(Kites::vm_config::config.getVlen())
    {
    }
    ~VlenGuard()
    {
        Kites::vm_config::config.setVlen(m_saved);
    }

  private:
    uint64_t m_saved;
};

} // namespace
} // namespace vtest_register_file

// ===== vector_int_arith_test =================================================================
// Tests for the RVV single-width integer add/subtract instructions:
// vadd.vv/.vx/.vi, vsub.vv/.vx, vrsub.vx/.vi.
namespace vtest_int_arith
{
namespace
{
using Kites::RegisterFile;

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

using Setup = std::function<void(RegisterFile &)>;

// Loads the program, lets @p setup seed registers, then runs it to the end.
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

void writeElements(RegisterFile &rf, size_t reg, size_t sew, const std::vector<uint64_t> &values)
{
    for (size_t i = 0; i < values.size(); ++i)
    {
        rf.WriteVectorElement(reg, i, sew, values[i]);
    }
}

std::vector<uint64_t> readElements(const RegisterFile &rf, size_t reg, size_t sew, size_t count)
{
    std::vector<uint64_t> values;
    for (size_t i = 0; i < count; ++i)
    {
        values.push_back(rf.ReadVectorElement(reg, i, sew));
    }
    return values;
}

using Elements = std::vector<uint64_t>;

} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------




// ---------------------------------------------------------------------------------------------
// Element results (single-cycle VM)
// ---------------------------------------------------------------------------------------------










// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
// Vector-to-vector dependencies are back to back: vector instructions execute and commit in
// EX in program order, so even the variants without hazard detection see each result. The only
// scalar dependency (x10) is spaced out.
const char *kChainProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "li x10, 7\n"
                            "nop\nnop\nnop\n"
                            "vadd.vi v1, v0, 3\n"   // 3
                            "vadd.vx v2, v1, x10\n" // 10
                            "vsub.vv v3, v2, v1\n"  // 7
                            "vrsub.vi v4, v3, -1\n" // -8
                            "vrsub.vx v5, v1, x10\n" // 4
                            "vsub.vx v6, v5, x10\n" // -3
                            "nop\nnop\nnop\n";

template <typename Vm> void expectChainResult()
{
    auto vm = runOn<Vm>(kChainProgram);
    const auto all = [](uint64_t v) { return Elements(4, v & 0xFFFFFFFF); };
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), all(3));
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), all(10));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4), all(7));
    EXPECT_EQ(readElements(vm->registers_, 4, 32, 4), all(-8));
    EXPECT_EQ(readElements(vm->registers_, 5, 32, 4), all(4));
    EXPECT_EQ(readElements(vm->registers_, 6, 32, 4), all(-3));
    // vd is a vector register: x1..x6 must not have been written.
    for (size_t reg = 1; reg <= 6; ++reg)
    {
        EXPECT_EQ(vm->registers_.ReadGpr(reg), 0u) << "x" << reg;
    }
}
} // namespace
} // namespace vtest_int_arith

// ===== vector_carry_test =====================================================================
// Tests for RVV add-with-carry / subtract-with-borrow (11.4): vadc, vsbc, vmadc, vmsbc.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: v0 supplies the carry/borrow-in, every body element is written, vadc/vsbc
// cannot write v0 and are reserved with vm = 1, and vmadc/vmsbc write a mask (even into v0).
namespace vtest_carry
{
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
} // namespace

namespace
{
uint64_t maskByte(const RegisterFile &rf, size_t reg)
{
    return rf.ReadVectorElement(reg, 0, 8);
}

// e8, vl = 4: carry-in (v0) at elements 1 and 2.
void carryData(RegisterFile &rf)
{
    writeElements(rf, 0, 8, {0b0110});
    writeElements(rf, 2, 8, {0xFF, 0xFF, 0x10, 0x00});
    writeElements(rf, 3, 8, {0x01, 0x00, 0x20, 0x00});
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------



// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------








// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kCarryProgram = "li x10, -1\n"
                            "nop\nnop\nnop\n"
                            "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "vmv.v.x v2, x10\n"             // 0xFFFFFFFF
                            "vmv.v.i v3, 1\n"
                            "vmadc.vv v0, v2, v3\n"         // every element carries
                            "vadc.vvm v4, v3, v3, v0\n"     // 1 + 1 + 1 = 3
                            "vmsbc.vx v5, v3, x10\n"        // 1 < 0xFFFFFFFF: borrow
                            "vsbc.vxm v6, v3, x10, v0\n"    // 1 - (-1) - 1 = 1
                            "nop\nnop\nnop\n";

template <typename Vm> void expectCarryResult()
{
    auto vm = runOn<Vm>(kCarryProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 0), 0x0Fu);
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 3));
    EXPECT_EQ(maskByte(rf, 5), 0x0Fu);
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 1));
}
} // namespace
} // namespace vtest_carry

// ===== vector_compare_test ===================================================================
// Tests for the RVV integer compares (11.8): vmseq, vmsne, vmslt{u}, vmsle{u}, vmsgt{u} and the
// assembler aliases vmsgt{u}.vv, vmsge{u}.vv, vmslt{u}.vi, vmsge{u}.vi.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: the result is a mask (one bit per element, a single register), a masked
// compare may write v0, unsigned .vi compares use the sign-extended simm5 as an unsigned SEW
// value, and the mask may overlap only the first register of a source group.
namespace vtest_compare
{
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
} // namespace vtest_compare

// ===== vector_shift_test =====================================================================
// Tests for the RVV single-width bit shifts (11.6): vsll, vsrl, vsra in .vv/.vx/.vi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: only the low log2(SEW) bits of the shift amount are used, .vi takes an
// unsigned 5-bit immediate, and vsra fills with the sign bit.
namespace vtest_shift
{
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
} // namespace

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------




// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------







// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kShiftProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                            "li x10, 3\n"
                            "nop\nnop\nnop\n"
                            "vadd.vi v1, v0, -8\n"   // 0xFFFFFFF8
                            "vsra.vi v2, v1, 2\n"    // -2 = 0xFFFFFFFE
                            "vsrl.vi v3, v1, 28\n"   // 0xF
                            "vsll.vx v4, v3, x10\n"  // 0x78
                            "vsra.vv v5, v4, v3\n"   // 0x78 >> 15 = 0
                            "vsll.vv v6, v3, v3\n"   // 0xF << 15 = 0x78000
                            "nop\nnop\nnop\n";

template <typename Vm> void expectShiftResult()
{
    auto vm = runOn<Vm>(kShiftProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 2, 32, 4), Elements(4, 0xFFFFFFFE));
    EXPECT_EQ(readElements(rf, 3, 32, 4), Elements(4, 0xF));
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 0x78));
    EXPECT_EQ(readElements(rf, 5, 32, 4), Elements(4, 0));
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 0x78000));
}
} // namespace
} // namespace vtest_shift

// ===== vector_narrow_shift_test ==============================================================
// Tests for the RVV narrowing right shifts (11.7): vnsrl and vnsra in .wv/.wx/.wi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: vs2 is 2*SEW wide, the result is truncated to SEW, the shift amount uses
// log2(2*SEW) bits, .wi takes an unsigned 5-bit immediate, and vd may overlap only the start
// of the wide vs2 group.
namespace vtest_narrow_shift
{
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

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------



// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------







// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kNarrowProgram = "vsetivli x0, 4, e32, m1, ta, ma\n"
                             "li x10, 4\n"
                             "nop\nnop\nnop\n"
                             "vadd.vi v2, v0, -8\n"        // e32 0xFFFFFFF8
                             "vsetivli x0, 4, e16, m1, ta, ma\n"
                             "vnsra.wi v4, v2, 2\n"        // -2 = 0xFFFE
                             "vnsrl.wx v5, v2, x10\n"      // 0x0FFFFFFF -> 0xFFFF
                             "vnsrl.wi v6, v2, 28\n"       // 0xF
                             "vnsra.wv v7, v2, v6\n"       // -8 >> 15 = -1 -> 0xFFFF
                             "vnsrl.wv v8, v2, v6\n"       // 0xFFFFFFF8 >> 15 = 0x1FFFF -> 0xFFFF
                             "nop\nnop\nnop\n";

template <typename Vm> void expectNarrowResult()
{
    auto vm = runOn<Vm>(kNarrowProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 4), Elements(4, 0xFFFE));
    EXPECT_EQ(readElements(rf, 5, 16, 4), Elements(4, 0xFFFF));
    EXPECT_EQ(readElements(rf, 6, 16, 4), Elements(4, 0xF));
    EXPECT_EQ(readElements(rf, 7, 16, 4), Elements(4, 0xFFFF));
    EXPECT_EQ(readElements(rf, 8, 16, 4), Elements(4, 0xFFFF));
}
} // namespace
} // namespace vtest_narrow_shift

// ===== vector_widen_ext_logic_test ===========================================================
// Tests for RVV widening integer add/subtract (11.2), integer extension (11.3) and bitwise
// logical (11.5) instructions. Encodings are checked against llvm-mc
// (-triple=riscv64 -mattr=+zve64x -show-encoding); legality checks follow Spike.
namespace vtest_widen_ext_logic
{
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




// ---------------------------------------------------------------------------------------------
// Widening add/subtract
// ---------------------------------------------------------------------------------------------










// ---------------------------------------------------------------------------------------------
// Integer extension
// ---------------------------------------------------------------------------------------------





// ---------------------------------------------------------------------------------------------
// Bitwise logical
// ---------------------------------------------------------------------------------------------




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
} // namespace vtest_widen_ext_logic

// ===== vector_mul_div_test ===================================================================
// Tests for RVV integer min/max (11.9), multiply (11.10), divide (11.11), widening multiply
// (11.12), multiply-add (11.13), widening multiply-add (11.14), merge (11.15) and move (11.16).
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
namespace vtest_mul_div
{
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

// ---------------------------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------------------------





// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------


















// ---------------------------------------------------------------------------------------------
// All VMs
// ---------------------------------------------------------------------------------------------

namespace
{
const char *kMulProgram = "li x10, 3\n"
                          "nop\nnop\nnop\n"
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vmv.v.i v1, 7\n"               // 7
                          "vmv.v.x v2, x10\n"             // 3
                          "vmul.vv v3, v1, v2\n"          // 21
                          "vdiv.vx v4, v3, x10\n"         // 7
                          "vrem.vx v5, v1, x10\n"         // 1
                          "vmacc.vv v3, v1, v2\n"         // 21 + 21 = 42
                          "vmax.vx v6, v5, x10\n"         // 3
                          "vmv.v.i v0, 5\n"               // mask bits 0 and 2
                          "vmerge.vvm v7, v1, v3, v0\n"   // [42, 7, 42, 7]
                          "vwmulu.vx v8, v1, x10\n"       // e64 21
                          "nop\nnop\nnop\n";

template <typename Vm> void expectMulResult()
{
    auto vm = runOn<Vm>(kMulProgram);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 3, 32, 4), Elements(4, 42));
    EXPECT_EQ(readElements(rf, 4, 32, 4), Elements(4, 7));
    EXPECT_EQ(readElements(rf, 5, 32, 4), Elements(4, 1));
    EXPECT_EQ(readElements(rf, 6, 32, 4), Elements(4, 3));
    EXPECT_EQ(readElements(rf, 7, 32, 4), (Elements{42, 7, 42, 7}));
    EXPECT_EQ(readElements(rf, 8, 64, 2), Elements(2, 21));
    EXPECT_EQ(readElements(rf, 9, 64, 2), Elements(2, 21));
}
} // namespace
} // namespace vtest_mul_div

// ===== vector_reduction_test =================================================================
// Tests for the RVV integer reductions (14.1 / 14.2): vredsum, vredand, vredor, vredxor,
// vredminu, vredmin, vredmaxu, vredmax and the widening vwredsumu / vwredsum.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: vd[0] = vs1[0] op vs2[active elements < vl]; vs2 is an LMUL group while vd
// and vs1 are single registers; only vd[0] is written; vl = 0 writes nothing; vstart != 0 is
// illegal; vd may overlap any source, v0 included.
namespace vtest_reduction
{
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



// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------









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
} // namespace vtest_reduction

// ===== vector_mask_test ======================================================================
// Tests for the RVV mask instructions (section 15): the vm*.mm logical ops and their pseudos
// (vmmv.m, vmnot.m, vmclr.m, vmset.m), vcpop.m, vfirst.m, vmsbf.m, vmsif.m, vmsof.m, viota.m
// and vid.v. Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x).
// Rules under test: mask bits are one per element; tail and masked-off bits/elements are left
// undisturbed; vcpop/vfirst write x[rd]; vstart != 0 is illegal for vcpop, vfirst, vms*f and
// viota; vms*f and viota may not overlap their source mask; masked vms*f/viota/vid cannot write v0.
namespace vtest_mask
{
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




// ---------------------------------------------------------------------------------------------
// Behaviour (single-cycle VM)
// ---------------------------------------------------------------------------------------------









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
} // namespace vtest_mask

// ===== vector_permute_test ===================================================================
// Tests for the RVV 1.0 permutation instructions (spec section 16): vmv.x.s, vmv.s.x, the slides,
// vrgather / vrgatherei16, vcompress and the whole-register moves vmv<nr>r.v.
// Encodings are built from the MATCH constants of the riscv-opcodes tables (encoding.h).
// Rules under test: vmv.x.s sign-extends and runs even with vl = 0; vmv.s.x writes element 0
// only when vstart < vl; slideup leaves elements below OFFSET alone; slidedown and vrgather read 0
// past VLMAX; OFFSET and the .vx index are unsigned XLEN values (not cut to SEW); slide1up/down
// insert the low SEW bits of x[rs1]; vcompress needs vstart = 0; vmv<nr>r.v ignores vl and
// vtype; and slide-ups, vrgather and vcompress may not overlap their sources.
namespace vtest_permute
{
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



// ---------------------------------------------------------------------------------------------
// Scalar moves
// ---------------------------------------------------------------------------------------------



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





// ---------------------------------------------------------------------------------------------
// Gather and compress
// ---------------------------------------------------------------------------------------------




// ---------------------------------------------------------------------------------------------
// Whole-register moves
// ---------------------------------------------------------------------------------------------


// ---------------------------------------------------------------------------------------------
// Reserved encodings, undo
// ---------------------------------------------------------------------------------------------



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
} // namespace vtest_permute

// ===== vector_load_store_test ================================================================
// Tests for the RVV 1.0 vector loads and stores (spec section 7): unit-stride, strided, indexed
// (ordered/unordered), mask, fault-only-first, segment and whole-register forms.
//
// Encodings are checked against the MATCH/MASK table of riscv-opcodes (extensions/rv_v), the
// table Spike and the GNU/LLVM toolchains are generated from. Element behaviour follows the spec
// and Spike: tail and masked-off elements are left undisturbed. VLEN is 128 (16 bytes/register).
namespace vtest_load_store
{
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







// ---------------------------------------------------------------------------------------------
// Unit-stride, EEW and masking
// ---------------------------------------------------------------------------------------------









// ---------------------------------------------------------------------------------------------
// Strided and indexed
// ---------------------------------------------------------------------------------------------






// ---------------------------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------------------------





// ---------------------------------------------------------------------------------------------
// Whole-register and mask
// ---------------------------------------------------------------------------------------------



// ---------------------------------------------------------------------------------------------
// Fault-only-first and faults
// ---------------------------------------------------------------------------------------------

namespace
{
// The last byte of the 64-bit address space is past the end of memory, so an access to it
// faults. a0 = 2^64 - 4: elements 0..2 of a byte load are fine, element 3 faults.
const char *kNearTop = "li a0, -4\n";
} // namespace



// ---------------------------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------------------------


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
} // namespace vtest_load_store

// ===== vector_stages_test ====================================================================
// The EX / MEM / WB split of vector instructions (vector_stages.h), as the single-cycle VM uses it:
// EX changes no state, MEM touches only memory (and only for loads/stores), WB commits the
// registers. The composed path (ExecuteVectorInstruction) is covered by every other vector test.
namespace vtest_stages
{
namespace
{
using Kites::RegisterFile;
using Kites::RVSSProcessor;
namespace rvv = Kites::rvv;

constexpr uint64_t kBase = 0x10000000;

uint32_t assembleOne(const std::string &line)
{
    std::istringstream stream(line + "\n");
    const Kites::AssembledProgram program = Kites::assemble(stream);
    if (program.text_buffer.size() != 1)
    {
        throw std::runtime_error("expected one instruction from: " + line);
    }
    return program.text_buffer[0];
}

/// Everything a vector instruction may change.
struct Snapshot
{
    std::vector<std::vector<uint8_t>> vregs;
    uint64_t vl = 0, vtype = 0, vstart = 0;
    std::vector<uint8_t> memory;

    bool operator==(const Snapshot &) const = default;
};

Snapshot Take(RVSSProcessor &vm)
{
    Snapshot s;
    for (size_t r = 0; r < RegisterFile::NUM_VR; ++r)
    {
        s.vregs.push_back(vm.registers_.ReadVectorRegister(r));
    }
    s.vl = vm.registers_.ReadCsr(RegisterFile::CSR_VL);
    s.vtype = vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE);
    s.vstart = vm.registers_.ReadCsr(RegisterFile::CSR_VSTART);
    for (uint64_t i = 0; i < 32; ++i)
    {
        s.memory.push_back(vm.memory_controller_.peekByte(kBase + i));
    }
    return s;
}

/// A VM with vl = 4, e32/m1, v1 = {1,2,3,4}, v2 = {10,20,30,40}, mem[kBase] = {5,6,7,8}.
class VectorStagesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Kites::setupVmStateDirectory();
        std::vector<Kites::RegisterChange> changes;
        std::vector<Kites::MemoryChange> memoryChanges;
        rvv::ExecuteVectorInstruction(assembleOne("vsetivli x0, 4, e32, m1, ta, ma"), 0, 0,
                                      vm.registers_, vm.memory_controller_, changes,
                                      memoryChanges);
        for (uint64_t i = 0; i < 4; ++i)
        {
            vm.registers_.WriteVectorElement(1, i, 32, i + 1);
            vm.registers_.WriteVectorElement(2, i, 32, 10 * (i + 1));
            vm.memory_controller_.writeWord(kBase + 4 * i, static_cast<uint32_t>(i + 5));
        }
    }

    std::vector<uint64_t> Elements(size_t reg)
    {
        std::vector<uint64_t> out;
        for (uint64_t i = 0; i < 4; ++i)
        {
            out.push_back(vm.registers_.ReadVectorElement(reg, i, 32));
        }
        return out;
    }

    RVSSProcessor vm;
    rvv::VectorScratch scratch;
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
};
} // namespace
} // namespace vtest_stages

// ===== vector_spec_sweep_test ================================================================
// Spec sweep for the RVV integer instructions: every arithmetic, compare, carry, widening,
// narrowing, extension and reduction instruction is run at every SEW it is legal for (e8, e16,
// e32, e64) on random data salted with edge values, unmasked and masked, and each destination
// element is compared with a reference model written directly from the RVV 1.0 formulas.
//
// Every run uses vl = VLMAX - 1, so the last element is tail and must stay undisturbed; masked
// runs check that inactive elements keep their old value (Kites, like Spike, implements the
// agnostic policies as undisturbed). The reference model is independent of the simulator: it
// works on raw SEW-bit values with explicit sign handling and 128-bit products.
namespace vtest_spec_sweep
{
namespace
{
using Kites::RegisterFile;
using u64 = uint64_t;
using i64 = int64_t;
using u128 = unsigned __int128;
using i128 = __int128;

constexpr u64 kVlenBits = 128;
constexpr uint32_t kVd = 8, kVs2 = 16, kVs1 = 24; // register groups never overlap
constexpr int kScalar = 10;                       // x10 holds the .vx operand

u64 Mask(u64 w)
{
    return w >= 64 ? ~0ULL : (1ULL << w) - 1;
}
i64 S(u64 v, u64 w) // value as a signed w-bit number
{
    v &= Mask(w);
    if (w >= 64)
        return static_cast<i64>(v);
    const u64 sign = 1ULL << (w - 1);
    return static_cast<i64>((v ^ sign) - sign);
}
u64 U(u64 v, u64 w)
{
    return v & Mask(w);
}

// Random values salted with the edge cases the spec singles out.
class Data
{
  public:
    explicit Data(u64 seed) : m_rng(seed) {}
    u64 operator()(u64 w)
    {
        const u64 edges[] = {0, 1, Mask(w), Mask(w) >> 1, (Mask(w) >> 1) + 1, 2, Mask(w) - 1};
        const u64 pick = m_rng() % 10;
        return U(pick < 7 ? edges[pick] : m_rng(), w);
    }
    bool Bit()
    {
        return (m_rng() & 1) != 0;
    }

  private:
    std::mt19937_64 m_rng;
};

struct Outcome
{
    std::unique_ptr<Kites::RVSSProcessor> vm;
    std::vector<uint8_t> before[32];
};

// Runs "vsetvli (vl = VLMAX - 1), e<sew>, m<lmul>" then the instruction, after setup().
Outcome Execute(const std::string &instruction, u64 sew, int lmulLog2,
            const std::function<void(RegisterFile &)> &setup)
{
    Kites::setupVmStateDirectory();
    const u64 vlmax = (kVlenBits / sew) << lmulLog2;
    std::ostringstream src;
    src << "li x6, " << vlmax - 1 << "\n"
        << "vsetvli x5, x6, e" << sew << ", m" << (1 << lmulLog2) << ", ta, ma\n"
        << instruction << "\n";
    std::istringstream stream(src.str());
    Outcome run;
    run.vm = std::make_unique<Kites::RVSSProcessor>();
    run.vm->LoadProgram(Kites::assemble(stream));
    setup(run.vm->registers_);
    for (uint32_t r = 0; r < 32; ++r)
        run.before[r] = run.vm->registers_.ReadVectorRegister(r);
    run.vm->DebugRun();
    return run;
}

u64 OldElement(const Outcome &run, uint32_t reg, u64 index, u64 eew)
{
    // Element index of a group: walk the byte snapshot like RegisterFile does.
    const u64 bytes = kVlenBits / 8;
    const u64 offset = index * (eew / 8);
    const uint32_t r = reg + static_cast<uint32_t>(offset / bytes);
    u64 v = 0;
    for (u64 b = 0; b < eew / 8; ++b)
        v |= static_cast<u64>(run.before[r][offset % bytes + b]) << (8 * b);
    return v;
}
bool OldMaskBit(const Outcome &run, uint32_t reg, u64 i)
{
    return (run.before[reg][i / 8] >> (i % 8)) & 1;
}

// ------------------------------------------------------------------------------------------
// Single-width: vd[i] = f(vs2[i], src, vd_old[i]) at SEW
// ------------------------------------------------------------------------------------------
enum class Src
{
    VV,
    VX,
    VI,  // simm5
    VUI, // uimm5 (shifts)
};

struct SingleOp
{
    const char *name; // without suffix
    std::vector<Src> forms;
    std::function<u64(u64 a, u64 b, u64 old, u64 w)> f; // a = vs2, b = vs1 / rs1 / imm
    bool sourceFirst = false; // vmacc family: "vd, src, vs2"
};

u64 DivU(u64 a, u64 b, u64 w)
{
    return b == 0 ? Mask(w) : a / b;
}
u64 DivS(u64 a, u64 b, u64 w)
{
    const i64 x = S(a, w), y = S(b, w);
    if (y == 0)
        return Mask(w);
    if (y == -1)
        return U(0 - a, w); // MIN / -1 = MIN
    return U(static_cast<u64>(x / y), w);
}
u64 RemU(u64 a, u64 b, u64)
{
    return b == 0 ? a : a % b;
}
u64 RemS(u64 a, u64 b, u64 w)
{
    const i64 x = S(a, w), y = S(b, w);
    if (y == 0)
        return a;
    if (y == -1)
        return 0;
    return static_cast<u64>(x % y);
}
u64 HighSS(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<i128>(S(a, w)) * S(b, w)) >> w);
}
u64 HighUU(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<u128>(U(a, w)) * U(b, w)) >> w);
}
u64 HighSU(u64 a, u64 b, u64 w)
{
    return static_cast<u64>((static_cast<i128>(S(a, w)) * static_cast<i128>(U(b, w))) >> w);
}

std::vector<SingleOp> SingleOps()
{
    const auto vxi = std::vector<Src>{Src::VV, Src::VX, Src::VI};
    const auto vx = std::vector<Src>{Src::VV, Src::VX};
    const auto shifts = std::vector<Src>{Src::VV, Src::VX, Src::VUI};
    return {
        {"vadd", vxi, [](u64 a, u64 b, u64, u64) { return a + b; }},
        {"vsub", vx, [](u64 a, u64 b, u64, u64) { return a - b; }},
        {"vrsub", {Src::VX, Src::VI}, [](u64 a, u64 b, u64, u64) { return b - a; }},
        {"vand", vxi, [](u64 a, u64 b, u64, u64) { return a & b; }},
        {"vor", vxi, [](u64 a, u64 b, u64, u64) { return a | b; }},
        {"vxor", vxi, [](u64 a, u64 b, u64, u64) { return a ^ b; }},
        {"vsll", shifts, [](u64 a, u64 b, u64, u64 w) { return a << (b & (w - 1)); }},
        {"vsrl", shifts, [](u64 a, u64 b, u64, u64 w) { return U(a, w) >> (b & (w - 1)); }},
        {"vsra", shifts,
         [](u64 a, u64 b, u64, u64 w) { return static_cast<u64>(S(a, w) >> (b & (w - 1))); }},
        {"vminu", vx, [](u64 a, u64 b, u64, u64 w) { return U(a, w) < U(b, w) ? a : b; }},
        {"vmin", vx, [](u64 a, u64 b, u64, u64 w) { return S(a, w) < S(b, w) ? a : b; }},
        {"vmaxu", vx, [](u64 a, u64 b, u64, u64 w) { return U(a, w) > U(b, w) ? a : b; }},
        {"vmax", vx, [](u64 a, u64 b, u64, u64 w) { return S(a, w) > S(b, w) ? a : b; }},
        {"vmul", vx, [](u64 a, u64 b, u64, u64) { return a * b; }},
        {"vmulh", vx, [](u64 a, u64 b, u64, u64 w) { return HighSS(a, b, w); }},
        {"vmulhu", vx, [](u64 a, u64 b, u64, u64 w) { return HighUU(a, b, w); }},
        {"vmulhsu", vx, [](u64 a, u64 b, u64, u64 w) { return HighSU(a, b, w); }},
        {"vdivu", vx, [](u64 a, u64 b, u64 , u64 w) { return DivU(U(a, w), U(b, w), w); }},
        {"vdiv", vx, [](u64 a, u64 b, u64, u64 w) { return DivS(a, b, w); }},
        {"vremu", vx, [](u64 a, u64 b, u64, u64 w) { return RemU(U(a, w), U(b, w), w); }},
        {"vrem", vx, [](u64 a, u64 b, u64, u64 w) { return RemS(a, b, w); }},
        {"vmacc", vx, [](u64 a, u64 b, u64 d, u64) { return d + b * a; }, true},
        {"vnmsac", vx, [](u64 a, u64 b, u64 d, u64) { return d - b * a; }, true},
        {"vmadd", vx, [](u64 a, u64 b, u64 d, u64) { return b * d + a; }, true},
        {"vnmsub", vx, [](u64 a, u64 b, u64 d, u64) { return a - b * d; }, true},
    };
}

std::string Suffix(Src s)
{
    switch (s)
    {
    case Src::VV:
        return ".vv";
    case Src::VX:
        return ".vx";
    default:
        return ".vi";
    }
}

// The source operand: register text and its SEW-bit value (simm5 sign-extended, x truncated).
struct Source
{
    std::string text;
    u64 scalar = 0; // for .vx / .vi
};
Source MakeSource(Src form, Data &data, u64 w, u64 &rs1)
{
    switch (form)
    {
    case Src::VV:
        return {"v" + std::to_string(kVs1)};
    case Src::VX:
        rs1 = data(64); // a full 64-bit x register: only its low SEW bits count
        return {"x" + std::to_string(kScalar), U(rs1, w)};
    case Src::VI:
    {
        const int imm = static_cast<int>(data(64) % 32) - 16;
        return {std::to_string(imm), U(static_cast<u64>(static_cast<i64>(imm)), w)};
    }
    case Src::VUI:
    {
        const int imm = static_cast<int>(data(64) % 32);
        return {std::to_string(imm), static_cast<u64>(imm)};
    }
    }
    return {};
}

void FillRandom(RegisterFile &rf, uint32_t reg, u64 count, u64 w, Data &data)
{
    for (u64 i = 0; i < count; ++i)
        rf.WriteVectorElement(reg, i, w, data(w));
}
void FillMask(RegisterFile &rf, Data &data)
{
    for (u64 i = 0; i < kVlenBits; ++i)
        rf.WriteMaskBit(0, i, data.Bit());
}

constexpr u64 kSews[] = {8, 16, 32, 64};
} // namespace







// ------------------------------------------------------------------------------------------
// Spec rules not tied to one instruction family
// ------------------------------------------------------------------------------------------
} // namespace vtest_spec_sweep

// ===== vector_pipeline_differential_test =====================================================
// Differential check of the 5-stage pipelines against the single-cycle VM for back-to-back
// dependencies between scalar and vector instructions: a scalar load feeding a vector .vx
// operand, a vector base address or vsetvl's AVL; vset*'s rd, vmv.x.s, vcpop.m and vfirst.m
// feeding scalar code; and a vector-derived value stored right away. The final x registers,
// vector registers, vl/vtype and .data must be identical.
//
// H_F (forwarding + hazard detection) and H_NF (stalls) must match with no help. NH_F has
// forwarding but no hazard detection, so a load-use pair needs one instruction in between; NH_NF
// (neither) is left out because it needs every dependency spaced by hand.
namespace vtest_pipeline_differential
{
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
} // namespace vtest_pipeline_differential

// ===== rv5s_vector_pipeline_test =============================================================
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
namespace vtest_rv5s_vector_pipeline
{
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





// --- Forwarding (no stalls) ---------------------------------------------------------------










// --- Stalls --------------------------------------------------------------------------------








// --- Whole programs -------------------------------------------------------------------------



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



// --- Datapath view ------------------------------------------------------------------------------
} // namespace vtest_rv5s_vector_pipeline

// ===== Edge values, spec examples, random programs ==========================================
// Shared by the edge-value, spec-example and random-program tests in both suites.
//
// Edge values: the integer operand pairs of the riscv-vector-tests generator
// (https://github.com/chipsalliance/riscv-vector-tests, Apache-2.0: configs/v/vadd.vv.toml and
// vdivu.vv.toml, its "base" rows and per-SEW rows), extended with the same kind of boundaries for
// every SEW: signed minimum against -1 (division overflow), sign boundaries, 0, 1, all ones and
// alternating bit patterns. Expected results come from an independent reference model below, not
// from a simulator. Floating-point and fixed-point (vsadd, vssub, vsmul, vssrl, vssra, vnclip,
// vaadd, vasub) instructions are not tested here.
namespace vtest_edge
{
namespace
{
/// Absolute address of a label: data labels are stored as offsets into .data.
uint64_t LabelAddress(const Kites::ProcessorBase &vm, const char *name)
{
    const auto &symbol = vm.program_.symbol_table.at(name);
    return symbol.isData ? Kites::vm_config::config.getDataSectionStart() + symbol.address
                         : symbol.address;
}

using Pair = std::pair<uint64_t, uint64_t>; // (vs2, vs1 or x[rs1])

const std::vector<Pair> &EdgePairs(unsigned sew)
{
    static const std::vector<Pair> base = {{0x0, 0x0}, {0x1, 0x2}, {0x3, 0xf}};
    static const std::vector<Pair> e8 = {
        // vadd.vv.toml sew8
        {0xf8, 0x00}, {0xf8, 0x08}, {0x7f, 0x00}, {0x7f, 0x07}, {0x7f, 0x01}, {0xff, 0x00},
        {0xff, 0xff},
        // vdivu.vv.toml sew8
        {0x7e, 0xb7}, {0xc0, 0xb7}, {0x00, 0x80}, {0x80, 0x00}, {0x80, 0x80}, {0xab, 0x7d},
        {0x7d, 0xab}, {0xff, 0x01}, {0x01, 0xff},
        // added: signed overflow, alternating bits, shift amounts past SEW
        {0x80, 0xff}, {0x55, 0xaa}, {0x01, 0x07}, {0x40, 0x0b}};
    static const std::vector<Pair> e16 = {
        // vadd.vv.toml sew16
        {0xfff8, 0x0000}, {0xff80, 0xfff8}, {0x7fff, 0x0000}, {0x7fff, 0x07ff}, {0x7fff, 0x0001},
        {0xffff, 0x0000}, {0xffff, 0xffff},
        // added
        {0x8000, 0xffff}, {0x8000, 0x0000}, {0x7fff, 0x8000}, {0xabcd, 0x7d00}, {0x0001, 0x000f},
        {0x5555, 0xaaaa}, {0x00ff, 0x0013}};
    static const std::vector<Pair> e32 = {
        // vadd.vv.toml sew32
        {0xfffffff8, 0x00000000}, {0xffffff80, 0xfffffff8}, {0x00007fff, 0x00000000},
        {0x00007fff, 0x000007ff}, {0x00007fff, 0x00000001}, {0xffffffff, 0x00000000},
        {0xffffffff, 0xffffffff},
        // added
        {0x80000000, 0xffffffff}, {0x80000000, 0x00000000}, {0x7fffffff, 0x80000000},
        {0xaaaaaaab, 0x0002fe7d}, {0x00000001, 0x0000001f}, {0x55555555, 0xaaaaaaaa}};
    static const std::vector<Pair> e64 = {
        // vadd.vv.toml sew64
        {0xffffffffffff8000, 0x0000000000000000}, {0xffffffff80000000, 0x0000000000000000},
        {0xffffffff80000000, 0xffffffffffff8000}, {0x0000000000007fff, 0x0000000000000000},
        {0x000000007fffffff, 0x0000000000007fff}, {0xffffffff80000000, 0x0000000000007fff},
        {0x000000007fffffff, 0xffffffffffff8000}, {0xffffffffffffffff, 0x0000000000000001},
        {0xffffffffffffffff, 0x0000000000000000}, {0xffffffffffffffff, 0xffffffffffffffff},
        // vdivu.vv.toml sew64
        {0xaaaaaaaaaaaaaaab, 0x000000000002fe7d},
        // added
        {0x8000000000000000, 0xffffffffffffffff}, {0x8000000000000000, 0x0000000000000000},
        {0x7fffffffffffffff, 0x8000000000000000}, {0x0000000000000001, 0x000000000000003f},
        {0x5555555555555555, 0xaaaaaaaaaaaaaaaa}};
    static std::map<unsigned, std::vector<Pair>> all;
    if (all.empty())
    {
        for (const auto &[w, rows] : std::vector<std::pair<unsigned, const std::vector<Pair> *>>{
                 {8, &e8}, {16, &e16}, {32, &e32}, {64, &e64}})
        {
            all[w] = base;
            all[w].insert(all[w].end(), rows->begin(), rows->end());
        }
    }
    return all.at(sew);
}

uint64_t Mask(unsigned sew)
{
    return sew == 64 ? ~0ULL : (1ULL << sew) - 1;
}

int64_t Signed(uint64_t value, unsigned sew)
{
    const unsigned shift = 64 - sew;
    return static_cast<int64_t>(value << shift) >> shift;
}

enum class Kind
{
    Value, ///< writes vd elements
    Mask,  ///< writes a mask (compares)
};

/// One integer instruction and its reference semantics.
struct EdgeOp
{
    const char *mnemonic; ///< e.g. "vadd.vv"
    Kind kind;
    uint64_t (*reference)(uint64_t vs2, uint64_t src, unsigned sew);
};

uint64_t RefAdd(uint64_t a, uint64_t b, unsigned s) { return (a + b) & Mask(s); }
uint64_t RefSub(uint64_t a, uint64_t b, unsigned s) { return (a - b) & Mask(s); }
uint64_t RefRsub(uint64_t a, uint64_t b, unsigned s) { return (b - a) & Mask(s); }
uint64_t RefAnd(uint64_t a, uint64_t b, unsigned) { return a & b; }
uint64_t RefOr(uint64_t a, uint64_t b, unsigned) { return a | b; }
uint64_t RefXor(uint64_t a, uint64_t b, unsigned) { return a ^ b; }
uint64_t RefSll(uint64_t a, uint64_t b, unsigned s) { return (a << (b & (s - 1))) & Mask(s); }
uint64_t RefSrl(uint64_t a, uint64_t b, unsigned s) { return a >> (b & (s - 1)); }
uint64_t RefSra(uint64_t a, uint64_t b, unsigned s)
{
    return static_cast<uint64_t>(Signed(a, s) >> (b & (s - 1))) & Mask(s);
}
uint64_t RefMul(uint64_t a, uint64_t b, unsigned s)
{
    return static_cast<uint64_t>(static_cast<unsigned __int128>(a) * b) & Mask(s);
}
uint64_t RefMulh(uint64_t a, uint64_t b, unsigned s)
{
    const __int128 p = static_cast<__int128>(Signed(a, s)) * Signed(b, s);
    return static_cast<uint64_t>(p >> s) & Mask(s);
}
uint64_t RefMulhu(uint64_t a, uint64_t b, unsigned s)
{
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    return static_cast<uint64_t>(p >> s) & Mask(s);
}
uint64_t RefMulhsu(uint64_t a, uint64_t b, unsigned s)
{
    const __int128 p = static_cast<__int128>(Signed(a, s)) * static_cast<__int128>(b);
    return static_cast<uint64_t>(p >> s) & Mask(s);
}
uint64_t RefDiv(uint64_t a, uint64_t b, unsigned s)
{
    if (b == 0)
        return Mask(s); // -1
    const int64_t x = Signed(a, s), y = Signed(b, s);
    if (y == -1 && x == Signed(1ULL << (s - 1), s))
        return a; // overflow: the dividend
    return static_cast<uint64_t>(x / y) & Mask(s);
}
uint64_t RefDivu(uint64_t a, uint64_t b, unsigned s) { return b == 0 ? Mask(s) : a / b; }
uint64_t RefRem(uint64_t a, uint64_t b, unsigned s)
{
    if (b == 0)
        return a;
    const int64_t x = Signed(a, s), y = Signed(b, s);
    if (y == -1)
        return 0;
    return static_cast<uint64_t>(x % y) & Mask(s);
}
uint64_t RefRemu(uint64_t a, uint64_t b, unsigned) { return b == 0 ? a : a % b; }
uint64_t RefMin(uint64_t a, uint64_t b, unsigned s) { return Signed(a, s) < Signed(b, s) ? a : b; }
uint64_t RefMinu(uint64_t a, uint64_t b, unsigned) { return a < b ? a : b; }
uint64_t RefMax(uint64_t a, uint64_t b, unsigned s) { return Signed(a, s) > Signed(b, s) ? a : b; }
uint64_t RefMaxu(uint64_t a, uint64_t b, unsigned) { return a > b ? a : b; }
uint64_t RefEq(uint64_t a, uint64_t b, unsigned) { return a == b; }
uint64_t RefNe(uint64_t a, uint64_t b, unsigned) { return a != b; }
uint64_t RefLt(uint64_t a, uint64_t b, unsigned s) { return Signed(a, s) < Signed(b, s); }
uint64_t RefLtu(uint64_t a, uint64_t b, unsigned) { return a < b; }
uint64_t RefLe(uint64_t a, uint64_t b, unsigned s) { return Signed(a, s) <= Signed(b, s); }
uint64_t RefLeu(uint64_t a, uint64_t b, unsigned) { return a <= b; }
uint64_t RefGt(uint64_t a, uint64_t b, unsigned s) { return Signed(a, s) > Signed(b, s); }
uint64_t RefGtu(uint64_t a, uint64_t b, unsigned) { return a > b; }

const std::vector<EdgeOp> &EdgeOps()
{
    static const std::vector<EdgeOp> ops = {
        {"vadd.vv", Kind::Value, RefAdd},     {"vsub.vv", Kind::Value, RefSub},
        {"vand.vv", Kind::Value, RefAnd},     {"vor.vv", Kind::Value, RefOr},
        {"vxor.vv", Kind::Value, RefXor},     {"vsll.vv", Kind::Value, RefSll},
        {"vsrl.vv", Kind::Value, RefSrl},     {"vsra.vv", Kind::Value, RefSra},
        {"vmul.vv", Kind::Value, RefMul},     {"vmulh.vv", Kind::Value, RefMulh},
        {"vmulhu.vv", Kind::Value, RefMulhu}, {"vmulhsu.vv", Kind::Value, RefMulhsu},
        {"vdiv.vv", Kind::Value, RefDiv},     {"vdivu.vv", Kind::Value, RefDivu},
        {"vrem.vv", Kind::Value, RefRem},     {"vremu.vv", Kind::Value, RefRemu},
        {"vmin.vv", Kind::Value, RefMin},     {"vminu.vv", Kind::Value, RefMinu},
        {"vmax.vv", Kind::Value, RefMax},     {"vmaxu.vv", Kind::Value, RefMaxu},
        {"vmseq.vv", Kind::Mask, RefEq},      {"vmsne.vv", Kind::Mask, RefNe},
        {"vmslt.vv", Kind::Mask, RefLt},      {"vmsltu.vv", Kind::Mask, RefLtu},
        {"vmsle.vv", Kind::Mask, RefLe},      {"vmsleu.vv", Kind::Mask, RefLeu},
        {"vadd.vx", Kind::Value, RefAdd},     {"vrsub.vx", Kind::Value, RefRsub},
        {"vsll.vx", Kind::Value, RefSll},     {"vsra.vx", Kind::Value, RefSra},
        {"vmulhsu.vx", Kind::Value, RefMulhsu}, {"vdiv.vx", Kind::Value, RefDiv},
        {"vremu.vx", Kind::Value, RefRemu},   {"vmaxu.vx", Kind::Value, RefMaxu},
        {"vmsgt.vx", Kind::Mask, RefGt},      {"vmsgtu.vx", Kind::Mask, RefGtu},
        {"vmsle.vx", Kind::Mask, RefLe},
    };
    return ops;
}

struct EdgeCase
{
    size_t op;    ///< index into EdgeOps()
    unsigned sew;
};

std::vector<EdgeCase> AllEdgeCases()
{
    std::vector<EdgeCase> cases;
    for (size_t op = 0; op < EdgeOps().size(); ++op)
        for (unsigned sew : {8u, 16u, 32u, 64u})
            cases.push_back({op, sew});
    return cases;
}

std::string EdgeCaseName(const ::testing::TestParamInfo<EdgeCase> &info)
{
    std::string name = EdgeOps()[info.param.op].mnemonic;
    for (char &c : name)
        if (c == '.')
            c = '_';
    return name + "_e" + std::to_string(info.param.sew);
}

bool IsVx(const EdgeOp &op)
{
    return std::string(op.mnemonic).find(".vx") != std::string::npos;
}

std::string Hex(uint64_t v)
{
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}

/// A program running @p op on @p rows. vv: one vector op over all rows. vx: the vector of vs2
/// values against each x[rs1] value in turn, one output row per scalar.
std::string EdgeProgram(const EdgeOp &op, unsigned sew, const std::vector<Pair> &rows)
{
    const char *directive = sew == 8 ? ".byte" : sew == 16 ? ".halfword" : sew == 32 ? ".word"
                                                                                       : ".dword";
    const char *load = sew == 8 ? "lb" : sew == 16 ? "lh" : sew == 32 ? "lw" : "ld";
    const unsigned bytes = sew / 8;
    std::ostringstream p;
    p << ".data\na: " << directive;
    for (size_t i = 0; i < rows.size(); ++i)
        p << (i ? ", " : " ") << Hex(rows[i].first);
    p << "\nb: " << directive;
    for (size_t i = 0; i < rows.size(); ++i)
        p << (i ? ", " : " ") << Hex(rows[i].second);
    p << "\nout: .zero 4096\n.text\n"
      << "    la s0, a\n    la s1, b\n    la s2, out\n"
      << "    li t1, " << rows.size() << "\n"
      << "    vsetvli t0, t1, e" << sew << ", m8, ta, ma\n"
      << "    vle" << sew << ".v v8, (s0)\n";
    const std::string store = op.kind == Kind::Mask ? "    vsm.v v24, (s2)\n"
                                                    : "    vse" + std::to_string(sew) +
                                                          ".v v24, (s2)\n";
    if (!IsVx(op))
    {
        p << "    vle" << sew << ".v v16, (s1)\n"
          << "    " << op.mnemonic << " v24, v8, v16\n"
          << store;
    }
    else
    {
        const size_t rowStride = op.kind == Kind::Mask ? 8 : rows.size() * bytes;
        p << "    li t3, " << rows.size() << "\n"
          << "next_scalar:\n"
          << "    " << load << " t2, 0(s1)\n"
          << "    " << op.mnemonic << " v24, v8, t2\n"
          << store << "    addi s1, s1, " << bytes << "\n"
          << "    addi s2, s2, " << rowStride << "\n"
          << "    addi t3, t3, -1\n"
          << "    bnez t3, next_scalar\n";
    }
    return p.str();
}

/// Runs the edge case on @p Vm and checks every output against the reference model. The pairs
/// go in chunks of at most 16 (VLMAX at e64/m8 with VLEN = 128).
template <typename Vm> void CheckEdgeCase(const EdgeCase &edge)
{
    const EdgeOp &op = EdgeOps()[edge.op];
    const unsigned sew = edge.sew;
    const unsigned bytes = sew / 8;
    const std::vector<Pair> &all = EdgePairs(sew);
    for (size_t start = 0; start < all.size(); start += 16)
    {
        const std::vector<Pair> rows(all.begin() + start,
                                     all.begin() + std::min(all.size(), start + 16));
        Kites::setupVmStateDirectory();
        std::istringstream stream(EdgeProgram(op, sew, rows));
        auto vm = std::make_unique<Vm>();
        vm->LoadProgram(Kites::assemble(stream));
        vm->DebugRun();
        const uint64_t out = LabelAddress(*vm, "out");
        const auto readValue = [&](uint64_t address)
        {
            uint64_t v = 0;
            for (unsigned i = 0; i < bytes; ++i)
                v |= static_cast<uint64_t>(vm->memory_controller_.peekByte(address + i)) << (8 * i);
            return v;
        };
        const auto readBit = [&](uint64_t address, size_t bit)
        { return (vm->memory_controller_.peekByte(address + bit / 8) >> (bit % 8)) & 1; };

        const size_t scalars = IsVx(op) ? rows.size() : 1;
        const size_t rowStride = op.kind == Kind::Mask ? 8 : rows.size() * bytes;
        for (size_t k = 0; k < scalars; ++k)
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const uint64_t vs2 = rows[i].first;
                const uint64_t src = IsVx(op) ? rows[k].second : rows[i].second;
                const uint64_t expected = op.reference(vs2, src, sew);
                const uint64_t actual = op.kind == Kind::Mask
                                            ? readBit(out + k * rowStride, i)
                                            : readValue(out + k * rowStride + i * bytes);
                EXPECT_EQ(actual, expected)
                    << op.mnemonic << " e" << sew << ": vs2 = " << Hex(vs2)
                    << ", " << (IsVx(op) ? "x[rs1]" : "vs1") << " = " << Hex(src);
            }
        }
    }
}
} // namespace
} // namespace vtest_edge

// Integer code examples from the RISC-V "V" vector extension specification (vector-vector add,
// memcpy, strlen, strcpy), written for Kites' assembler, plus a conditional (masked) loop. Each
// returns its inputs and the expected result, computed in C++.
namespace vtest_spec_examples
{
namespace
{
struct SpecProgram
{
    std::string name;
    std::string source;
    std::function<void(Kites::ProcessorBase &)> check;
};

uint8_t Byte(Kites::ProcessorBase &vm, uint64_t address)
{
    return vm.memory_controller_.peekByte(address);
}

uint32_t Word32(Kites::ProcessorBase &vm, uint64_t address)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | Byte(vm, address + i);
    return v;
}

uint64_t Symbol(Kites::ProcessorBase &vm, const char *name)
{
    return vtest_edge::LabelAddress(vm, name);
}

std::vector<SpecProgram> SpecPrograms()
{
    std::vector<SpecProgram> programs;

    { // vvaddint32: c[i] = a[i] + b[i], n = 37, strip-mined at e32/m1
        std::ostringstream src;
        src << ".data\na: .word";
        for (int i = 0; i < 37; ++i)
            src << (i ? ", " : " ") << (i * 7 - 100);
        src << "\nb: .word";
        for (int i = 0; i < 37; ++i)
            src << (i ? ", " : " ") << (1000 - i * i);
        src << "\nc: .zero 160\n.text\n"
               "    li a0, 37\n    la a1, a\n    la a2, b\n    la a3, c\n"
               "vvaddint32:\n"
               "    vsetvli t0, a0, e32, m1, ta, ma\n"
               "    vle32.v v0, (a1)\n"
               "    sub a0, a0, t0\n"
               "    slli t0, t0, 2\n"
               "    add a1, a1, t0\n"
               "    vle32.v v1, (a2)\n"
               "    add a2, a2, t0\n"
               "    vadd.vv v2, v0, v1\n"
               "    vse32.v v2, (a3)\n"
               "    add a3, a3, t0\n"
               "    bnez a0, vvaddint32\n";
        programs.push_back({"vvaddint32", src.str(), [](Kites::ProcessorBase &vm)
                            {
                                const uint64_t c = Symbol(vm, "c");
                                for (int i = 0; i < 37; ++i)
                                    EXPECT_EQ(static_cast<int32_t>(Word32(vm, c + 4 * i)),
                                              (i * 7 - 100) + (1000 - i * i))
                                        << "c[" << i << "]";
                            }});
    }

    { // memcpy of 300 bytes with e8/m8 strips
        std::ostringstream src;
        src << ".data\nsrc: .byte";
        for (int i = 0; i < 300; ++i)
            src << (i ? ", " : " ") << ((i * 37 + 11) & 0xff);
        src << "\ndst: .zero 320\n.text\n"
               "    la a0, dst\n    la a1, src\n    li a2, 300\n"
               "    mv a3, a0\n"
               "memcpy_loop:\n"
               "    vsetvli t0, a2, e8, m8, ta, ma\n"
               "    vle8.v v0, (a1)\n"
               "    add a1, a1, t0\n"
               "    sub a2, a2, t0\n"
               "    vse8.v v0, (a3)\n"
               "    add a3, a3, t0\n"
               "    bnez a2, memcpy_loop\n";
        programs.push_back({"memcpy", src.str(), [](Kites::ProcessorBase &vm)
                            {
                                const uint64_t dst = Symbol(vm, "dst");
                                for (int i = 0; i < 300; ++i)
                                    EXPECT_EQ(Byte(vm, dst + i), (i * 37 + 11) & 0xff)
                                        << "dst[" << i << "]";
                                EXPECT_EQ(Byte(vm, dst + 300), 0) << "wrote past the end";
                            }});
    }

    const std::string text = "The quick brown fox jumps over the lazy dog and vector strings span "
                             "several strips of the e8 m8 loop when they are this long which "
                             "is the point of testing them here with fault only first loads";
    { // strlen with vle8ff.v / vmseq.vi / vfirst.m
        const std::string src = ".data\nstr: .string \"" + text + "\"\n.text\n"
                                "    la a0, str\n"
                                "    mv a3, a0\n"
                                "strlen_loop:\n"
                                "    vsetvli a1, x0, e8, m8, ta, ma\n"
                                "    vle8ff.v v8, (a3)\n"
                                "    csrrs a1, vl, x0\n"
                                "    vmseq.vi v0, v8, 0\n"
                                "    vfirst.m a2, v0\n"
                                "    add a3, a3, a1\n"
                                "    bltz a2, strlen_loop\n"
                                "    add a0, a0, a1\n"
                                "    add a3, a3, a2\n"
                                "    sub a0, a3, a0\n";
        const size_t length = text.size();
        programs.push_back({"strlen", src, [length](Kites::ProcessorBase &vm)
                            { EXPECT_EQ(vm.registers_.ReadGpr(10), length); }});
    }

    { // strcpy: copies up to and including the terminator with a vmsif.m mask
        const std::string src = ".data\nstr: .string \"" + text + "\"\n"
                                "copy: .zero 300\n.text\n"
                                "    la a0, copy\n    la a1, str\n"
                                "    mv a2, a0\n"
                                "    li t0, -1\n"
                                "strcpy_loop:\n"
                                "    vsetvli x0, t0, e8, m8, ta, ma\n"
                                "    vle8ff.v v8, (a1)\n"
                                "    csrrs t1, vl, x0\n"
                                "    vmseq.vi v1, v8, 0\n"
                                "    vfirst.m a3, v1\n"
                                "    add a1, a1, t1\n"
                                "    vmsif.m v0, v1\n"
                                "    vse8.v v8, (a2), v0.t\n"
                                "    add a2, a2, t1\n"
                                "    bltz a3, strcpy_loop\n";
        programs.push_back({"strcpy", src, [text](Kites::ProcessorBase &vm)
                            {
                                const uint64_t copy = Symbol(vm, "copy");
                                for (size_t i = 0; i < text.size(); ++i)
                                    EXPECT_EQ(Byte(vm, copy + i), static_cast<uint8_t>(text[i]))
                                        << "copy[" << i << "]";
                                EXPECT_EQ(Byte(vm, copy + text.size()), 0);
                            }});
    }

    { // conditional: c[i] = a[i] < 5 ? a[i] + b[i] : b[i], with a mask and vmerge-free masking
        std::ostringstream src;
        src << ".data\na: .word";
        for (int i = 0; i < 20; ++i)
            src << (i ? ", " : " ") << (i % 9);
        src << "\nb: .word";
        for (int i = 0; i < 20; ++i)
            src << (i ? ", " : " ") << (100 + i);
        src << "\nc: .zero 80\n.text\n"
               "    li a0, 20\n    la a1, a\n    la a2, b\n    la a3, c\n"
               "cond_loop:\n"
               "    vsetvli t0, a0, e32, m2, ta, mu\n"
               "    vle32.v v8, (a1)\n"
               "    vle32.v v4, (a2)\n"
               "    vmslt.vi v0, v8, 5\n"
               "    vadd.vv v4, v4, v8, v0.t\n"
               "    vse32.v v4, (a3)\n"
               "    sub a0, a0, t0\n"
               "    slli t0, t0, 2\n"
               "    add a1, a1, t0\n"
               "    add a2, a2, t0\n"
               "    add a3, a3, t0\n"
               "    bnez a0, cond_loop\n";
        programs.push_back({"conditional", src.str(), [](Kites::ProcessorBase &vm)
                            {
                                const uint64_t c = Symbol(vm, "c");
                                for (int i = 0; i < 20; ++i)
                                {
                                    const int a = i % 9, b = 100 + i;
                                    EXPECT_EQ(static_cast<int32_t>(Word32(vm, c + 4 * i)),
                                              a < 5 ? a + b : b)
                                        << "c[" << i << "]";
                                }
                            }});
    }
    return programs;
}

template <typename Vm> void RunSpecProgram(const SpecProgram &program)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(program.source);
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(Kites::assemble(stream));
    vm->DebugRun();
    program.check(*vm);
}
} // namespace
} // namespace vtest_spec_examples

// Random straight-line programs mixing vector arithmetic, vector loads/stores, vset*, scalar
// loads/stores and vector<->scalar moves with dense dependencies. The pipelines must end in the
// same state as the single-cycle VM (custom stress for forwarding and hazard detection).
namespace vtest_random
{
namespace
{
std::string RandomProgram(uint32_t seed, int length)
{
    std::mt19937 rng(seed);
    const auto pick = [&rng](int n) { return static_cast<int>(rng() % n); };
    std::ostringstream p;
    p << ".data\nbuf: .word";
    for (int i = 0; i < 64; ++i)
        p << (i ? ", " : " ") << static_cast<int32_t>(rng());
    p << "\n.text\n    la s0, buf\n    vsetivli t0, 4, e32, m1, tu, mu\n";
    const char *scalarRegs[] = {"t1", "t2", "t3", "a0", "a1"};
    const char *arith[] = {"vadd", "vsub", "vmul", "vand", "vor", "vxor", "vmaxu", "vmin",
                           "vsll", "vsrl", "vdivu", "vremu"};
    for (int n = 0; n < length; ++n)
    {
        const int vd = 1 + pick(7), vs1 = 1 + pick(7), vs2 = 1 + pick(7);
        const char *x = scalarRegs[pick(5)];
        const char *y = scalarRegs[pick(5)];
        const int offset = 4 * pick(48);
        switch (pick(12))
        {
        case 0:
        case 1:
        case 2:
            p << "    " << arith[pick(12)] << ".vv v" << vd << ", v" << vs2 << ", v" << vs1 << "\n";
            break;
        case 3:
            p << "    " << arith[pick(12)] << ".vx v" << vd << ", v" << vs2 << ", " << x << "\n";
            break;
        case 4:
            p << "    addi t4, s0, " << offset << "\n    vle32.v v" << vd << ", (t4)\n";
            break;
        case 5:
            p << "    addi t4, s0, " << offset << "\n    vse32.v v" << vs2 << ", (t4)\n";
            break;
        case 6:
            p << "    lw " << x << ", " << offset << "(s0)\n";
            break;
        case 7:
            p << "    sw " << x << ", " << offset << "(s0)\n";
            break;
        case 8:
            p << "    vmv.x.s " << x << ", v" << vs2 << "\n";
            break;
        case 9:
            p << "    add " << x << ", " << x << ", " << y << "\n";
            break;
        case 10:
            p << "    vsetivli t0, " << (1 + pick(4)) << ", e32, m1, tu, mu\n";
            break;
        default:
            p << "    vredsum.vs v" << vd << ", v" << vs2 << ", v" << vs1 << "\n";
            break;
        }
    }
    return p.str();
}

struct MachineState
{
    std::vector<uint64_t> gprs;
    std::vector<std::vector<uint8_t>> vregs;
    uint64_t vl = 0, vtype = 0;
    std::vector<uint8_t> data;
};

template <typename Vm> MachineState RunRandom(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    auto vm = std::make_unique<Vm>();
    vm->LoadProgram(Kites::assemble(stream));
    vm->DebugRun();
    MachineState state;
    for (size_t r = 0; r < 32; ++r)
    {
        state.gprs.push_back(vm->registers_.ReadGpr(r));
        state.vregs.push_back(vm->registers_.ReadVectorRegister(r));
    }
    state.vl = vm->registers_.ReadCsr(Kites::RegisterFile::CSR_VL);
    state.vtype = vm->registers_.ReadCsr(Kites::RegisterFile::CSR_VTYPE);
    const uint64_t buf = vtest_edge::LabelAddress(*vm, "buf");
    for (uint64_t i = 0; i < 256; ++i)
        state.data.push_back(vm->memory_controller_.peekByte(buf + i));
    return state;
}

void ExpectSameMachineState(const MachineState &expected, const MachineState &actual,
                            const std::string &source)
{
    for (size_t r = 1; r < 32; ++r)
        EXPECT_EQ(actual.gprs[r], expected.gprs[r]) << "x" << r << "\n" << source;
    for (size_t r = 0; r < 32; ++r)
        EXPECT_EQ(actual.vregs[r], expected.vregs[r]) << "v" << r;
    EXPECT_EQ(actual.vl, expected.vl);
    EXPECT_EQ(actual.vtype, expected.vtype);
    EXPECT_EQ(actual.data, expected.data);
}
} // namespace
} // namespace vtest_random
