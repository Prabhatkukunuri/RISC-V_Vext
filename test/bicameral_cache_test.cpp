// The Bicameral Cache (processor/cache/bicameral): a Scalar Cache and a sectored Vector Cache with
// cross lookup and one-sided SC -> VC migration, plus its wiring into MemoryController.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_map>

#include "assembler/assembler.h"
#include "processor/cache/bicameral/bicameral_cache.h"
#include "processor/main_memory.h"
#include "processor/memory_controller.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;

namespace
{
constexpr uint64_t kBase = 0x10000000; // 1 KB aligned

class BicameralCacheTest : public ::testing::Test
{
  protected:
    MainMemory ram;
    BicameralCache cache{ram};

    /// True if the byte at @p address is held by the Scalar Cache / the Vector Cache.
    bool inScalar(uint64_t address) { return cache.getScalarCache()->probe(address, 1); }
    bool inVector(uint64_t address) { return cache.getVectorCache().hasSector(address); }
};
} // namespace

TEST_F(BicameralCacheTest, DefaultGeometryIsTwoEqualHalves)
{
    const BicameralConfig &config = cache.getConfig();
    EXPECT_EQ(config.scalarBytes(), 4096u); // 16 sets x 4 ways x 64 B
    EXPECT_EQ(config.vectorBytes(), 4096u); // 4 lines x 16 sectors x 64 B
    EXPECT_EQ(config.totalBytes(), 8192u);
    EXPECT_EQ(cache.getScalarCache()->getWayCount(), 4u);
    EXPECT_EQ(cache.getVectorCache().getLineSizeInBytes(), 1024u);
}

TEST_F(BicameralCacheTest, RejectsScalarLineDifferentFromSector)
{
    BicameralConfig config;
    config.scalar.lineSizeInBytes = 32;
    EXPECT_THROW(cache.reconfigure(config), std::invalid_argument);
}

TEST(BicameralConfigTest, FromL1SizeDerivesIndicesAndLines)
{
    const BicameralConfig a = BicameralConfig::fromL1Size(8192, 64, 16, 4);
    EXPECT_EQ(a.scalar.lineCount, 16u) << "4 KB / (64 B x 4 ways)";
    EXPECT_EQ(a.scalar.lineSizeInBytes, 64u);
    EXPECT_EQ(a.vectorLineCount, 4u) << "4 KB / 1 KB lines";
    EXPECT_EQ(a.scalarBytes(), a.vectorBytes());
    EXPECT_EQ(a.totalBytes(), 8192u);
    EXPECT_TRUE(a.validationError().empty());

    const BicameralConfig b = BicameralConfig::fromL1Size(16384, 32, 8, 2);
    EXPECT_EQ(b.scalar.lineCount, 128u);
    EXPECT_EQ(b.vectorLineCount, 32u);
    EXPECT_TRUE(b.validationError().empty());
}

TEST_F(BicameralCacheTest, RejectsUnequalHalves)
{
    BicameralConfig config;
    config.vectorLineCount = 2; // VC 2 KB, SC 4 KB
    EXPECT_FALSE(config.validationError().empty());
    EXPECT_THROW(cache.reconfigure(config), std::invalid_argument);
}

TEST_F(BicameralCacheTest, RejectsGeometryThatDoesNotFit)
{
    // 1 KB L1: each half is 512 B, smaller than one 1 KB VC line
    EXPECT_THROW(cache.reconfigure(BicameralConfig::fromL1Size(1024, 64, 16, 4)),
                 std::invalid_argument);
    // 1 KB L1 with 16 ways of 64 B: one SC index (1 KB) does not fit either
    EXPECT_THROW(cache.reconfigure(BicameralConfig::fromL1Size(1024, 64, 2, 16)),
                 std::invalid_argument);
}

TEST_F(BicameralCacheTest, LineMissThenSectorMissThenHit)
{
    cache.vectorRead(kBase, 4); // no line for the block
    cache.vectorRead(kBase + 64, 4); // line present, sector 1 not
    cache.vectorRead(kBase + 4, 4); // sector 0 present

    const BicameralStats stats = cache.getStats();
    EXPECT_EQ(stats.vectorLineMisses, 1u);
    EXPECT_EQ(stats.vectorSectorMisses, 1u);
    EXPECT_EQ(stats.vectorHits, 1u);

    // Only the two touched sectors were fetched.
    const VectorCache &vc = cache.getVectorCache();
    const VectorCacheLine &line = vc.getLine(vc.findLine(kBase));
    EXPECT_TRUE(line.sectorValid[0]);
    EXPECT_TRUE(line.sectorValid[1]);
    for (size_t s = 2; s < vc.getSectorsPerLine(); ++s)
    {
        EXPECT_FALSE(line.sectorValid[s]) << "sector " << s;
    }
}

TEST_F(BicameralCacheTest, LineMissEvictsLruLineWritingBackOnlyDirtySectors)
{
    ram.writeWord(kBase + 128, 0x11111111); // sector 2 of block 0, read but never written
    cache.vectorWrite(kBase, 4, 0xAABBCCDD); // sector 0 of block 0: dirty
    cache.vectorRead(kBase + 128, 4); // sector 2 of block 0: clean
    for (uint64_t block = 1; block <= 3; ++block) // fill the other three lines
    {
        cache.vectorRead(kBase + block * 1024, 4);
    }
    EXPECT_EQ(ram.readWord(kBase), 0u) << "write-back: nothing reaches memory before eviction";

    cache.vectorRead(kBase + 4 * 1024, 4); // a 5th block: evicts block 0 (least recently used)

    EXPECT_EQ(cache.getStats().vectorLineMisses, 5u);
    EXPECT_EQ(cache.getStats().vectorWriteBacks, 1u) << "only the dirty sector is written back";
    EXPECT_EQ(ram.readWord(kBase), 0xAABBCCDDu);
    EXPECT_FALSE(inVector(kBase));
    EXPECT_TRUE(inVector(kBase + 1024));
}

TEST_F(BicameralCacheTest, VectorAccessMigratesSectorFromScalarCache)
{
    cache.scalarWrite(kBase + 8, 4, 0x12345678); // into the SC, dirty
    ASSERT_TRUE(inScalar(kBase + 8));

    EXPECT_EQ(cache.vectorRead(kBase + 8, 4), 0x12345678u);

    EXPECT_EQ(cache.getStats().migrations, 1u);
    EXPECT_FALSE(inScalar(kBase + 8)) << "one-sided migration moves, it does not copy";
    EXPECT_TRUE(inVector(kBase + 8));
    // The dirty bit travelled with the data: evicting it now must write it back.
    cache.writeBackAll();
    EXPECT_EQ(ram.readWord(kBase + 8), 0x12345678u);
}

TEST_F(BicameralCacheTest, ScalarAccessToVectorDataIsServedInPlace)
{
    cache.vectorWrite(kBase, 8, 0x0102030405060708);
    const size_t scalarAccessesBefore =
        cache.getStats().scalarHits + cache.getStats().scalarMisses;

    EXPECT_EQ(cache.scalarRead(kBase + 4, 4), 0x01020304u);
    cache.scalarWrite(kBase, 1, 0xFF);

    const BicameralStats stats = cache.getStats();
    EXPECT_EQ(stats.crossHits, 2u);
    EXPECT_EQ(stats.scalarHits + stats.scalarMisses, scalarAccessesBefore);
    EXPECT_TRUE(inVector(kBase)) << "a scalar access never moves data back to the SC";
    EXPECT_FALSE(inScalar(kBase));
    EXPECT_EQ(cache.vectorRead(kBase, 1), 0xFFu);
}

TEST_F(BicameralCacheTest, NoAddressIsEverInBothCaches)
{
    std::mt19937_64 rng(7);
    for (int i = 0; i < 4000; ++i)
    {
        const uint64_t address = kBase + (rng() % (8 * 1024)) / 8 * 8;
        const bool vector = rng() % 2;
        const bool write = rng() % 2;
        if (vector)
        {
            write ? cache.vectorWrite(address, 8, rng()) : (void)cache.vectorRead(address, 8);
        }
        else
        {
            write ? cache.scalarWrite(address, 8, rng()) : (void)cache.scalarRead(address, 8);
        }
        ASSERT_FALSE(inScalar(address) && inVector(address)) << "access " << i;
    }
}

// Random scalar and vector accesses of every size, aligned or not, must read back exactly what a
// plain byte array would, for both write policies of both caches.
class BicameralDifferentialTest : public ::testing::TestWithParam<WritePolicy>
{
};

TEST_P(BicameralDifferentialTest, MatchesFlatMemory)
{
    MainMemory ram;
    // 4 KB L1: 2 VC lines and an 8-index SC, small so lines get evicted often
    BicameralConfig config    = BicameralConfig::fromL1Size(4096, 64, 16, 4);
    config.scalar.writePolicy = GetParam();
    config.vectorWritePolicy  = GetParam();
    BicameralCache cache(ram, config);

    std::unordered_map<uint64_t, uint8_t> reference;
    std::mt19937_64 rng(42);
    const unsigned sizes[] = {1, 2, 4, 8};
    for (int i = 0; i < 20000; ++i)
    {
        const unsigned size = sizes[rng() % 4];
        const uint64_t address = kBase + rng() % (6 * 1024);
        const bool vector = rng() % 2;
        if (rng() % 2)
        {
            const uint64_t value = rng();
            vector ? cache.vectorWrite(address, size, value)
                   : cache.scalarWrite(address, size, value);
            for (unsigned b = 0; b < size; ++b)
            {
                reference[address + b] = static_cast<uint8_t>(value >> (8 * b));
            }
        }
        else
        {
            const uint64_t got =
                vector ? cache.vectorRead(address, size) : cache.scalarRead(address, size);
            for (unsigned b = 0; b < size; ++b)
            {
                const auto it = reference.find(address + b);
                const uint8_t expected = it == reference.end() ? 0 : it->second;
                ASSERT_EQ(static_cast<uint8_t>(got >> (8 * b)), expected)
                    << "access " << i << " address " << address + b;
            }
        }
    }
    cache.writeBackAll();
    for (const auto &[address, value] : reference)
    {
        ASSERT_EQ(ram.readByte(address), value) << "memory at " << address;
    }
}

INSTANTIATE_TEST_SUITE_P(WritePolicies, BicameralDifferentialTest,
                         ::testing::Values(WritePolicy::WriteBack, WritePolicy::WriteThrough));

TEST(BicameralL2Test, RequiredL2IsAtLeastL1)
{
    const BicameralConfig config; // 8 KB, 64 B sectors
    const CacheConfig tiny{1, 16, 2, WritePolicy::WriteThrough, AllocationPolicy::NoWriteAllocate,
                           ReplacementPolicy::LRU};
    const CacheConfig l2 = BicameralCache::requiredL2(config, tiny);
    EXPECT_GE(l2.lineSizeInBytes, 64u);
    EXPECT_GE(l2.lineCount * l2.wayCount * l2.lineSizeInBytes, 8192u);
    EXPECT_EQ(l2.wayCount, 2u) << "ways are kept";

    const CacheConfig big{512, 128, 8, WritePolicy::WriteBack, AllocationPolicy::WriteAllocate,
                          ReplacementPolicy::FIFO};
    const CacheConfig same = BicameralCache::requiredL2(config, big);
    EXPECT_EQ(same.lineCount, big.lineCount) << "a big enough L2 is left alone";
    EXPECT_EQ(same.lineSizeInBytes, big.lineSizeInBytes);
}

TEST(BicameralMemoryControllerTest, DisabledLeavesVectorAccessesInL1)
{
    MemoryController memory;
    ASSERT_FALSE(memory.isBicameralEnabled());
    memory.vectorWriteWord(kBase, 7);
    EXPECT_EQ(memory.vectorReadWord(kBase), 7u);
    EXPECT_GT(memory.getL1Cache()->getHitCount() + memory.getL1Cache()->getMissCount(), 0u);
    const BicameralStats stats = memory.getBicameralCache()->getStats();
    EXPECT_EQ(stats.vectorHits + stats.vectorSectorMisses + stats.vectorLineMisses, 0u);
}

TEST(BicameralMemoryControllerTest, EnablingEnlargesL2AndRoutesAccesses)
{
    MemoryController memory;
    memory.writeWord(kBase, 0x55); // through the plain L1 before switching
    memory.setBicameralEnabled(true);

    const Cache *l2 = memory.getL2Cache();
    EXPECT_GE(l2->getCacheSizeInBytes(), 8192u);
    EXPECT_GE(l2->getLineSizeInBytes(), 64u);

    EXPECT_EQ(memory.vectorReadWord(kBase), 0x55u) << "data written before enabling survives";
    memory.writeWord(kBase + 4096, 3);
    const BicameralStats stats = memory.getBicameralCache()->getStats();
    EXPECT_EQ(stats.vectorLineMisses, 1u);
    EXPECT_EQ(stats.scalarMisses, 1u);

    memory.setBicameralEnabled(false);
    EXPECT_EQ(memory.readWord(kBase + 4096), 3u) << "data written while enabled survives";
}

TEST(BicameralMemoryControllerTest, VectorAddRunsOnRvssWithBicameralCache)
{
    setupVmStateDirectory();
    std::ifstream file("../examples/vector_add.s");
    ASSERT_TRUE(file.good());
    std::stringstream source;
    source << file.rdbuf();
    std::istringstream stream(source.str());

    RVSSProcessor vm;
    vm.memory_controller_.setBicameralEnabled(true);
    vm.LoadProgram(assemble(stream));
    vm.DebugRun();

    const uint64_t c = kBase + 80;
    for (uint64_t i = 0; i < 10; ++i)
    {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; ++b)
        {
            word |= static_cast<uint32_t>(vm.memory_controller_.peekByte(c + 4 * i + b)) << (8 * b);
        }
        EXPECT_EQ(word, 11 * (i + 1)) << "c[" << i << "]";
    }
    const BicameralStats stats = vm.memory_controller_.getBicameralCache()->getStats();
    EXPECT_GT(stats.vectorLineMisses, 0u);
    EXPECT_GT(stats.vectorHits, 0u);

    vm.memory_controller_.writeBackAllCaches();
    EXPECT_EQ(vm.memory_controller_.readWord_d(c + 36), 110u) << "written back to main memory";
}

// examples/bicameral_cache_check.s is the walkthrough for checking the cache in the GUI; its
// comments give the statistics after each step. This runs it with the configuration those
// comments ask for (1 KB L1, 64 B sectors, 4 sectors per VC line, 2-way SC) and checks the
// final numbers, so the walkthrough stays right.
TEST(BicameralMemoryControllerTest, GuiWalkthroughExampleGivesDocumentedStats)
{
    setupVmStateDirectory();
    std::ifstream file("../examples/bicameral_cache_check.s");
    ASSERT_TRUE(file.good());
    std::stringstream source;
    source << file.rdbuf();
    std::istringstream stream(source.str());

    RVSSProcessor vm;
    MemoryController &memory = vm.memory_controller_;
    memory.setBicameralEnabled(true);
    memory.reconfigureBicameral(BicameralConfig::fromL1Size(1024, 64, 4, 2));
    memory.getL2Cache()->reconfigure(CacheConfig{
        .lineCount         = 8,
        .lineSizeInBytes   = 64,
        .wayCount          = 2,
        .writePolicy       = WritePolicy::WriteThrough,
        .allocationPolicy  = AllocationPolicy::NoWriteAllocate,
        .replacementPolicy = ReplacementPolicy::LRU,
    }); // the L2 the example's header asks for: 1 KB
    ASSERT_EQ(memory.getL2Cache()->getCacheSizeInBytes(), 1024u);
    ASSERT_EQ(memory.getBicameralCache()->getConfig().vectorLineCount, 2u);
    ASSERT_EQ(memory.getBicameralCache()->getConfig().scalar.lineCount, 4u);
    vm.LoadProgram(assemble(stream));
    vm.DebugRun();

    const BicameralStats s = memory.getBicameralCache()->getStats();
    EXPECT_EQ(s.scalarHits, 1u);
    EXPECT_EQ(s.scalarMisses, 1u);
    EXPECT_EQ(s.scalarWriteBacks, 0u);
    EXPECT_EQ(s.vectorHits, 19u);
    EXPECT_EQ(s.vectorSectorMisses, 2u);
    EXPECT_EQ(s.vectorLineMisses, 3u);
    EXPECT_EQ(s.vectorWriteBacks, 1u);
    EXPECT_EQ(s.migrations, 1u);
    EXPECT_EQ(s.crossHits, 1u);

    EXPECT_EQ(vm.registers_.ReadGpr(28), 42u) << "t3";
    EXPECT_EQ(vm.registers_.ReadGpr(29), 18u) << "t4 (cross-hit)";

    // The evicted dirty sector reached main memory; the migrated 42 is still only in the VC.
    EXPECT_EQ(memory.readWord_d(kBase + 0x80), 18u);
    EXPECT_EQ(memory.readWord_d(kBase + 0x8C), 24u);
    EXPECT_EQ(memory.readWord_d(kBase + 0x400), 0u);
    memory.writeBackAllCaches();
    EXPECT_EQ(memory.readWord_d(kBase + 0x400), 42u);
}
