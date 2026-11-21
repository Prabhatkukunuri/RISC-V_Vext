/**
 * @file bicameral_cache.cpp
 * @brief The Bicameral Cache: cross lookup and one-sided SC -> VC migration.
 */

#include "processor/cache/bicameral/bicameral_cache.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace Kites
{
namespace
{
void validate(const BicameralConfig &config)
{
    if (const std::string error = config.validationError(); !error.empty())
    {
        throw std::invalid_argument("Bicameral Cache: " + error);
    }
}

std::array<uint8_t, 8> toBytes(uint64_t value)
{
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    return bytes;
}
} // namespace

BicameralCache::BicameralCache(MemoryDevice &next, const BicameralConfig &config)
    : QObject(nullptr), m_next(next), m_config(config),
      m_scalar(next, config.scalar.lineCount, config.scalar.lineSizeInBytes,
               config.scalar.wayCount, config.scalar.writePolicy, config.scalar.allocationPolicy,
               config.scalar.replacementPolicy),
      m_vector(next, config.vectorLineCount, config.sectorsPerLine, config.sectorSize,
               config.vectorWritePolicy, config.vectorAllocationPolicy,
               config.vectorReplacementPolicy)
{
    validate(config);
}

void BicameralCache::reconfigure(const BicameralConfig &config)
{
    validate(config);
    // Nothing the program wrote may be lost: write both caches back before dropping them.
    m_vector.writeBackAll();
    m_scalar.reconfigure(config.scalar); // writes back and invalidates the SC itself
    m_vector.configure(config.vectorLineCount, config.sectorsPerLine, config.sectorSize,
                       config.vectorWritePolicy, config.vectorAllocationPolicy,
                       config.vectorReplacementPolicy);
    m_config = config;
    reset();
}

bool BicameralCache::inOneSector(uint64_t address, unsigned size) const
{
    return m_vector.sectorBase(address) == m_vector.sectorBase(address + size - 1);
}

// ── Scalar side ──────────────────────────────────────────────────────────────

uint64_t BicameralCache::scalarRead(uint64_t address, unsigned size)
{
    uint64_t value = 0;
    if (inOneSector(address, size))
    {
        value = scalarReadUnit(address, size);
    }
    else // misaligned across two sectors: one byte at a time, each looked up on its own
    {
        for (unsigned i = 0; i < size; ++i)
        {
            value |= scalarReadUnit(address + i, 1) << (8 * i);
        }
    }
    emit bicameralUpdatedSignal();
    return value;
}

void BicameralCache::scalarWrite(uint64_t address, unsigned size, uint64_t value)
{
    if (inOneSector(address, size))
    {
        scalarWriteUnit(address, size, value);
    }
    else
    {
        for (unsigned i = 0; i < size; ++i)
        {
            scalarWriteUnit(address + i, 1, (value >> (8 * i)) & 0xFF);
        }
    }
    emit bicameralUpdatedSignal();
}

uint64_t BicameralCache::scalarReadUnit(uint64_t address, unsigned size)
{
    // SC first; on an SC miss look in the VC before going to L2 (cross lookup).
    if (!m_scalar.probe(address, size) && m_vector.hasSector(address))
    {
        ++m_crossHits; // one-sided: the sector stays in the VC
        m_vector.touch(address);
        uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i)
        {
            value |= static_cast<uint64_t>(m_vector.readByte(address + i)) << (8 * i);
        }
        return value;
    }
    switch (size) // SC hit, or a miss in both: the SC fills the line from L2
    {
    case 1:
        return m_scalar.readByte(address);
    case 2:
        return m_scalar.readHalfWord(address);
    case 4:
        return m_scalar.readWord(address);
    default:
        return m_scalar.readDoubleWord(address);
    }
}

void BicameralCache::scalarWriteUnit(uint64_t address, unsigned size, uint64_t value)
{
    if (!m_scalar.probe(address, size) && m_vector.hasSector(address))
    {
        ++m_crossHits;
        m_vector.touch(address);
        const auto bytes = toBytes(value);
        m_vector.writeBytes(address, std::span<const uint8_t>(bytes.data(), size));
        return;
    }
    switch (size)
    {
    case 1:
        m_scalar.writeByte(address, static_cast<uint8_t>(value));
        break;
    case 2:
        m_scalar.writeHalfWord(address, static_cast<uint16_t>(value));
        break;
    case 4:
        m_scalar.writeWord(address, static_cast<uint32_t>(value));
        break;
    default:
        m_scalar.writeDoubleWord(address, value);
        break;
    }
}

// ── Vector side ──────────────────────────────────────────────────────────────

bool BicameralCache::ensureVectorSector(uint64_t address, bool isWrite)
{
    const VectorCache::Lookup lookup = m_vector.lookup(address);
    if (lookup == VectorCache::Lookup::Hit)
    {
        ++m_vectorHits;
        m_vector.touch(address);
        return true;
    }
    (lookup == VectorCache::Lookup::SectorMiss ? m_vectorSectorMisses : m_vectorLineMisses)++;

    const uint64_t sector = m_vector.sectorBase(address);
    const bool inScalar = m_scalar.probe(sector, 1); // an SC line is exactly one sector
    if (isWrite && !inScalar && m_config.vectorAllocationPolicy == AllocationPolicy::NoWriteAllocate)
    {
        return false; // the store goes straight to L2; nothing is allocated or evicted
    }

    const size_t line = m_vector.lineFor(address); // a line miss evicts a whole line here
    if (inScalar)
    {
        // One-sided migration: move the sector out of the SC, keeping its dirty bit.
        const std::optional<CacheLine> taken = m_scalar.extractLine(sector);
        m_vector.installSector(line, address, taken->data, taken->dirty);
        ++m_migrations;
    }
    else
    {
        m_vector.fetchSector(line, address); // exactly one sector from L2
    }
    m_vector.touch(address);
    return true;
}

uint64_t BicameralCache::vectorReadUnit(uint64_t address, unsigned size)
{
    ensureVectorSector(address, false);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
    {
        value |= static_cast<uint64_t>(m_vector.readByte(address + i)) << (8 * i);
    }
    return value;
}

void BicameralCache::vectorWriteUnit(uint64_t address, unsigned size, uint64_t value)
{
    const auto bytes = toBytes(value);
    const std::span<const uint8_t> data(bytes.data(), size);
    if (ensureVectorSector(address, true))
    {
        m_vector.writeBytes(address, data);
    }
    else
    {
        m_next.writeLine(address, data);
    }
}

uint64_t BicameralCache::vectorRead(uint64_t address, unsigned size)
{
    uint64_t value = 0;
    if (inOneSector(address, size))
    {
        value = vectorReadUnit(address, size);
    }
    else
    {
        for (unsigned i = 0; i < size; ++i)
        {
            value |= vectorReadUnit(address + i, 1) << (8 * i);
        }
    }
    emit bicameralUpdatedSignal();
    return value;
}

void BicameralCache::vectorWrite(uint64_t address, unsigned size, uint64_t value)
{
    if (inOneSector(address, size))
    {
        vectorWriteUnit(address, size, value);
    }
    else
    {
        for (unsigned i = 0; i < size; ++i)
        {
            vectorWriteUnit(address + i, 1, (value >> (8 * i)) & 0xFF);
        }
    }
    emit bicameralUpdatedSignal();
}

// ── Bookkeeping ──────────────────────────────────────────────────────────────

uint8_t BicameralCache::peekByte(uint64_t address)
{
    if (m_vector.hasSector(address))
    {
        return m_vector.readByte(address);
    }
    return m_scalar.peekByte(address); // the SC, else its next level
}

void BicameralCache::writeBackAll()
{
    m_scalar.writeBackAll();
    m_vector.writeBackAll();
    emit bicameralUpdatedSignal();
}

void BicameralCache::reset()
{
    m_scalar.reset();
    m_vector.reset();
    m_vectorHits         = 0;
    m_vectorSectorMisses = 0;
    m_vectorLineMisses   = 0;
    m_migrations         = 0;
    m_crossHits          = 0;
    emit bicameralUpdatedSignal();
}

BicameralStats BicameralCache::getStats() const
{
    BicameralStats stats;
    stats.scalarHits         = m_scalar.getHitCount();
    stats.scalarMisses       = m_scalar.getMissCount();
    stats.scalarWriteBacks   = m_scalar.getWriteBackCount();
    stats.vectorHits         = m_vectorHits;
    stats.vectorSectorMisses = m_vectorSectorMisses;
    stats.vectorLineMisses   = m_vectorLineMisses;
    stats.vectorWriteBacks   = m_vector.getSectorWriteBackCount();
    stats.migrations         = m_migrations;
    stats.crossHits          = m_crossHits;
    return stats;
}

CacheConfig BicameralCache::requiredL2(const BicameralConfig &config, const CacheConfig &l2,
                                       size_t otherL1Bytes)
{
    CacheConfig result     = l2;
    result.lineSizeInBytes = std::max(l2.lineSizeInBytes, config.sectorSize);
    const size_t needed    = std::max(config.totalBytes(), otherL1Bytes);
    while (result.lineCount * result.wayCount * result.lineSizeInBytes < needed)
    {
        result.lineCount *= 2;
    }
    return result;
}

} // namespace Kites
