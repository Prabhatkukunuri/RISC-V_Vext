/**
 * @file v_formats.cpp
 * @brief Parsers for the RVV instruction formats: the vset* configuration instructions
 * (vsetvli rd, rs1, vtype and vsetivli rd, uimm, vtype; vsetvl uses the plain O_GPR_C_GPR_C_GPR
 * format), integer arithmetic (vd, vs2, vs1|rs1|simm5) and loads/stores (vd, (rs1)[, rs2|vs2]).
 */

#include "assembler/parser.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_memory.h"
#include "utils/utils.h"

#include <string>
#include <vector>

namespace Kites
{
bool Parser::parseVtypeOperands(int first, uint64_t maxImm, uint64_t &vtype)
{
    const unsigned int line = currentToken().line_number;

    // Collect "<operand> (, <operand>)*" up to the end of the line.
    std::vector<Token> operands;
    int offset = first;
    bool wellFormed = true;
    while (true)
    {
        Token operand = peekToken(offset);
        if (operand.type == TokenType::EOF_ || operand.line_number != line ||
            operand.type == TokenType::COMMA)
        {
            wellFormed = false;
            break;
        }
        operands.push_back(operand);
        Token next = peekToken(offset + 1);
        if (next.type == TokenType::EOF_ || next.line_number != line)
        {
            break;
        }
        if (next.type != TokenType::COMMA)
        {
            wellFormed = false;
            break;
        }
        offset += 2;
    }

    auto fail = [&](const Token &at, const std::string &detail)
    {
        recordError(ParseError(at.line_number, "Invalid vtype: " + detail));
        errors_.all_errors.emplace_back(
            errors::SyntaxError("Invalid vtype", detail, filename_, at.line_number,
                                at.column_number, GetLineFromFile(filename_, at.line_number)));
        skipCurrentLine();
        return false;
    };

    const std::string expected = "Expected: <sew>[, <lmul>][, ta|tu][, ma|mu], e.g. e32, m1, ta, ma";
    if (!wellFormed || operands.empty())
    {
        return fail(operands.empty() ? peekToken(first) : operands.back(), expected);
    }

    // A raw vtypei, as GNU as also accepts.
    if (operands.size() == 1 && operands[0].type == TokenType::NUM)
    {
        const int64_t value = std::stoll(operands[0].value);
        if (value < 0 || static_cast<uint64_t>(value) > maxImm)
        {
            recordError(ParseError(operands[0].line_number, "Immediate value out of range"));
            errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
                "Immediate value out of range",
                "Expected: 0 <= vtypei <= " + std::to_string(maxImm), filename_,
                operands[0].line_number, operands[0].column_number,
                GetLineFromFile(filename_, operands[0].line_number)));
            skipCurrentLine();
            return false;
        }
        vtype = static_cast<uint64_t>(value);
        return true;
    }

    // Symbolic form. The lexer tags these words as label references since they follow a comma.
    size_t index = 0;
    uint64_t sew = 0;
    if (!rvv::ParseSewName(operands[index].value, sew))
    {
        return fail(operands[index], "Expected SEW (e8, e16, e32 or e64), got '" +
                                         operands[index].value + "'");
    }
    ++index;

    int lmulLog2 = 0; // default m1
    if (index < operands.size() && rvv::ParseLmulName(operands[index].value, lmulLog2))
    {
        ++index;
    }

    bool tailAgnostic = false; // default tu
    if (index < operands.size() &&
        (operands[index].value == "ta" || operands[index].value == "tu"))
    {
        tailAgnostic = operands[index].value == "ta";
        ++index;
    }

    bool maskAgnostic = false; // default mu
    if (index < operands.size() &&
        (operands[index].value == "ma" || operands[index].value == "mu"))
    {
        maskAgnostic = operands[index].value == "ma";
        ++index;
    }

    if (index != operands.size())
    {
        return fail(operands[index], "Unexpected '" + operands[index].value + "'. " + expected);
    }

    vtype = rvv::EncodeVtype(sew, lmulLog2, tailAgnostic, maskAgnostic);
    return true;
}

bool Parser::parse_O_GPR_C_GPR_C_VTYPE()
{
    const unsigned int line = currentToken().line_number;
    if (!(peekToken(1).line_number == line && peekToken(1).type == TokenType::GP_REGISTER &&
          peekToken(2).line_number == line && peekToken(2).type == TokenType::COMMA &&
          peekToken(3).line_number == line && peekToken(3).type == TokenType::GP_REGISTER &&
          peekToken(4).line_number == line && peekToken(4).type == TokenType::COMMA &&
          peekToken(5).line_number == line && peekToken(5).type != TokenType::EOF_))
    {
        return false;
    }

    ICUnit block;
    block.setOpcode(currentToken().value);
    block.setLineNumber(line);
    block.setInstructionIndex(instruction_index_);
    block.setRd(reg_alias_to_name.at(peekToken(1).value));
    block.setRs1(reg_alias_to_name.at(peekToken(3).value));

    uint64_t vtype = 0;
    if (!parseVtypeOperands(5, 0x7FF, vtype)) // vsetvli has an 11-bit zimm
    {
        return true; // error already recorded
    }
    block.setVtype(static_cast<uint32_t>(vtype));

    skipCurrentLine();
    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
    return true;
}

bool Parser::parse_O_GPR_C_I_C_VTYPE()
{
    const unsigned int line = currentToken().line_number;
    if (!(peekToken(1).line_number == line && peekToken(1).type == TokenType::GP_REGISTER &&
          peekToken(2).line_number == line && peekToken(2).type == TokenType::COMMA &&
          peekToken(3).line_number == line && peekToken(3).type == TokenType::NUM &&
          peekToken(4).line_number == line && peekToken(4).type == TokenType::COMMA &&
          peekToken(5).line_number == line && peekToken(5).type != TokenType::EOF_))
    {
        return false;
    }

    ICUnit block;
    block.setOpcode(currentToken().value);
    block.setLineNumber(line);
    block.setInstructionIndex(instruction_index_);
    block.setRd(reg_alias_to_name.at(peekToken(1).value));

    const int64_t avl = std::stoll(peekToken(3).value);
    if (avl < 0 || avl > 31)
    {
        recordError(ParseError(peekToken(3).line_number, "Immediate value out of range"));
        errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
            "Immediate value out of range", "Expected: 0 <= uimm <= 31", filename_,
            peekToken(3).line_number, peekToken(3).column_number,
            GetLineFromFile(filename_, peekToken(3).line_number)));
        skipCurrentLine();
        return true;
    }
    block.setImm(std::to_string(avl));

    uint64_t vtype = 0;
    if (!parseVtypeOperands(5, 0x3FF, vtype)) // vsetivli has a 10-bit zimm
    {
        return true;
    }
    block.setVtype(static_cast<uint32_t>(vtype));

    skipCurrentLine();
    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
    return true;
}
namespace
{
/**
 * A compare the ISA doesn't encode, which the assembler rewrites into one it does (RVV 1.0,
 * 11.8; the same mapping llvm-mc uses).
 */
struct VectorCompareAlias
{
    const char *name;
    const char *target;
    bool swapSources;       ///< .vv: vd, va, vb  ->  target vd, vb, va
    bool decrementImm;      ///< .vi: imm         ->  target imm - 1 (so imm is -15..16)
    const char *zeroTarget; ///< .vi with imm 0: target.vv vd, va, va instead (or nullptr)
};

const VectorCompareAlias kVectorCompareAliases[] = {
    {"vmsgt.vv", "vmslt.vv", true, false, nullptr},
    {"vmsgtu.vv", "vmsltu.vv", true, false, nullptr},
    {"vmsge.vv", "vmsle.vv", true, false, nullptr},
    {"vmsgeu.vv", "vmsleu.vv", true, false, nullptr},
    {"vmslt.vi", "vmsle.vi", false, true, nullptr},
    // va <u 0 is always false and va >=u 0 always true; imm - 1 would wrap, so compare va
    // with itself instead.
    {"vmsltu.vi", "vmsleu.vi", false, true, "vmsne.vv"},
    {"vmsge.vi", "vmsgt.vi", false, true, nullptr},
    {"vmsgeu.vi", "vmsgtu.vi", false, true, "vmseq.vv"},
};

const VectorCompareAlias *FindVectorCompareAlias(const std::string &name)
{
    for (const VectorCompareAlias &alias : kVectorCompareAliases)
    {
        if (name == alias.name)
        {
            return &alias;
        }
    }
    return nullptr;
}
} // namespace

bool Parser::parseVectorArith(std::optional<TokenType> sourceType)
{
    const unsigned int line = currentToken().line_number;
    auto on = [&](int n, TokenType type)
    { return peekToken(n).line_number == line && peekToken(n).type == type; };
    auto endsAt = [&](int n)
    { return peekToken(n).type == TokenType::EOF_ || peekToken(n).line_number != line; };

    const VectorCompareAlias *alias = FindVectorCompareAlias(currentToken().value);
    std::string opcode = alias ? alias->target : currentToken().value;
    const rvv::VectorArithInfo *opInfo = rvv::FindVectorArith(opcode);
    const rvv::VectorArithOp op = opInfo ? opInfo->op : rvv::VectorArithOp::Add;

    // The operand layout depends on the instruction:
    //   most                vd, vs2[, source][, v0.t]
    //   vmacc, vwmacc, ...  vd, source, vs2[, v0.t]
    //   vmerge, vadc, ...   vd, vs2, source, v0       (fixed vm = 0: v0 is data, required)
    //   vmadc.vv, ...       vd, vs2, source           (fixed vm = 1: never masked)
    //   vmv.v.*, vmv.s.x    vd, source                (never masked; vs2 is encoded as v0)
    //   vmv.x.s             rd, vs2                   (a GPR destination; never masked)
    enum class MaskOperand
    {
        Optional,
        Required,
        None
    };
    const bool gprDestination = op == rvv::VectorArithOp::MoveXS;
    std::vector<TokenType> operands{gprDestination ? TokenType::GP_REGISTER
                                                   : TokenType::VEC_REGISTER};
    int vs2At = -1;    // operand index of vs2, -1 if it is implicitly v0
    int sourceAt = -1; // operand index of the source, -1 if there is none
    MaskOperand maskOperand = MaskOperand::Optional;
    if (gprDestination)
    {
        operands.push_back(TokenType::VEC_REGISTER);
        vs2At = 1;
        maskOperand = MaskOperand::None;
    }
    else if (op == rvv::VectorArithOp::Move || op == rvv::VectorArithOp::MoveSX)
    {
        operands.push_back(sourceType.value_or(TokenType::VEC_REGISTER));
        sourceAt = 1;
        maskOperand = MaskOperand::None;
    }
    else if (opInfo && rvv::HasSourceFirstSyntax(*opInfo) && sourceType)
    {
        operands.push_back(*sourceType);
        operands.push_back(TokenType::VEC_REGISTER);
        sourceAt = 1;
        vs2At = 2;
    }
    else
    {
        operands.push_back(TokenType::VEC_REGISTER);
        vs2At = 1;
        if (sourceType)
        {
            operands.push_back(*sourceType);
            sourceAt = 2;
        }
        if (opInfo && opInfo->vmField == 0)
        {
            maskOperand = MaskOperand::Required;
        }
        else if (opInfo && opInfo->vmField == 1)
        {
            maskOperand = MaskOperand::None;
        }
    }

    // Operand k is token 2k + 1, with a comma before each one after the first.
    for (size_t k = 0; k < operands.size(); ++k)
    {
        const int at = static_cast<int>(2 * k + 1);
        if ((k > 0 && !on(at - 1, TokenType::COMMA)) || !on(at, operands[k]) ||
            peekToken(at).value == "v0.t") // "v0.t" is only valid as the mask operand
        {
            return false;
        }
    }
    const int end = static_cast<int>(2 * operands.size()); // first token after the operands
    const auto maskIs = [&](const char *name)
    {
        return on(end, TokenType::COMMA) && on(end + 1, TokenType::VEC_REGISTER) &&
               peekToken(end + 1).value == name && endsAt(end + 2);
    };
    bool masked = false;
    switch (maskOperand)
    {
    case MaskOperand::Optional:
        masked = maskIs("v0.t");
        if (!masked && !endsAt(end))
        {
            return false;
        }
        break;
    case MaskOperand::Required:
        if (!maskIs("v0"))
        {
            return false;
        }
        masked = true; // vm = 0
        break;
    case MaskOperand::None:
        if (!endsAt(end))
        {
            return false;
        }
        break;
    }

    auto reject = [&](const Token &at, const std::string &title, const std::string &detail)
    {
        recordError(ParseError(at.line_number, title));
        errors_.all_errors.emplace_back(errors::SyntaxError(title, detail, filename_,
                                                            at.line_number, at.column_number,
                                                            GetLineFromFile(filename_, at.line_number)));
        skipCurrentLine();
        return true;
    };

    const Token &vd = peekToken(1);
    const std::string vs2 = vs2At < 0 ? "v0" : peekToken(2 * vs2At + 1).value;
    const Token &source = peekToken(sourceAt < 0 ? 0 : 2 * sourceAt + 1);
    // The source operand's token type (vmv.v.v reaches here with no sourceType).
    const std::optional<TokenType> sourceKind =
        sourceAt < 0 ? std::nullopt : std::optional<TokenType>(operands[sourceAt]);
    // Compares write a mask, which may go to v0 even under v0.t.
    const bool isCompare = opInfo && opInfo->shape == rvv::VectorOpShape::Compare;
    if (masked && vd.value == "v0" && !isCompare)
    {
        return reject(vd, "Destination overlaps the mask register",
                      opInfo && opInfo->vmField == 0
                          ? "This instruction reads v0 as an operand, so it cannot write v0"
                          : "A masked instruction (v0.t) cannot write v0");
    }

    // Overlaps that are illegal at every LMUL: the destination can't start on a source with a
    // narrower element width. (Overlaps that depend on LMUL are checked when it runs.)
    if (opInfo)
    {
        const bool narrowVs2 = opInfo->shape == rvv::VectorOpShape::Widening ||
                               opInfo->shape == rvv::VectorOpShape::Extension;
        const bool narrowVs1 = (opInfo->shape == rvv::VectorOpShape::Widening ||
                                opInfo->shape == rvv::VectorOpShape::WideningW) &&
                               sourceKind == TokenType::VEC_REGISTER;
        if ((narrowVs2 && vd.value == vs2) || (narrowVs1 && vd.value == source.value))
        {
            return reject(vd, "Destination overlaps a source",
                          "vd cannot be the same register as a source with a narrower element "
                          "width");
        }
        // Section 16: slide-ups, vrgather and vcompress may not overlap their sources at all.
        const bool noVs2Overlap = op == rvv::VectorArithOp::SlideUp ||
                                  op == rvv::VectorArithOp::Slide1Up ||
                                  op == rvv::VectorArithOp::Gather ||
                                  op == rvv::VectorArithOp::GatherEi16 ||
                                  op == rvv::VectorArithOp::Compress;
        const bool noVs1Overlap = (op == rvv::VectorArithOp::Gather ||
                                   op == rvv::VectorArithOp::GatherEi16 ||
                                   op == rvv::VectorArithOp::Compress) &&
                                  sourceKind == TokenType::VEC_REGISTER;
        if ((noVs2Overlap && vd.value == vs2) || (noVs1Overlap && vd.value == source.value))
        {
            return reject(vd, "Destination overlaps a source",
                          "For slide-ups, vrgather and vcompress, vd must not overlap a source");
        }
    }

    ICUnit block;
    block.setLineNumber(line);
    block.setInstructionIndex(instruction_index_);
    block.setRd(gprDestination ? reg_alias_to_name.at(vd.value) : vd.value);
    block.setRs2(vs2);
    block.setVm(masked ? 0 : 1);

    if (sourceKind == TokenType::NUM)
    {
        // Shifts take an unsigned uimm (0..31), the decrementing compare aliases -15..16 and
        // everything else a signed simm5 (-16..15).
        const bool isUnsigned = opInfo && rvv::HasUnsignedImmediate(*opInfo);
        const bool decrement = alias && alias->decrementImm;
        const int64_t low = isUnsigned ? 0 : decrement ? -15 : -16;
        const int64_t high = isUnsigned ? 31 : decrement ? 16 : 15;
        const int64_t imm = std::stoll(source.value);
        if (imm < low || imm > high)
        {
            recordError(ParseError(source.line_number, "Immediate value out of range"));
            errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
                "Immediate value out of range",
                isUnsigned ? "Expected: 0 <= uimm <= 31"
                : decrement ? "Expected: -15 <= imm <= 16"
                            : "Expected: -16 <= simm5 <= 15",
                filename_, source.line_number, source.column_number,
                GetLineFromFile(filename_, source.line_number)));
            skipCurrentLine();
            return true;
        }
        if (decrement && imm == 0 && alias->zeroTarget)
        {
            opcode = alias->zeroTarget; // compare va with itself
            block.setRs1(vs2);
        }
        else
        {
            block.setImm(std::to_string(decrement ? imm - 1 : imm));
        }
    }
    else if (sourceKind == TokenType::GP_REGISTER)
    {
        block.setRs1(reg_alias_to_name.at(source.value));
    }
    else if (sourceKind == TokenType::VEC_REGISTER)
    {
        block.setRs1(source.value);
        if (alias && alias->swapSources)
        {
            block.setRs1(vs2);
            block.setRs2(source.value);
        }
    }
    block.setOpcode(opcode);

    skipCurrentLine();
    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
    return true;
}

bool Parser::parse_O_VR_C_VR_C_VR()
{
    return parseVectorArith(TokenType::VEC_REGISTER);
}

bool Parser::parse_O_VR_C_VR_C_GPR()
{
    return parseVectorArith(TokenType::GP_REGISTER);
}

bool Parser::parse_O_VR_C_VR_C_I()
{
    return parseVectorArith(TokenType::NUM);
}

bool Parser::parse_O_VR_C_VR()
{
    return parseVectorArith(std::nullopt);
}

// The operand order of these comes from the instruction (see parseVectorArith).

bool Parser::parse_O_VR_C_GPR_C_VR()
{
    return parseVectorArith(TokenType::GP_REGISTER);
}

bool Parser::parse_O_VR_C_VR_C_VR_C_V0()
{
    return parseVectorArith(TokenType::VEC_REGISTER);
}

bool Parser::parse_O_VR_C_VR_C_GPR_C_V0()
{
    return parseVectorArith(TokenType::GP_REGISTER);
}

bool Parser::parse_O_VR_C_VR_C_I_C_V0()
{
    return parseVectorArith(TokenType::NUM);
}

bool Parser::parse_O_VR_C_GPR()
{
    return parseVectorArith(TokenType::GP_REGISTER);
}

bool Parser::parse_O_VR_C_I()
{
    return parseVectorArith(TokenType::NUM);
}

bool Parser::parse_O_GPR_C_VR()
{
    return parseVectorArith(std::nullopt); // vmv.x.s rd, vs2
}

bool Parser::parseVectorMemory(TokenType extraType)
{
    const unsigned int line = currentToken().line_number;
    auto on = [&](int n, TokenType type)
    { return peekToken(n).line_number == line && peekToken(n).type == type; };
    auto endsAt = [&](int n)
    { return peekToken(n).type == TokenType::EOF_ || peekToken(n).line_number != line; };
    auto isPlainVr = [&](int n)
    { return on(n, TokenType::VEC_REGISTER) && peekToken(n).value != "v0.t"; };

    // vd , [0] ( rs1 ) [, rs2|vs2] [, v0.t]
    if (!(isPlainVr(1) && on(2, TokenType::COMMA)))
    {
        return false;
    }
    int n = 3;
    const bool hasOffset = on(n, TokenType::NUM);
    if (hasOffset)
    {
        ++n;
    }
    if (!(on(n, TokenType::LPAREN) && on(n + 1, TokenType::GP_REGISTER) &&
          on(n + 2, TokenType::RPAREN)))
    {
        return false;
    }
    const int rs1At = n + 1;
    n += 3;
    int extraAt = -1;
    if (extraType != TokenType::EOF_)
    {
        const bool extraOk = extraType == TokenType::VEC_REGISTER
                                 ? isPlainVr(n + 1)
                                 : on(n + 1, TokenType::GP_REGISTER);
        if (!(on(n, TokenType::COMMA) && extraOk))
        {
            return false;
        }
        extraAt = n + 1;
        n += 2;
    }
    bool masked = false;
    if (on(n, TokenType::COMMA) && on(n + 1, TokenType::VEC_REGISTER) &&
        peekToken(n + 1).value == "v0.t" && endsAt(n + 2))
    {
        masked = true;
    }
    else if (!endsAt(n))
    {
        return false;
    }

    auto fail = [&](const Token &at, const std::string &title, const std::string &detail)
    {
        recordError(ParseError(at.line_number, title));
        errors_.all_errors.emplace_back(errors::SyntaxError(title, detail, filename_,
                                                            at.line_number, at.column_number,
                                                            GetLineFromFile(filename_,
                                                                            at.line_number)));
        skipCurrentLine();
        return true;
    };

    if (hasOffset && std::stoll(peekToken(3).value) != 0)
    {
        recordError(ParseError(peekToken(3).line_number, "Immediate value out of range"));
        errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
            "Immediate value out of range", "Vector loads/stores take no offset: expected 0",
            filename_, peekToken(3).line_number, peekToken(3).column_number,
            GetLineFromFile(filename_, peekToken(3).line_number)));
        skipCurrentLine();
        return true;
    }

    const rvv::VectorMemFields &fields = *rvv::LookupVectorMemMnemonic(currentToken().value);
    const Token &data = peekToken(1);
    const uint32_t dataReg = static_cast<uint32_t>(std::stoul(data.value.substr(1)));
    if (masked && (fields.mode == rvv::VectorMemMode::Mask ||
                   fields.mode == rvv::VectorMemMode::WholeRegister))
    {
        return fail(peekToken(n + 1), "Instruction cannot be masked",
                    currentToken().value + " is always unmasked");
    }
    if (masked && !fields.isStore && dataReg == 0)
    {
        return fail(data, "Destination overlaps the mask register",
                    "A masked instruction (v0.t) cannot write v0");
    }
    if (fields.mode == rvv::VectorMemMode::WholeRegister && dataReg % fields.nfields != 0)
    {
        return fail(data, "Misaligned register group",
                    currentToken().value + " needs a register number that is a multiple of " +
                        std::to_string(fields.nfields));
    }

    ICUnit block;
    block.setOpcode(currentToken().value);
    block.setLineNumber(line);
    block.setInstructionIndex(instruction_index_);
    block.setRd(data.value);
    block.setRs1(reg_alias_to_name.at(peekToken(rs1At).value));
    if (extraAt >= 0)
    {
        const std::string &extra = peekToken(extraAt).value;
        block.setRs2(extraType == TokenType::GP_REGISTER ? reg_alias_to_name.at(extra) : extra);
    }
    block.setVm(masked ? 0 : 1);

    skipCurrentLine();
    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
    return true;
}

bool Parser::parse_O_VR_C_LP_GPR_RP()
{
    return parseVectorMemory(TokenType::EOF_);
}

bool Parser::parse_O_VR_C_LP_GPR_RP_C_GPR()
{
    return parseVectorMemory(TokenType::GP_REGISTER);
}

bool Parser::parse_O_VR_C_LP_GPR_RP_C_VR()
{
    return parseVectorMemory(TokenType::VEC_REGISTER);
}
} // namespace Kites
