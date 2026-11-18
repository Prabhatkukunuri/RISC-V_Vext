/**
 * @file vector_memory.h
 * @brief RVV 1.0 vector loads and stores (spec section 7): encoding, decoding, mnemonics and
 * execution, shared by the assembler, the disassembler and every VM.
 *
 * Vector loads/stores reuse the LOAD-FP (0000111) and STORE-FP (0100111) major opcodes; the
 * width field tells them apart from the scalar FP ones (000/101/110/111 are vector EEWs 8/16/32/64,
 * 010/011 are flw/fld).
 *
 *   31 29 | 28  | 27 26 | 25 | 24   20 | 19 15 | 14 12 | 11  7 | 6     0
 *    nf   | mew |  mop  | vm | lumop / |  rs1  | width | vd /  | 0000111 (load)
 *         |     |       |    | rs2/vs2 |       |       | vs3   | 0100111 (store)
 */

#ifndef VECTOR_MEMORY_H
#define VECTOR_MEMORY_H

#include <cstdint>
#include <string>
#include <vector>

namespace Kites
{
class MemoryController;
class RegisterFile;
struct MemoryChange;
struct RegisterChange;

namespace rvv
{
inline constexpr uint32_t OPCODE_LOAD_FP = 0b0000111;
inline constexpr uint32_t OPCODE_STORE_FP = 0b0100111;

/**
 * @brief The addressing mode of a vector load/store (mop, plus lumop/sumop for unit-stride).
 */
enum class VectorMemMode
{
    UnitStride,       ///< vle/vse, vlseg/vsseg        (mop 00, umop 00000)
    FaultOnlyFirst,   ///< vle<eew>ff, vlseg<nf>e<eew>ff (mop 00, lumop 10000, loads only)
    Mask,             ///< vlm.v / vsm.v               (mop 00, umop 01011, EEW = 8)
    WholeRegister,    ///< vl<nf>re<eew> / vs<nf>r     (mop 00, umop 01000)
    Strided,          ///< vlse/vsse, vlsseg/vssseg    (mop 10)
    IndexedUnordered, ///< vluxei/vsuxei, ...seg       (mop 01)
    IndexedOrdered,   ///< vloxei/vsoxei, ...seg       (mop 11)
};

/**
 * @brief Everything the encoding fixes about a vector load/store except its register operands.
 */
struct VectorMemFields
{
    bool isStore = false;
    VectorMemMode mode = VectorMemMode::UnitStride;
    /// Width field as bits: the data EEW, or the index EEW for indexed modes (data is SEW).
    uint32_t eew = 8;
    /// NFIELDS = nf + 1: fields per segment, or registers moved by a whole-register access.
    uint32_t nfields = 1;
};

/**
 * @brief True if the word is a LOAD-FP/STORE-FP word with a vector width (000, 101, 110, 111),
 * including reserved vector encodings.
 */
bool IsVectorMemoryInstruction(uint32_t instruction);

/**
 * @brief Decodes a vector load/store. Returns false for anything that is not one, and for
 * reserved encodings (mew = 1, an unassigned lumop/sumop, whole-register nf not 1/2/4/8, a
 * whole-register store with EEW != 8, a masked mask/whole-register access, segmented vlm/vsm).
 */
bool DecodeVectorMem(uint32_t instruction, VectorMemFields &out);

/**
 * @brief Encodes a vector load/store.
 *
 * @param vd The data register: vd for loads, vs3 for stores.
 * @param field24_20 rs2 (stride) for strided, vs2 (index) for indexed, ignored otherwise.
 */
uint32_t EncodeVectorMem(const VectorMemFields &fields, uint32_t vd, uint32_t rs1,
                         uint32_t field24_20, bool masked);

/**
 * @brief The assembler mnemonic for the fields, e.g. "vlsseg3e16.v" or "vl2re32.v".
 */
std::string VectorMemMnemonic(const VectorMemFields &fields);

/**
 * @brief Looks up a vector load/store mnemonic (including the aliases vl<nf>r.v of vl<nf>re8.v
 * and vle1.v/vse1.v of vlm.v/vsm.v); nullptr if it isn't one.
 */
const VectorMemFields *LookupVectorMemMnemonic(const std::string &mnemonic);

/**
 * @brief Every vector load/store mnemonic the assembler accepts.
 */
const std::vector<std::string> &VectorMemMnemonics();

/**
 * @brief Disassembles a vector load/store as GNU as / llvm-mc write it, e.g.
 * "vlse32.v v1, (x10), x11, v0.t"; "" if the word isn't a valid one.
 */
std::string DisassembleVectorMem(uint32_t instruction);

/**
 * @brief Executes a vector load/store (any word IsVectorMemoryInstruction() accepts).
 *
 * Elements are accessed in order, one memory access per element, through the cache hierarchy.
 * Tail and masked-off elements are left undisturbed (as Spike does). A fault-only-first load
 * that faults on an element other than element 0 reduces vl instead of trapping.
 *
 * @param rs1Value x[rs1], the base address.
 * @param rs2Value x[rs2], the byte stride of strided accesses (unused otherwise).
 * @throws std::runtime_error for an illegal instruction or a memory access fault.
 */
void ExecuteVectorMemory(uint32_t instruction, uint64_t rs1Value, uint64_t rs2Value,
                         RegisterFile &registers, MemoryController &memory,
                         std::vector<RegisterChange> &changes,
                         std::vector<MemoryChange> &memoryChanges);

} // namespace rvv
} // namespace Kites

#endif // VECTOR_MEMORY_H
