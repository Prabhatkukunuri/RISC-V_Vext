// Vector extension tests on the single-cycle VM (RVSS) and the vector units it uses: the
// register file and CSRs, vset*, every integer instruction group, loads/stores, the staged
// EX/MEM/WB split and the spec-rule sweeps. Assembler encodings are checked here too.
// Tests that run on the 5-stage pipelines are in vector_rv5s_tests.cpp.

#include "vector_test_helpers.h"

// ===== vset_test =============================================================================
// Tests for the RVV configuration-setting instructions: vsetvli, vsetivli and vsetvl.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding);
// the vl/vtype rules follow RVV 1.0 section 6 and Spike's set_vl().
namespace vtest_vset
{

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

TEST(VsetRulesTest, VlmaxForEveryLegalSewLmul)
{
    // VLMAX = LMUL * VLEN / SEW, VLEN = 128.
    struct Case
    {
        uint64_t sew;
        int lmulLog2;
        uint64_t vlmax;
    };
    // All 22 legal pairs: every SEW with m1..m8, and the fractional LMULs with SEW <= LMUL * ELEN.
    const Case cases[] = {
        {8, 0, 16},  {8, 1, 32},  {8, 2, 64},  {8, 3, 128},
        {16, 0, 8},  {16, 1, 16}, {16, 2, 32}, {16, 3, 64},
        {32, 0, 4},  {32, 1, 8},  {32, 2, 16}, {32, 3, 32},
        {64, 0, 2},  {64, 1, 4},  {64, 2, 8},  {64, 3, 16},
        {8, -1, 8},  {8, -2, 4},  {8, -3, 2},
        {16, -1, 4}, {16, -2, 2},
        {32, -1, 2},
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

} // namespace vtest_vset

// ===== vector_register_file_test =============================================================
namespace vtest_register_file
{

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

} // namespace vtest_register_file

// ===== vector_int_arith_test =================================================================
// Tests for the RVV single-width integer add/subtract instructions:
// vadd.vv/.vx/.vi, vsub.vv/.vx, vrsub.vx/.vi.
namespace vtest_int_arith
{

TEST(VectorIntArithAssemblerTest, EncodingsMatchLlvmMc)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vadd.vv v1, v2, v3", 0x022180d7},
        {"vadd.vv v8, v16, v24, v0.t", 0x010c0457},
        {"vadd.vx v4, v5, x10", 0x02554257},
        {"vadd.vx v31, v30, t6, v0.t", 0x01efcfd7},
        {"vadd.vi v6, v7, -16", 0x02783357},
        {"vadd.vi v6, v7, 15, v0.t", 0x0077b357},
        {"vsub.vv v1, v2, v3", 0x0a2180d7},
        {"vsub.vx v9, v10, a1, v0.t", 0x08a5c4d7},
        {"vrsub.vx v12, v13, t0", 0x0ed2c657},
        {"vrsub.vi v14, v15, -1", 0x0effb757},
        {"vrsub.vi v0, v1, 0", 0x0e103057},
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

TEST(VectorIntArithAssemblerTest, IllegalFormsAreRejected)
{
    const std::vector<std::string> bad = {
        "vsub.vi v1, v2, 3\n",           // no such form
        "vrsub.vv v1, v2, v3\n",         // no such form
        "vadd.vi v1, v2, 16\n",          // simm5 is -16..15
        "vadd.vi v1, v2, -17\n",
        "vadd.vv v0, v1, v2, v0.t\n",    // masked op can't write the mask register
        "vadd.vv v1, v2, x3\n",          // wrong operand kinds
        "vadd.vx v1, v2, v3\n",
        "vadd.vv v1, v2, v3, v1.t\n",    // only v0 can be a mask
        "vadd.vv v1, v2, v3, v0\n",
        "vadd.vv v1, v2\n",
        "vadd.vv v1, v2, v0.t\n",
    };
    for (const std::string &line : bad)
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

TEST(VectorIntArithAssemblerTest, DisassemblyRoundTrips)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x022180d7), "vadd.vv v1, v2, v3");
    EXPECT_EQ(disassemble(0x010c0457), "vadd.vv v8, v16, v24, v0.t");
    EXPECT_EQ(disassemble(0x02554257), "vadd.vx v4, v5, x10");
    EXPECT_EQ(disassemble(0x02783357), "vadd.vi v6, v7, -16");
    EXPECT_EQ(disassemble(0x0077b357), "vadd.vi v6, v7, 15, v0.t");
    EXPECT_EQ(disassemble(0x0ed2c657), "vrsub.vx v12, v13, x5");
    EXPECT_EQ(disassemble(0x0effb757), "vrsub.vi v14, v15, -1");
}

TEST(VectorIntArithTest, VaddVvWrapsAtSew32)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vadd.vv v1, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 32, {1, 2, 3, 0xFFFFFFFF});
                                              writeElements(rf, 3, 32, {10, 20, 30, 1});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{11, 22, 33, 0}));
}

TEST(VectorIntArithTest, VsubVvAtSew64)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsub.vv v1, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 64, {100, 0});
                                              writeElements(rf, 3, 64, {1, 1});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 64, 2), (Elements{99, ~0ULL}));
}

TEST(VectorIntArithTest, ScalarOperandIsTruncatedToSew)
{
    // x10 = 0x1_0000_0105: only the low 8 bits (0x05) take part at e8.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0x10000\n"
                                          "slli x10, x10, 16\n"
                                          "addi x10, x10, 0x105\n"
                                          "vsetivli x0, 16, e8, m1, ta, ma\n"
                                          "vadd.vx v1, v2, x10\n"
                                          "vsub.vx v3, v2, x10\n"
                                          "vrsub.vx v4, v2, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 8, {0, 1, 0xFF, 0xFB}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 8, 4), (Elements{0x05, 0x06, 0x04, 0x00}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 4), (Elements{0xFB, 0xFC, 0xFA, 0xF6}));
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x05, 0x04, 0x06, 0x0A}));
}

TEST(VectorIntArithTest, ImmediateIsSignExtendedToSew)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e16, m1, ta, ma\n"
                                          "vadd.vi v1, v2, -16\n"
                                          "vadd.vi v3, v2, 15\n"
                                          "vrsub.vi v4, v2, -1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 16, {0, 20, 0xFFFF}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 16, 3), (Elements{0xFFF0, 4, 0xFFEF}));
    EXPECT_EQ(readElements(vm->registers_, 3, 16, 3), (Elements{15, 35, 14}));
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 3), (Elements{0xFFFF, 0xFFEB, 0}));
}

TEST(VectorIntArithTest, MaskedOffAndTailElementsAreUndisturbed)
{
    // vl = 3 of VLMAX 4; v0 = 0b0101 enables elements 0 and 2. Element 1 is masked off,
    // element 3 is tail: both keep vd's old value.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vadd.vi v1, v2, 1, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 1, 32, {7, 7, 7, 7});
                                              writeElements(rf, 2, 32, {10, 20, 30, 40});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{11, 7, 31, 7}));
}

TEST(VectorIntArithTest, StartsAtVstartAndClearsIt)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "csrrwi x0, vstart, 2\n"
                                          "vadd.vi v1, v2, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 32, {10, 20, 30, 40}); });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{0, 0, 31, 41}));
    EXPECT_EQ(vm->registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VectorIntArithTest, Lmul2GroupSpansTwoRegisters)
{
    // e32, m2: VLMAX 8, so v2/v3 and v4/v5 act as 8-element groups.
    auto vm = runOn<Kites::RVSSProcessor>("vsetvli x5, x0, e32, m2, ta, ma\n"
                                          "vadd.vi v2, v4, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 4, 32, {0, 1, 2, 3, 4, 5, 6, 7}); });
    EXPECT_EQ(vm->registers_.ReadGpr(5), 8u);
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{1, 2, 3, 4}));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4), (Elements{5, 6, 7, 8}));
}

TEST(VectorIntArithTest, IllegalInstructionsTrap)
{
    // No vsetvli: vtype is still vill.
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vadd.vv v1, v2, v3\n"), std::runtime_error);
    // LMUL = 2 needs even register numbers.
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e32, m2, ta, ma\n"
                                             "vadd.vv v3, v2, v4\n"),
                 std::runtime_error);
    EXPECT_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e32, m2, ta, ma\n"
                                             "vadd.vx v2, v5, x1\n"),
                 std::runtime_error);

    // A masked op writing v0 can't be assembled, so patch vd to 0 in the machine code.
    Kites::AssembledProgram program = assembleSource("vsetivli x0, 4, e32, m1, ta, ma\n"
                                                     "vadd.vv v1, v2, v3, v0.t\n");
    program.text_buffer[1] &= ~(0x1Fu << 7);
    Kites::setupVmStateDirectory();
    Kites::RVSSProcessor vm;
    vm.LoadProgram(program);
    EXPECT_THROW(vm.DebugRun(), std::runtime_error);
}

TEST(VectorIntArithTest, UndoAndRedoRestoreDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vadd.vi v1, v2, 5\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 32, {9, 9, 9, 9});
                                              writeElements(rf, 2, 32, {1, 2, 3, 4});
                                          });
    const Elements after{6, 7, 8, 9};
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), after);

    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), (Elements{9, 9, 9, 9}));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), after);
}

} // namespace vtest_int_arith

// ===== vector_carry_test =====================================================================
// Tests for RVV add-with-carry / subtract-with-borrow (11.4): vadc, vsbc, vmadc, vmsbc.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: v0 supplies the carry/borrow-in, every body element is written, vadc/vsbc
// cannot write v0 and are reserved with vm = 1, and vmadc/vmsbc write a mask (even into v0).
namespace vtest_carry
{

TEST(VectorCarryAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vadc.vvm v1, v2, v3, v0", 0x402180d7},  {"vadc.vxm v1, v2, a0, v0", 0x402540d7},
        {"vadc.vim v1, v2, -16, v0", 0x402830d7}, {"vmadc.vvm v1, v2, v3, v0", 0x442180d7},
        {"vmadc.vxm v0, v2, a0, v0", 0x44254057}, {"vmadc.vim v1, v2, 15, v0", 0x4427b0d7},
        {"vmadc.vv v1, v2, v3", 0x462180d7},      {"vmadc.vx v1, v2, a0", 0x462540d7},
        {"vmadc.vi v1, v2, -1", 0x462fb0d7},      {"vsbc.vvm v1, v2, v3, v0", 0x482180d7},
        {"vsbc.vxm v1, v2, a0, v0", 0x482540d7},  {"vmsbc.vvm v1, v2, v3, v0", 0x4c2180d7},
        {"vmsbc.vxm v1, v2, a0, v0", 0x4c2540d7}, {"vmsbc.vv v1, v2, v3", 0x4e2180d7},
        {"vmsbc.vx v1, v2, a0", 0x4e2540d7},
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
    }

    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0x402830d7), "vadc.vim v1, v2, -16, v0");
    EXPECT_EQ(disassemble(0x44254057), "vmadc.vxm v0, v2, x10, v0");
    EXPECT_EQ(disassemble(0x462180d7), "vmadc.vv v1, v2, v3");
}

TEST(VectorCarryAssemblerTest, OperandRules)
{
    for (const std::string &line : {
             "vadc.vvm v0, v2, v3, v0\n",     // v0 holds the carry-in
             "vsbc.vxm v0, v2, a0, v0\n",
             "vadc.vvm v1, v2, v3\n",         // the v0 operand is required
             "vadc.vvm v1, v2, v3, v0.t\n",   // ...and is written "v0"
             "vmadc.vv v1, v2, v3, v0.t\n",   // the no-carry forms take no mask
             "vmadc.vv v1, v2, v3, v0\n",
             "vadc.vim v1, v2, 16, v0\n",     // simm5
             "vsbc.vim v1, v2, 3, v0\n",      // no subtract-immediate forms
             "vmsbc.vi v1, v2, 3\n",
         })
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // The carry-out may go to v0.
    EXPECT_NO_THROW(assembleSource("vmadc.vvm v0, v2, v3, v0\nvmsbc.vv v0, v2, v3\n"));
}

TEST(VectorCarryTest, VectorVectorForms)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vvm v4, v2, v3, v0\n"
                                          "vmadc.vvm v5, v2, v3, v0\n"
                                          "vmadc.vv v6, v2, v3\n"
                                          "vsbc.vvm v7, v2, v3, v0\n"
                                          "vmsbc.vvm v8, v2, v3, v0\n"
                                          "vmsbc.vv v9, v2, v3\n",
                                          carryData);
    const RegisterFile &rf = vm->registers_;
    // Every element is written, whatever its v0 bit.
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));
    EXPECT_EQ(maskByte(rf, 5), 0x03u); // 0xFF+1 and 0xFF+0+1 carry
    EXPECT_EQ(maskByte(rf, 6), 0x01u); // without the carry-in only 0xFF+1 does
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0xFE, 0xFE, 0xEF, 0x00}));
    EXPECT_EQ(maskByte(rf, 8), 0x04u); // 0x10 - 0x20 - 1 < 0
    EXPECT_EQ(maskByte(rf, 9), 0x04u);
}

TEST(VectorCarryTest, BorrowInAloneMakesTheDifferenceNegative)
{
    // Equal operands: x - x = 0, so only the borrow-in can make the result negative.
    // v0 = 0b0101: borrow-in on elements 0 and 2. The mirror case for carries: x + (-1 - x) =
    // 0xFF, which only overflows with the carry-in.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmsbc.vvm v8, v2, v2, v0\n"
                                          "vmsbc.vv v9, v2, v2\n"
                                          "vsbc.vvm v10, v2, v2, v0\n"
                                          "vmadc.vvm v11, v2, v3, v0\n"
                                          "vmadc.vv v12, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {5, 5, 0x80, 0});
                                              writeElements(rf, 3, 8, {0xFA, 0xFA, 0x7F, 0xFF});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 8), 0x05u); // 5-5-1 and 0x80-0x80-1 borrow; no borrow-in, no borrow
    EXPECT_EQ(maskByte(rf, 9), 0x00u); // without the borrow-in nothing borrows
    EXPECT_EQ(readElements(rf, 10, 8, 4), (Elements{0xFF, 0x00, 0xFF, 0x00}));
    EXPECT_EQ(maskByte(rf, 11), 0x05u); // 5+0xFA+1 and 0x80+0x7F+1 = 0x100 carry
    EXPECT_EQ(maskByte(rf, 12), 0x00u); // 0xFF sums never carry on their own
}

TEST(VectorCarryTest, ScalarAndImmediateForms)
{
    // The immediate -1 is 0xFF at e8 (sign-extended, then an unsigned SEW value).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 1\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vxm v10, v2, x10, v0\n"
                                          "vmadc.vxm v11, v2, x10, v0\n"
                                          "vmsbc.vx v12, v2, x10\n"
                                          "vadc.vim v13, v2, -1, v0\n"
                                          "vmadc.vi v14, v2, -1\n"
                                          "vmadc.vim v15, v2, -1, v0\n",
                                          carryData);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 4), (Elements{0x00, 0x01, 0x12, 0x01}));
    EXPECT_EQ(maskByte(rf, 11), 0x03u);
    EXPECT_EQ(maskByte(rf, 12), 0x08u); // only 0 - 1 borrows
    EXPECT_EQ(readElements(rf, 13, 8, 4), (Elements{0xFE, 0xFF, 0x10, 0xFF}));
    EXPECT_EQ(maskByte(rf, 14), 0x07u);
    EXPECT_EQ(maskByte(rf, 15), 0x07u);
}

TEST(VectorCarryTest, CarryOutAt64Bits)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmadc.vv v4, v2, v3\n"
                                          "vmsbc.vv v5, v2, v3\n"
                                          "vmadc.vvm v6, v2, v8, v0\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b01});
                                              writeElements(rf, 2, 64, {~0ULL, 5});
                                              writeElements(rf, 3, 64, {1, 7});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 4), 0x01u); // 2^64 - 1 + 1 carries
    EXPECT_EQ(maskByte(vm->registers_, 5), 0x02u); // 5 - 7 borrows
    EXPECT_EQ(maskByte(vm->registers_, 6), 0x01u); // 2^64 - 1 + 0 + carry-in 1
}

TEST(VectorCarryTest, CarryOutCanReplaceTheCarryIn)
{
    // vmadc.vvm v0, ..., v0: bit i of v0 is read before it is replaced.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmadc.vvm v0, v2, v3, v0\n",
                                          carryData);
    EXPECT_EQ(maskByte(vm->registers_, 0), 0x03u);
}

TEST(VectorCarryTest, TwoWordAddition)
{
    // Two 128-bit sums, low words in v4/v8 and high words in v5/v9:
    //   (1 : 2^64-1) + (2 : 1) = (4 : 0)   and   (0 : 5) + (0 : 6) = (0 : 11)
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmadc.vv v0, v4, v8\n"       // carry out of the low words
                                          "vadd.vv v4, v4, v8\n"        // low sum
                                          "vadc.vvm v5, v5, v9, v0\n",  // high sum + carry
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 64, {~0ULL, 5});
                                              writeElements(rf, 5, 64, {1, 0});
                                              writeElements(rf, 8, 64, {1, 6});
                                              writeElements(rf, 9, 64, {2, 0});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 2), (Elements{0, 11}));
    EXPECT_EQ(readElements(vm->registers_, 5, 64, 2), (Elements{4, 0}));
}

TEST(VectorCarryTest, ReservedEncodingsAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vadc.vvm v4, v2, v3, v0\n",
                                          carryData);
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 4), (Elements{0x00, 0x00, 0x31, 0x00}));

    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    // vadc.vvm v1, v2, v3 with vm = 1 is reserved.
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x402180d7 | (1u << 25), 0, 0,
                                                      vm->registers_, vm->memory_controller_,
                                                      changes, memoryChanges),
                 std::runtime_error);
    // vadc.vvm v0, v2, v3, v0 is reserved (vd = v0).
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x40218057, 0, 0, vm->registers_, vm->memory_controller_,
                                                      changes, memoryChanges),
                 std::runtime_error);
}

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

TEST(VectorCompareAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmseq.vv v1, v2, v3", 0x622180d7},        {"vmseq.vx v1, v2, a0, v0.t", 0x602540d7},
        {"vmseq.vi v1, v2, -16", 0x622830d7},       {"vmsne.vv v0, v2, v3, v0.t", 0x64218057},
        {"vmsne.vx v4, v8, t0", 0x6682c257},        {"vmsne.vi v5, v6, 15", 0x6667b2d7},
        {"vmsltu.vv v1, v2, v3", 0x6a2180d7},       {"vmsltu.vx v1, v2, a1", 0x6a25c0d7},
        {"vmslt.vv v1, v2, v3, v0.t", 0x6c2180d7},  {"vmslt.vx v1, v2, a1", 0x6e25c0d7},
        {"vmsleu.vv v1, v2, v3", 0x722180d7},       {"vmsleu.vx v1, v2, a1", 0x7225c0d7},
        {"vmsleu.vi v1, v2, -1", 0x722fb0d7},       {"vmsle.vv v1, v2, v3", 0x762180d7},
        {"vmsle.vx v1, v2, a1", 0x7625c0d7},        {"vmsle.vi v1, v2, 7, v0.t", 0x7423b0d7},
        {"vmsgtu.vx v1, v2, a1", 0x7a25c0d7},       {"vmsgtu.vi v1, v2, 3", 0x7a21b0d7},
        {"vmsgt.vx v1, v2, a1", 0x7e25c0d7},        {"vmsgt.vi v1, v2, -3, v0.t", 0x7c2eb0d7},
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
    }
    EXPECT_EQ(Kites::instruction_set::disassemble(0x722fb0d7), "vmsleu.vi v1, v2, -1");
}

TEST(VectorCompareAssemblerTest, AliasesMatchLlvmMc)
{
    // Each alias assembles to the same word llvm-mc emits for it.
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vmsgt.vv v1, v2, v3", 0x6e3100d7},        // vmslt.vv v1, v3, v2
        {"vmsgtu.vv v1, v2, v3, v0.t", 0x683100d7}, // vmsltu.vv v1, v3, v2, v0.t
        {"vmsge.vv v1, v2, v3", 0x763100d7},        // vmsle.vv v1, v3, v2
        {"vmsgeu.vv v1, v2, v3", 0x723100d7},       // vmsleu.vv v1, v3, v2
        {"vmslt.vi v1, v2, 16", 0x7627b0d7},        // vmsle.vi v1, v2, 15
        {"vmslt.vi v1, v2, -15", 0x762830d7},       // vmsle.vi v1, v2, -16
        {"vmsltu.vi v1, v2, 16", 0x7227b0d7},       // vmsleu.vi v1, v2, 15
        {"vmsltu.vi v1, v2, 0", 0x662100d7},        // vmsne.vv v1, v2, v2
        {"vmsltu.vi v1, v2, -15", 0x722830d7},      // vmsleu.vi v1, v2, -16
        {"vmsge.vi v1, v2, 16", 0x7e27b0d7},        // vmsgt.vi v1, v2, 15
        {"vmsge.vi v1, v2, -15, v0.t", 0x7c2830d7}, // vmsgt.vi v1, v2, -16, v0.t
        {"vmsgeu.vi v1, v2, 0", 0x622100d7},        // vmseq.vv v1, v2, v2
        {"vmsgeu.vi v1, v2, 5", 0x7a2230d7},        // vmsgtu.vi v1, v2, 4
    };
    for (const auto &[text, word] : cases)
    {
        const Kites::AssembledProgram program = assembleSource(text + "\n");
        ASSERT_EQ(program.text_buffer.size(), 1u) << text;
        EXPECT_EQ(program.text_buffer[0], word) << text;
    }
}

TEST(VectorCompareAssemblerTest, OperandRules)
{
    for (const std::string &line :
         {"vmseq.vi v1, v2, 16\n", "vmsle.vi v1, v2, -17\n", "vmslt.vi v1, v2, -16\n",
          "vmslt.vi v1, v2, 17\n", "vmsltu.vi v1, v2, 17\n", "vmsgeu.vi v1, v2, -16\n",
          "vmslt.vi v1, v2, a0\n", "vmsltu.vv v1, v2, a0\n",
          // Not in the ISA, and the expansions need the mask-logical instructions.
          "vmsge.vx v1, v2, a0\n", "vmsltu.vi.x v1, v2, 3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // A compare may write its mask into v0 even when masked by v0.
    EXPECT_NO_THROW(assembleSource("vmslt.vv v0, v2, v3, v0.t\nvmslt.vi v0, v2, 3, v0.t\n"));
    // ...but other instructions still may not.
    EXPECT_THROW(assembleSource("vadd.vv v0, v2, v3, v0.t\n"), std::runtime_error);
}

TEST(VectorCompareTest, EveryCompareSignedAndUnsigned)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 5\n"
                                          "vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmseq.vv v4, v2, v3\n"
                                          "vmsne.vv v5, v2, v3\n"
                                          "vmsltu.vv v6, v2, v3\n"
                                          "vmslt.vv v7, v2, v3\n"
                                          "vmsleu.vv v8, v2, v3\n"
                                          "vmsle.vv v9, v2, v3\n"
                                          "vmseq.vx v10, v2, x10\n"
                                          "vmslt.vx v11, v2, x10\n"
                                          "vmsltu.vx v12, v2, x10\n"
                                          "vmsle.vi v13, v2, 5\n"
                                          "vmsleu.vi v14, v2, 5\n"
                                          "vmsgt.vx v15, v2, x10\n"
                                          "vmsgtu.vi v16, v2, 5\n"
                                          "vmsne.vi v17, v2, 5\n",
                                          compareData);
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x25u);  // equal at 0, 2, 5
    EXPECT_EQ(maskByte(rf, 5), 0xDAu);
    EXPECT_EQ(maskByte(rf, 6), 0x8Au);  // unsigned: 127 < 128 at element 3
    EXPECT_EQ(maskByte(rf, 7), 0x92u);  // signed: -128 < 127 at element 4
    EXPECT_EQ(maskByte(rf, 8), 0xAFu);
    EXPECT_EQ(maskByte(rf, 9), 0xB7u);
    EXPECT_EQ(maskByte(rf, 10), 0x44u);
    EXPECT_EQ(maskByte(rf, 11), 0x33u); // -128 and -1 are below 5 signed...
    EXPECT_EQ(maskByte(rf, 12), 0x03u); // ...but not unsigned
    EXPECT_EQ(maskByte(rf, 13), 0x77u);
    EXPECT_EQ(maskByte(rf, 14), 0x47u);
    EXPECT_EQ(maskByte(rf, 15), 0x88u);
    EXPECT_EQ(maskByte(rf, 16), 0xB8u);
    EXPECT_EQ(maskByte(rf, 17), 0xBBu);
    // Only the 8 body bits are written; the rest of the register is untouched.
    EXPECT_EQ(maskByte(rf, 5, 1), 0u);
}

TEST(VectorCompareTest, AllOnesScalarIsMaxUnsignedAndMinusOneSigned)
{
    // x10 = -1 (all 64 bits set). At e8 it acts as 0xFF: 255 unsigned, -1 signed.
    // v2 = [0, 0x7F, 0x80, 0xFF] = unsigned [0, 127, 128, 255] = signed [0, 127, -128, -1].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmseq.vx  v4, v2, x10\n"
                                          "vmsne.vx  v5, v2, x10\n"
                                          "vmsltu.vx v6, v2, x10\n"
                                          "vmslt.vx  v7, v2, x10\n"
                                          "vmsleu.vx v8, v2, x10\n"
                                          "vmsle.vx  v9, v2, x10\n"
                                          "vmsgtu.vx v10, v2, x10\n"
                                          "vmsgt.vx  v11, v2, x10\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmseq.vx  v12, v3, x10\n"
                                          "vmsltu.vx v13, v3, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0x00, 0x7F, 0x80, 0xFF});
                                              writeElements(rf, 3, 64, {~0ULL, 5});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x08u);  // == : only 0xFF
    EXPECT_EQ(maskByte(rf, 5), 0x07u);  // !=
    EXPECT_EQ(maskByte(rf, 6), 0x07u);  // <u 255: everything but 0xFF
    EXPECT_EQ(maskByte(rf, 7), 0x04u);  // <s -1: only -128
    EXPECT_EQ(maskByte(rf, 8), 0x0Fu);  // <=u 255: always
    EXPECT_EQ(maskByte(rf, 9), 0x0Cu);  // <=s -1: -128 and -1
    EXPECT_EQ(maskByte(rf, 10), 0x00u); // >u 255: never
    EXPECT_EQ(maskByte(rf, 11), 0x03u); // >s -1: 0 and 127
    // At e64 the scalar is used whole: 2^64 - 1.
    EXPECT_EQ(maskByte(rf, 12), 0x01u);
    EXPECT_EQ(maskByte(rf, 13), 0x02u);
}

TEST(VectorCompareTest, UnsignedImmediateIsSignExtendedThenUnsigned)
{
    // -1 is 0xFF at e8 and 0xFFFFFFFFFFFFFFFF at e64; -2 is 0xFE / 0x...FE.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmsleu.vi v4, v2, -1\n"
                                          "vmsgtu.vi v5, v2, -2\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vmsgtu.vi v6, v7, -2\n",
                                          [](RegisterFile &rf)
                                          {
                                              compareData(rf);
                                              writeElements(rf, 7, 64, {~0ULL, ~0ULL - 1});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 4), 0xFFu);
    EXPECT_EQ(maskByte(vm->registers_, 5), 0x20u); // only 0xFF (element 5)
    EXPECT_EQ(maskByte(vm->registers_, 6), 0x01u);
}

TEST(VectorCompareTest, AliasesBehaveAsNamed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vmsgt.vv v4, v2, v3\n"
                                          "vmsge.vv v5, v2, v3\n"
                                          "vmslt.vi v6, v2, 6\n"
                                          "vmsltu.vi v7, v2, 0\n"
                                          "vmsgeu.vi v8, v2, 0\n"
                                          "vmsge.vi v9, v2, 6\n"
                                          "vmsltu.vi v10, v2, 16\n"
                                          "vmsgeu.vi v11, v2, 5\n",
                                          [](RegisterFile &rf)
                                          {
                                              compareData(rf);
                                              writeElements(rf, 7, 8, {0xFF});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 4), 0x48u);  // signed va > vb at 3 and 6
    EXPECT_EQ(maskByte(rf, 5), 0x6Du);  // NOT (va < vb)
    EXPECT_EQ(maskByte(rf, 6), 0x77u);
    EXPECT_EQ(maskByte(rf, 7), 0x00u);  // va <u 0: never
    EXPECT_EQ(maskByte(rf, 8), 0xFFu);  // va >=u 0: always
    EXPECT_EQ(maskByte(rf, 9), 0x88u);
    EXPECT_EQ(maskByte(rf, 10), 0xC7u);
    EXPECT_EQ(maskByte(rf, 11), 0xFCu);
}

TEST(VectorCompareTest, MaskedCompareIntoV0AndsTheConditions)
{
    // (a < b) && (b < c), the spec's two-instruction idiom.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, mu\n"
                                          "vmslt.vv v0, v2, v3\n"
                                          "vmslt.vv v0, v3, v4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {1, 5, 2, 9});
                                              writeElements(rf, 3, 8, {3, 4, 6, 10});
                                              writeElements(rf, 4, 8, {5, 5, 5, 11});
                                          });
    EXPECT_EQ(maskByte(vm->registers_, 0), 0x09u);
}

TEST(VectorCompareTest, MaskedOffAndTailBitsUndisturbedAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmseq.vi v5, v2, 0, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0, 0, 1, 1});
                                              writeElements(rf, 5, 8, {0xFF, 0xFF});
                                          });
    // Bit 0 = 1, bit 2 = 0; masked-off bits 1, 3 and tail bits 4.. keep their 1s.
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFBu);
    EXPECT_EQ(maskByte(vm->registers_, 5, 1), 0xFFu);
    vm->Undo();
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFFu);
    vm->Redo();
    EXPECT_EQ(maskByte(vm->registers_, 5), 0xFBu);
}

TEST(VectorCompareTest, InPlaceOverAnLmul2Source)
{
    // vd == vs2 at LMUL = 2: the mask lands in v4 while the 32 elements of v4-v5 are read.
    Elements data;
    for (int i = 0; i < 32; ++i)
    {
        data.push_back(i % 3 == 0 ? 1 : 2);
    }
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 32\n"
                                          "vsetvli x0, x5, e8, m2, ta, ma\n"
                                          "vmseq.vi v4, v4, 1\n",
                                          [&](RegisterFile &rf) { writeElements(rf, 4, 8, data); });
    uint64_t expected = 0;
    for (int i = 0; i < 32; i += 3)
    {
        expected |= 1ULL << i;
    }
    EXPECT_EQ(vm->registers_.ReadVectorElement(4, 0, 32), expected);
    // Bytes 4..15 of v4 are past the 32 mask bits: the original data.
    for (size_t i = 4; i < 16; ++i)
    {
        EXPECT_EQ(maskByte(vm->registers_, 4, i), data[i]) << i;
    }
}

TEST(VectorCompareTest, IllegalOverlapsAndConfigurationsTrap)
{
    // vd inside the vs2 group but not at its start.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vv v5, v4, v8\n");
    // vd inside the vs1 group but not at its start.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vv v9, v2, v8\n");
    // vs2 must be aligned to its group.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvmseq.vi v1, v3, 0\n");
    // vtype.vill (no vsetvli yet).
    expectIllegal("vmseq.vi v1, v2, 0\n");
    // vd at the start of a group, and anywhere at all outside it, is fine.
    EXPECT_NO_THROW(runOn<Kites::RVSSProcessor>("vsetvli x0, x0, e8, m2, ta, ma\n"
                                                "vmseq.vv v4, v4, v8\n"
                                                "vmseq.vv v8, v4, v8\n"
                                                "vmseq.vv v1, v4, v8\n"));
}

} // namespace vtest_compare

// ===== vector_shift_test =====================================================================
// Tests for the RVV single-width bit shifts (11.6): vsll, vsrl, vsra in .vv/.vx/.vi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: only the low log2(SEW) bits of the shift amount are used, .vi takes an
// unsigned 5-bit immediate, and vsra fills with the sign bit.
namespace vtest_shift
{

TEST(VectorShiftAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vsll.vv v1, v2, v3", 0x962180d7},
        {"vsll.vx v4, v5, a0, v0.t", 0x94554257},
        {"vsll.vi v6, v7, 31", 0x967fb357},
        {"vsrl.vv v8, v9, v10, v0.t", 0xa0950457},
        {"vsrl.vx v11, v12, t0", 0xa2c2c5d7},
        {"vsrl.vi v13, v14, 0", 0xa2e036d7},
        {"vsra.vv v15, v16, v17", 0xa70887d7},
        {"vsra.vx v18, v19, s1", 0xa734c957},
        {"vsra.vi v3, v1, 2", 0xa61131d7},
        {"vsra.vi v20, v21, 17, v0.t", 0xa558ba57},
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
    }
}

TEST(VectorShiftAssemblerTest, ImmediateIsUnsignedInDisassembly)
{
    using Kites::instruction_set::disassemble;
    // uimm 31 and 17 have bit 4 set: as a signed simm5 they would read -1 and -15.
    EXPECT_EQ(disassemble(0x967fb357), "vsll.vi v6, v7, 31");
    EXPECT_EQ(disassemble(0xa558ba57), "vsra.vi v20, v21, 17, v0.t");
    EXPECT_EQ(disassemble(0xa2c2c5d7), "vsrl.vx v11, v12, x5");
    // The signed instructions are unchanged.
    EXPECT_EQ(disassemble(0x2e2fb0d7), "vxor.vi v1, v2, -1");
}

TEST(VectorShiftAssemblerTest, ImmediateRangeIsZeroTo31)
{
    for (const std::string &line : {"vsll.vi v1, v2, 32\n", "vsrl.vi v1, v2, -1\n",
                                    "vsra.vi v1, v2, 100\n", "vsra.vi v0, v1, 3, v0.t\n",
                                    "vsll.vv v1, v2, a0\n", "vsra.vx v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    EXPECT_NO_THROW(assembleSource("vsll.vi v1, v2, 0\nvsra.vi v1, v2, 31\n"));
    // vadd.vi still uses the signed range.
    EXPECT_THROW(assembleSource("vadd.vi v1, v2, 16\n"), std::runtime_error);
}

TEST(VectorShiftTest, LogicalVersusArithmeticRightShift)
{
    // 0x81 has its sign bit set: vsrl fills with 0, vsra with 1.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 1\n"
                                          "vsrl.vi v3, v1, 1\n"
                                          "vsra.vi v4, v1, 1\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 8, {0x81, 0x7E}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 2), (Elements{0x02, 0xFC}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 2), (Elements{0x40, 0x3F}));
    EXPECT_EQ(readElements(vm->registers_, 4, 8, 2), (Elements{0xC0, 0x3F}));
}

TEST(VectorShiftTest, VvUsesEachElementsOwnShiftAmountModuloSew)
{
    // e16: amounts 0, 1, 15 and 16. 16 & 15 = 0, so the last element is not shifted.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e16, m1, ta, ma\n"
                                          "vsra.vv v4, v1, v3\n"
                                          "vsrl.vv v5, v1, v3\n"
                                          "vsll.vv v6, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 16, Elements(4, 0x8000));
                                              writeElements(rf, 2, 16, Elements(4, 1));
                                              writeElements(rf, 3, 16, {0, 1, 15, 16});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x8000, 0xC000, 0xFFFF, 0x8000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 16, 4), (Elements{0x8000, 0x4000, 0x0001, 0x8000}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{0x0001, 0x0002, 0x8000, 0x0001}));
}

TEST(VectorShiftTest, VxScalarAmountIsMaskedToLog2Sew)
{
    // At e8 only 3 bits count: 9 shifts by 1. At e64, 6 bits: 68 shifts by 4.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 9\n"
                                          "li x11, 68\n"
                                          "vsetivli x0, 2, e8, m1, ta, ma\n"
                                          "vsll.vx v2, v1, x10\n"
                                          "vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsrl.vx v4, v3, x11\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 1, 8, {0x01, 0x40});
                                              writeElements(rf, 3, 64, {0xF0, 0x100});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 2), (Elements{0x02, 0x80}));
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 2), (Elements{0x0F, 0x10}));
}

TEST(VectorShiftTest, ViImmediateIsUnsignedEvenAt64Bits)
{
    // uimm 31 must shift by 31. A sign-extended immediate would be -1, i.e. a shift of 63.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e64, m1, ta, ma\n"
                                          "vsra.vi v2, v1, 31\n"
                                          "vsll.vi v3, v1, 31\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 64, {0x8000000000000000, 1}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 64, 2), (Elements{0xFFFFFFFF00000000, 0}));
    EXPECT_EQ(readElements(vm->registers_, 3, 64, 2), (Elements{0, 0x80000000}));
}

TEST(VectorShiftTest, MaskedAndTailElementsAreUndisturbed)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b011});
                                              writeElements(rf, 1, 32, {1, 2, 3, 4});
                                              writeElements(rf, 2, 32, {9, 9, 9, 9});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x10, 0x20, 9, 9}));
}

TEST(VectorShiftTest, UndoRestoresDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vsll.vi v2, v1, 8\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 1, 32, {1, 2, 3, 4}); });
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x100, 0x200, 0x300, 0x400}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 2, 32, 4), (Elements{0x100, 0x200, 0x300, 0x400}));
}

} // namespace vtest_shift

// ===== vector_narrow_shift_test ==============================================================
// Tests for the RVV narrowing right shifts (11.7): vnsrl and vnsra in .wv/.wx/.wi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: vs2 is 2*SEW wide, the result is truncated to SEW, the shift amount uses
// log2(2*SEW) bits, .wi takes an unsigned 5-bit immediate, and vd may overlap only the start
// of the wide vs2 group.
namespace vtest_narrow_shift
{

TEST(VectorNarrowShiftAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vnsrl.wv v1, v2, v3", 0xb22180d7},
        {"vnsrl.wx v4, v6, a0, v0.t", 0xb0654257},
        {"vnsrl.wi v8, v10, 31", 0xb2afb457},
        {"vnsra.wv v12, v14, v16, v0.t", 0xb4e80657},
        {"vnsra.wx v2, v2, t0", 0xb622c157},
        {"vnsra.wi v3, v4, 0", 0xb64031d7},
        {"vnsra.wi v5, v6, 17, v0.t", 0xb468b2d7},
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
    }
}

TEST(VectorNarrowShiftAssemblerTest, DisassemblyAndImmediateRange)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0xb2afb457), "vnsrl.wi v8, v10, 31");
    EXPECT_EQ(disassemble(0xb468b2d7), "vnsra.wi v5, v6, 17, v0.t");
    EXPECT_EQ(disassemble(0xb622c157), "vnsra.wx v2, v2, x5");

    for (const std::string &line : {"vnsrl.wi v1, v2, 32\n", "vnsra.wi v1, v2, -1\n",
                                    "vnsrl.wi v0, v2, 3, v0.t\n", "vnsra.wv v1, v2, a0\n",
                                    "vnsrl.wx v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // vd may be vs2 itself: the lowest-numbered part of the wide group.
    EXPECT_NO_THROW(assembleSource("vnsrl.wi v2, v2, 0\nvnsra.wi v1, v2, 31\n"));
}

TEST(VectorNarrowShiftTest, ShiftsWideElementAndTruncates)
{
    // e16 results from e32 sources in the v2-v3 group.
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 4, e16, m1, ta, ma\n"
        "vnsrl.wi v4, v2, 16\n"
        "vnsrl.wi v5, v2, 20\n"
        "vnsra.wi v6, v2, 20\n",
        [](RegisterFile &rf)
        { writeElements(rf, 2, 32, {0x80000000, 0x12345678, 0xFFFF0000, 0x0000FFFF}); });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x8000, 0x1234, 0xFFFF, 0x0000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 16, 4), (Elements{0x0800, 0x0123, 0x0FFF, 0x0000}));
    EXPECT_EQ(readElements(vm->registers_, 6, 16, 4), (Elements{0xF800, 0x0123, 0xFFFF, 0x0000}));
}

TEST(VectorNarrowShiftTest, AmountIsMaskedToLog2OfTwiceSew)
{
    // e8 results from e16 sources: 4 bits of the amount count, not 3. A shift by 8 (or 24)
    // must really move the high byte down; masking to 3 bits would make it a shift by 0.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 24\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vnsrl.wx v2, v4, x10\n"
                                          "vnsrl.wi v3, v4, 8\n"
                                          "vnsra.wv v7, v4, v6\n"
                                          "vnsrl.wv v8, v4, v6\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 4, 16,
                                                            {0x1234, 0x8000, 0xABCD, 0x00FF});
                                              writeElements(rf, 6, 8, {8, 15, 16, 0});
                                          });
    EXPECT_EQ(readElements(vm->registers_, 2, 8, 4), (Elements{0x12, 0x80, 0xAB, 0x00}));
    EXPECT_EQ(readElements(vm->registers_, 3, 8, 4), (Elements{0x12, 0x80, 0xAB, 0x00}));
    // Per-element amounts 8, 15, 16 (& 15 = 0) and 0.
    EXPECT_EQ(readElements(vm->registers_, 7, 8, 4), (Elements{0x12, 0xFF, 0xCD, 0xFF}));
    EXPECT_EQ(readElements(vm->registers_, 8, 8, 4), (Elements{0x12, 0x01, 0xCD, 0xFF}));
}

TEST(VectorNarrowShiftTest, InPlaceOverTheWideSourceKeepsTheTail)
{
    // vd == vs2: each narrow result lands below the wide elements still to be read.
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 4, e16, m1, ta, ma\n"
        "vnsrl.wi v2, v2, 16\n",
        [](RegisterFile &rf)
        { writeElements(rf, 2, 32, {0x00010002, 0x00030004, 0x00050006, 0x00070008}); });
    // Elements 4..7 are the tail: the untouched upper half of the old v2.
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 8), (Elements{1, 3, 5, 7, 6, 5, 8, 7}));
}

TEST(VectorNarrowShiftTest, MaskedOffElementsAndUndo)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e16, m1, ta, ma\n"
                                          "vnsrl.wi v4, v2, 4, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 32, {0x10, 0x20, 0x30, 0x40});
                                              writeElements(rf, 4, 16, Elements(4, 9));
                                          });
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x1, 9, 0x3, 9}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), Elements(4, 9));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 4, 16, 4), (Elements{0x1, 9, 0x3, 9}));
}

TEST(VectorNarrowShiftTest, IllegalConfigurationsTrap)
{
    // 2*SEW = 128 exceeds ELEN.
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvnsrl.wi v1, v2, 0\n");
    // LMUL = 8 would make vs2 a 16-register group.
    expectIllegal("vsetvli x0, x0, e8, m8, ta, ma\nvnsrl.wi v0, v16, 0\n");
    // vs2 is a 2-register group at LMUL = 1, so it must be even.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvnsrl.wi v1, v3, 0\n");
    // vd may not overlap the upper half of vs2.
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvnsra.wi v3, v2, 0\n");
    // vd at LMUL = 2 must be even.
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvnsrl.wi v1, v4, 0\n");
}

TEST(VectorNarrowShiftTest, FractionalLmulAllowsVdNextToVs2)
{
    // At LMUL = 1/2 vs2 has EMUL = 1, so v3 is outside it.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 2, e16, mf2, ta, ma\n"
                                          "vnsrl.wi v3, v2, 8\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 32, {0x123400, 0xABCD00}); });
    EXPECT_EQ(readElements(vm->registers_, 3, 16, 2), (Elements{0x1234, 0xABCD}));
}

} // namespace vtest_narrow_shift

// ===== vector_widen_ext_logic_test ===========================================================
// Tests for RVV widening integer add/subtract (11.2), integer extension (11.3) and bitwise
// logical (11.5) instructions. Encodings are checked against llvm-mc
// (-triple=riscv64 -mattr=+zve64x -show-encoding); legality checks follow Spike.
namespace vtest_widen_ext_logic
{

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

TEST(VectorLogicalTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 4; v0 = 0b0101 enables elements 0 and 2. Every destination starts as
    // 0x5A5A5A5A, so element 1 (masked off) and element 3 (tail) must visibly keep it.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 0xFF\n"
                                          "vsetivli x0, 3, e32, m1, ta, ma\n"
                                          "vand.vv v4, v2, v3, v0.t\n"
                                          "vor.vx v5, v2, x10, v0.t\n"
                                          "vxor.vi v6, v2, -1, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 32, {0xF0F0F0F0, 1, 0x12345678, 2});
                                              writeElements(rf, 3, 32, {0x0F0F0FFF, 3, 0x0000FFFF, 4});
                                              for (size_t r = 4; r <= 6; ++r)
                                              {
                                                  writeElements(rf, r, 32, Elements(4, 0x5A5A5A5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 32, 4), (Elements{0x000000F0, 0x5A5A5A5A, 0x00005678, 0x5A5A5A5A}));
    EXPECT_EQ(readElements(rf, 5, 32, 4), (Elements{0xF0F0F0FF, 0x5A5A5A5A, 0x123456FF, 0x5A5A5A5A}));
    EXPECT_EQ(readElements(rf, 6, 32, 4), (Elements{0x0F0F0F0F, 0x5A5A5A5A, 0xEDCBA987, 0x5A5A5A5A}));
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

} // namespace vtest_widen_ext_logic

// ===== vector_mul_div_test ===================================================================
// Tests for RVV integer min/max (11.9), multiply (11.10), divide (11.11), widening multiply
// (11.12), multiply-add (11.13), widening multiply-add (11.14), merge (11.15) and move (11.16).
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
namespace vtest_mul_div
{

TEST(VectorMulDivAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vminu.vv v1, v2, v3", 0x122180d7},
        {"vmul.vv v1, v2, v3", 0x9621a0d7},
        {"vmul.vx v1, v2, a0, v0.t", 0x942560d7},
        {"vmulh.vv v1, v2, v3", 0x9e21a0d7},
        {"vmulh.vx v1, v2, a0", 0x9e2560d7},
        {"vmulhu.vv v1, v2, v3", 0x9221a0d7},
        {"vmulhu.vx v1, v2, a0", 0x922560d7},
        {"vmulhsu.vv v1, v2, v3, v0.t", 0x9821a0d7},
        {"vmulhsu.vx v1, v2, a0", 0x9a2560d7},
        {"vdivu.vv v1, v2, v3", 0x8221a0d7},
        {"vdivu.vx v1, v2, a0", 0x822560d7},
        {"vdiv.vv v1, v2, v3", 0x8621a0d7},
        {"vdiv.vx v1, v2, a0, v0.t", 0x842560d7},
        {"vremu.vv v1, v2, v3", 0x8a21a0d7},
        {"vremu.vx v1, v2, a0", 0x8a2560d7},
        {"vrem.vv v1, v2, v3", 0x8e21a0d7},
        {"vrem.vx v1, v2, a0", 0x8e2560d7},
        {"vwmul.vv v2, v4, v5", 0xee42a157},
        {"vwmul.vx v2, v4, a0", 0xee456157},
        {"vwmulu.vv v2, v4, v5, v0.t", 0xe042a157},
        {"vwmulu.vx v2, v4, a0", 0xe2456157},
        {"vwmulsu.vv v2, v4, v5", 0xea42a157},
        {"vwmulsu.vx v2, v4, a0", 0xea456157},
        {"vmacc.vv v1, v2, v3", 0xb63120d7},
        {"vmacc.vx v1, a0, v3, v0.t", 0xb43560d7},
        {"vnmsac.vv v1, v2, v3", 0xbe3120d7},
        {"vnmsac.vx v1, a0, v3", 0xbe3560d7},
        {"vmadd.vv v1, v2, v3", 0xa63120d7},
        {"vmadd.vx v1, a0, v3", 0xa63560d7},
        {"vnmsub.vv v1, v2, v3", 0xae3120d7},
        {"vnmsub.vx v1, a0, v3", 0xae3560d7},
        {"vwmaccu.vv v2, v4, v5", 0xf2522157},
        {"vwmaccu.vx v2, a0, v5", 0xf2556157},
        {"vwmacc.vv v2, v4, v5, v0.t", 0xf4522157},
        {"vwmacc.vx v2, a0, v5", 0xf6556157},
        {"vwmaccsu.vv v2, v4, v5", 0xfe522157},
        {"vwmaccsu.vx v2, a0, v5", 0xfe556157},
        {"vwmaccus.vx v2, a0, v5", 0xfa556157},
        {"vmerge.vvm v1, v2, v3, v0", 0x5c2180d7},
        {"vmerge.vxm v1, v2, a0, v0", 0x5c2540d7},
        {"vmerge.vim v1, v2, -16, v0", 0x5c2830d7},
        {"vmv.v.v v1, v3", 0x5e0180d7},
        {"vmv.v.x v1, a0", 0x5e0540d7},
        {"vmv.v.i v1, 15", 0x5e07b0d7},
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
    }
}

TEST(VectorMulDivAssemblerTest, MinMaxEncodings)
{
    // funct6 000100 vminu, 000101 vmin, 000110 vmaxu, 000111 vmax; OPIVV and OPIVX only.
    const Kites::AssembledProgram program = assembleSource("vminu.vv v1, v2, v3\n"
                                                           "vmin.vx v4, v5, a0, v0.t\n"
                                                           "vmaxu.vv v11, v12, v13\n"
                                                           "vmax.vx v19, v20, a7, v0.t\n");
    EXPECT_EQ(program.text_buffer, (std::vector<uint32_t>{0x122180d7, 0x14554257, 0x1ac685d7,
                                                          0x1d48c9d7}));
}

TEST(VectorMulDivAssemblerTest, DisassemblyUsesEachInstructionsOperandOrder)
{
    using Kites::instruction_set::disassemble;
    EXPECT_EQ(disassemble(0xb43560d7), "vmacc.vx v1, x10, v3, v0.t");
    EXPECT_EQ(disassemble(0xb63120d7), "vmacc.vv v1, v2, v3");
    EXPECT_EQ(disassemble(0xfa556157), "vwmaccus.vx v2, x10, v5");
    EXPECT_EQ(disassemble(0x5c2830d7), "vmerge.vim v1, v2, -16, v0");
    EXPECT_EQ(disassemble(0x5e0180d7), "vmv.v.v v1, v3");
    EXPECT_EQ(disassemble(0x5e0540d7), "vmv.v.x v1, x10");
    EXPECT_EQ(disassemble(0x5e07b0d7), "vmv.v.i v1, 15");
    EXPECT_EQ(disassemble(0x9821a0d7), "vmulhsu.vv v1, v2, v3, v0.t");
}

TEST(VectorMulDivAssemblerTest, OperandRules)
{
    for (const std::string &line : {
             "vmacc.vx v1, v2, a0\n",          // rs1 comes before vs2
             "vmerge.vvm v1, v2, v3\n",        // the v0 operand is required
             "vmerge.vvm v1, v2, v3, v0.t\n",  // ...and is written "v0"
             "vmerge.vvm v0, v2, v3, v0\n",    // cannot overwrite its own mask
             "vmerge.vim v1, v2, 16, v0\n",    // simm5
             "vmv.v.v v1, v2, v0.t\n",         // never masked
             "vmv.v.i v1, 16\n",               // simm5
             "vmv.v.x v1, v2\n",
             "vwmacc.vv v2, v2, v3\n",         // vd overlaps a narrower vs1
             "vwmul.vv v4, v4, v5\n",          // vd overlaps a narrower vs2
             "vwmaccus.vv v2, v4, v5\n",       // only a .vx form exists
             "vmin.vi v1, v2, 3\n",            // no .vi forms
             "vmul.vv v0, v1, v2, v0.t\n",
         })
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
}

TEST(VectorMinMaxTest, SignedAndUnsigned)
{
    // vs2 signed [0, 1, 5, 127, -128, -1, 5, 6]; vs1 signed [0, 2, 5, -128, 127, -1, 4, 7].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 8, e8, m1, ta, ma\n"
                                          "vminu.vv v4, v2, v3\n"
                                          "vmin.vv v5, v2, v3\n"
                                          "vmaxu.vv v6, v2, v3\n"
                                          "vmax.vv v7, v2, v3\n"
                                          "vminu.vx v8, v2, x10\n"
                                          "vmin.vx v9, v2, x10\n"
                                          "vmaxu.vx v10, v2, x10\n"
                                          "vmax.vx v11, v2, x10\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0, 1, 5, 0x7F, 0x80, 0xFF, 5, 6});
                                              writeElements(rf, 3, 8, {0, 2, 5, 0x80, 0x7F, 0xFF, 4, 7});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 8), (Elements{0, 1, 5, 0x7F, 0x7F, 0xFF, 4, 6}));
    EXPECT_EQ(readElements(rf, 5, 8, 8), (Elements{0, 1, 5, 0x80, 0x80, 0xFF, 4, 6}));
    EXPECT_EQ(readElements(rf, 6, 8, 8), (Elements{0, 2, 5, 0x80, 0x80, 0xFF, 5, 7}));
    EXPECT_EQ(readElements(rf, 7, 8, 8), (Elements{0, 2, 5, 0x7F, 0x7F, 0xFF, 5, 7}));
    // x10 = -1 is 0xFF at e8: the largest unsigned value, the smallest-but-one signed one.
    EXPECT_EQ(readElements(rf, 8, 8, 8), (Elements{0, 1, 5, 0x7F, 0x80, 0xFF, 5, 6}));
    EXPECT_EQ(readElements(rf, 9, 8, 8), (Elements{0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 10, 8, 8), Elements(8, 0xFF));
    EXPECT_EQ(readElements(rf, 11, 8, 8), (Elements{0, 1, 5, 0x7F, 0xFF, 0xFF, 5, 6}));
}

TEST(VectorMinMaxTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vminu.vv v4, v2, v3, v0.t\n"
                                          "vmin.vx  v5, v2, x10, v0.t\n"
                                          "vmaxu.vv v6, v2, v3, v0.t\n"
                                          "vmax.vx  v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x7F, 1, 0x80, 2});
                                              writeElements(rf, 3, 8, {0x80, 3, 0x7F, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x7F, 0x5A, 0x7F, 0x5A})); // unsigned min
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0xFF, 0x5A, 0x80, 0x5A})); // signed min vs -1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x80, 0x5A, 0x80, 0x5A})); // unsigned max
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x7F, 0x5A, 0xFF, 0x5A})); // signed max vs -1
}

TEST(VectorMultiplyTest, LowAndHighHalvesAtE8)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmul.vv v4, v2, v3\n"
                                          "vmulh.vv v5, v2, v3\n"
                                          "vmulhu.vv v6, v2, v3\n"
                                          "vmulhsu.vv v7, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {3, 0x80, 0xFF, 0x7F});
                                              writeElements(rf, 3, 8, {5, 0x80, 0xFF, 2});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x0F, 0x00, 0x01, 0xFE}));
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x00, 0x40, 0x00, 0x00})); // -128*-128, -1*-1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x00, 0x40, 0xFE, 0x00})); // 255*255 = 0xFE01
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x00, 0xC0, 0xFF, 0x00})); // -128*128, -1*255
}

TEST(VectorMultiplyTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it. x10 = -1 is 0xFF.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmul.vv    v4, v2, v3, v0.t\n"
                                          "vmulh.vv   v5, v2, v3, v0.t\n"
                                          "vmulhu.vx  v6, v2, x10, v0.t\n"
                                          "vmulhsu.vx v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x80, 1, 0xFF, 2});
                                              writeElements(rf, 3, 8, {0x80, 3, 0xFF, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // 0x80 x 0x80 = 0x4000, 0xFF x 0xFF = 0xFE01: low halves
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0x00, 0x5A, 0x01, 0x5A}));
    // signed: -128 x -128 = 0x4000, -1 x -1 = 0x0001: high halves
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x40, 0x5A, 0x00, 0x5A}));
    // unsigned: 128 x 255 = 0x7F80, 255 x 255 = 0xFE01
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x7F, 0x5A, 0xFE, 0x5A}));
    // signed x unsigned: -128 x 255 = 0x8080, -1 x 255 = 0xFF01
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x80, 0x5A, 0xFF, 0x5A}));
}

TEST(VectorMultiplyTest, HighHalfAtE64Uses128BitProducts)
{
    auto vm = runOn<Kites::RVSSProcessor>(
        "vsetivli x0, 2, e64, m1, ta, ma\n"
        "vmul.vv v4, v2, v3\n"
        "vmulh.vv v5, v2, v3\n"
        "vmulhu.vv v6, v2, v3\n"
        "vmulhsu.vv v7, v2, v3\n",
        [](RegisterFile &rf)
        {
            writeElements(rf, 2, 64, {~0ULL, 0x8000000000000000});
            writeElements(rf, 3, 64, {~0ULL, 0x8000000000000000});
        });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 64, 2), (Elements{1, 0}));
    EXPECT_EQ(readElements(rf, 5, 64, 2), (Elements{0, 0x4000000000000000}));
    EXPECT_EQ(readElements(rf, 6, 64, 2), (Elements{0xFFFFFFFFFFFFFFFE, 0x4000000000000000}));
    // signed(-1) * unsigned(2^64 - 1) and signed(-2^63) * unsigned(2^63).
    EXPECT_EQ(readElements(rf, 7, 64, 2), (Elements{0xFFFFFFFFFFFFFFFF, 0xC000000000000000}));
}

TEST(VectorDivideTest, EdgeCasesMatchTheScalarMExtension)
{
    // vs2 signed [7, -128, -7, 9, -128] / vs1 signed [2, -1, 2, 0, 0]
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 5, e8, m1, ta, ma\n"
                                          "vdiv.vv v4, v2, v3\n"
                                          "vrem.vv v5, v2, v3\n"
                                          "vdivu.vv v6, v2, v3\n"
                                          "vremu.vv v7, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {7, 0x80, 0xF9, 9, 0x80});
                                              writeElements(rf, 3, 8, {2, 0xFF, 2, 0, 0});
                                          });
    const RegisterFile &rf = vm->registers_;
    // -128 / -1 overflows to -128 (remainder 0); x / 0 is all ones (remainder x).
    EXPECT_EQ(readElements(rf, 4, 8, 5), (Elements{3, 0x80, 0xFD, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 5, 8, 5), (Elements{1, 0, 0xFF, 9, 0x80}));
    EXPECT_EQ(readElements(rf, 6, 8, 5), (Elements{3, 0, 0x7C, 0xFF, 0xFF}));
    EXPECT_EQ(readElements(rf, 7, 8, 5), (Elements{1, 0x80, 1, 9, 0x80}));
}

TEST(VectorDivideTest, MaskedOffAndTailElementsKeepTheirOldValues)
{
    // vl = 3 of VLMAX 16; v0 = 0b0101 enables elements 0 and 2. Every destination starts as 0x5A,
    // so element 1 (masked off) and element 3 (tail) must visibly keep it. Element 0 divides by
    // zero (.vv) or is MIN / -1 (.vx, x10 = -1).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vdivu.vv v4, v2, v3, v0.t\n"
                                          "vdiv.vx  v5, v2, x10, v0.t\n"
                                          "vremu.vv v6, v2, v3, v0.t\n"
                                          "vrem.vx  v7, v2, x10, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0x80, 1, 0xF9, 2});
                                              writeElements(rf, 3, 8, {0, 3, 2, 4});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 0x5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{0xFF, 0x5A, 0x7C, 0x5A})); // 128/0, 249/2
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{0x80, 0x5A, 0x07, 0x5A})); // -128/-1, -7/-1
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{0x80, 0x5A, 0x01, 0x5A})); // 128%0, 249%2
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0x00, 0x5A, 0x00, 0x5A})); // -128%-1, -7%-1
}

TEST(VectorDivideTest, SignedOverflowAtE64)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 1, e64, m1, ta, ma\n"
                                          "vdiv.vx v4, v2, x10\n"
                                          "vrem.vx v5, v2, x10\n",
                                          [](RegisterFile &rf)
                                          { writeElements(rf, 2, 64, {0x8000000000000000}); });
    EXPECT_EQ(readElements(vm->registers_, 4, 64, 1), (Elements{0x8000000000000000}));
    EXPECT_EQ(readElements(vm->registers_, 5, 64, 1), (Elements{0}));
}

TEST(VectorMultiplyTest, WideningMultiplySignedness)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmulu.vv v4, v2, v3\n"
                                          "vwmul.vv v6, v2, v3\n"
                                          "vwmulsu.vv v8, v2, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 0x80, 3});
                                              writeElements(rf, 3, 8, {0xFF, 2, 0xFE});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 3), (Elements{0xFE01, 0x0100, 0x02FA}));
    EXPECT_EQ(readElements(rf, 6, 16, 3), (Elements{0x0001, 0xFF00, 0xFFFA}));
    EXPECT_EQ(readElements(rf, 8, 16, 3), (Elements{0xFF01, 0xFF00, 0x02FA}));
}

TEST(VectorMultiplyTest, WideningMaskedOffAndTailElementsKeepTheirOldValues)
{
    // e8 sources, e16 results. vl = 3; v0 = 0b0101 enables elements 0 and 2. Every 16-bit
    // destination element starts as 0x5A5A, so element 1 (masked off) and element 3 (tail)
    // must visibly keep it. x10 = -1 is 0xFF.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmul.vv   v4, v2, v3, v0.t\n"
                                          "vwmulu.vx  v6, v2, x10, v0.t\n"
                                          "vwmulsu.vv v8, v2, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0xFF, 1, 0x80, 2});
                                              writeElements(rf, 3, 8, {0xFF, 3, 2, 4});
                                              for (size_t r = 4; r <= 8; r += 2)
                                              {
                                                  writeElements(rf, r, 16, Elements(4, 0x5A5A));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // signed: -1 x -1 = 1, -128 x 2 = -256
    EXPECT_EQ(readElements(rf, 4, 16, 4), (Elements{0x0001, 0x5A5A, 0xFF00, 0x5A5A}));
    // unsigned: 255 x 255 = 0xFE01, 128 x 255 = 0x7F80
    EXPECT_EQ(readElements(rf, 6, 16, 4), (Elements{0xFE01, 0x5A5A, 0x7F80, 0x5A5A}));
    // signed vs2 x unsigned vs1: -1 x 255 = -255, -128 x 2 = -256
    EXPECT_EQ(readElements(rf, 8, 16, 4), (Elements{0xFF01, 0x5A5A, 0xFF00, 0x5A5A}));
}

TEST(VectorMultiplyAddTest, SingleWidthForms)
{
    // vs1 = v2 [3, -1, 2], vs2 = v3 [4, 5, 128]; each destination starts as [10, 10, 10].
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 2\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmacc.vv v4, v2, v3\n"
                                          "vnmsac.vv v5, v2, v3\n"
                                          "vmadd.vv v6, v2, v3\n"
                                          "vnmsub.vv v7, v2, v3\n"
                                          "vmacc.vx v8, x10, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {3, 0xFF, 2});
                                              writeElements(rf, 3, 8, {4, 5, 0x80});
                                              for (size_t r = 4; r <= 8; ++r)
                                              {
                                                  writeElements(rf, r, 8, {10, 10, 10});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 3), (Elements{22, 5, 10}));     // vd + vs1*vs2
    EXPECT_EQ(readElements(rf, 5, 8, 3), (Elements{0xFE, 15, 10}));  // vd - vs1*vs2
    EXPECT_EQ(readElements(rf, 6, 8, 3), (Elements{34, 0xFB, 0x94})); // vs1*vd + vs2
    EXPECT_EQ(readElements(rf, 7, 8, 3), (Elements{0xE6, 15, 0x6C})); // vs2 - vs1*vd
    EXPECT_EQ(readElements(rf, 8, 8, 3), (Elements{18, 20, 10}));
}

TEST(VectorMultiplyAddTest, MaskedOffAndTailElementsAreNotComputed)
{
    // vl = 3; v0 = 0b0101 enables elements 0 and 2. Every vd starts as 10. Elements 1 (masked
    // off) and 3 (tail) would change if they were computed, so they must stay exactly 10.
    // vs1 = v2 [3, 7, -1, 9], vs2 = v3 [4, 6, 5, 8], x10 = 2.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, 2\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vmacc.vv  v4, v2, v3, v0.t\n"
                                          "vnmsac.vx v5, x10, v3, v0.t\n"
                                          "vmadd.vv  v6, v2, v3, v0.t\n"
                                          "vnmsub.vx v7, x10, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {3, 7, 0xFF, 9});
                                              writeElements(rf, 3, 8, {4, 6, 5, 8});
                                              for (size_t r = 4; r <= 7; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(4, 10));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 8, 4), (Elements{22, 10, 5, 10}));     // vd + vs1*vs2
    EXPECT_EQ(readElements(rf, 5, 8, 4), (Elements{2, 10, 0, 10}));      // vd - 2*vs2
    EXPECT_EQ(readElements(rf, 6, 8, 4), (Elements{34, 10, 0xFB, 10}));  // vs1*vd + vs2
    EXPECT_EQ(readElements(rf, 7, 8, 4), (Elements{0xF0, 10, 0xF1, 10})); // vs2 - 2*vd
}

TEST(VectorMultiplyAddTest, WideningFormsAndSignedness)
{
    // vs1 = v2 [0xFF, 2, 0x80], vs2 = v3 [0xFF, 0xFE, 3]; each e16 destination starts at 1000.
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmaccu.vv v4, v2, v3\n"
                                          "vwmacc.vv v6, v2, v3\n"
                                          "vwmaccsu.vv v8, v2, v3\n"
                                          "vwmaccus.vx v10, x10, v3\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 2, 0x80});
                                              writeElements(rf, 3, 8, {0xFF, 0xFE, 3});
                                              for (size_t r = 4; r <= 10; r += 2)
                                              {
                                                  writeElements(rf, r, 16, {1000, 1000, 1000});
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 4, 16, 3), (Elements{489, 1508, 1384})); // 1000+65025 wraps
    EXPECT_EQ(readElements(rf, 6, 16, 3), (Elements{1001, 996, 616}));
    EXPECT_EQ(readElements(rf, 8, 16, 3), (Elements{745, 1508, 616}));  // signed vs1
    EXPECT_EQ(readElements(rf, 10, 16, 3), (Elements{745, 490, 1765})); // unsigned rs1 = 255
}

TEST(VectorMergeMoveTest, MergeWritesEveryBodyElementAndMoveSplats)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -7\n"
                                          "vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vmerge.vvm v5, v2, v3, v0\n"
                                          "vmerge.vxm v6, v2, x10, v0\n"
                                          "vmerge.vim v7, v2, -16, v0\n"
                                          "vmv.v.v v8, v3\n"
                                          "vmv.v.x v9, x10\n"
                                          "vmv.v.i v10, 15\n"
                                          // The spec's mask-to-elements idiom.
                                          "vmv.v.i v11, 0\n"
                                          "vmerge.vim v11, v11, 1, v0\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {1, 2, 3, 4});
                                              writeElements(rf, 3, 8, {10, 20, 30, 40});
                                              for (size_t r = 5; r <= 10; ++r)
                                              {
                                                  writeElements(rf, r, 8, Elements(5, 9));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // Element 4 is the tail: undisturbed.
    EXPECT_EQ(readElements(rf, 5, 8, 5), (Elements{10, 2, 30, 4, 9}));
    EXPECT_EQ(readElements(rf, 6, 8, 5), (Elements{0xF9, 2, 0xF9, 4, 9}));
    EXPECT_EQ(readElements(rf, 7, 8, 5), (Elements{0xF0, 2, 0xF0, 4, 9}));
    EXPECT_EQ(readElements(rf, 8, 8, 5), (Elements{10, 20, 30, 40, 9}));
    EXPECT_EQ(readElements(rf, 9, 8, 5), (Elements{0xF9, 0xF9, 0xF9, 0xF9, 9}));
    EXPECT_EQ(readElements(rf, 10, 8, 5), (Elements{15, 15, 15, 15, 9}));
    EXPECT_EQ(readElements(rf, 11, 8, 4), (Elements{1, 0, 1, 0}));
}

TEST(VectorMergeMoveTest, MoveWithNonZeroVs2IsReservedAndUndoWorks)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e32, m1, ta, ma\n"
                                          "vmv.v.i v1, -3\n");
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0xFFFFFFFD));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4), Elements(4, 0xFFFFFFFD));

    // vmv.v.v v1, v3 with the vs2 field set to v2 instead of v0.
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x5e0180d7 | (2u << 20), 0, 0,
                                                      vm->registers_, vm->memory_controller_,
                                                      changes, memoryChanges),
                 std::runtime_error);
}

TEST(VectorMultiplyAddTest, WideningMaskedOffAndTailElementsAreNotComputed)
{
    // e8 sources, e16 accumulators all starting at 1000. vl = 3; v0 = 0b0101 enables elements 0
    // and 2. Elements 1 (masked off) and 3 (tail) would change if computed, so they must stay
    // 1000. vs1 = v2 [0xFF, 7, 0x80, 9], vs2 = v3 [0xFF, 6, 3, 8], x10 = -1 (0xFF).
    auto vm = runOn<Kites::RVSSProcessor>("li x10, -1\n"
                                          "vsetivli x0, 3, e8, m1, ta, ma\n"
                                          "vwmaccu.vv  v4, v2, v3, v0.t\n"
                                          "vwmacc.vx   v6, x10, v3, v0.t\n"
                                          "vwmaccsu.vv v8, v2, v3, v0.t\n"
                                          "vwmaccus.vx v10, x10, v3, v0.t\n",
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 0, 8, {0b0101});
                                              writeElements(rf, 2, 8, {0xFF, 7, 0x80, 9});
                                              writeElements(rf, 3, 8, {0xFF, 6, 3, 8});
                                              for (size_t r = 4; r <= 10; r += 2)
                                              {
                                                  writeElements(rf, r, 16, Elements(4, 1000));
                                              }
                                          });
    const RegisterFile &rf = vm->registers_;
    // unsigned x unsigned: 1000 + 255*255 wraps to 489; 1000 + 128*3
    EXPECT_EQ(readElements(rf, 4, 16, 4), (Elements{489, 1000, 1384, 1000}));
    // signed -1 x signed vs2: 1000 + 1; 1000 - 3
    EXPECT_EQ(readElements(rf, 6, 16, 4), (Elements{1001, 1000, 997, 1000}));
    // signed vs1 x unsigned vs2: 1000 - 255; 1000 - 384
    EXPECT_EQ(readElements(rf, 8, 16, 4), (Elements{745, 1000, 616, 1000}));
    // unsigned 255 x signed vs2: 1000 - 255; 1000 + 765
    EXPECT_EQ(readElements(rf, 10, 16, 4), (Elements{745, 1000, 1765, 1000}));
}

TEST(VectorMultiplyAddTest, IllegalWideningConfigurationsTrap)
{
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwmacc.vv v2, v4, v5\n");
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwmul.vv v2, v4, v5\n");
    expectIllegal("vsetvli x0, x0, e8, m1, ta, ma\nvwmaccu.vv v3, v4, v5\n"); // odd vd group
}

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

TEST(VectorReductionAssemblerTest, EncodingsMatchLlvmMcAndDecodeBack)
{
    const std::vector<std::pair<std::string, uint32_t>> cases = {
        {"vredsum.vs v1, v2, v3", 0x0221a0d7},       {"vredand.vs v1, v2, v3, v0.t", 0x0421a0d7},
        {"vredor.vs v1, v2, v3", 0x0a21a0d7},        {"vredxor.vs v1, v2, v3", 0x0e21a0d7},
        {"vredminu.vs v1, v2, v3", 0x1221a0d7},      {"vredmin.vs v1, v2, v3", 0x1621a0d7},
        {"vredmaxu.vs v1, v2, v3", 0x1a21a0d7},      {"vredmax.vs v1, v2, v3", 0x1e21a0d7},
        {"vwredsumu.vs v1, v2, v3", 0xc22180d7},     {"vwredsum.vs v1, v2, v3, v0.t", 0xc42180d7},
        {"vredsum.vs v0, v2, v3, v0.t", 0x0021a057}, {"vredsum.vs v2, v2, v2", 0x02212157},
        {"vwredsum.vs v2, v2, v3", 0xc6218157},
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
}

TEST(VectorReductionAssemblerTest, OperandRules)
{
    for (const std::string &line : {"vredsum.vs v1, v2\n", "vredsum.vs v1, v2, a0\n",
                                    "vredsum.vs v1, v2, 3\n", "vwredsum.vs v1, v2, v3, v0\n",
                                    "vredsum.vv v1, v2, v3\n"})
    {
        EXPECT_THROW(assembleSource(line), std::runtime_error) << line;
    }
    // Unlike ordinary arithmetic, a reduction may write v0 under v0.t and overlap any source.
    EXPECT_NO_THROW(assembleSource("vredsum.vs v0, v2, v3, v0.t\nvwredsum.vs v2, v2, v3\n"));
}

TEST(VectorReductionTest, EveryOperationSignedAndUnsigned)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n"
                                          "vredand.vs v11, v2, v4\n"
                                          "vredor.vs v12, v2, v3\n"
                                          "vredxor.vs v13, v2, v3\n"
                                          "vredminu.vs v14, v2, v4\n"
                                          "vredmin.vs v15, v2, v4\n"
                                          "vredmaxu.vs v16, v2, v3\n"
                                          "vredmax.vs v17, v2, v3\n",
                                          reductionData);
    const RegisterFile &rf = vm->registers_;
    // 5 + 241 + 243 + 127 + 245 = 861 = 0x35D, truncated to 8 bits.
    EXPECT_EQ(readElements(rf, 10, 8, 4), (Elements{0x5D, 0x99, 0x99, 0x99}));
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0x71}));
    EXPECT_EQ(readElements(rf, 12, 8, 1), (Elements{0xFF}));
    EXPECT_EQ(readElements(rf, 13, 8, 1), (Elements{0x8D}));
    // vs1 = 0xFF: the largest unsigned value but -1 signed.
    EXPECT_EQ(readElements(rf, 14, 8, 1), (Elements{0x7F})); // unsigned min of 255, 241, .., 127
    EXPECT_EQ(readElements(rf, 15, 8, 1), (Elements{0xF1})); // signed min: -15
    EXPECT_EQ(readElements(rf, 16, 8, 1), (Elements{0xF5})); // unsigned max: 245
    EXPECT_EQ(readElements(rf, 17, 8, 1), (Elements{0x7F})); // signed max: 127
    // Only element 0 is written; the rest of vd is tail.
    EXPECT_EQ(readElements(rf, 17, 8, 4), (Elements{0x7F, 0x99, 0x99, 0x99}));
}

TEST(VectorReductionTest, AllOnesVs1ForMax)
{
    // vs1[0] = 0xFF: the largest unsigned value, but -1 signed.
    // v2 = [0xF1, 0xF3, 0x7F, 0xF5] has a positive element (127); v5 is all negative.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredmaxu.vs v10, v2, v4\n"
                                          "vredmax.vs  v11, v2, v4\n"
                                          "vredmax.vs  v12, v5, v4\n",
                                          [](RegisterFile &rf)
                                          {
                                              reductionData(rf);
                                              writeElements(rf, 5, 8, {0xF1, 0xF3, 0x80, 0xF5});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 1), (Elements{0xFF})); // 255 beats every element
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0x7F})); // -1 loses to 127
    EXPECT_EQ(readElements(rf, 12, 8, 1), (Elements{0xFF})); // -1 beats all-negative elements
}

TEST(VectorReductionTest, MaskedOffElementsAreLeftOut)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3, v0.t\n"
                                          "vredmaxu.vs v11, v2, v3, v0.t\n"
                                          "vredsum.vs v0, v2, v3, v0.t\n", // v0 may be vd
                                          [](RegisterFile &rf)
                                          {
                                              reductionData(rf);
                                              writeElements(rf, 0, 8, {0b0101}); // 0 and 2
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 8, 1), (Elements{0x75})); // 5 + 241 + 127 = 373
    EXPECT_EQ(readElements(rf, 11, 8, 1), (Elements{0xF1}));
    EXPECT_EQ(readElements(rf, 0, 8, 1), (Elements{0x75}));
}

TEST(VectorReductionTest, Vs2IsAnLmulGroup)
{
    Elements data;
    for (uint64_t i = 1; i <= 20; ++i)
    {
        data.push_back(i);
    }
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 20\n"
                                          "vsetvli x0, x5, e8, m2, ta, ma\n"
                                          "vredsum.vs v8, v4, v1\n"
                                          "vredmaxu.vs v9, v4, v1\n",
                                          [&](RegisterFile &rf) { writeElements(rf, 4, 8, data); });
    EXPECT_EQ(readElements(vm->registers_, 8, 8, 1), (Elements{210})); // 1 + 2 + ... + 20
    EXPECT_EQ(readElements(vm->registers_, 9, 8, 1), (Elements{20}));
}

TEST(VectorReductionTest, WideningSumsAreTwiceSew)
{
    // vs2 e8 [0xFF, 0xFF, 0x80, 0x01]; vs1[0] e16 = 1000.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vwredsumu.vs v10, v2, v3\n"
                                          "vwredsum.vs v11, v2, v3\n"
                                          "vwredsum.vs v2, v2, v3\n", // vd may overlap vs2
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 2, 8, {0xFF, 0xFF, 0x80, 0x01});
                                              writeElements(rf, 3, 16, {1000});
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(readElements(rf, 10, 16, 1), (Elements{1639})); // 1000 + 255 + 255 + 128 + 1
    EXPECT_EQ(readElements(rf, 11, 16, 1), (Elements{871}));  // 1000 - 1 - 1 - 128 + 1
    EXPECT_EQ(readElements(rf, 2, 16, 1), (Elements{871}));
}

TEST(VectorReductionTest, ZeroVlWritesNothing)
{
    auto vm = runOn<Kites::RVSSProcessor>("li x5, 0\n"
                                          "vsetvli x0, x5, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n",
                                          reductionData);
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 4), Elements(4, 0x99));
}

TEST(VectorReductionTest, IllegalConfigurationsTrap)
{
    expectIllegal("vredsum.vs v1, v2, v3\n");                                  // vtype.vill
    expectIllegal("vsetvli x0, x0, e64, m1, ta, ma\nvwredsum.vs v1, v2, v3\n"); // 2*SEW > ELEN
    expectIllegal("vsetvli x0, x0, e8, m2, ta, ma\nvredsum.vs v1, v3, v4\n");  // misaligned vs2

    // A non-zero vstart is illegal for a reduction.
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n", reductionData);
    std::vector<Kites::RegisterChange> changes;
    std::vector<Kites::MemoryChange> memoryChanges;
    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 1);
    EXPECT_THROW(Kites::rvv::ExecuteVectorInstruction(0x0221a0d7, 0, 0, vm->registers_,
                                                      vm->memory_controller_, changes,
                                                      memoryChanges),
                 std::runtime_error);
    vm->registers_.WriteCsr(RegisterFile::CSR_VSTART, 0);
    EXPECT_NO_THROW(Kites::rvv::ExecuteVectorInstruction(0x0221a0d7, 0, 0, vm->registers_,
                                                         vm->memory_controller_, changes,
                                                         memoryChanges));
}

TEST(VectorReductionTest, UndoRestoresDestination)
{
    auto vm = runOn<Kites::RVSSProcessor>("vsetivli x0, 4, e8, m1, ta, ma\n"
                                          "vredsum.vs v10, v2, v3\n",
                                          reductionData);
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x5D}));
    vm->Undo();
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x99}));
    vm->Redo();
    EXPECT_EQ(readElements(vm->registers_, 10, 8, 1), (Elements{0x5D}));
}

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
                                          "vmsif.m v15, v4\n"
                                          "vmsof.m v16, v4\n"
                                          "vmsbf.m v14, v3, v0.t\n",  // spec example
                                          [](RegisterFile &rf)
                                          {
                                              writeElements(rf, 3, 8, {0x94}); // 1001 0100
                                              writeElements(rf, 0, 8, {0xC3}); // 1100 0011
                                              writeElements(rf, 14, 8, {0x3C});
                                              writeElements(rf, 16, 8, {0xAA}); // must be cleared
                                          });
    const RegisterFile &rf = vm->registers_;
    EXPECT_EQ(maskByte(rf, 10), 0x03u);
    EXPECT_EQ(maskByte(rf, 11), 0x07u);
    EXPECT_EQ(maskByte(rf, 12), 0x04u);
    EXPECT_EQ(maskByte(rf, 13), 0xFFu); // no set bit: everything is "before the first"
    EXPECT_EQ(maskByte(rf, 15), 0xFFu); // ... and "up to and including" it
    EXPECT_EQ(maskByte(rf, 16), 0x00u); // ... but there is no "only first" bit
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

// Spec section 8: an element that is not naturally aligned is either transferred or raises an
// address-misaligned exception. Kites, like its scalar loads/stores, transfers it.
TEST(VectorLoadStoreTest, MisalignedElementsAreTransferred)
{
    auto vm = runOn<Rvss>("lui a0, 0x10000\n"
                          "addi a1, a0, 1\n"           // odd base
                          "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "vle32.v v1, (a1)\n"         // words at +1, +5, +9, +13
                          "li t0, 3\n"
                          "vlse16.v v2, (a1), t0\n"    // halfwords at +1, +4, +7, +10
                          "vluxei8.v v3, (a0), v4\n"   // words at the odd offsets in v4
                          "addi a2, a0, 67\n"
                          "vse32.v v1, (a2)\n",        // store to 0x10000043
                          [](Rvss &m)
                          {
                              Bytes bytes;
                              for (int i = 0; i < 32; ++i)
                              {
                                  bytes.push_back(static_cast<uint8_t>(0x10 + i));
                              }
                              writeBytes(m, kBase, bytes);
                              writeElements(m.registers_, 4, 8, {3, 1, 7, 5});
                          });
    EXPECT_EQ(readElements(vm->registers_, 1, 32, 4),
              (Elements{0x14131211, 0x18171615, 0x1C1B1A19, 0x201F1E1D}));
    EXPECT_EQ(readElements(vm->registers_, 2, 16, 4), (Elements{0x1211, 0x1514, 0x1817, 0x1B1A}));
    EXPECT_EQ(readElements(vm->registers_, 3, 32, 4),
              (Elements{0x16151413, 0x14131211, 0x1A191817, 0x18171615}));
    Bytes expected;
    for (int i = 1; i <= 16; ++i)
    {
        expected.push_back(static_cast<uint8_t>(0x10 + i));
    }
    EXPECT_EQ(readBytes(*vm, kBase + 67, 16), expected);
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

} // namespace vtest_load_store

// ===== vector_stages_test ====================================================================
// The EX / MEM / WB split of vector instructions (vector_stages.h), as the single-cycle VM uses it:
// EX changes no state, MEM touches only memory (and only for loads/stores), WB commits the
// registers. The composed path (ExecuteVectorInstruction) is covered by every other vector test.
namespace vtest_stages
{

TEST_F(VectorStagesTest, ArithmeticWritesVdOnlyInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vadd.vv v3, v1, v2"), 0, 0,
                                                       vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";
    ASSERT_EQ(stage.pendingWrites.size(), 1u);
    EXPECT_EQ(stage.pendingWrites[0].reg_index, 3u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(Take(vm), before) << "MEM changed state for an arithmetic instruction";
    EXPECT_TRUE(memoryChanges.empty());

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(Elements(3), (std::vector<uint64_t>{11, 22, 33, 44}));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reg_type, 3u);
    EXPECT_EQ(changes[0].old_bytes, before.vregs[3]);
}

TEST_F(VectorStagesTest, VsetWritesVlAndVtypeOnlyInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(
        assembleOne("vsetvli t0, a0, e16, m2, ta, ma"), 3, 0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed vl/vtype";
    EXPECT_TRUE(stage.result.writesRd);
    EXPECT_EQ(stage.result.rdValue, 3u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    const rvv::VectorExecResult rd = rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(rd.rdValue, 3u);
    EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VL), 3u);
    EXPECT_NE(vm.registers_.ReadCsr(RegisterFile::CSR_VTYPE), before.vtype);
}

TEST_F(VectorStagesTest, StoreWritesMemoryInMemoryStage)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vse32.v v2, (a0)"), kBase,
                                                       0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";
    ASSERT_EQ(stage.memPlan.accesses.size(), 4u);
    EXPECT_EQ(stage.memPlan.accesses[3].address, kBase + 12);
    EXPECT_EQ(stage.memPlan.accesses[3].storeValue, 40u);

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(vm.memory_controller_.peekByte(kBase), 10u);
    EXPECT_EQ(vm.memory_controller_.peekByte(kBase + 12), 40u);
    EXPECT_EQ(memoryChanges.size(), 4u);
    Snapshot afterMem = Take(vm);
    afterMem.memory = before.memory;
    EXPECT_EQ(afterMem, before) << "MEM changed a register";

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_TRUE(changes.empty());
}

TEST_F(VectorStagesTest, LoadReadsInMemoryStageAndWritesVdInWriteBack)
{
    const Snapshot before = Take(vm);
    rvv::VectorExStage stage = rvv::VectorExecuteStage(assembleOne("vle32.v v4, (a0)"), kBase,
                                                       0, vm.registers_, scratch);
    EXPECT_EQ(Take(vm), before) << "EX changed state";

    rvv::VectorMemoryStage(stage, vm.memory_controller_, memoryChanges);
    EXPECT_EQ(stage.memData.loadedValues, (std::vector<uint64_t>{5, 6, 7, 8}));
    EXPECT_EQ(Take(vm), before) << "MEM wrote vd";

    rvv::VectorWriteBackStage(stage, vm.registers_, changes);
    EXPECT_EQ(Elements(4), (std::vector<uint64_t>{5, 6, 7, 8}));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reg_index, 4u);
}

TEST_F(VectorStagesTest, IllegalInstructionTrapsInExecuteStage)
{
    // At e32/m1, vle64.v has EMUL = 2, so v1 is a misaligned group: illegal, caught in EX.
    EXPECT_THROW(rvv::VectorExecuteStage(assembleOne("vle64.v v1, (a0)"), kBase, 0,
                                         vm.registers_, scratch),
                 std::runtime_error);
}

TEST(VectorStagesRvssTest, VectorAddExampleRunsAndUndoesThroughThePhases)
{
    Kites::setupVmStateDirectory();
    std::ifstream file("../examples/vector_add.s");
    ASSERT_TRUE(file.good());
    std::stringstream source;
    source << file.rdbuf();
    std::istringstream stream(source.str());

    RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.DebugRun();
    const uint64_t c = kBase + 80; // after a[10] and b[10]
    for (uint64_t i = 0; i < 10; ++i)
    {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; ++b)
        {
            word |= static_cast<uint32_t>(vm.memory_controller_.peekByte(c + 4 * i + b)) << (8 * b);
        }
        EXPECT_EQ(word, 11 * (i + 1)) << "c[" << i << "]";
    }

    for (int guard = 0; guard < 1000 && vm.program_counter_ != 0; ++guard)
    {
        vm.Undo();
    }
    ASSERT_EQ(vm.program_counter_, 0u);
    for (uint64_t i = 0; i < 40; ++i)
    {
        EXPECT_EQ(vm.memory_controller_.peekByte(c + i), 0u) << "byte " << i << " of c";
    }
    EXPECT_EQ(vm.registers_.ReadVectorRegister(3),
              std::vector<uint8_t>(vm.registers_.GetVlenBytes(), 0));
}

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

TEST(VectorSpecSweepTest, SingleWidthArithmeticAtEverySew)
{
    Data data(1);
    for (const SingleOp &op : SingleOps())
        for (Src form : op.forms)
            for (u64 w : kSews)
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string vd = "v" + std::to_string(kVd);
                    const std::string vs2 = "v" + std::to_string(kVs2);
                    const std::string text =
                        std::string(op.name) + Suffix(form) + " " + vd + ", " +
                        (op.sourceFirst ? src.text + ", " + vs2 : vs2 + ", " + src.text) +
                        (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, w, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    const RegisterFile &rf = run.vm->registers_;
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        const u64 old = OldElement(run, kVd, i, w);
                        const bool active = i < vl && (!masked || OldMaskBit(run, 0, i));
                        u64 expected = old;
                        if (active)
                        {
                            const u64 a = OldElement(run, kVs2, i, w);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w)
                                                          : src.scalar;
                            expected = U(op.f(a, b, old, w), w);
                        }
                        ASSERT_EQ(rf.ReadVectorElement(kVd, i, w), expected) << "element " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, MergeMoveAndCarryAtEverySew)
{
    Data data(2);
    struct Op
    {
        const char *name;
        std::vector<Src> forms;
        bool usesV0;  // vmerge / vadc / vsbc: ", v0" operand, every body element written
        int kind;     // 0 merge, 1 move, 2 adc, 3 sbc
    };
    const std::vector<Op> ops = {
        {"vmerge", {Src::VV, Src::VX, Src::VI}, true, 0},
        {"vmv.v", {Src::VV, Src::VX, Src::VI}, false, 1},
        {"vadc", {Src::VV, Src::VX, Src::VI}, true, 2},
        {"vsbc", {Src::VV, Src::VX}, true, 3},
    };
    for (const Op &op : ops)
        for (Src form : op.forms)
            for (u64 w : kSews)
            {
                const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                u64 rs1 = 0;
                const Source src = MakeSource(form, data, w, rs1);
                std::string text;
                if (op.kind == 1)
                    text = std::string("vmv.v.") + Suffix(form).substr(2) + " v8, " + src.text;
                else
                    text = std::string(op.name) + Suffix(form) + "m v8, v16, " + src.text + ", v0";
                SCOPED_TRACE(text + " at e" + std::to_string(w));
                Outcome run = Execute(text, w, 0,
                                  [&](RegisterFile &rf)
                                  {
                                      FillRandom(rf, kVd, vlmax, w, data);
                                      FillRandom(rf, kVs2, vlmax, w, data);
                                      FillRandom(rf, kVs1, vlmax, w, data);
                                      FillMask(rf, data);
                                      rf.WriteGpr(kScalar, rs1);
                                  });
                for (u64 i = 0; i < vlmax; ++i)
                {
                    u64 expected = OldElement(run, kVd, i, w);
                    if (i < vl)
                    {
                        const u64 a = OldElement(run, kVs2, i, w);
                        const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                        const u64 m = OldMaskBit(run, 0, i);
                        switch (op.kind)
                        {
                        case 0: expected = m ? b : a; break;
                        case 1: expected = b; break;
                        case 2: expected = U(a + b + m, w); break;
                        default: expected = U(a - b - m, w); break;
                        }
                    }
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                        << "element " << i;
                }
            }
}

TEST(VectorSpecSweepTest, CompareAndCarryOutMasksAtEverySew)
{
    Data data(3);
    struct Op
    {
        const char *name;
        std::vector<Src> forms;
        std::function<bool(u64 a, u64 b, u64 w)> f;
    };
    const std::vector<Op> compares = {
        {"vmseq", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) == U(b, w); }},
        {"vmsne", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) != U(b, w); }},
        {"vmsltu", {Src::VV, Src::VX}, [](u64 a, u64 b, u64 w) { return U(a, w) < U(b, w); }},
        {"vmslt", {Src::VV, Src::VX}, [](u64 a, u64 b, u64 w) { return S(a, w) < S(b, w); }},
        {"vmsleu", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) <= U(b, w); }},
        {"vmsle", {Src::VV, Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return S(a, w) <= S(b, w); }},
        {"vmsgtu", {Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return U(a, w) > U(b, w); }},
        {"vmsgt", {Src::VX, Src::VI}, [](u64 a, u64 b, u64 w) { return S(a, w) > S(b, w); }},
    };
    for (const Op &op : compares)
        for (Src form : op.forms)
            for (u64 w : kSews)
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string text = std::string(op.name) + Suffix(form) + " v8, v16, " +
                                             src.text + (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, 16, 8, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < kVlenBits; ++i)
                    {
                        bool expected = OldMaskBit(run, kVd, i);
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 a = OldElement(run, kVs2, i, w);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            expected = op.f(a, b, w);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadMaskBit(kVd, i), expected) << "bit " << i;
                    }
                }

    // vmadc / vmsbc: the exact sum or difference before truncation (carry / borrow out).
    for (bool borrow : {false, true})
        for (bool withCarryIn : {false, true})
            for (Src form : {Src::VV, Src::VX, Src::VI})
                for (u64 w : kSews)
                {
                    if (borrow && form == Src::VI)
                        continue; // no vmsbc.vi / vmsbc.vim
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    const std::string text = std::string(borrow ? "vmsbc" : "vmadc") + Suffix(form) +
                                             (withCarryIn ? "m" : "") + " v8, v16, " + src.text +
                                             (withCarryIn ? ", v0" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, 16, 8, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < kVlenBits; ++i)
                    {
                        bool expected = OldMaskBit(run, kVd, i);
                        if (i < vl)
                        {
                            const u128 a = U(OldElement(run, kVs2, i, w), w);
                            const u128 b =
                                U(form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar, w);
                            const u128 c = withCarryIn && OldMaskBit(run, 0, i) ? 1 : 0;
                            expected = borrow ? a < b + c : ((a + b + c) >> w) != 0;
                        }
                        ASSERT_EQ(run.vm->registers_.ReadMaskBit(kVd, i), expected) << "bit " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, WideningArithmeticAtEveryLegalSew)
{
    Data data(4);
    struct Op
    {
        const char *name;
        bool wideVs2;     // .wv / .wx
        bool sourceFirst; // vwmacc*: vd, src, vs2
        bool vs2Signed, srcSigned;
        std::function<u64(u64 a, u64 b, u64 old)> f; // a, b already extended to 64 bits
        std::vector<Src> forms = {Src::VV, Src::VX};
    };
    const auto add = [](u64 a, u64 b, u64) { return a + b; };
    const auto sub = [](u64 a, u64 b, u64) { return a - b; };
    const auto mul = [](u64 a, u64 b, u64) { return a * b; };
    const auto macc = [](u64 a, u64 b, u64 d) { return d + a * b; };
    const std::vector<Op> ops = {
        {"vwaddu", false, false, false, false, add}, {"vwadd", false, false, true, true, add},
        {"vwsubu", false, false, false, false, sub}, {"vwsub", false, false, true, true, sub},
        {"vwaddu", true, false, false, false, add},  {"vwadd", true, false, true, true, add},
        {"vwsubu", true, false, false, false, sub},  {"vwsub", true, false, true, true, sub},
        {"vwmulu", false, false, false, false, mul}, {"vwmul", false, false, true, true, mul},
        {"vwmulsu", false, false, true, false, mul},
        {"vwmaccu", false, true, false, false, macc}, {"vwmacc", false, true, true, true, macc},
        {"vwmaccsu", false, true, false, true, macc},
        {"vwmaccus", false, true, true, false, macc, {Src::VX}},
    };
    for (const Op &op : ops)
        for (Src form : op.forms)
            for (u64 w : {8, 16, 32})
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1, ww = 2 * w;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    std::string suffix = Suffix(form);
                    if (op.wideVs2)
                        suffix[1] = 'w';
                    const std::string text =
                        std::string(op.name) + suffix + " v8, " +
                        (op.sourceFirst ? src.text + ", v16" : "v16, " + src.text) +
                        (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, ww, data);
                                          FillRandom(rf, kVs2, vlmax, op.wideVs2 ? ww : w, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    const auto ext = [](u64 v, u64 bits, bool isSigned)
                    { return isSigned ? static_cast<u64>(S(v, bits)) : U(v, bits); };
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        const u64 old = OldElement(run, kVd, i, ww);
                        u64 expected = old;
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 a = op.wideVs2 ? OldElement(run, kVs2, i, ww)
                                                     : ext(OldElement(run, kVs2, i, w), w,
                                                           op.vs2Signed);
                            const u64 rawB =
                                form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            expected = U(op.f(a, ext(rawB, w, op.srcSigned), old), ww);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, ww), expected)
                            << "element " << i;
                    }
                }
}

TEST(VectorSpecSweepTest, NarrowingShiftsAndExtensionsAtEveryLegalSew)
{
    Data data(5);
    for (bool arithmetic : {false, true})
        for (Src form : {Src::VV, Src::VX, Src::VUI})
            for (u64 w : {8, 16, 32})
                for (bool masked : {false, true})
                {
                    const u64 vlmax = kVlenBits / w, vl = vlmax - 1, ww = 2 * w;
                    u64 rs1 = 0;
                    const Source src = MakeSource(form, data, w, rs1);
                    std::string suffix = Suffix(form);
                    suffix[1] = 'w';
                    const std::string text = std::string(arithmetic ? "vnsra" : "vnsrl") + suffix +
                                             " v8, v16, " + src.text + (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w));
                    Outcome run = Execute(text, w, 0,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, vlmax, w, data);
                                          FillRandom(rf, kVs2, vlmax, ww, data);
                                          FillRandom(rf, kVs1, vlmax, w, data);
                                          FillMask(rf, data);
                                          rf.WriteGpr(kScalar, rs1);
                                      });
                    for (u64 i = 0; i < vlmax; ++i)
                    {
                        u64 expected = OldElement(run, kVd, i, w);
                        if (i < vl && (!masked || OldMaskBit(run, 0, i)))
                        {
                            const u64 wide = OldElement(run, kVs2, i, ww);
                            const u64 b = form == Src::VV ? OldElement(run, kVs1, i, w) : src.scalar;
                            const unsigned sh = static_cast<unsigned>(b & (ww - 1)); // log2(2*SEW)
                            expected = U(arithmetic ? static_cast<u64>(S(wide, ww) >> sh)
                                                    : U(wide, ww) >> sh,
                                         w);
                        }
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                            << "element " << i;
                    }
                }

    for (unsigned factor : {2u, 4u, 8u})
        for (bool isSigned : {false, true})
            for (u64 w : kSews)
            {
                if (w / factor < 8)
                    continue; // source EEW below 8 is illegal (checked elsewhere)
                const u64 vlmax = kVlenBits / w, vl = vlmax - 1, nw = w / factor;
                const std::string text = std::string(isSigned ? "vsext" : "vzext") + ".vf" +
                                         std::to_string(factor) + " v8, v16, v0.t";
                SCOPED_TRACE(text + " at e" + std::to_string(w));
                Outcome run = Execute(text, w, 0,
                                  [&](RegisterFile &rf)
                                  {
                                      FillRandom(rf, kVd, vlmax, w, data);
                                      FillRandom(rf, kVs2, vlmax, nw, data);
                                      FillMask(rf, data);
                                  });
                for (u64 i = 0; i < vlmax; ++i)
                {
                    u64 expected = OldElement(run, kVd, i, w);
                    if (i < vl && OldMaskBit(run, 0, i))
                    {
                        const u64 v = OldElement(run, kVs2, i, nw);
                        expected = U(isSigned ? static_cast<u64>(S(v, nw)) : U(v, nw), w);
                    }
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, w), expected)
                        << "element " << i;
                }
            }
}

TEST(VectorSpecSweepTest, ReductionsAtEverySewAndLmul)
{
    Data data(6);
    struct Op
    {
        const char *name;
        bool widening, isSigned;
        std::function<u64(u64 acc, u64 x, u64 w)> f; // values already extended
    };
    const std::vector<Op> ops = {
        {"vredsum", false, false, [](u64 a, u64 x, u64) { return a + x; }},
        {"vredand", false, false, [](u64 a, u64 x, u64) { return a & x; }},
        {"vredor", false, false, [](u64 a, u64 x, u64) { return a | x; }},
        {"vredxor", false, false, [](u64 a, u64 x, u64) { return a ^ x; }},
        {"vredminu", false, false, [](u64 a, u64 x, u64 w) { return U(x, w) < U(a, w) ? x : a; }},
        {"vredmin", false, true, [](u64 a, u64 x, u64 w) { return S(x, w) < S(a, w) ? x : a; }},
        {"vredmaxu", false, false, [](u64 a, u64 x, u64 w) { return U(x, w) > U(a, w) ? x : a; }},
        {"vredmax", false, true, [](u64 a, u64 x, u64 w) { return S(x, w) > S(a, w) ? x : a; }},
        {"vwredsumu", true, false, [](u64 a, u64 x, u64) { return a + x; }},
        {"vwredsum", true, true, [](u64 a, u64 x, u64) { return a + x; }},
    };
    for (const Op &op : ops)
        for (u64 w : kSews)
            for (int lmulLog2 : {0, 1})
                for (bool masked : {false, true})
                {
                    if (op.widening && w == 64)
                        continue;
                    const u64 vlmax = (kVlenBits / w) << lmulLog2, vl = vlmax - 1;
                    const u64 accW = op.widening ? 2 * w : w;
                    const std::string text = std::string(op.name) + ".vs v8, v16, v24" +
                                             (masked ? ", v0.t" : "");
                    SCOPED_TRACE(text + " at e" + std::to_string(w) + " m" +
                                 std::to_string(1 << lmulLog2));
                    Outcome run = Execute(text, w, lmulLog2,
                                      [&](RegisterFile &rf)
                                      {
                                          FillRandom(rf, kVd, kVlenBits / accW, accW, data);
                                          FillRandom(rf, kVs2, vlmax, w, data);
                                          FillRandom(rf, kVs1, kVlenBits / accW, accW, data);
                                          FillMask(rf, data);
                                      });
                    const auto ext = [&](u64 v, u64 bits)
                    { return op.isSigned ? static_cast<u64>(S(v, bits)) : U(v, bits); };
                    u64 acc = ext(OldElement(run, kVs1, 0, accW), accW);
                    for (u64 i = 0; i < vl; ++i)
                        if (!masked || OldMaskBit(run, 0, i))
                            acc = op.f(acc, ext(OldElement(run, kVs2, i, w), w), accW);
                    ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, 0, accW), U(acc, accW));
                    for (u64 i = 1; i < kVlenBits / accW; ++i) // the rest of vd is tail
                        ASSERT_EQ(run.vm->registers_.ReadVectorElement(kVd, i, accW),
                                  OldElement(run, kVd, i, accW))
                            << "element " << i;
                }
}

TEST(VectorSpecRulesTest, VlZeroAndVstartAtLeastVlWriteNothingAndClearVstart)
{
    // vl = 0: no element is written. vstart >= vl: likewise. Either way vstart ends at 0.
    for (const char *setVl : {"li x6, 0\nvsetvli x5, x6, e32, m1, ta, ma\n",
                              "vsetivli x0, 2, e32, m1, ta, ma\ncsrrwi x0, vstart, 3\n"})
        for (const char *instruction :
             {"vadd.vv v8, v16, v24", "vwadd.vv v8, v16, v24", "vmseq.vv v8, v16, v24",
              "vmand.mm v8, v16, v24", "vid.v v8", "vmerge.vvm v8, v16, v24, v0",
              "vnsrl.wi v8, v16, 1", "vmacc.vv v8, v24, v16"})
        {
            SCOPED_TRACE(std::string(setVl) + instruction);
            Kites::setupVmStateDirectory();
            std::istringstream stream(std::string(setVl) + instruction + "\n");
            Kites::RVSSProcessor vm;
            vm.LoadProgram(Kites::assemble(stream));
            for (uint32_t r : {8u, 9u})
                for (u64 i = 0; i < 4; ++i)
                    vm.registers_.WriteVectorElement(r, i, 32, 0x55555555);
            vm.DebugRun();
            for (uint32_t r : {8u, 9u})
                for (u64 i = 0; i < 4; ++i)
                    ASSERT_EQ(vm.registers_.ReadVectorElement(r, i, 32), 0x55555555u);
            EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
        }
}

TEST(VectorSpecRulesTest, VlZeroForCountAndFind)
{
    // vcpop.m counts nothing and vfirst.m finds nothing when vl = 0.
    Kites::setupVmStateDirectory();
    std::istringstream stream("li x6, 0\nvsetvli x5, x6, e8, m1, ta, ma\n"
                              "vcpop.m x10, v16\nvfirst.m x11, v16\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 8, 0xFF);
    vm.registers_.WriteGpr(10, 99);
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadGpr(10), 0u);
    EXPECT_EQ(vm.registers_.ReadGpr(11), ~0ULL);
}

TEST(VectorSpecRulesTest, MaskLogicalAndVidStartAtVstart)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetivli x0, 8, e16, m1, ta, ma\n"
                              "csrrwi x0, vstart, 3\n"
                              "vmxnor.mm v8, v16, v16\n" // all ones from bit 3
                              "csrrwi x0, vstart, 5\n"
                              "vid.v v9\n");             // elements 5..7 only
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    for (u64 i = 0; i < 8; ++i)
        vm.registers_.WriteVectorElement(9, i, 16, 0xAAAA);
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadVectorElement(8, 0, 8), 0xF8u); // bits 3..7
    for (u64 i = 0; i < 8; ++i)
        EXPECT_EQ(vm.registers_.ReadVectorElement(9, i, 16), i < 5 ? 0xAAAAu : i) << i;
    EXPECT_EQ(vm.registers_.ReadCsr(RegisterFile::CSR_VSTART), 0u);
}

TEST(VectorSpecRulesTest, FractionalLmulUsesFewerElements)
{
    // e16, mf2: VLMAX = 128 / 16 / 2 = 4, all held in one register.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetvli x5, x0, e16, mf2, ta, ma\nvadd.vi v8, v16, 1\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    for (u64 i = 0; i < 8; ++i)
    {
        vm.registers_.WriteVectorElement(16, i, 16, i);
        vm.registers_.WriteVectorElement(8, i, 16, 0x7777);
    }
    vm.DebugRun();
    EXPECT_EQ(vm.registers_.ReadGpr(5), 4u);
    for (u64 i = 0; i < 8; ++i)
        EXPECT_EQ(vm.registers_.ReadVectorElement(8, i, 16), i < 4 ? i + 1 : 0x7777u) << i;
}

TEST(VectorSpecRulesTest, IotaAndIdAcrossAnLmulGroup)
{
    // e16, m2: 16 elements in v8-v9; viota counts the set bits of v16 below each element.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetvli x5, x0, e16, m2, ta, ma\n"
                              "viota.m v8, v16\n"
                              "vid.v v12\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 16, 0xA5C3); // bits of elements 0..15
    vm.DebugRun();
    u64 count = 0;
    for (u64 i = 0; i < 16; ++i)
    {
        EXPECT_EQ(vm.registers_.ReadVectorElement(8, i, 16), count) << i;
        count += (0xA5C3 >> i) & 1;
        EXPECT_EQ(vm.registers_.ReadVectorElement(12, i, 16), i) << i;
    }
}

TEST(VectorSpecRulesTest, MaskedSetFirstVariants)
{
    // v16 = 0b00110100, v0 = 0b11011011; active elements 0,1,3,4,6,7.
    // Active vs2 bits: el0 0, el1 0, el3 0, el4 1, el6 0, el7 0 -> first active one is el4.
    Kites::setupVmStateDirectory();
    std::istringstream stream("vsetivli x0, 8, e8, m1, ta, ma\n"
                              "vmsbf.m v8, v16, v0.t\n"
                              "vmsif.m v9, v16, v0.t\n"
                              "vmsof.m v10, v16, v0.t\n");
    Kites::RVSSProcessor vm;
    vm.LoadProgram(Kites::assemble(stream));
    vm.registers_.WriteVectorElement(16, 0, 8, 0x34);
    vm.registers_.WriteVectorElement(0, 0, 8, 0xDB);
    for (uint32_t r : {8u, 9u, 10u})
        vm.registers_.WriteVectorElement(r, 0, 8, 0x24); // inactive bits 2, 5 are 1
    vm.DebugRun();
    // Active results: sbf el0,1,3 = 1, el4,6,7 = 0; sif adds el4; sof only el4.
    EXPECT_EQ(vm.registers_.ReadVectorElement(8, 0, 8), 0x0Bu | 0x24u);
    EXPECT_EQ(vm.registers_.ReadVectorElement(9, 0, 8), 0x1Bu | 0x24u);
    EXPECT_EQ(vm.registers_.ReadVectorElement(10, 0, 8), 0x10u | 0x24u);
}

// parseVectorArith rejects a mnemonic with no rvv::kVectorArith row ("Unsupported vector
// instruction"). This catches the mistake earlier: every mnemonic the syntax map routes there must
// either have a row or be one of the aliases/pseudos the parser rewrites to one.
TEST(VectorSpecRulesTest, EveryArithMnemonicHasADecodeRow)
{
    using Kites::instruction_set::SyntaxType;
    const std::set<SyntaxType> arithSyntaxes = {
        SyntaxType::O_VR_C_VR_C_VR,      SyntaxType::O_VR_C_VR_C_GPR,
        SyntaxType::O_VR_C_VR_C_I,       SyntaxType::O_VR_C_VR,
        SyntaxType::O_VR_C_GPR_C_VR,     SyntaxType::O_VR_C_VR_C_VR_C_V0,
        SyntaxType::O_VR_C_VR_C_GPR_C_V0, SyntaxType::O_VR_C_VR_C_I_C_V0,
        SyntaxType::O_VR_C_GPR,          SyntaxType::O_VR_C_I,
        SyntaxType::O_GPR_C_VR,          SyntaxType::O_VR};
    const std::set<std::string> rewritten = {
        "vmsgt.vv", "vmsgtu.vv", "vmsge.vv", "vmsgeu.vv", "vmsge.vi", "vmsgeu.vi",
        "vmslt.vi", "vmsltu.vi", "vmmv.m",   "vmnot.m",   "vmclr.m",  "vmset.m"};
    size_t checked = 0;
    for (const auto &[mnemonic, syntaxes] : Kites::instruction_set::instruction_syntax_map)
    {
        bool routed = false;
        for (SyntaxType syntax : syntaxes)
        {
            routed = routed || arithSyntaxes.count(syntax) != 0;
        }
        if (!routed)
        {
            continue;
        }
        ++checked;
        EXPECT_TRUE(Kites::rvv::FindVectorArith(mnemonic) != nullptr || rewritten.count(mnemonic))
            << mnemonic << " reaches parseVectorArith but has no kVectorArith row";
    }
    EXPECT_GT(checked, 150u);
}

} // namespace vtest_spec_sweep

// ===== Edge values (riscv-vector-tests operand pairs) ======================================
// Every listed integer instruction at SEW 8/16/32/64 on the edge-value operand pairs, checked
// element by element against the reference model in vector_test_helpers.h.
namespace vtest_edge
{
class VectorEdgeValueRvssTest : public ::testing::TestWithParam<EdgeCase>
{
};

TEST_P(VectorEdgeValueRvssTest, MatchesTheReferenceModel)
{
    CheckEdgeCase<Kites::RVSSProcessor>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(IntegerOps, VectorEdgeValueRvssTest,
                         ::testing::ValuesIn(AllEdgeCases()), EdgeCaseName);
} // namespace vtest_edge

// ===== Spec examples ======================================================================
namespace vtest_spec_examples
{
TEST(VectorSpecExampleRvssTest, ProgramsComputeTheExpectedResults)
{
    for (const SpecProgram &program : SpecPrograms())
    {
        SCOPED_TRACE(program.name);
        RunSpecProgram<Kites::RVSSProcessor>(program);
    }
}
} // namespace vtest_spec_examples

// ===== Custom: masking, tail and vl edge cases ==============================================
namespace vtest_custom
{
namespace
{
std::unique_ptr<Kites::RVSSProcessor> RunRvss(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    auto vm = std::make_unique<Kites::RVSSProcessor>();
    vm->LoadProgram(Kites::assemble(stream));
    vm->DebugRun();
    return vm;
}

uint64_t Element(Kites::ProcessorBase &vm, size_t reg, size_t index, size_t sew)
{
    return vm.registers_.ReadVectorElement(reg, index, sew);
}
} // namespace

TEST(VectorCustomRvssTest, MaskedOffAndTailElementsStayUndisturbed)
{
    // v0 = 0b0101: elements 0 and 2 active; vl = 3 of 4, so element 3 is tail.
    auto vm = RunRvss("    vsetivli t0, 4, e32, m1, tu, mu\n"
                      "    vmv.v.i v2, 7\n"
                      "    vmv.v.i v3, 1\n"
                      "    vid.v v4\n"
                      "    vmseq.vi v0, v4, 0\n"
                      "    vmseq.vi v5, v4, 2\n"
                      "    vmor.mm v0, v0, v5\n"
                      "    vsetivli t0, 3, e32, m1, tu, mu\n"
                      "    vadd.vv v2, v3, v3, v0.t\n");
    EXPECT_EQ(Element(*vm, 2, 0, 32), 2u);
    EXPECT_EQ(Element(*vm, 2, 1, 32), 7u) << "masked off";
    EXPECT_EQ(Element(*vm, 2, 2, 32), 2u);
    EXPECT_EQ(Element(*vm, 2, 3, 32), 7u) << "tail";
}

TEST(VectorCustomRvssTest, VlZeroWritesNothing)
{
    auto vm = RunRvss("    vsetivli t0, 4, e32, m1, tu, mu\n"
                      "    vmv.v.i v2, 9\n"
                      "    vsetivli t0, 0, e32, m1, tu, mu\n"
                      "    vadd.vi v2, v2, 1\n");
    EXPECT_EQ(vm->registers_.ReadGpr(5), 0u);
    for (size_t i = 0; i < 4; ++i)
        EXPECT_EQ(Element(*vm, 2, i, 32), 9u);
}

TEST(VectorCustomRvssTest, AvlAboveVlmaxIsClampedAcrossLmul)
{
    // VLEN = 128: VLMAX = LMUL * 128 / SEW.
    auto vm = RunRvss("    li t1, 1000\n"
                      "    vsetvli a0, t1, e8, m1, ta, ma\n"
                      "    vsetvli a1, t1, e16, m4, ta, ma\n"
                      "    vsetvli a2, t1, e64, m8, ta, ma\n"
                      "    vsetvli a3, t1, e32, mf2, ta, ma\n");
    EXPECT_EQ(vm->registers_.ReadGpr(10), 16u);
    EXPECT_EQ(vm->registers_.ReadGpr(11), 32u);
    EXPECT_EQ(vm->registers_.ReadGpr(12), 16u);
    EXPECT_EQ(vm->registers_.ReadGpr(13), 2u);
}

TEST(VectorCustomRvssTest, SignedDivisionOverflowAndDivideByZeroAtEverySew)
{
    for (unsigned sew : {8u, 16u, 32u, 64u})
    {
        SCOPED_TRACE(sew);
        const std::string e = "e" + std::to_string(sew);
        auto vm = RunRvss("    vsetivli t0, 2, " + e + ", m1, ta, ma\n"
                          "    li t1, 1\n"
                          "    slli t1, t1, " + std::to_string(sew - 1) + "\n" // signed minimum
                          "    vmv.v.x v1, t1\n"
                          "    vmv.v.i v2, -1\n"
                          "    vmv.v.i v3, 0\n"
                          "    vdiv.vv v4, v1, v2\n"  // overflow: minimum
                          "    vrem.vv v5, v1, v2\n"  // overflow: 0
                          "    vdivu.vv v6, v1, v3\n" // / 0: all ones
                          "    vremu.vv v7, v1, v3\n"); // % 0: the dividend
        const uint64_t min = 1ULL << (sew - 1);
        const uint64_t ones = sew == 64 ? ~0ULL : (1ULL << sew) - 1;
        EXPECT_EQ(Element(*vm, 4, 1, sew), min);
        EXPECT_EQ(Element(*vm, 5, 1, sew), 0u);
        EXPECT_EQ(Element(*vm, 6, 1, sew), ones);
        EXPECT_EQ(Element(*vm, 7, 1, sew), min);
    }
}
} // namespace vtest_custom
