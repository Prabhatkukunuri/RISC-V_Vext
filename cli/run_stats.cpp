/**
 * @file run_stats.cpp
 * @brief Dynamic instruction statistics for kites-cli --stats (see run_stats.h).
 */
#include "run_stats.h"

#include "processor/vector/vector_config.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace Kites::cli
{
namespace
{
constexpr uint32_t OPCODE_LOAD = 0x03;
constexpr uint32_t OPCODE_LOAD_FP = 0x07; // also vector loads
constexpr uint32_t OPCODE_OP_IMM = 0x13;
constexpr uint32_t OPCODE_AUIPC = 0x17;
constexpr uint32_t OPCODE_OP_IMM_32 = 0x1B;
constexpr uint32_t OPCODE_STORE = 0x23;
constexpr uint32_t OPCODE_STORE_FP = 0x27; // also vector stores
constexpr uint32_t OPCODE_OP = 0x33;
constexpr uint32_t OPCODE_LUI = 0x37;
constexpr uint32_t OPCODE_OP_32 = 0x3B;
constexpr uint32_t OPCODE_OP_V = 0x57;
constexpr uint32_t OPCODE_BRANCH = 0x63;
constexpr uint32_t OPCODE_JALR = 0x67;
constexpr uint32_t OPCODE_JAL = 0x6F;

// LOAD-FP / STORE-FP width fields 0, 5, 6, 7 are the vector element widths 8, 16, 32, 64.
bool isVectorWidth(uint32_t funct3, unsigned &eewBits)
{
    switch (funct3)
    {
    case 0:
        eewBits = 8;
        return true;
    case 5:
        eewBits = 16;
        return true;
    case 6:
        eewBits = 32;
        return true;
    case 7:
        eewBits = 64;
        return true;
    default:
        return false;
    }
}

std::string jsonString(const std::string &text)
{
    std::string out = "\"";
    for (char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}
} // namespace

RunStatsCollector::RunStatsCollector(const AssembledProgram &program,
                                     const RegisterFile &registers, uint64_t vlenBits)
    : m_program(program), m_registers(registers), m_vlenBits(vlenBits)
{
}

void RunStatsCollector::onClock(const ProcessorState &state)
{
    const uint64_t pc = state.lastExecutedPC;
    if (pc % 4 != 0 || pc / 4 >= m_program.text_buffer.size())
    {
        return;
    }
    const uint32_t instruction = m_program.text_buffer[pc / 4];
    const uint64_t nextPc = state.programCounters.empty() ? pc + 4 : state.programCounters[0];
    ++m_stats.retiredSeen;

    const uint32_t opcode = instruction & 0x7F;
    const uint32_t funct3 = (instruction >> 12) & 0x7;
    unsigned eew = 0;
    switch (opcode)
    {
    case OPCODE_OP:
    case OPCODE_OP_32:
    case OPCODE_OP_IMM:
    case OPCODE_OP_IMM_32:
    case OPCODE_LUI:
    case OPCODE_AUIPC:
        ++m_stats.scalarAlu;
        break;
    case OPCODE_LOAD:
        ++m_stats.scalarLoad;
        m_stats.scalarLoadBytes += 1ULL << (funct3 & 0x3);
        break;
    case OPCODE_STORE:
        ++m_stats.scalarStore;
        m_stats.scalarStoreBytes += 1ULL << (funct3 & 0x3);
        break;
    case OPCODE_LOAD_FP:
    case OPCODE_STORE_FP:
    {
        const bool isStore = opcode == OPCODE_STORE_FP;
        if (isVectorWidth(funct3, eew))
        {
            classifyVectorMemory(instruction, isStore);
        }
        else if (isStore)
        {
            ++m_stats.scalarStore;
            m_stats.scalarStoreBytes += 1ULL << (funct3 & 0x3);
        }
        else
        {
            ++m_stats.scalarLoad;
            m_stats.scalarLoadBytes += 1ULL << (funct3 & 0x3);
        }
        break;
    }
    case OPCODE_BRANCH:
        ++m_stats.branch;
        if (nextPc != pc + 4)
        {
            ++m_stats.branchTaken;
        }
        break;
    case OPCODE_JAL:
    case OPCODE_JALR:
        ++m_stats.jump;
        break;
    case OPCODE_OP_V:
        if (funct3 == 0x7)
        {
            ++m_stats.vectorConfig;
            const char *name = (instruction >> 31) == 0              ? "vsetvli"
                               : ((instruction >> 30) & 0x3) == 0x3 ? "vsetivli"
                                                                     : "vsetvl";
            ++m_stats.vectorByMnemonic[name];
        }
        else
        {
            classifyVectorArith(instruction);
        }
        break;
    default:
        ++m_stats.scalarOther;
        break;
    }
}

void RunStatsCollector::classifyVectorMemory(uint32_t instruction, bool isStore)
{
    unsigned eew = 8;
    isVectorWidth((instruction >> 12) & 0x7, eew);
    const uint64_t vl = m_registers.ReadCsr(RegisterFile::CSR_VL);
    const uint64_t vtype = m_registers.ReadCsr(RegisterFile::CSR_VTYPE);
    const unsigned sew = 8U << ((vtype >> 3) & 0x7);
    const unsigned mop = (instruction >> 26) & 0x3;
    const unsigned nf = ((instruction >> 29) & 0x7) + 1;
    const unsigned lumop = (instruction >> 20) & 0x1F;

    uint64_t elements = 0;
    uint64_t bytes = 0;
    std::string kind;
    if (mop == 0 && lumop == 0x08) // whole-register vl<nf>r / vs<nf>r
    {
        bytes = nf * m_vlenBits / 8;
        elements = bytes / (eew / 8);
        kind = "whole-register";
    }
    else if (mop == 0 && lumop == 0x0B) // vlm.v / vsm.v
    {
        bytes = (vl + 7) / 8;
        elements = vl;
        kind = "mask";
    }
    else if (mop == 0) // unit-stride, fault-only-first, unit-stride segment
    {
        elements = vl * nf;
        bytes = elements * eew / 8;
        kind = nf > 1 ? "segment" : (lumop == 0x10 ? "fault-only-first" : "unit-stride");
    }
    else // strided (mop 2) or indexed (mop 1, 3): data elements are EEW or SEW wide
    {
        elements = vl * nf;
        bytes = elements * (mop == 2 ? eew : sew) / 8;
        kind = mop == 2 ? "strided" : "indexed";
    }

    if (isStore)
    {
        ++m_stats.vectorStore;
        m_stats.vectorStoreBytes += bytes;
    }
    else
    {
        ++m_stats.vectorLoad;
        m_stats.vectorLoadBytes += bytes;
    }
    m_stats.vectorElements += elements;
    ++m_stats.vectorByMnemonic[std::string(isStore ? "store " : "load ") + kind];
}

void RunStatsCollector::classifyVectorArith(uint32_t instruction)
{
    ++m_stats.vectorCompute;
    const uint64_t vl = m_registers.ReadCsr(RegisterFile::CSR_VL);
    const uint64_t vtype = m_registers.ReadCsr(RegisterFile::CSR_VTYPE);
    const unsigned sew = 8U << ((vtype >> 3) & 0x7);

    const rvv::VectorArithInfo *info = rvv::DecodeVectorArith(instruction);
    uint64_t elements = vl;
    if (info)
    {
        if (info->op == rvv::VectorArithOp::WholeMove)
        {
            elements = info->factor * m_vlenBits / sew; // vmv<nr>r.v ignores vl
        }
        else if (info->op == rvv::VectorArithOp::MoveXS || info->op == rvv::VectorArithOp::MoveSX)
        {
            elements = 1;
        }
        ++m_stats.vectorByMnemonic[info->mnemonic];
    }
    else
    {
        ++m_stats.vectorByMnemonic["(unknown OP-V)"];
    }
    m_stats.vectorElements += elements;
}

std::string RunStatsCollector::write(const std::filesystem::path &path, const std::string &vmName,
                                     uint64_t cycles, uint64_t instructionsRetired,
                                     bool mixAvailable) const
{
    const RunStats &s = m_stats;
    const uint64_t vectorInstructions =
        s.vectorConfig + s.vectorLoad + s.vectorStore + s.vectorCompute;

    std::ostringstream json;
    json << "{\n";
    json << "  \"vm\": " << jsonString(vmName) << ",\n";
    json << "  \"vlen\": " << m_vlenBits << ",\n";
    json << "  \"cycles\": " << cycles << ",\n";
    json << "  \"instructions_retired\": " << instructionsRetired << ",\n";
    if (!mixAvailable)
    {
        json << "  \"instruction_mix\": null,\n  \"memory_bytes\": null,\n  \"vector\": null\n";
    }
    else
    {
        json << "  \"instruction_mix\": {\n"
             << "    \"scalar_alu\": " << s.scalarAlu << ",\n"
             << "    \"scalar_load\": " << s.scalarLoad << ",\n"
             << "    \"scalar_store\": " << s.scalarStore << ",\n"
             << "    \"branch\": " << s.branch << ",\n"
             << "    \"branch_taken\": " << s.branchTaken << ",\n"
             << "    \"jump\": " << s.jump << ",\n"
             << "    \"scalar_other\": " << s.scalarOther << ",\n"
             << "    \"vector_config\": " << s.vectorConfig << ",\n"
             << "    \"vector_load\": " << s.vectorLoad << ",\n"
             << "    \"vector_store\": " << s.vectorStore << ",\n"
             << "    \"vector_compute\": " << s.vectorCompute << "\n"
             << "  },\n";
        json << "  \"memory_bytes\": {\n"
             << "    \"scalar_load\": " << s.scalarLoadBytes << ",\n"
             << "    \"scalar_store\": " << s.scalarStoreBytes << ",\n"
             << "    \"vector_load\": " << s.vectorLoadBytes << ",\n"
             << "    \"vector_store\": " << s.vectorStoreBytes << "\n"
             << "  },\n";
        json << "  \"vector\": {\n"
             << "    \"instructions\": " << vectorInstructions << ",\n"
             << "    \"elements\": " << s.vectorElements << ",\n"
             << "    \"by_mnemonic\": {";
        bool first = true;
        for (const auto &[name, count] : s.vectorByMnemonic)
        {
            json << (first ? "\n" : ",\n") << "      " << jsonString(name) << ": " << count;
            first = false;
        }
        json << (first ? "}\n" : "\n    }\n") << "  }\n";
    }
    json << "}\n";
    std::ofstream(path) << json.str();

    std::ostringstream summary;
    summary << "Statistics (" << vmName << ", VLEN=" << m_vlenBits << "):\n"
            << "  cycles " << cycles << ", instructions retired " << instructionsRetired;
    if (instructionsRetired)
    {
        summary << ", CPI " << std::fixed << std::setprecision(3)
                << static_cast<double>(cycles) / static_cast<double>(instructionsRetired);
    }
    summary << "\n";
    if (mixAvailable)
    {
        summary << "  scalar: " << s.scalarAlu << " alu, " << s.scalarLoad + s.scalarStore
                << " load/store, " << s.branch << " branch (" << s.branchTaken << " taken), "
                << s.jump << " jump, " << s.scalarOther << " other\n"
                << "  vector: " << vectorInstructions << " instructions (" << s.vectorConfig
                << " vset, " << s.vectorLoad + s.vectorStore << " load/store, "
                << s.vectorCompute << " compute), " << s.vectorElements << " elements\n"
                << "  memory: " << s.scalarLoadBytes + s.vectorLoadBytes << " bytes loaded, "
                << s.scalarStoreBytes + s.vectorStoreBytes << " bytes stored\n";
    }
    else
    {
        summary << "  (instruction mix is collected on the single-cycle VM only: --vm rvss)\n";
    }
    return summary.str();
}

} // namespace Kites::cli
