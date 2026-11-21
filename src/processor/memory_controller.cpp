#include "memory_controller.h"

namespace Kites
{
    
MemoryController::MemoryController() : 
l2_cache_(static_cast<MemoryDevice&>(memory_)), 
l1_cache_(static_cast<MemoryDevice&>(l2_cache_)), 
instruction_cache_(static_cast<MemoryDevice&>(l2_cache_)), // casting needed here otherwise
bicameral_cache_(static_cast<MemoryDevice&>(l2_cache_))
{}                                                        // compiler think we are calling 
                                                          // copy constructor


void MemoryController::reset()
{
    memory_.reset();
    l1_cache_.reset();
    l2_cache_.reset();
    instruction_cache_.reset();
    bicameral_cache_.reset();
    emit memoryResetSignal(); // this will notify views to reset themselves
}

void MemoryController::writeByte(uint64_t address, uint8_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.scalarWrite(address, 1, value);
    }
    else
    {
        l1_cache_.writeByte(address, value);
    }
    emit memoryUpdated(address, 1);
}

void MemoryController::writeHalfWord(uint64_t address, uint16_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.scalarWrite(address, 2, value);
    }
    else
    {
        l1_cache_.writeHalfWord(address, value);
    }
    emit memoryUpdated(address, 2);
}

void MemoryController::writeWord(uint64_t address, uint32_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.scalarWrite(address, 4, value);
    }
    else
    {
        l1_cache_.writeWord(address, value);
    }
    emit memoryUpdated(address, 4);
}

void MemoryController::writeDoubleWord(uint64_t address, uint64_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.scalarWrite(address, 8, value);
    }
    else
    {
        l1_cache_.writeDoubleWord(address, value);
    }
    emit memoryUpdated(address, 8);
}

uint8_t MemoryController::readByte(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint8_t>(bicameral_cache_.scalarRead(address, 1));
    }
    return l1_cache_.readByte(address);
}

uint8_t MemoryController::vectorReadByte(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint8_t>(bicameral_cache_.vectorRead(address, 1));
    }
    return l1_cache_.readByte(address);
}

void MemoryController::vectorWriteByte(uint64_t address, uint8_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.vectorWrite(address, 1, value);
    }
    else
    {
        l1_cache_.writeByte(address, value);
    }
    emit memoryUpdated(address, 1);
}

uint16_t MemoryController::readHalfWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint16_t>(bicameral_cache_.scalarRead(address, 2));
    }
    return l1_cache_.readHalfWord(address);
}

uint16_t MemoryController::vectorReadHalfWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint16_t>(bicameral_cache_.vectorRead(address, 2));
    }
    return l1_cache_.readHalfWord(address);
}

void MemoryController::vectorWriteHalfWord(uint64_t address, uint16_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.vectorWrite(address, 2, value);
    }
    else
    {
        l1_cache_.writeHalfWord(address, value);
    }
    emit memoryUpdated(address, 2);
}

uint32_t MemoryController::readWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint32_t>(bicameral_cache_.scalarRead(address, 4));
    }
    return l1_cache_.readWord(address);
}

uint32_t MemoryController::vectorReadWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint32_t>(bicameral_cache_.vectorRead(address, 4));
    }
    return l1_cache_.readWord(address);
}

void MemoryController::vectorWriteWord(uint64_t address, uint32_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.vectorWrite(address, 4, value);
    }
    else
    {
        l1_cache_.writeWord(address, value);
    }
    emit memoryUpdated(address, 4);
}

uint64_t MemoryController::readDoubleWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint64_t>(bicameral_cache_.scalarRead(address, 8));
    }
    return l1_cache_.readDoubleWord(address);
}

uint64_t MemoryController::vectorReadDoubleWord(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return static_cast<uint64_t>(bicameral_cache_.vectorRead(address, 8));
    }
    return l1_cache_.readDoubleWord(address);
}

void MemoryController::vectorWriteDoubleWord(uint64_t address, uint64_t value)
{
    if (bicameral_enabled_)
    {
        bicameral_cache_.vectorWrite(address, 8, value);
    }
    else
    {
        l1_cache_.writeDoubleWord(address, value);
    }
    emit memoryUpdated(address, 8);
}

// Functions to read memory directly with cache bypass

uint8_t MemoryController::readByte_d(uint64_t address)
{
    return memory_.readByte(address);
}

uint16_t MemoryController::readHalfWord_d(uint64_t address)
{
    return memory_.readHalfWord(address);
}

uint32_t MemoryController::readWord_d(uint64_t address)
{
    return memory_.readWord(address);
}

uint64_t MemoryController::readDoubleWord_d(uint64_t address)
{
    return memory_.readDoubleWord(address);
}

uint8_t MemoryController::peekByte(uint64_t address)
{
    if (bicameral_enabled_)
    {
        return bicameral_cache_.peekByte(address);
    }
    return l1_cache_.peekByte(address);
}

// ── Bicameral Cache ──────────────────────────────────────────────────────────

void MemoryController::ensureL2FitsBicameral()
{
    const CacheConfig current  = l2_cache_.getConfig();
    const CacheConfig required = BicameralCache::requiredL2(
        bicameral_cache_.getConfig(), current, instruction_cache_.getCacheSizeInBytes());
    if (required.lineCount != current.lineCount ||
        required.lineSizeInBytes != current.lineSizeInBytes)
    {
        l2_cache_.reconfigure(required); // writes L2's own dirty lines back first
    }
}

void MemoryController::setBicameralEnabled(bool enabled)
{
    if (enabled == bicameral_enabled_)
    {
        return;
    }
    // The cache being switched off may hold data newer than L2: write it back and empty it, so
    // the other one never reads a stale copy.
    if (enabled)
    {
        l1_cache_.flush();
    }
    else
    {
        bicameral_cache_.writeBackAll();
        bicameral_cache_.reset();
    }
    bicameral_enabled_ = enabled;
    if (enabled)
    {
        ensureL2FitsBicameral();
    }
    emit bicameralEnabledChangedSignal(enabled);
}

bool MemoryController::isBicameralEnabled() const
{
    return bicameral_enabled_;
}

void MemoryController::reconfigureBicameral(const BicameralConfig &config)
{
    bicameral_cache_.reconfigure(config);
    if (bicameral_enabled_)
    {
        ensureL2FitsBicameral();
    }
}

BicameralCache *MemoryController::getBicameralCache()
{
    return &bicameral_cache_;
}

void MemoryController::writeBackAllCaches()
{
    l1_cache_.writeBackAll();
    bicameral_cache_.writeBackAll();
    l2_cache_.writeBackAll(); // after L1, which may have just written into L2
}

// function to read from instruction cache
uint32_t MemoryController::readInstruction(uint64_t address)
{
    return instruction_cache_.readWord(address);
}

// Functions to write directly to memory with cache bypass
void MemoryController::writeByte_d(uint64_t address, uint8_t value)
{
    memory_.writeByte(address, value);
    emit memoryUpdated(address, 1);
}
void MemoryController::writeHalfWord_d(uint64_t address, uint16_t value)
{
    memory_.writeHalfWord(address, value);
    emit memoryUpdated(address, 2);
}
void MemoryController::writeWord_d(uint64_t address, uint32_t value)
{
    memory_.writeWord(address, value);
    emit memoryUpdated(address, 4);
}
void MemoryController::writeDoubleWord_d(uint64_t address, uint64_t value)
{
    memory_.writeDoubleWord(address, value);
    emit memoryUpdated(address, 8);
}

void MemoryController::printMemory(const uint64_t address, unsigned int rows)
{
    memory_.printMemory(address, rows);
}

void MemoryController::dumpMemory(std::vector<std::string> args)
{
    memory_.dumpMemory(args);
}

void MemoryController::getMemoryPoint(std::string address)
{
    return memory_.getMemoryPoint(address);
}

Cache *MemoryController::getL1Cache()
{
    return &l1_cache_;
}
Cache *MemoryController::getL2Cache()
{
    return &l2_cache_;
}
Cache *MemoryController::getInstructionCache()
{
    return &instruction_cache_;
}
}