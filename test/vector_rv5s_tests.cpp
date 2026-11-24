// Vector extension tests on the 5-stage pipelines: every instruction group on all four rv5s
// variants against RVSS, scalar/vector dependencies, and the pipelined vector unit of H_F
// (pipeline registers, vector forwarding, hdu_vector, CSR writes in WB, the datapath view).

#include "vector_test_helpers.h"

// ===== vset_test =============================================================================
// Tests for the RVV configuration-setting instructions: vsetvli, vsetivli and vsetvl.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding);
// the vl/vtype rules follow RVV 1.0 section 6 and Spike's set_vl().
namespace vtest_vset
{

TEST(VsetPipelineTest, SpacedProgramOnEveryVariant)
{
    expectSpacedProgramResult<Kites::RVSSProcessor>();
    expectSpacedProgramResult<Kites::RV5StageProcessorHF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorHNF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorNHF>();
    expectSpacedProgramResult<Kites::RV5StageProcessorNHNF>();
}

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

} // namespace vtest_vset

// ===== vector_int_arith_test =================================================================
// Tests for the RVV single-width integer add/subtract instructions:
// vadd.vv/.vx/.vi, vsub.vv/.vx, vrsub.vx/.vi.
namespace vtest_int_arith
{

TEST(VectorIntArithPipelineTest, ChainOnEveryVariant)
{
    expectChainResult<Kites::RVSSProcessor>();
    expectChainResult<Kites::RV5StageProcessorHF>();
    expectChainResult<Kites::RV5StageProcessorHNF>();
    expectChainResult<Kites::RV5StageProcessorNHF>();
    expectChainResult<Kites::RV5StageProcessorNHNF>();
}

TEST(VectorIntArithPipelineTest, ScalarOperandIsForwarded)
{
    const char *program = "vsetivli x0, 4, e32, m1, ta, ma\n"
                          "li x11, 100\n"
                          "vadd.vx v7, v0, x11\n";
    for (const auto &vm : {std::unique_ptr<Kites::ProcessorBase>(
                               runOn<Kites::RV5StageProcessorHF>(program)),
                           std::unique_ptr<Kites::ProcessorBase>(
                               runOn<Kites::RV5StageProcessorHNF>(program))})
    {
        EXPECT_EQ(readElements(vm->registers_, 7, 32, 4), Elements(4, 100));
    }
}

TEST(VectorIntArithPipelineTest, UndoToStartClearsVectorResults)
{
    auto vm = runOn<Kites::RV5StageProcessorHF>(kChainProgram);
    for (uint64_t cycles = vm->cycle_s_; cycles > 0; --cycles)
    {
        vm->Undo();
    }
    for (size_t reg = 1; reg <= 6; ++reg)
    {
        EXPECT_EQ(readElements(vm->registers_, reg, 32, 4), Elements(4, 0)) << "v" << reg;
    }
}

} // namespace vtest_int_arith

// ===== vector_carry_test =====================================================================
// Tests for RVV add-with-carry / subtract-with-borrow (11.4): vadc, vsbc, vmadc, vmsbc.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: v0 supplies the carry/borrow-in, every body element is written, vadc/vsbc
// cannot write v0 and are reserved with vm = 1, and vmadc/vmsbc write a mask (even into v0).
namespace vtest_carry
{

TEST(VectorCarryPipelineTest, ProgramOnEveryVariant)
{
    expectCarryResult<Kites::RVSSProcessor>();
    expectCarryResult<Kites::RV5StageProcessorHF>();
    expectCarryResult<Kites::RV5StageProcessorHNF>();
    expectCarryResult<Kites::RV5StageProcessorNHF>();
    expectCarryResult<Kites::RV5StageProcessorNHNF>();
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

TEST(VectorComparePipelineTest, CompareProgramOnEveryVariant)
{
    expectCompareResult<Kites::RVSSProcessor>();
    expectCompareResult<Kites::RV5StageProcessorHF>();
    expectCompareResult<Kites::RV5StageProcessorHNF>();
    expectCompareResult<Kites::RV5StageProcessorNHF>();
    expectCompareResult<Kites::RV5StageProcessorNHNF>();
}

} // namespace vtest_compare

// ===== vector_shift_test =====================================================================
// Tests for the RVV single-width bit shifts (11.6): vsll, vsrl, vsra in .vv/.vx/.vi forms.
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
// Rules under test: only the low log2(SEW) bits of the shift amount are used, .vi takes an
// unsigned 5-bit immediate, and vsra fills with the sign bit.
namespace vtest_shift
{

TEST(VectorShiftPipelineTest, ShiftProgramOnEveryVariant)
{
    expectShiftResult<Kites::RVSSProcessor>();
    expectShiftResult<Kites::RV5StageProcessorHF>();
    expectShiftResult<Kites::RV5StageProcessorHNF>();
    expectShiftResult<Kites::RV5StageProcessorNHF>();
    expectShiftResult<Kites::RV5StageProcessorNHNF>();
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

TEST(VectorNarrowShiftPipelineTest, NarrowProgramOnEveryVariant)
{
    expectNarrowResult<Kites::RVSSProcessor>();
    expectNarrowResult<Kites::RV5StageProcessorHF>();
    expectNarrowResult<Kites::RV5StageProcessorHNF>();
    expectNarrowResult<Kites::RV5StageProcessorNHF>();
    expectNarrowResult<Kites::RV5StageProcessorNHNF>();
}

} // namespace vtest_narrow_shift

// ===== vector_widen_ext_logic_test ===========================================================
// Tests for RVV widening integer add/subtract (11.2), integer extension (11.3) and bitwise
// logical (11.5) instructions. Encodings are checked against llvm-mc
// (-triple=riscv64 -mattr=+zve64x -show-encoding); legality checks follow Spike.
namespace vtest_widen_ext_logic
{

TEST(VectorWidenExtLogicPipelineTest, MixedProgramOnEveryVariant)
{
    expectMixedResult<Kites::RVSSProcessor>();
    expectMixedResult<Kites::RV5StageProcessorHF>();
    expectMixedResult<Kites::RV5StageProcessorHNF>();
    expectMixedResult<Kites::RV5StageProcessorNHF>();
    expectMixedResult<Kites::RV5StageProcessorNHNF>();
}

} // namespace vtest_widen_ext_logic

// ===== vector_mul_div_test ===================================================================
// Tests for RVV integer min/max (11.9), multiply (11.10), divide (11.11), widening multiply
// (11.12), multiply-add (11.13), widening multiply-add (11.14), merge (11.15) and move (11.16).
// Encodings are checked against llvm-mc (-triple=riscv64 -mattr=+zve64x -show-encoding).
namespace vtest_mul_div
{

TEST(VectorMulDivPipelineTest, ProgramOnEveryVariant)
{
    expectMulResult<Kites::RVSSProcessor>();
    expectMulResult<Kites::RV5StageProcessorHF>();
    expectMulResult<Kites::RV5StageProcessorHNF>();
    expectMulResult<Kites::RV5StageProcessorNHF>();
    expectMulResult<Kites::RV5StageProcessorNHNF>();
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

TEST(VectorReductionPipelineTest, ProgramOnEveryVariant)
{
    expectReductionResult<Kites::RVSSProcessor>();
    expectReductionResult<Kites::RV5StageProcessorHF>();
    expectReductionResult<Kites::RV5StageProcessorHNF>();
    expectReductionResult<Kites::RV5StageProcessorNHF>();
    expectReductionResult<Kites::RV5StageProcessorNHNF>();
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

TEST(VectorMaskPipelineTest, ProgramOnEveryVariant)
{
    expectMaskResult<Kites::RVSSProcessor>("RVSSProcessor");
    expectMaskResult<Kites::RV5StageProcessorHF>("RV5StageProcessorHF");
    expectMaskResult<Kites::RV5StageProcessorHNF>("RV5StageProcessorHNF");
    expectMaskResult<Kites::RV5StageProcessorNHF>("RV5StageProcessorNHF");
    expectMaskResult<Kites::RV5StageProcessorNHNF>("RV5StageProcessorNHNF");
}

TEST(VectorMaskPipelineTest, RdIsForwardedOrStalledFor)
{
    expectBackToBackUse<Kites::RVSSProcessor>("RVSSProcessor");
    expectBackToBackUse<Kites::RV5StageProcessorHF>("RV5StageProcessorHF");
    expectBackToBackUse<Kites::RV5StageProcessorHNF>("RV5StageProcessorHNF");
    expectBackToBackUse<Kites::RV5StageProcessorNHF>("RV5StageProcessorNHF");
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

TEST(VectorPermutePipelineTest, ProgramOnEveryVariant)
{
    expectPermuteResult<Kites::RVSSProcessor>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorHF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorHNF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorNHF>(kPermuteProgram);
    expectPermuteResult<Kites::RV5StageProcessorNHNF>(kPermuteProgramScheduled);
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

} // namespace vtest_load_store

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

} // namespace vtest_rv5s_vector_pipeline

// ===== Edge values (riscv-vector-tests operand pairs) on the pipelines =======================
// The same cases as VectorEdgeValueRvssTest, on the two hazard-detecting pipelines (the no-hazard
// variants need hand-spaced programs, which these are not).
namespace vtest_edge
{
class VectorEdgeValueRv5sTest : public ::testing::TestWithParam<EdgeCase>
{
};

TEST_P(VectorEdgeValueRv5sTest, HfMatchesTheReferenceModel)
{
    CheckEdgeCase<Kites::RV5StageProcessorHF>(GetParam());
}

TEST_P(VectorEdgeValueRv5sTest, HnfMatchesTheReferenceModel)
{
    CheckEdgeCase<Kites::RV5StageProcessorHNF>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(IntegerOps, VectorEdgeValueRv5sTest,
                         ::testing::ValuesIn(AllEdgeCases()), EdgeCaseName);
} // namespace vtest_edge

// ===== Spec examples on the pipelines =========================================================
namespace vtest_spec_examples
{
TEST(VectorSpecExampleRv5sTest, ProgramsComputeTheExpectedResultsOnHf)
{
    for (const SpecProgram &program : SpecPrograms())
    {
        SCOPED_TRACE(program.name);
        RunSpecProgram<Kites::RV5StageProcessorHF>(program);
    }
}

TEST(VectorSpecExampleRv5sTest, ProgramsComputeTheExpectedResultsOnHnf)
{
    for (const SpecProgram &program : SpecPrograms())
    {
        SCOPED_TRACE(program.name);
        RunSpecProgram<Kites::RV5StageProcessorHNF>(program);
    }
}
} // namespace vtest_spec_examples

// ===== Custom: random dependent programs, pipelines against RVSS ==============================
namespace vtest_random
{
TEST(VectorRandomProgramRv5sTest, HazardDetectingPipelinesMatchRvss)
{
    for (uint32_t seed = 1; seed <= 25; ++seed)
    {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const std::string source = RandomProgram(seed, 60);
        const MachineState reference = RunRandom<Kites::RVSSProcessor>(source);
        ExpectSameMachineState(reference, RunRandom<Kites::RV5StageProcessorHF>(source), source);
        ExpectSameMachineState(reference, RunRandom<Kites::RV5StageProcessorHNF>(source), source);
        if (HasFailure())
            break;
    }
}
} // namespace vtest_random
