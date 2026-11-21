/**
 * @file bicameral_config.h
 * @brief Geometry and policies of the Bicameral Cache (Scalar Cache + Vector Cache).
 */

#pragma once

#include "processor/cache/cacheconfig.h"

#include <cstddef>
#include <string>

namespace Kites
{

/**
 * @brief Configuration of the Bicameral Cache.
 *
 * The L1 is split in two equal halves. The default is 8 KB:
 *   Scalar Cache  16 indices x 4 ways x 64 B lines           = 4 KB
 *   Vector Cache  4 lines x 16 sectors x 64 B (1 KB lines)   = 4 KB
 * A Scalar Cache line is one sector, so sectorSize must equal scalar.lineSizeInBytes.
 * fromL1Size() derives the whole geometry from the L1 size, the sector size, the sectors per
 * Vector Cache line and the Scalar Cache associativity; validationError() checks the rules.
 */
struct BicameralConfig
{
    CacheConfig scalar{
        .lineCount         = 16, // indices (sets)
        .lineSizeInBytes   = 64,
        .wayCount          = 4,
        .writePolicy       = WritePolicy::WriteBack,
        .allocationPolicy  = AllocationPolicy::WriteAllocate,
        .replacementPolicy = ReplacementPolicy::LRU,
    };

    size_t vectorLineCount = 4;  ///< fully associative: every line is a way of the single set
    size_t sectorsPerLine  = 16;
    size_t sectorSize      = 64; ///< bytes; the unit moved between L1 and L2

    WritePolicy vectorWritePolicy             = WritePolicy::WriteBack;
    AllocationPolicy vectorAllocationPolicy   = AllocationPolicy::WriteAllocate;
    ReplacementPolicy vectorReplacementPolicy = ReplacementPolicy::LRU;

    [[nodiscard]] size_t scalarBytes() const
    {
        return scalar.lineCount * scalar.wayCount * scalar.lineSizeInBytes;
    }
    [[nodiscard]] size_t vectorLineBytes() const { return sectorsPerLine * sectorSize; }
    [[nodiscard]] size_t vectorBytes() const { return vectorLineCount * vectorLineBytes(); }
    [[nodiscard]] size_t totalBytes() const { return scalarBytes() + vectorBytes(); }

    /**
     * @brief Geometry derived from the L1 size: each half is l1Bytes / 2. The Vector Cache is
     * fully associative with (l1Bytes / 2) / (sectorSize * sectorsPerLine) lines; the Scalar
     * Cache has sectorSize-byte lines, scalarWays ways and (l1Bytes / 2) / (sectorSize *
     * scalarWays) indices. Policies keep their defaults. A count that comes out as 0 means the
     * combination does not fit, which validationError() reports.
     */
    [[nodiscard]] static BicameralConfig fromL1Size(size_t l1Bytes, size_t sectorSize,
                                                    size_t sectorsPerLine, size_t scalarWays)
    {
        BicameralConfig config;
        const size_t half              = l1Bytes / 2;
        config.sectorSize              = sectorSize;
        config.sectorsPerLine          = sectorsPerLine;
        config.scalar.lineSizeInBytes  = sectorSize;
        config.scalar.wayCount         = scalarWays;
        const size_t scalarIndexBytes  = sectorSize * scalarWays;
        const size_t vectorLineBytes   = sectorSize * sectorsPerLine;
        config.scalar.lineCount        = scalarIndexBytes ? half / scalarIndexBytes : 0;
        config.vectorLineCount         = vectorLineBytes ? half / vectorLineBytes : 0;
        return config;
    }

    /// Empty if the geometry is valid, otherwise why it is not.
    [[nodiscard]] std::string validationError() const
    {
        if (sectorSize == 0 || sectorsPerLine == 0 || scalar.wayCount == 0)
        {
            return "the sector size, sectors per line and associativity must be at least 1";
        }
        if (scalar.lineSizeInBytes != sectorSize)
        {
            return "the Scalar Cache line size (" + std::to_string(scalar.lineSizeInBytes) +
                   " B) must equal the sector size (" + std::to_string(sectorSize) + " B)";
        }
        if (vectorLineCount == 0)
        {
            return "one Vector Cache line (" + std::to_string(vectorLineBytes()) +
                   " B) does not fit in half of the L1";
        }
        if (scalar.lineCount == 0)
        {
            return "one Scalar Cache index (" +
                   std::to_string(scalar.wayCount * scalar.lineSizeInBytes) +
                   " B = ways x sector size) does not fit in half of the L1";
        }
        if (scalarBytes() != vectorBytes())
        {
            return "the Scalar Cache (" + std::to_string(scalarBytes()) +
                   " B) and the Vector Cache (" + std::to_string(vectorBytes()) +
                   " B) must be the same size";
        }
        return {};
    }
};

/**
 * @brief Counters of the Bicameral Cache. Scalar accesses served by the Scalar Cache itself are
 * counted by its Cache (scalarHits/scalarMisses); the rest are counted here.
 */
struct BicameralStats
{
    size_t scalarHits       = 0; ///< scalar access found in the SC
    size_t scalarMisses     = 0; ///< scalar access found in neither cache (filled into the SC)
    size_t scalarWriteBacks = 0; ///< SC lines written back to L2

    size_t vectorHits         = 0; ///< vector access whose sector is valid in the VC
    size_t vectorSectorMisses = 0; ///< VC line present, sector invalid (nothing evicted)
    size_t vectorLineMisses   = 0; ///< no VC line for the block (a whole line evicted)
    size_t vectorWriteBacks   = 0; ///< dirty sectors written back to L2, one per sector

    size_t migrations = 0; ///< vector access found its sector in the SC: moved SC -> VC
    size_t crossHits  = 0; ///< scalar access found its sector in the VC: served there, not moved
};

} // namespace Kites
