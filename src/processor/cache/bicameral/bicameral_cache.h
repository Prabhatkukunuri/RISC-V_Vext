/**
 * @file bicameral_cache.h
 * @brief The Bicameral Cache: an L1 data cache split into a Scalar Cache and a Vector Cache.
 *
 * After "The Bicameral Cache: a split cache for vector architectures" (arXiv 2407.15440):
 *   - Scalar Cache (SC): set associative, short lines -> temporal locality of scalar data;
 *   - Vector Cache (VC): fully associative, long lines split into sectors -> spatial locality of
 *     vector data (see vector_cache.h for the two kinds of VC miss).
 * One SC line is one sector, the unit moved between L1 and L2.
 *
 * Cross lookup and one-sided migration (paper section III-C): a sector is never in both caches.
 *   vector access: VC hit -> served; else SC has the sector -> moved SC -> VC (migration);
 *                  else fetched from L2 into the VC.
 *   scalar access: SC hit -> served; else VC has the sector -> served from the VC in place
 *                  (cross hit, not moved); else fetched from L2 into the SC.
 *
 * Deliberate differences from the paper:
 *   - an L2 sits below (the paper connects the caches straight to the memory controller); every
 *     L1 <-> L2 transfer is still exactly one sector, like the paper's 512-bit bus;
 *   - no write buffers and no memory-side prefetch: Kites caches have no timing model, so dirty
 *     sectors are written back synchronously.
 */

#pragma once

#include "processor/cache/bicameral/bicameral_config.h"
#include "processor/cache/bicameral/vector_cache.h"
#include "processor/cache/cache.h"

#include <QObject>
#include <cstdint>

namespace Kites
{

class BicameralCache : public QObject
{
    Q_OBJECT
  public:
    explicit BicameralCache(MemoryDevice &next, const BicameralConfig &config = {});
    BicameralCache(const BicameralCache &) = delete;

    /// Writes back all dirty data, empties both caches, zeroes the counters and applies
    /// @p config. Throws std::invalid_argument if the SC line size isn't the sector size.
    void reconfigure(const BicameralConfig &config);
    [[nodiscard]] const BicameralConfig &getConfig() const { return m_config; }

    /// Little-endian access of @p size bytes (1, 2, 4 or 8).
    uint64_t scalarRead(uint64_t address, unsigned size);
    void scalarWrite(uint64_t address, unsigned size, uint64_t value);
    uint64_t vectorRead(uint64_t address, unsigned size);
    void vectorWrite(uint64_t address, unsigned size, uint64_t value);

    /// The byte a read would return (VC, then SC, then the next level), changing nothing.
    [[nodiscard]] uint8_t peekByte(uint64_t address);

    /// Writes every dirty line and sector to the next level; contents stay valid and clean.
    void writeBackAll();
    /// Invalidates both caches (nothing written back) and zeroes the counters.
    void reset();

    [[nodiscard]] BicameralStats getStats() const;
    Cache *getScalarCache() { return &m_scalar; }
    [[nodiscard]] const VectorCache &getVectorCache() const { return m_vector; }

    /**
     * @brief The smallest change to @p l2 that keeps L2 at least as big as L1: capacity at least
     * the SC plus the VC (and at least @p otherL1Bytes, e.g. the instruction cache), and lines at
     * least one sector long. L2 indices (sets) are doubled first; ways and policies are kept.
     */
    [[nodiscard]] static CacheConfig requiredL2(const BicameralConfig &config,
                                                const CacheConfig &l2, size_t otherL1Bytes = 0);

  signals:
    /// Something in either cache changed (contents or counters).
    void bicameralUpdatedSignal();

  private:
    /// Makes the sector holding @p address valid in the VC; returns false only for a store miss
    /// under no-write-allocate, which then goes straight to the next level.
    bool ensureVectorSector(uint64_t address, bool isWrite);
    uint64_t scalarReadUnit(uint64_t address, unsigned size);
    void scalarWriteUnit(uint64_t address, unsigned size, uint64_t value);
    uint64_t vectorReadUnit(uint64_t address, unsigned size);
    void vectorWriteUnit(uint64_t address, unsigned size, uint64_t value);
    /// True if [address, address + size) lies in one sector.
    [[nodiscard]] bool inOneSector(uint64_t address, unsigned size) const;

    MemoryDevice &m_next;
    BicameralConfig m_config;
    Cache m_scalar;
    VectorCache m_vector;

    size_t m_vectorHits         = 0;
    size_t m_vectorSectorMisses = 0;
    size_t m_vectorLineMisses   = 0;
    size_t m_migrations         = 0;
    size_t m_crossHits          = 0;
};

} // namespace Kites
