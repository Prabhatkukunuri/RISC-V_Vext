/**
 * @file vector_memory.cpp
 * @brief RVV 1.0 vector loads and stores (spec section 7).
 *
 * Policies follow Spike, like the arithmetic: tail and masked-off elements are left undisturbed,
 * and elements are accessed in element order (which also satisfies the ordered-indexed forms).
 */

#include "processor/vector/vector_memory.h"

#include "processor/memory_controller.h"
#include "processor/processor_base.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_internal.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace Kites::rvv
{
using namespace detail;

namespace
{
constexpr uint32_t MOP_UNIT_STRIDE = 0b00;
constexpr uint32_t MOP_INDEXED_UNORDERED = 0b01;
constexpr uint32_t MOP_STRIDED = 0b10;
constexpr uint32_t MOP_INDEXED_ORDERED = 0b11;

// lumop / sumop values (bits 24:20) of the unit-stride forms.
constexpr uint32_t UMOP_UNIT_STRIDE = 0b00000;
constexpr uint32_t UMOP_WHOLE_REGISTER = 0b01000;
constexpr uint32_t UMOP_MASK = 0b01011;
constexpr uint32_t UMOP_FAULT_ONLY_FIRST = 0b10000;

constexpr std::array<uint32_t, 4> kEews = {8, 16, 32, 64};

/// The width field for an EEW; mew (bit 28) is always 0 since EEW <= 64.
uint32_t WidthBits(uint32_t eew)
{
    switch (eew)
    {
    case 8:
        return 0b000;
    case 16:
        return 0b101;
    case 32:
        return 0b110;
    default:
        return 0b111;
    }
}

/// The EEW for a width field, or 0 if the width is not a vector one.
uint32_t EewFromWidth(uint32_t width)
{
    switch (width)
    {
    case 0b000:
        return 8;
    case 0b101:
        return 16;
    case 0b110:
        return 32;
    case 0b111:
        return 64;
    default:
        return 0;
    }
}

bool IsIndexed(VectorMemMode mode)
{
    return mode == VectorMemMode::IndexedUnordered || mode == VectorMemMode::IndexedOrdered;
}

int Log2(uint64_t value)
{
    int result = 0;
    while ((uint64_t{1} << result) < value)
    {
        ++result;
    }
    return result;
}

std::string Hex(uint64_t value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

struct MnemonicTable
{
    std::unordered_map<std::string, VectorMemFields> byName;
    std::vector<std::string> names;

    void Add(const VectorMemFields &fields)
    {
        Add(VectorMemMnemonic(fields), fields);
    }

    void Add(const std::string &name, const VectorMemFields &fields)
    {
        byName.emplace(name, fields);
        names.push_back(name);
    }
};

const MnemonicTable &Table()
{
    static const MnemonicTable table = []
    {
        MnemonicTable t;
        for (bool isStore : {false, true})
        {
            for (uint32_t eew : kEews)
            {
                // Unit-stride, strided and indexed, each with NFIELDS = 1 (plain) .. 8 (segment).
                for (uint32_t nfields = 1; nfields <= 8; ++nfields)
                {
                    for (VectorMemMode mode :
                         {VectorMemMode::UnitStride, VectorMemMode::FaultOnlyFirst,
                          VectorMemMode::Strided, VectorMemMode::IndexedUnordered,
                          VectorMemMode::IndexedOrdered})
                    {
                        if (isStore && mode == VectorMemMode::FaultOnlyFirst)
                        {
                            continue;
                        }
                        t.Add({isStore, mode, eew, nfields});
                    }
                }
                // Whole-register loads take any EEW; whole-register stores only EEW = 8.
                if (!isStore || eew == 8)
                {
                    for (uint32_t nfields : {1u, 2u, 4u, 8u})
                    {
                        t.Add({isStore, VectorMemMode::WholeRegister, eew, nfields});
                    }
                }
            }
            t.Add({isStore, VectorMemMode::Mask, 8, 1});
        }
        // Aliases: vl<nf>r.v for vl<nf>re8.v, and the pre-1.0 vle1.v/vse1.v that the spec keeps
        // for vlm.v/vsm.v.
        for (uint32_t nfields : {1u, 2u, 4u, 8u})
        {
            t.Add("vl" + std::to_string(nfields) + "r.v",
                  {false, VectorMemMode::WholeRegister, 8, nfields});
        }
        t.Add("vle1.v", {false, VectorMemMode::Mask, 8, 1});
        t.Add("vse1.v", {true, VectorMemMode::Mask, 8, 1});
        return t;
    }();
    return table;
}

// ---------------------------------------------------------------------------------------------
// Element access
// ---------------------------------------------------------------------------------------------

/// Raised internally by the element accessors so callers can tell a memory fault apart from an
/// illegal instruction.
struct AccessFault
{
    uint64_t address;
    std::string reason;
};

uint64_t LoadElement(MemoryController &memory, uint64_t address, uint32_t eew)
{
    try
    {
        switch (eew)
        {
        case 8:
            return memory.readByte(address);
        case 16:
            return memory.readHalfWord(address);
        case 32:
            return memory.readWord(address);
        default:
            return memory.readDoubleWord(address);
        }
    }
    catch (const std::out_of_range &error)
    {
        throw AccessFault{address, error.what()};
    }
}

void StoreElement(MemoryController &memory, uint64_t address, uint32_t eew, uint64_t value,
                  std::vector<MemoryChange> &memoryChanges)
{
    const uint32_t bytes = eew / 8;
    try
    {
        // Undo bookkeeping reads use peekByte so they don't show up as cache accesses. Peeking
        // first also means a faulting element writes nothing.
        std::vector<uint8_t> before(bytes);
        for (uint32_t b = 0; b < bytes; ++b)
        {
            before[b] = memory.peekByte(address + b);
        }
        switch (eew)
        {
        case 8:
            memory.writeByte(address, static_cast<uint8_t>(value));
            break;
        case 16:
            memory.writeHalfWord(address, static_cast<uint16_t>(value));
            break;
        case 32:
            memory.writeWord(address, static_cast<uint32_t>(value));
            break;
        default:
            memory.writeDoubleWord(address, value);
            break;
        }
        std::vector<uint8_t> after(bytes);
        for (uint32_t b = 0; b < bytes; ++b)
        {
            after[b] = memory.peekByte(address + b);
        }
        if (before != after)
        {
            memoryChanges.push_back({address, std::move(before), std::move(after)});
        }
    }
    catch (const std::out_of_range &error)
    {
        throw AccessFault{address, error.what()};
    }
}

[[noreturn]] void ThrowAccessFault(uint32_t instruction, bool isStore, uint64_t element,
                                   const AccessFault &fault)
{
    char word[16];
    snprintf(word, sizeof(word), "0x%08x", instruction);
    throw std::runtime_error(std::string(isStore ? "Store" : "Load") + " access fault in " +
                             word + " at element " + std::to_string(element) + ", address " +
                             Hex(fault.address) + ": " + fault.reason);
}

/**
 * @brief Checks the RVV 1.0 section 5.2 rule for a load whose destination group (data EEW)
 * overlaps its index group (index EEW).
 */
void CheckIndexOverlap(uint32_t instruction, uint32_t vd, size_t destRegs, uint32_t dataEew,
                       uint32_t vs2, size_t indexRegs, uint32_t indexEew, int indexEmulLog2,
                       uint32_t nfields)
{
    const bool overlap = vd < vs2 + indexRegs && vs2 < vd + destRegs;
    if (!overlap)
    {
        return;
    }
    bool allowed = false;
    if (nfields > 1)
    {
        allowed = false; // segment loads: the destination may never overlap the index
    }
    else if (dataEew == indexEew)
    {
        allowed = true;
    }
    else if (dataEew < indexEew)
    {
        allowed = vd == vs2; // in the lowest-numbered part of the index group
    }
    else
    {
        // In the highest-numbered part of the destination group, from an index EMUL >= 1.
        allowed = indexEmulLog2 >= 0 && vs2 + indexRegs == vd + destRegs;
    }
    if (!allowed)
    {
        ThrowIllegal(instruction, "destination v" + std::to_string(vd) +
                                      " overlaps the index register group v" +
                                      std::to_string(vs2));
    }
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------------------------

bool IsVectorMemoryInstruction(uint32_t instruction)
{
    const uint32_t opcode = instruction & 0x7F;
    return (opcode == OPCODE_LOAD_FP || opcode == OPCODE_STORE_FP) &&
           EewFromWidth((instruction >> 12) & 0x7) != 0;
}

bool DecodeVectorMem(uint32_t instruction, VectorMemFields &out)
{
    if (!IsVectorMemoryInstruction(instruction))
    {
        return false;
    }
    if ((instruction >> 28) & 1)
    {
        return false; // mew = 1: EEW > 64, reserved
    }

    VectorMemFields fields;
    fields.isStore = (instruction & 0x7F) == OPCODE_STORE_FP;
    fields.eew = EewFromWidth((instruction >> 12) & 0x7);
    fields.nfields = ((instruction >> 29) & 0x7) + 1;
    const bool masked = ((instruction >> 25) & 1) == 0;
    const uint32_t umop = (instruction >> 20) & 0x1F;

    switch ((instruction >> 26) & 0x3)
    {
    case MOP_UNIT_STRIDE:
        switch (umop)
        {
        case UMOP_UNIT_STRIDE:
            fields.mode = VectorMemMode::UnitStride;
            break;
        case UMOP_FAULT_ONLY_FIRST:
            if (fields.isStore)
            {
                return false;
            }
            fields.mode = VectorMemMode::FaultOnlyFirst;
            break;
        case UMOP_WHOLE_REGISTER:
        {
            const uint32_t n = fields.nfields;
            if (masked || !(n == 1 || n == 2 || n == 4 || n == 8) ||
                (fields.isStore && fields.eew != 8))
            {
                return false;
            }
            fields.mode = VectorMemMode::WholeRegister;
            break;
        }
        case UMOP_MASK:
            if (masked || fields.eew != 8 || fields.nfields != 1)
            {
                return false;
            }
            fields.mode = VectorMemMode::Mask;
            break;
        default:
            return false;
        }
        break;
    case MOP_INDEXED_UNORDERED:
        fields.mode = VectorMemMode::IndexedUnordered;
        break;
    case MOP_STRIDED:
        fields.mode = VectorMemMode::Strided;
        break;
    case MOP_INDEXED_ORDERED:
        fields.mode = VectorMemMode::IndexedOrdered;
        break;
    }
    out = fields;
    return true;
}

uint32_t EncodeVectorMem(const VectorMemFields &fields, uint32_t vd, uint32_t rs1,
                         uint32_t field24_20, bool masked)
{
    uint32_t mop = MOP_UNIT_STRIDE;
    uint32_t field = field24_20 & 0x1F;
    switch (fields.mode)
    {
    case VectorMemMode::UnitStride:
        field = UMOP_UNIT_STRIDE;
        break;
    case VectorMemMode::FaultOnlyFirst:
        field = UMOP_FAULT_ONLY_FIRST;
        break;
    case VectorMemMode::Mask:
        field = UMOP_MASK;
        break;
    case VectorMemMode::WholeRegister:
        field = UMOP_WHOLE_REGISTER;
        break;
    case VectorMemMode::Strided:
        mop = MOP_STRIDED;
        break;
    case VectorMemMode::IndexedUnordered:
        mop = MOP_INDEXED_UNORDERED;
        break;
    case VectorMemMode::IndexedOrdered:
        mop = MOP_INDEXED_ORDERED;
        break;
    }
    const uint32_t opcode = fields.isStore ? OPCODE_STORE_FP : OPCODE_LOAD_FP;
    return ((fields.nfields - 1) & 0x7) << 29 | mop << 26 | (masked ? 0u : 1u) << 25 |
           field << 20 | (rs1 & 0x1F) << 15 | WidthBits(fields.eew) << 12 | (vd & 0x1F) << 7 |
           opcode;
}

std::string VectorMemMnemonic(const VectorMemFields &fields)
{
    const std::string ls = fields.isStore ? "s" : "l";
    const std::string eew = std::to_string(fields.eew);
    const std::string seg = fields.nfields > 1 ? "seg" + std::to_string(fields.nfields) : "";
    switch (fields.mode)
    {
    case VectorMemMode::UnitStride:
        return "v" + ls + seg + "e" + eew + ".v";
    case VectorMemMode::FaultOnlyFirst:
        return "v" + ls + seg + "e" + eew + "ff.v";
    case VectorMemMode::Mask:
        return "v" + ls + "m.v";
    case VectorMemMode::WholeRegister:
        return fields.isStore ? "vs" + std::to_string(fields.nfields) + "r.v"
                              : "vl" + std::to_string(fields.nfields) + "re" + eew + ".v";
    case VectorMemMode::Strided:
        return "v" + ls + "s" + seg + "e" + eew + ".v";
    case VectorMemMode::IndexedUnordered:
        return "v" + ls + "ux" + seg + "ei" + eew + ".v";
    case VectorMemMode::IndexedOrdered:
        return "v" + ls + "ox" + seg + "ei" + eew + ".v";
    }
    return "";
}

const VectorMemFields *LookupVectorMemMnemonic(const std::string &mnemonic)
{
    const auto &byName = Table().byName;
    const auto it = byName.find(mnemonic);
    return it == byName.end() ? nullptr : &it->second;
}

const std::vector<std::string> &VectorMemMnemonics()
{
    return Table().names;
}

std::string DisassembleVectorMem(uint32_t instruction)
{
    VectorMemFields fields;
    if (!DecodeVectorMem(instruction, fields))
    {
        return "";
    }
    const uint32_t vd = (instruction >> 7) & 0x1F;
    const uint32_t rs1 = (instruction >> 15) & 0x1F;
    const uint32_t field24_20 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;

    std::string text = VectorMemMnemonic(fields) + " v" + std::to_string(vd) + ", (x" +
                       std::to_string(rs1) + ")";
    if (fields.mode == VectorMemMode::Strided)
    {
        text += ", x" + std::to_string(field24_20);
    }
    else if (IsIndexed(fields.mode))
    {
        text += ", v" + std::to_string(field24_20);
    }
    if (masked)
    {
        text += ", v0.t";
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

void ExecuteVectorMemory(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                         RegisterFile &registers, MemoryController &memory,
                         std::vector<RegisterChange> &changes,
                         std::vector<MemoryChange> &memoryChanges)
{
    VectorMemFields fields;
    if (!DecodeVectorMem(instruction, fields))
    {
        ThrowIllegal(instruction, "reserved vector load/store encoding");
    }

    const uint32_t vd = (instruction >> 7) & 0x1F; // vs3 for stores
    const uint32_t vs2 = (instruction >> 20) & 0x1F;
    const bool masked = ((instruction >> 25) & 1) == 0;
    const char *dataName = fields.isStore ? "vs3" : "vd";
    const uint64_t vstart = registers.ReadCsr(RegisterFile::CSR_VSTART);
    const uint64_t vl = registers.ReadCsr(RegisterFile::CSR_VL);

    uint32_t dataEew = fields.eew; // width of each data element in memory and in registers
    uint32_t segmentFields = fields.nfields;
    size_t fieldRegs = 1;  // registers per field (EMUL, at least 1)
    size_t totalRegs = 1;  // registers the instruction reads (store) or writes (load)
    uint64_t evl = vl;     // elements processed: [vstart, evl)

    switch (fields.mode)
    {
    case VectorMemMode::WholeRegister:
        // Independent of vtype and vl: NFIELDS registers moved as EEW-wide elements.
        RequireAligned(instruction, vd, fields.nfields, dataName);
        segmentFields = 1;
        fieldRegs = totalRegs = fields.nfields;
        evl = fields.nfields * registers.GetVlenBytes() * 8 / dataEew;
        break;

    case VectorMemMode::Mask:
        // vl mask bits, moved as ceil(vl / 8) bytes.
        RequireValidVtype(instruction, registers);
        evl = (vl + 7) / 8;
        break;

    default:
    {
        const Vtype vtype = RequireValidVtype(instruction, registers);
        const int sewLog2 = Log2(vtype.sew);

        // Data EMUL = (EEW / SEW) * LMUL; indexed accesses keep data at SEW/LMUL.
        int dataEmulLog2 = vtype.lmulLog2;
        if (IsIndexed(fields.mode))
        {
            dataEew = static_cast<uint32_t>(vtype.sew);
        }
        else
        {
            dataEmulLog2 = Log2(fields.eew) - sewLog2 + vtype.lmulLog2;
        }
        if (dataEmulLog2 < -3 || dataEmulLog2 > 3)
        {
            ThrowIllegal(instruction, "EMUL = EEW/SEW * LMUL is out of range [1/8, 8]");
        }
        fieldRegs = dataEmulLog2 > 0 ? size_t{1} << dataEmulLog2 : 1;
        totalRegs = fieldRegs * segmentFields;
        RequireAligned(instruction, vd, fieldRegs, dataName);
        if (totalRegs > 8)
        {
            ThrowIllegal(instruction, "NFIELDS * EMUL exceeds 8 registers");
        }
        if (vd + totalRegs > RegisterFile::NUM_VR)
        {
            ThrowIllegal(instruction, "segment register groups run past v31");
        }

        if (IsIndexed(fields.mode))
        {
            const int indexEmulLog2 = Log2(fields.eew) - sewLog2 + vtype.lmulLog2;
            if (indexEmulLog2 < -3 || indexEmulLog2 > 3)
            {
                ThrowIllegal(instruction,
                             "index EMUL = EEW/SEW * LMUL is out of range [1/8, 8]");
            }
            const size_t indexRegs = indexEmulLog2 > 0 ? size_t{1} << indexEmulLog2 : 1;
            RequireAligned(instruction, vs2, indexRegs, "vs2");
            if (!fields.isStore)
            {
                CheckIndexOverlap(instruction, vd, totalRegs, dataEew, vs2, indexRegs,
                                  fields.eew, indexEmulLog2, fields.nfields);
            }
        }
        break;
    }
    }

    if (masked && !fields.isStore && vd == 0)
    {
        ThrowIllegal(instruction, "a masked load cannot write v0");
    }

    const uint64_t elementBytes = dataEew / 8;
    std::unique_ptr<DestinationRecorder> recorder;
    if (!fields.isStore)
    {
        recorder = std::make_unique<DestinationRecorder>(registers, vd, totalRegs);
    }

    uint64_t newVl = vl;
    std::array<uint64_t, 8> segment{};
    for (uint64_t i = vstart; i < evl; ++i)
    {
        if (masked && !registers.ReadMaskBit(0, i))
        {
            continue; // masked-off: no memory access, destination undisturbed
        }

        uint64_t base = 0;
        switch (fields.mode)
        {
        case VectorMemMode::Strided:
            base = rs1Value + i * rs2Value; // x[rs2] is a signed byte stride; wraps like XLEN
            break;
        case VectorMemMode::IndexedUnordered:
        case VectorMemMode::IndexedOrdered:
            // Index elements are zero-extended byte offsets.
            base = rs1Value + registers.ReadVectorElement(vs2, i, fields.eew);
            break;
        default:
            base = rs1Value + i * segmentFields * elementBytes;
            break;
        }

        try
        {
            if (fields.isStore)
            {
                for (uint32_t f = 0; f < segmentFields; ++f)
                {
                    const uint64_t value = registers.ReadVectorElement(vd + f * fieldRegs, i, dataEew);
                    StoreElement(memory, base + f * elementBytes, dataEew, value, memoryChanges);
                }
                continue;
            }
            // Read the whole segment first so a faulting segment leaves every field untouched.
            for (uint32_t f = 0; f < segmentFields; ++f)
            {
                segment[f] = LoadElement(memory, base + f * elementBytes, dataEew);
            }
        }
        catch (const AccessFault &fault)
        {
            if (fields.mode == VectorMemMode::FaultOnlyFirst && i != 0)
            {
                newVl = i; // trim vl to the faulting element instead of trapping
                break;
            }
            ThrowAccessFault(instruction, fields.isStore, i, fault);
        }
        for (uint32_t f = 0; f < segmentFields; ++f)
        {
            registers.WriteVectorElement(vd + f * fieldRegs, i, dataEew, segment[f]);
        }
    }

    if (recorder)
    {
        recorder->Record(changes);
    }
    if (newVl != vl)
    {
        WriteCsrRecorded(registers, RegisterFile::CSR_VL, newVl, changes);
    }
    ClearVstart(registers, changes);
}

} // namespace Kites::rvv
