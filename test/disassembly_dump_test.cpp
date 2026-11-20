// disassembly.txt (the GUI's disassembly view) must print vector operands in assembly order:
// vd, vs2, vs1 — not the generic ICUnit order rd, rs1, rs2, which swapped the two sources.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "utils/utils.h"

namespace
{
std::string DumpFor(const std::string &source)
{
    Kites::setupVmStateDirectory();
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "kites_disassembly_dump_test.txt";
    Kites::DumpDisasssembly(path, program);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
} // namespace

TEST(DisassemblyDumpTest, VectorOperandsAppearInAssemblyOrder)
{
    const std::string dump = DumpFor("    vsetvli t0, a0, e32, m1, ta, ma\n"
                                     "    vsub.vv v3, v1, v2\n"
                                     "    vsub.vx v4, v1, t1, v0.t\n"
                                     "    vmacc.vv v5, v1, v2\n"
                                     "    vmerge.vvm v6, v1, v2, v0\n"
                                     "    vmv.v.v v7, v1\n"
                                     "    vle32.v v1, (a1)\n"
                                     "    add t2, t0, t1\n");
    EXPECT_NE(dump.find("vsetvli x5, x10, e32, m1, ta, ma"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vsub.vv v3, v1, v2"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vsub.vx v4, v1, x6, v0.t"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vmacc.vv v5, v1, v2"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vmerge.vvm v6, v1, v2, v0"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vmv.v.v v7, v1\n"), std::string::npos) << dump;
    EXPECT_NE(dump.find("vle32.v v1, (x11)"), std::string::npos) << dump;
    EXPECT_NE(dump.find("add x7, x5, x6"), std::string::npos) << dump; // scalar path unchanged
}
