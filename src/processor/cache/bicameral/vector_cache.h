/**
 * @file vector_cache.h
 * @brief The Vector Cache (VC) of the Bicameral Cache: fully associative, long sectored lines.
 *
 * An address splits into  | tag | sector | byte in sector |  (no set index: one set).
 * Each line has one tag and, per sector, a valid bit and a dirty bit, so:
 *   - sector miss: the line's tag matches but the sector is invalid -> fetch that one sector from
 *     the next level; nothing is evicted;
 *   - line miss: no line has the tag -> evict a whole line (writing back only its dirty sectors,
 *     one sector at a time), install the tag with every sector invalid, fetch one sector.
 * Every transfer to or from the next level is exactly one sector.
 *
 * The VC only stores and moves data; the lookup order, the SC -> VC migration and the statistics
 * belong to BicameralCache.
 */

#pragma once

#include "processor/cache/cacheconfig.h"
#include "processor/cache/policies/cache_replacement_policy.h"
#include "processor/memory_device.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Kites
{

struct VectorCacheLine
{
    bool valid   = false;
    uint64_t tag = 0;

    // replacement metadata, same meaning as in CacheLine
    uint64_t age        = 0;
    uint64_t insertTime = 0;
    uint64_t lastAccess = 0;
    uint64_t frequency  = 0;

    std::vector<uint8_t> data;        ///< sectorsPerLine * sectorSize bytes
    std::vector<uint8_t> sectorValid; ///< one flag per sector
    std::vector<uint8_t> sectorDirty; ///< one flag per sector
};

class VectorCache
{
  public:
    enum class Lookup
    {
        Hit,
        SectorMiss,
        LineMiss
    };

    VectorCache(MemoryDevice &next, size_t lineCount, size_t sectorsPerLine, size_t sectorSize,
                WritePolicy writePolicy, AllocationPolicy allocationPolicy,
                ReplacementPolicy replacementPolicy);

    /// Drops all contents (without writing anything back) and sets a new geometry and policies.
    void configure(size_t lineCount, size_t sectorsPerLine, size_t sectorSize,
                   WritePolicy writePolicy, AllocationPolicy allocationPolicy,
                   ReplacementPolicy replacementPolicy);

    /// Classifies an access to @p address without changing anything.
    [[nodiscard]] Lookup lookup(uint64_t address) const;
    /// True if the sector holding @p address is present and valid.
    [[nodiscard]] bool hasSector(uint64_t address) const;

    /// Line index for @p address's block, allocating one (line miss) if needed: the victim's
    /// dirty sectors are written back first. Returns the line index.
    size_t lineFor(uint64_t address);
    /// Fetches the sector holding @p address from the next level into line @p line.
    void fetchSector(size_t line, uint64_t address);
    /// Puts @p bytes (one whole sector, e.g. migrated from the Scalar Cache) into line @p line.
    void installSector(size_t line, uint64_t address, std::span<const uint8_t> bytes, bool dirty);
    /// Marks the line holding @p address as just used (replacement bookkeeping).
    void touch(uint64_t address);

    /// Byte access to a sector that is valid (hasSector must be true).
    [[nodiscard]] uint8_t readByte(uint64_t address) const;
    /// Writes into a valid sector; write-back marks it dirty, write-through also writes the bytes
    /// to the next level.
    void writeBytes(uint64_t address, std::span<const uint8_t> bytes);

    /// Writes every dirty sector back; everything stays valid and becomes clean.
    void writeBackAll();
    /// Invalidates everything (nothing is written back) and zeroes the counters.
    void reset();

    [[nodiscard]] size_t getLineCount() const { return m_lines.size(); }
    [[nodiscard]] size_t getSectorsPerLine() const { return m_sectorsPerLine; }
    [[nodiscard]] size_t getSectorSize() const { return m_sectorSize; }
    [[nodiscard]] size_t getLineSizeInBytes() const { return m_sectorsPerLine * m_sectorSize; }
    [[nodiscard]] size_t getSectorWriteBackCount() const { return m_sectorWriteBacks; }
    [[nodiscard]] WritePolicy getWritePolicy() const { return m_writePolicy; }
    [[nodiscard]] AllocationPolicy getAllocationPolicy() const { return m_allocationPolicy; }
    [[nodiscard]] ReplacementPolicy getReplacementPolicy() const;
    [[nodiscard]] const VectorCacheLine &getLine(size_t index) const { return m_lines[index]; }

    // Address decomposition: | tag | sector | byte |
    [[nodiscard]] uint64_t getTag(uint64_t address) const { return address >> m_lineBits; }
    [[nodiscard]] size_t getSector(uint64_t address) const
    {
        return static_cast<size_t>(address >> m_sectorBits) & (m_sectorsPerLine - 1);
    }
    [[nodiscard]] size_t getLineOffset(uint64_t address) const
    {
        return static_cast<size_t>(address & ((uint64_t{1} << m_lineBits) - 1));
    }
    [[nodiscard]] uint64_t sectorBase(uint64_t address) const
    {
        return address & ~((uint64_t{1} << m_sectorBits) - 1);
    }
    /// The line holding @p address's block, or getLineCount() if none.
    [[nodiscard]] size_t findLine(uint64_t address) const;

  private:
    void writeBackSector(size_t line, size_t sector);
    CacheLineView viewOf(const VectorCacheLine &line) const;
    CacheContextView context() const;

    MemoryDevice &m_next;
    std::vector<VectorCacheLine> m_lines;
    size_t m_sectorsPerLine = 0;
    size_t m_sectorSize     = 0;
    size_t m_sectorBits     = 0; ///< log2(sectorSize)
    size_t m_lineBits       = 0; ///< log2(sectorsPerLine * sectorSize)
    WritePolicy m_writePolicy{};
    AllocationPolicy m_allocationPolicy{};
    std::unique_ptr<CacheReplacementPolicy> m_policy;
    uint64_t m_clock            = 0;
    size_t m_sectorWriteBacks   = 0;
};

} // namespace Kites
