/**
 * @file vector_config.h
 * @brief vtype decoding and the vsetvli/vsetivli/vsetvl rules (RVV 1.0, section 6), shared by
 * the single-cycle and pipelined VMs.
 */

#ifndef VECTOR_CONFIG_H
#define VECTOR_CONFIG_H

#include <cstdint>
#include <string>

namespace Kites::rvv
{
/// ELEN for Zve64x: the widest element the implementation supports.
inline constexpr uint64_t ELEN = 64;

/// Opcode of every vector instruction (OP-V), and the funct3 that marks the vset* family.
inline constexpr uint32_t OPCODE_OP_V = 0b1010111;
inline constexpr uint32_t FUNCT3_OPCFG = 0b111;

/// funct3 values that select the operand form of OP-V arithmetic.
inline constexpr uint32_t FUNCT3_OPIVV = 0b000; ///< vector-vector, integer
inline constexpr uint32_t FUNCT3_OPMVV = 0b010; ///< vector-vector, mask/multiply/reduction
inline constexpr uint32_t FUNCT3_OPIVI = 0b011; ///< vector-immediate (simm5), integer
inline constexpr uint32_t FUNCT3_OPIVX = 0b100; ///< vector-scalar (x[rs1]), integer
inline constexpr uint32_t FUNCT3_OPMVX = 0b110; ///< vector-scalar, mask/multiply/reduction

/**
 * @brief Which instruction of the vset* family an encoded word is.
 */
enum class VsetKind
{
    None,     ///< Not a vset* instruction.
    Vsetvli,  ///< vsetvli rd, rs1, vtypei   (bit 31 = 0)
    Vsetivli, ///< vsetivli rd, uimm, vtypei (bits 31:30 = 11)
    Vsetvl,   ///< vsetvl rd, rs1, rs2       (bits 31:25 = 1000000)
};

/**
 * @brief Classifies a 32-bit instruction word as one of the vset* forms, or None.
 */
VsetKind DecodeVsetKind(uint32_t instruction);

/**
 * @brief The vtypei immediate of a vsetvli (11 bits) or vsetivli (10 bits).
 */
uint64_t VsetImmediateVtype(uint32_t instruction);

/**
 * @brief Element operation of an OP-V arithmetic instruction (picked by funct6).
 */
enum class VectorArithOp
{
    Add,    ///< vd[i] = vs2[i] + src
    Sub,    ///< vd[i] = vs2[i] - src
    Rsub,   ///< vd[i] = src - vs2[i]
    And,    ///< vd[i] = vs2[i] & src
    Or,     ///< vd[i] = vs2[i] | src
    Xor,    ///< vd[i] = vs2[i] ^ src
    Sll,    ///< vd[i] = vs2[i] << (src % SEW)
    Srl,    ///< vd[i] = vs2[i] >> (src % width of vs2), zero-filling
    Sra,    ///< vd[i] = vs2[i] >> (src % width of vs2), sign-filling
    Extend, ///< vd[i] = extend(vs2[i]) (vzext/vsext, no second source)
    Eq,     ///< mask bit i = vs2[i] == src
    Ne,     ///< mask bit i = vs2[i] != src
    Lt,     ///< mask bit i = vs2[i] <  src (signed or unsigned per isSigned)
    Le,     ///< mask bit i = vs2[i] <= src
    Gt,     ///< mask bit i = vs2[i] >  src
    Min,    ///< vd[i] = min(vs2[i], src) (signed or unsigned per isSigned)
    Max,    ///< vd[i] = max(vs2[i], src)
    Mul,    ///< vd[i] = low half of vs2[i] * src (the whole product when widening)
    Mulh,   ///< vd[i] = high SEW bits of the 2*SEW-bit product vs2[i] * src
    Div,    ///< vd[i] = vs2[i] / src (RISC-V results for /0 and overflow)
    Rem,    ///< vd[i] = vs2[i] % src
    Macc,   ///< vd[i] = +(src * vs2[i]) + vd[i]
    Nmsac,  ///< vd[i] = -(src * vs2[i]) + vd[i]
    Madd,   ///< vd[i] = (src * vd[i]) + vs2[i]
    Nmsub,  ///< vd[i] = -(src * vd[i]) + vs2[i]
    Merge,  ///< vd[i] = v0.mask[i] ? src : vs2[i] (every body element)
    Move,   ///< vd[i] = src (vmv.v.v / .v.x / .v.i)
    Adc,    ///< vd[i] = vs2[i] + src + v0.mask[i] (every body element)
    Sbc,    ///< vd[i] = vs2[i] - src - v0.mask[i] (every body element)
    Madc,   ///< mask bit i = carry out of vs2[i] + src [+ v0.mask[i] when vm = 0]
    Msbc,   ///< mask bit i = borrow out of vs2[i] - src [- v0.mask[i] when vm = 0]

    // Section 16, permutation (shape Permute): each has its own element mapping.
    MoveXS,     ///< x[rd] = sign-extended vs2[0]                      (vmv.x.s)
    MoveSX,     ///< vd[0] = x[rs1] if vstart < vl                     (vmv.s.x)
    SlideUp,    ///< vd[i + OFFSET] = vs2[i]; vd[i < OFFSET] unchanged
    SlideDown,  ///< vd[i] = vs2[i + OFFSET], 0 past VLMAX
    Slide1Up,   ///< vd[0] = x[rs1], vd[i + 1] = vs2[i]
    Slide1Down, ///< vd[i] = vs2[i + 1], vd[vl - 1] = x[rs1]
    Gather,     ///< vd[i] = index >= VLMAX ? 0 : vs2[index], index = vs1[i] | x[rs1] | uimm
    GatherEi16, ///< vrgather with 16-bit indices in vs1 (EMUL = 16/SEW * LMUL)
    Compress,   ///< pack the vs2 elements whose bit in mask vs1 is set to the start of vd
    WholeMove,  ///< copy NREG = vs1 field + 1 whole registers (vmv<nr>r.v)
};

/**
 * @brief Element widths of an instruction's operands, relative to SEW.
 */
enum class VectorOpShape
{
    SingleWidth, ///< vd, vs2 and the source are all SEW wide
    Widening,    ///< vd is 2*SEW, vs2 and the source are SEW (vwadd.vv, .vx, ...)
    WideningW,   ///< vd and vs2 are 2*SEW, the source is SEW (vwadd.wv, .wx, ...)
    Extension,   ///< vd is SEW, vs2 is SEW/factor (vzext.vf2, vsext.vf8, ...)
    Narrowing,   ///< vd and the source are SEW, vs2 is 2*SEW (vnsrl.wv, vnsra.wi, ...)
    Compare,     ///< vs2 and the source are SEW; vd is a mask (one bit per element, one register)
    Permute,     ///< section 16: slides, gathers, compress and moves (see VectorArithOp)
};

/**
 * @brief One supported OP-V arithmetic encoding.
 */
struct VectorArithInfo
{
    uint32_t funct6;
    uint32_t funct3;
    int vs1Field; ///< Fixed vs1 field that picks the op (vzext/vsext), or -1 if it's an operand.
    VectorArithOp op;
    VectorOpShape shape;
    bool isSigned; ///< Narrow operands are sign-extended (vwadd, vsext), else zero-extended.
    unsigned factor; ///< Extension: vs2 is SEW/factor wide. 1 otherwise.
    const char *mnemonic;
    /// Signedness of the source (vs1 / rs1) when it differs from vs2's (vmulhsu, vwmaccsu...):
    /// 0 unsigned, 1 signed, -1 the same as isSigned.
    int sourceSigned = -1;
    /// Fixed vm bit that picks the instruction, or -1 if vm is an ordinary mask. With 0 the
    /// instruction reads v0 as data (vmerge's selector, vadc's carry-in) and writes every body
    /// element; the assembly then ends in a required ", v0". With 1 it takes no mask at all.
    int vmField = -1;
};

/// Whether the vs1 / rs1 / immediate operand is sign-extended.
inline bool SourceIsSigned(const VectorArithInfo &info)
{
    return info.sourceSigned < 0 ? info.isSigned : info.sourceSigned == 1;
}

/// The multiply-add family writes "vd, vs1/rs1, vs2": the source comes before vs2.
inline bool HasSourceFirstSyntax(const VectorArithInfo &info)
{
    return info.op == VectorArithOp::Macc || info.op == VectorArithOp::Nmsac ||
           info.op == VectorArithOp::Madd || info.op == VectorArithOp::Nmsub;
}

/**
 * @brief True if the .vi form takes an unsigned 5-bit immediate (uimm, 0..31) rather than a
 * signed one (simm5, -16..15): the shifts, the slides and vrgather.vi.
 */
bool HasUnsignedImmediate(const VectorArithInfo &info);

/**
 * @brief Looks up an OP-V arithmetic word; nullptr if it isn't one this VM implements.
 */
const VectorArithInfo *DecodeVectorArith(uint32_t instruction);

/**
 * @brief Looks up an implemented OP-V arithmetic instruction by mnemonic; nullptr if unknown.
 */
const VectorArithInfo *FindVectorArith(const std::string &mnemonic);

/**
 * @brief The mnemonic of an implemented OP-V arithmetic word, or "" if there is none.
 */
std::string VectorArithMnemonic(uint32_t instruction);

/**
 * @brief The fields of a vtype value.
 */
struct Vtype
{
    uint64_t sew = 0;  ///< Selected element width in bits: 8 << vsew.
    int lmulLog2 = 0;  ///< log2(LMUL), -3..3. Fractional LMUL is negative.
    bool tailAgnostic = false;
    bool maskAgnostic = false;
};

/**
 * @brief Decodes a vtype, or returns false if this VM can't support it (vtype.vill).
 *
 * Follows Spike: vill when any reserved bit (XLEN-1:8) is set, vlmul is the reserved 0b100,
 * SEW > ELEN, or SEW > LMUL * ELEN for a fractional LMUL.
 */
bool DecodeVtype(uint64_t vtype, Vtype &out);

/**
 * @brief VLMAX = LMUL * VLEN / SEW for a valid vtype.
 */
uint64_t Vlmax(const Vtype &vtype, uint64_t vlenBits);

/**
 * @brief The result of a vset* instruction: what to write to vl, vtype and rd.
 */
struct VsetResult
{
    uint64_t vl = 0;
    uint64_t vtype = 0;
};

/**
 * @brief Applies the vset* rules.
 *
 * @param requestedVtype The new vtype (vtypei, or x[rs2] for vsetvl).
 * @param avl The application vector length (x[rs1], or the uimm of vsetivli).
 * @param rdIsX0 / rs1IsX0 Select the special AVL cases. For vsetivli pass rs1IsX0 = false:
 *        its uimm is always the AVL, even when it is 0.
 * @param currentVl The vl before the instruction, kept (capped to VLMAX) when rd = rs1 = x0.
 */
VsetResult ComputeVset(uint64_t requestedVtype, uint64_t avl, bool rdIsX0, bool rs1IsX0,
                       uint64_t currentVl, uint64_t vlenBits);

/**
 * @brief Encodes vtype fields as a vtypei immediate.
 */
uint64_t EncodeVtype(uint64_t sew, int lmulLog2, bool tailAgnostic, bool maskAgnostic);

/**
 * @brief Formats a vtypei the way GNU as writes it, e.g. "e32, m1, ta, ma". A vtypei that
 * doesn't decode is printed as a number.
 */
std::string FormatVtype(uint64_t vtype);

/**
 * @brief Parses an assembler SEW token ("e8", "e16", "e32", "e64") into bits; false if invalid.
 */
bool ParseSewName(const std::string &name, uint64_t &sew);

/**
 * @brief Parses an assembler LMUL token ("m1", "m2", "m4", "m8", "mf2", "mf4", "mf8") into
 * log2(LMUL); false if invalid.
 */
bool ParseLmulName(const std::string &name, int &lmulLog2);

} // namespace Kites::rvv

#endif // VECTOR_CONFIG_H
