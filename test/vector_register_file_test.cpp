#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "config/config.h"
#include "processor/registers.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

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

TEST(VectorRegisterFileTest, ResetStateMatchesSpike)
{
    RegisterFile rf;
    const size_t vlenBytes = Kites::vm_config::config.getVlen() / 8;

    EXPECT_EQ(rf.GetVlenBytes(), vlenBytes);
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VLENB), vlenBytes);
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VTYPE), RegisterFile::VTYPE_VILL);
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VL), 0u);
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VSTART), 0u);
    for (size_t reg = 0; reg < RegisterFile::NUM_VR; ++reg)
    {
        EXPECT_EQ(rf.ReadVectorRegister(reg), std::vector<uint8_t>(vlenBytes, 0)) << "v" << reg;
    }
}

TEST(VectorRegisterFileTest, ElementsAreLittleEndianAcrossWidths)
{
    RegisterFile rf;

    rf.WriteVectorElement(3, 1, 32, 0xAABBCCDD);
    // e32 element 1 is bytes 4..7 of v3.
    EXPECT_EQ(rf.ReadVectorElement(3, 1, 32), 0xAABBCCDDu);
    EXPECT_EQ(rf.ReadVectorElement(3, 4, 8), 0xDDu);
    EXPECT_EQ(rf.ReadVectorElement(3, 7, 8), 0xAAu);
    EXPECT_EQ(rf.ReadVectorElement(3, 2, 16), 0xCCDDu);
    EXPECT_EQ(rf.ReadVectorElement(3, 0, 64), 0xAABBCCDD00000000u);
    // Neighbouring registers untouched.
    EXPECT_EQ(rf.ReadVectorElement(2, 0, 64), 0u);
    EXPECT_EQ(rf.ReadVectorElement(4, 0, 64), 0u);
}

TEST(VectorRegisterFileTest, WriteTruncatesToSew)
{
    RegisterFile rf;

    rf.WriteVectorElement(1, 0, 8, 0x1FF);
    EXPECT_EQ(rf.ReadVectorElement(1, 0, 8), 0xFFu);
    EXPECT_EQ(rf.ReadVectorElement(1, 1, 8), 0u);

    rf.WriteVectorElement(1, 1, 16, 0xFFFF'1234);
    EXPECT_EQ(rf.ReadVectorElement(1, 1, 16), 0x1234u);
}

TEST(VectorRegisterFileTest, IndexPastRegisterContinuesIntoNextRegister)
{
    RegisterFile rf;
    const size_t elementsPerReg = rf.GetVlenBytes() / 8;

    // Element elementsPerReg of v4 at e64 is element 0 of v5: what an LMUL=2 group needs.
    rf.WriteVectorElement(4, elementsPerReg, 64, 0x0123456789ABCDEF);
    EXPECT_EQ(rf.ReadVectorElement(5, 0, 64), 0x0123456789ABCDEFu);
}

TEST(VectorRegisterFileTest, MaskBits)
{
    RegisterFile rf;

    rf.WriteMaskBit(0, 0, true);
    rf.WriteMaskBit(0, 9, true);
    EXPECT_TRUE(rf.ReadMaskBit(0, 0));
    EXPECT_FALSE(rf.ReadMaskBit(0, 1));
    EXPECT_TRUE(rf.ReadMaskBit(0, 9));
    EXPECT_EQ(rf.ReadVectorElement(0, 0, 16), 0x0201u);

    rf.WriteMaskBit(0, 0, false);
    EXPECT_FALSE(rf.ReadMaskBit(0, 0));
    EXPECT_TRUE(rf.ReadMaskBit(0, 9));
}

TEST(VectorRegisterFileTest, InvalidAccessesThrow)
{
    RegisterFile rf;
    const size_t lastE64 = RegisterFile::NUM_VR * rf.GetVlenBytes() / 8 - 1;

    EXPECT_THROW(rf.ReadVectorElement(32, 0, 8), std::out_of_range);
    EXPECT_THROW(rf.ReadVectorElement(31, rf.GetVlenBytes() / 8, 64), std::out_of_range);
    EXPECT_NO_THROW(rf.ReadVectorElement(0, lastE64, 64));
    EXPECT_THROW(rf.ReadVectorElement(0, lastE64 + 1, 64), std::out_of_range);
    EXPECT_THROW(rf.ReadVectorElement(0, 0, 12), std::invalid_argument);
    EXPECT_THROW(rf.ReadMaskBit(0, rf.GetVlenBytes() * 8), std::out_of_range);
    EXPECT_THROW(rf.WriteVectorRegister(0, std::vector<uint8_t>(3, 0)), std::invalid_argument);
}

TEST(VectorRegisterFileTest, VlenComesFromConfigOnReset)
{
    VlenGuard guard;
    Kites::vm_config::config.setVlen(256);

    RegisterFile rf;
    EXPECT_EQ(rf.GetVlenBytes(), 32u);
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VLENB), 32u);
    EXPECT_EQ(rf.ReadVectorRegister(31).size(), 32u);

    Kites::vm_config::config.setVlen(64);
    rf.Reset();
    EXPECT_EQ(rf.ReadCsr(RegisterFile::CSR_VLENB), 8u);
}

TEST(VectorRegisterFileTest, InvalidVlenRejected)
{
    VlenGuard guard;
    EXPECT_THROW(Kites::vm_config::config.setVlen(32), std::invalid_argument);
    EXPECT_THROW(Kites::vm_config::config.setVlen(192), std::invalid_argument);
    EXPECT_THROW(Kites::vm_config::config.setVlen(131072), std::invalid_argument);
}

TEST(VectorRegisterFileTest, ReadOnlyCsrAddresses)
{
    EXPECT_TRUE(Kites::IsReadOnlyCsr(RegisterFile::CSR_VL));
    EXPECT_TRUE(Kites::IsReadOnlyCsr(RegisterFile::CSR_VTYPE));
    EXPECT_TRUE(Kites::IsReadOnlyCsr(RegisterFile::CSR_VLENB));
    EXPECT_FALSE(Kites::IsReadOnlyCsr(RegisterFile::CSR_VSTART));
    EXPECT_FALSE(Kites::IsReadOnlyCsr(0x003)); // fcsr
}

TEST(VectorRegisterFileTest, VectorRegisterNames)
{
    EXPECT_TRUE(Kites::IsValidVectorRegister("v0"));
    EXPECT_TRUE(Kites::IsValidVectorRegister("v31"));
    EXPECT_FALSE(Kites::IsValidVectorRegister("v32"));
    EXPECT_FALSE(Kites::IsValidVectorRegister("x1"));
}

TEST(VectorCsrInstructionTest, CsrrsReadsVectorCsrs)
{
    auto vm = runSource("csrrs x5, vlenb, x0\n"
                        "csrrs x6, vtype, x0\n"
                        "csrrs x7, vl, x0\n");
    EXPECT_EQ(vm->registers_.ReadGpr(5), vm->registers_.GetVlenBytes());
    EXPECT_EQ(vm->registers_.ReadGpr(6), RegisterFile::VTYPE_VILL);
    EXPECT_EQ(vm->registers_.ReadGpr(7), 0u);
}

TEST(VectorCsrInstructionTest, VstartIsWritable)
{
    auto vm = runSource("li x1, 3\n"
                        "csrrw x0, vstart, x1\n"
                        "csrrs x2, vstart, x0\n");
    EXPECT_EQ(vm->registers_.ReadGpr(2), 3u);
}

TEST(VectorCsrInstructionTest, WritingReadOnlyVectorCsrThrows)
{
    EXPECT_THROW(runSource("li x1, 4\n"
                           "csrrw x0, vl, x1\n"),
                 std::runtime_error);
    EXPECT_THROW(runSource("li x1, 4\n"
                           "csrrs x0, vlenb, x1\n"),
                 std::runtime_error);
}

TEST(VectorUndoRedoTest, VectorRegisterChangeIsUndoneAndRedone)
{
    auto vm = runSource("addi x1, x0, 1\n");
    ASSERT_FALSE(vm->undo_stack_.empty());

    const size_t vlenBytes = vm->registers_.GetVlenBytes();
    std::vector<uint8_t> before(vlenBytes, 0);
    std::vector<uint8_t> after(vlenBytes, 0xA5);

    // No vector instruction exists yet, so attach a vector write to the last step by hand.
    vm->registers_.WriteVectorRegister(2, after);
    Kites::RegisterChange change{2, 3, 0, 0};
    change.old_bytes = before;
    change.new_bytes = after;
    vm->undo_stack_.top().register_changes.push_back(change);

    vm->Undo();
    EXPECT_EQ(vm->registers_.ReadVectorRegister(2), before);
    EXPECT_EQ(vm->registers_.ReadGpr(1), 0u);

    vm->Redo();
    EXPECT_EQ(vm->registers_.ReadVectorRegister(2), after);
    EXPECT_EQ(vm->registers_.ReadGpr(1), 1u);
}
