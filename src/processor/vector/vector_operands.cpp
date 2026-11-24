/**
 * @file vector_operands.cpp
 * @brief Register-use decoding of vector instructions, for pipeline forwarding and hazards.
 */

#include "processor/vector/vector_operands.h"

#include "processor/vector/vector_config.h"
#include "processor/vector/vector_memory.h"

#include <algorithm>

namespace Kites::rvv
{
namespace
{
constexpr uint32_t kMaxGroup = 8;

uint32_t Field(uint32_t instruction, int lsb)
{
    return (instruction >> lsb) & 0x1F;
}

/// log2 of a power of two between 8 and 64.
int Log2(uint64_t value)
{
    int log = 0;
    while ((1ULL << log) < value)
    {
        ++log;
    }
    return log;
}

/// Registers in a group of EMUL = 2^emulLog2 (at least one, at most eight).
uint32_t Regs(int emulLog2)
{
    if (emulLog2 <= 0)
    {
        return 1;
    }
    return std::min<uint32_t>(1u << emulLog2, kMaxGroup);
}

void SetWrite(VectorRegisterUse &use, uint32_t vd, uint32_t count)
{
    count = std::min<uint32_t>(count, 32 - vd);
    use.writeBase = static_cast<uint8_t>(vd);
    use.writeCount = static_cast<uint8_t>(count);
}

void DecodeArith(uint32_t instruction, const Vtype &vtype, VectorRegisterUse &use)
{
    use.readsVlInEx = true;
    const VectorArithInfo *info = DecodeVectorArith(instruction);
    if (info == nullptr)
    {
        use.exReads = ~0u; // traps in EX anyway; read everything to stay safe
        return;
    }
    const uint32_t vd = Field(instruction, 7);
    const uint32_t vs1 = Field(instruction, 15);
    const uint32_t vs2 = Field(instruction, 20);
    const bool vm = (instruction >> 25) & 1;
    const uint32_t funct3 = (instruction >> 12) & 0x7;

    const int g = vtype.lmulLog2 > 0 ? vtype.lmulLog2 : 0; // log2 of the group size
    const uint32_t G = Regs(g);
    const uint32_t G2 = Regs(g + 1);

    use.readsRs1 = funct3 == FUNCT3_OPIVX || funct3 == FUNCT3_OPMVX;
    const bool vs1IsRegister = (funct3 == FUNCT3_OPIVV || funct3 == FUNCT3_OPMVV) &&
                               info->vs1Field < 0 && info->op != VectorArithOp::WholeMove;

    uint32_t vs1Regs = 0;
    uint32_t vs2Regs = 0;
    uint32_t vdRegs = 0;

    switch (info->shape)
    {
    case VectorOpShape::SingleWidth:
        vs2Regs = G, vs1Regs = G, vdRegs = G;
        break;
    case VectorOpShape::Widening:
        vs2Regs = G, vs1Regs = G, vdRegs = G2;
        break;
    case VectorOpShape::WideningW:
        vs2Regs = G2, vs1Regs = G, vdRegs = G2;
        break;
    case VectorOpShape::Extension:
        vs2Regs = G, vdRegs = G;
        break;
    case VectorOpShape::Narrowing:
        vs2Regs = G2, vs1Regs = G, vdRegs = G;
        break;
    case VectorOpShape::Compare:
        vs2Regs = G, vs1Regs = G, vdRegs = 1;
        break;
    case VectorOpShape::Reduction:
    case VectorOpShape::WideningReduction:
        vs2Regs = G, vs1Regs = 1, vdRegs = 1;
        break;
    case VectorOpShape::Permute:
        switch (info->op)
        {
        case VectorArithOp::MoveXS:
            vs2Regs = 1;
            break;
        case VectorArithOp::MoveSX:
            vdRegs = 1;
            break;
        case VectorArithOp::GatherEi16:
            vs2Regs = G, vs1Regs = G2, vdRegs = G;
            break;
        case VectorArithOp::Compress:
            vs2Regs = G, vs1Regs = 1, vdRegs = G;
            break;
        case VectorArithOp::WholeMove:
        {
            const uint32_t nreg = std::min<uint32_t>(vs1 + 1, kMaxGroup);
            vs2Regs = nreg, vdRegs = nreg;
            break;
        }
        default: // slides, vrgather
            vs2Regs = G, vs1Regs = G, vdRegs = G;
            break;
        }
        break;
    case VectorOpShape::Mask:
        switch (info->op)
        {
        case VectorArithOp::Cpop:
        case VectorArithOp::First:
            vs2Regs = 1;
            break;
        case VectorArithOp::Iota:
            vs2Regs = 1, vdRegs = G;
            break;
        case VectorArithOp::Id:
            vdRegs = G;
            break;
        default: // mask logical ops, vmsbf/vmsif/vmsof
            vs2Regs = 1, vs1Regs = 1, vdRegs = 1;
            break;
        }
        break;
    }

    uint32_t reads = VectorGroupMask(vs2, vs2Regs);
    if (vs1IsRegister)
    {
        reads |= VectorGroupMask(vs1, vs1Regs);
    }
    if (vdRegs > 0)
    {
        SetWrite(use, vd, vdRegs);
        reads |= VectorGroupMask(vd, vdRegs); // undisturbed elements keep the old vd
    }
    if (info->vmField == 0 || (!vm && info->vmField < 0))
    {
        reads |= 1u; // v0: the mask, or the data of vmerge/vadc
    }
    use.exReads = reads;
}

void DecodeMemory(uint32_t instruction, const Vtype &vtype, bool vtypeValid,
                  VectorRegisterUse &use)
{
    use.isMemory = true;
    use.readsRs1 = true; // base address
    const uint32_t vd = Field(instruction, 7);
    VectorMemFields fields;
    if (!DecodeVectorMem(instruction, fields))
    {
        // A reserved encoding traps in MEM. Assume the widest possible load.
        use.isStore = ((instruction & 0x7F) == OPCODE_STORE_FP);
        use.isLoad = !use.isStore;
        use.readsRs2 = true;
        if (use.isLoad)
        {
            SetWrite(use, vd, kMaxGroup);
        }
        return;
    }
    use.isStore = fields.isStore;
    use.isLoad = !fields.isStore;
    use.isFaultOnlyFirst = fields.mode == VectorMemMode::FaultOnlyFirst;
    use.writesVlVtype = use.isFaultOnlyFirst;
    use.readsRs2 = fields.mode == VectorMemMode::Strided;
    if (!use.isLoad)
    {
        return;
    }

    uint32_t regs = kMaxGroup;
    switch (fields.mode)
    {
    case VectorMemMode::Mask:
        regs = 1;
        break;
    case VectorMemMode::WholeRegister:
        regs = fields.nfields;
        break;
    case VectorMemMode::IndexedOrdered:
    case VectorMemMode::IndexedUnordered:
        if (vtypeValid) // data elements are SEW wide: EMUL = LMUL
        {
            regs = Regs(vtype.lmulLog2) * fields.nfields;
        }
        break;
    default: // unit-stride, fault-only-first, strided: EMUL = EEW / SEW * LMUL
        if (vtypeValid)
        {
            const int emulLog2 =
                vtype.lmulLog2 + Log2(fields.eew) - Log2(vtype.sew);
            regs = Regs(emulLog2) * fields.nfields;
        }
        break;
    }
    SetWrite(use, vd, std::min<uint32_t>(regs, kMaxGroup));
}
} // namespace

uint32_t VectorGroupMask(uint32_t base, uint32_t count)
{
    uint32_t mask = 0;
    for (uint32_t r = base; r < base + count && r < 32; ++r)
    {
        mask |= 1u << r;
    }
    return mask;
}

VectorRegisterUse DecodeVectorRegisterUse(uint32_t instruction, uint64_t vtypeValue)
{
    VectorRegisterUse use;
    Vtype vtype;
    const bool vtypeValid = DecodeVtype(vtypeValue, vtype);
    if (!vtypeValid)
    {
        // vill: anything but vset* traps. Assume the largest groups so nothing is missed.
        vtype.sew = 8;
        vtype.lmulLog2 = 3;
    }

    if (IsVectorMemoryInstruction(instruction))
    {
        DecodeMemory(instruction, vtype, vtypeValid, use);
        return use;
    }

    const VsetKind vset = DecodeVsetKind(instruction);
    if (vset != VsetKind::None)
    {
        use.isVset = true;
        use.readsVlInEx = true; // vsetvli x0, x0 keeps vl
        use.writesVlVtype = true;
        use.readsRs1 = vset != VsetKind::Vsetivli;
        use.readsRs2 = vset == VsetKind::Vsetvl;
        return use;
    }

    DecodeArith(instruction, vtype, use);
    return use;
}

} // namespace Kites::rvv
