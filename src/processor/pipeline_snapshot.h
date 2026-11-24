/**
 * @file pipeline_snapshot.h
 * @brief What the four pipeline registers of a 5-stage VM hold at the end of one clock cycle.
 * The pipeline view builds its space-time diagram from one of these per cycle.
 */
#pragma once

#include <QMetaType>

#include <array>
#include <cstdint>
#include <string>

namespace Kites
{
struct PipelineSnapshot
{
    uint64_t cycle = 0; ///< cycles completed (cycle_s_ after the step)
    /// PCs in IF/ID, ID/EX, EX/MEM, MEM/WB; INVALID_PC for a bubble.
    std::array<uint64_t, 4> pcs{};
    /// Disassembly of each PC ("" for a bubble).
    std::array<std::string, 4> texts{};
};
} // namespace Kites

Q_DECLARE_METATYPE(Kites::PipelineSnapshot)
