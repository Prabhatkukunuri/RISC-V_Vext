/**
 * @file vector_cache.cpp
 * @brief The Vector Cache of the Bicameral Cache.
 */

#include "processor/cache/bicameral/vector_cache.h"

#include "processor/cache/cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Kites
{

VectorCache::VectorCache(MemoryDevice &next, size_t lineCount, size_t sectorsPerLine,
                         size_t sectorSize, WritePolicy writePolicy,
                         AllocationPolicy allocationPolicy, ReplacementPolicy replacementPolicy)
    : m_next(next)
{
    configure(lineCount, sectorsPerLine, sectorSize, writePolicy, allocationPolicy,
              replacementPolicy);
}

void VectorCache::configure(size_t lineCount, size_t sectorsPerLine, size_t sectorSize,
                            WritePolicy writePolicy, AllocationPolicy allocationPolicy,
                            ReplacementPolicy replacementPolicy)
{
    if (lineCount == 0 || !std::has_single_bit(sectorsPerLine) || !std::has_single_bit(sectorSize))
    {
        throw std::invalid_argument("Vector Cache: need at least one line, and powers of two for "
                                    "sectors per line and sector size");
    }
    m_sectorsPerLine   = sectorsPerLine;
    m_sectorSize       = sectorSize;
    m_sectorBits       = static_cast<size_t>(std::countr_zero(sectorSize));
    m_lineBits         = m_sectorBits + static_cast<size_t>(std::countr_zero(sectorsPerLine));
    m_writePolicy      = writePolicy;
    m_allocationPolicy = allocationPolicy;
    m_policy           = makeReplacementPolicy(replacementPolicy);
    m_lines.assign(lineCount, VectorCacheLine{});
    reset();
}

ReplacementPolicy VectorCache::getReplacementPolicy() const
{
    return m_policy->type();
}

void VectorCache::reset()
{
    for (auto &line : m_lines)
    {
        line = VectorCacheLine{};
        line.data.assign(getLineSizeInBytes(), 0);
        line.sectorValid.assign(m_sectorsPerLine, 0);
        line.sectorDirty.assign(m_sectorsPerLine, 0);
    }
    m_clock            = 0;
    m_sectorWriteBacks = 0;
}

size_t VectorCache::findLine(uint64_t address) const
{
    const uint64_t tag = getTag(address);
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        if (m_lines[i].valid && m_lines[i].tag == tag)
        {
            return i;
        }
    }
    return m_lines.size();
}

VectorCache::Lookup VectorCache::lookup(uint64_t address) const
{
    const size_t line = findLine(address);
    if (line == m_lines.size())
    {
        return Lookup::LineMiss;
    }
    return m_lines[line].sectorValid[getSector(address)] ? Lookup::Hit : Lookup::SectorMiss;
}

bool VectorCache::hasSector(uint64_t address) const
{
    return lookup(address) == Lookup::Hit;
}

CacheLineView VectorCache::viewOf(const VectorCacheLine &line) const
{
    const bool dirty = std::any_of(line.sectorDirty.begin(), line.sectorDirty.end(),
                                   [](uint8_t d) { return d != 0; });
    return {line.valid,      line.tag,        line.age, line.frequency,
            line.insertTime, line.lastAccess, dirty};
}

CacheContextView VectorCache::context() const
{
    // One set, every line a way; blockSize is the line size.
    return {1, m_lines.size(), getLineSizeInBytes(), m_clock};
}

size_t VectorCache::lineFor(uint64_t address)
{
    const size_t found = findLine(address);
    if (found != m_lines.size())
    {
        return found;
    }

    // Line miss: an invalid line if there is one, else ask the replacement policy.
    size_t victim = m_lines.size();
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        if (!m_lines[i].valid)
        {
            victim = i;
            break;
        }
    }
    const CacheRequestView request{address, 0, 0, getLineOffset(address), 1, false,
                                   getTag(address)};
    if (victim == m_lines.size())
    {
        std::vector<CacheLineView> views;
        views.reserve(m_lines.size());
        for (const auto &line : m_lines)
        {
            views.push_back(viewOf(line));
        }
        victim = m_policy->chooseVictim(views, request, context());
        m_policy->onEvict(views[victim], request, context());
        for (size_t sector = 0; sector < m_sectorsPerLine; ++sector)
        {
            writeBackSector(victim, sector); // only valid + dirty sectors are written
        }
    }

    VectorCacheLine &line = m_lines[victim];
    line.valid      = true;
    line.tag        = getTag(address);
    line.insertTime = ++m_clock;
    line.lastAccess = line.insertTime;
    line.age        = line.insertTime;
    line.frequency  = 0;
    std::fill(line.sectorValid.begin(), line.sectorValid.end(), 0);
    std::fill(line.sectorDirty.begin(), line.sectorDirty.end(), 0);
    m_policy->onInsert(viewOf(line), request, context());
    return victim;
}

void VectorCache::fetchSector(size_t line, uint64_t address)
{
    const uint64_t base = sectorBase(address);
    const std::span<const uint8_t> bytes = m_next.readLine(base, m_sectorSize);
    installSector(line, address, bytes, false);
}

void VectorCache::installSector(size_t line, uint64_t address, std::span<const uint8_t> bytes,
                                bool dirty)
{
    if (bytes.size() != m_sectorSize)
    {
        throw std::invalid_argument("Vector Cache: a sector must be exactly " +
                                    std::to_string(m_sectorSize) + " bytes");
    }
    VectorCacheLine &target = m_lines[line];
    const size_t sector = getSector(address);
    std::memcpy(target.data.data() + sector * m_sectorSize, bytes.data(), m_sectorSize);
    target.sectorValid[sector] = 1;
    target.sectorDirty[sector] = dirty ? 1 : 0;
}

void VectorCache::touch(uint64_t address)
{
    const size_t index = findLine(address);
    if (index == m_lines.size())
    {
        return;
    }
    VectorCacheLine &line = m_lines[index];
    line.lastAccess = ++m_clock;
    line.age        = line.lastAccess;
    ++line.frequency;
    const CacheRequestView request{address, 0, index, getLineOffset(address), 1, false, line.tag};
    m_policy->onAccess(viewOf(line), request, context());
}

uint8_t VectorCache::readByte(uint64_t address) const
{
    const size_t index = findLine(address);
    if (index == m_lines.size() || !m_lines[index].sectorValid[getSector(address)])
    {
        throw std::logic_error("Vector Cache: read of a sector that is not present");
    }
    return m_lines[index].data[getLineOffset(address)];
}

void VectorCache::writeBytes(uint64_t address, std::span<const uint8_t> bytes)
{
    const size_t index = findLine(address);
    const size_t sector = getSector(address);
    if (index == m_lines.size() || !m_lines[index].sectorValid[sector] ||
        getSector(address + bytes.size() - 1) != sector)
    {
        throw std::logic_error("Vector Cache: write outside one present sector");
    }
    VectorCacheLine &line = m_lines[index];
    std::memcpy(line.data.data() + getLineOffset(address), bytes.data(), bytes.size());
    if (m_writePolicy == WritePolicy::WriteThrough)
    {
        m_next.writeLine(address, bytes);
    }
    else
    {
        line.sectorDirty[sector] = 1;
    }
}

void VectorCache::writeBackSector(size_t lineIndex, size_t sector)
{
    VectorCacheLine &line = m_lines[lineIndex];
    if (!line.valid || !line.sectorValid[sector] || !line.sectorDirty[sector])
    {
        return;
    }
    const uint64_t base = (line.tag << m_lineBits) | (static_cast<uint64_t>(sector) << m_sectorBits);
    m_next.writeLine(base, std::span<const uint8_t>(line.data.data() + sector * m_sectorSize,
                                                    m_sectorSize));
    line.sectorDirty[sector] = 0;
    ++m_sectorWriteBacks;
}

void VectorCache::writeBackAll()
{
    for (size_t line = 0; line < m_lines.size(); ++line)
    {
        for (size_t sector = 0; sector < m_sectorsPerLine; ++sector)
        {
            writeBackSector(line, sector);
        }
    }
}

} // namespace Kites
