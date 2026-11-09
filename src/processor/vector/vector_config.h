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
    Extend, ///< vd[i] = extend(vs2[i]) (vzext/vsext, no second source)
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
};

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
