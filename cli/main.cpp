/**
 * @file main.cpp
 * @brief Entry point for the Kites command-line interface.
 */
#include "assembler/assembler.h"
#include "command_handler/command_handler.h"
#include "common/assembled_program.h"
#include "common/globals.h"
#include "config/config.h"
#include "processor/processor_manager.h"
#include "processor/processor_types.h"
#include "repl.h"
#include "run_stats.h"
#include "utils/utils.h"

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
void printUsage(const char *programName)
{
    std::cerr
        << "Usage: " << programName
        << " [assembly-file] [--vm <name>] [--mem-dump <hex_addr> <row_count>]... [--stats]\n"
        << "       [--vlen <bits>] [--bicameral] [--repl|-i]\n"
        << "  assembly-file   optional; if given without --repl, assembles, runs to\n"
        << "                  completion, and dumps state (batch mode)\n"
        << "  --vm <name>     rvss (default) | rv5-nh-nf | rv5-h-nf | rv5-nh-f | rv5-h-f\n"
        << "  --mem-dump      dump <row_count> 8-byte rows starting at <hex_addr>; "
           "repeatable (batch mode only)\n"
        << "  --stats         collect run statistics (batch mode only): prints a summary and\n"
        << "                  writes vm_state/stats_dump.json; the instruction mix, memory\n"
        << "                  traffic and vector work need --vm rvss\n"
        << "  --vlen <bits>   vector register length (default 128)\n"
        << "  --bicameral     use the Bicameral Cache (Scalar Cache + sectored Vector Cache)\n"
        << "                  as the L1 data cache; L2 is enlarged to at least its size\n"
        << "  --repl, -i      drop into the interactive REPL instead of auto-running;\n"
        << "                  if assembly-file is given it is loaded before the prompt\n"
        << "  (running with no assembly-file also enters the REPL)\n";
}

const std::unordered_map<std::string, Kites::ProcessorType> processorTypes = {
    {"rvss", Kites::ProcessorType::RVSS},
    {"rv5-nh-nf", Kites::ProcessorType::RV5Stage_NH_NF},
    {"rv5-h-nf", Kites::ProcessorType::RV5Stage_H_NF},
    {"rv5-nh-f", Kites::ProcessorType::RV5Stage_NH_F},
    {"rv5-h-f", Kites::ProcessorType::RV5Stage_H_F},
};
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Kites::setupVmStateDirectory();

    std::string assemblyFile;
    std::string processorTypeStr = "rvss";
    std::vector<std::string> memDumpArgs;
    bool replRequested = false;
    bool statsRequested = false;
    bool bicameralRequested = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--vm")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --vm requires a value.\n";
                printUsage(argv[0]);
                return 1;
            }
            processorTypeStr = argv[++i];
        }
        else if (arg == "--mem-dump")
        {
            if (i + 2 >= argc)
            {
                std::cerr << "Error: --mem-dump requires <hex_addr> <row_count>.\n";
                printUsage(argv[0]);
                return 1;
            }
            memDumpArgs.push_back(argv[++i]);
            memDumpArgs.push_back(argv[++i]);
        }
        else if (arg == "--stats")
        {
            statsRequested = true;
        }
        else if (arg == "--vlen")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: " << arg << " requires a value in bits.\n";
                printUsage(argv[0]);
                return 1;
            }
            try
            {
                Kites::vm_config::config.setVlen(std::stoull(argv[++i]));
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error: invalid " << arg << " value: " << e.what() << "\n";
                return 1;
            }
        }
        else if (arg == "--bicameral")
        {
            bicameralRequested = true;
        }
        else if (arg == "--repl" || arg == "-i")
        {
            replRequested = true;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Error: unknown argument '" << arg << "'.\n";
            printUsage(argv[0]);
            return 1;
        }
        else if (assemblyFile.empty())
        {
            assemblyFile = arg;
        }
        else
        {
            std::cerr << "Error: unexpected extra argument '" << arg << "'.\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    auto processorTypeIt = processorTypes.find(processorTypeStr);
    if (processorTypeIt == processorTypes.end())
    {
        std::cerr << "Error: unknown VM type '" << processorTypeStr << "'.\n";
        printUsage(argv[0]);
        return 1;
    }

    if (replRequested || assemblyFile.empty())
    {
        Kites::ProcessorManager manager(nullptr, processorTypeIt->second);
        manager.setStepDelay(0);

        Kites::command_handler::ReplState state;
        if (!assemblyFile.empty())
        {
            try
            {
                Kites::AssembledProgram program = Kites::assemble(assemblyFile);
                manager.loadProgram(program);
                // LoadProgram plants a breakpoint at the end of the program; the GUI replaces
                // it with the editor's breakpoints before every run, so do the same here.
                // Otherwise the pipelined VMs, which keep clocking past the end of the program
                // to drain, stop there and wait forever.
                manager.setBreakpoints(state.breakpoints);
                state.currentProgram = program;
                state.programLoaded = true;
                std::cout << "Loaded '" << assemblyFile << "': " << program.text_buffer.size()
                           << " instruction word(s), " << program.data_buffer.size()
                           << " data item(s).\n";
            }
            catch (const std::exception &e)
            {
                std::cerr << "Assembly failed: " << e.what() << "\n";
            }
        }

        return Kites::RunRepl(manager, state);
    }

    Kites::AssembledProgram program;
    try
    {
        program = Kites::assemble(assemblyFile);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Assembly failed: " << e.what() << std::endl;
        return 1;
    }

    Kites::ProcessorManager manager(nullptr, processorTypeIt->second);

    bool runFailed = false;
    std::string runErrorMsg;
    QObject::connect(&manager, &Kites::ProcessorManager::runErrorSignal, &manager,
                      [&](const QString &message, int /*sourceLine*/)
                      {
                          runFailed = true;
                          runErrorMsg = message.toStdString();
                      });

    // ProcessorBase::step_delay_ defaults to 1000ms and is meant to pace the
    // GUI's per-instruction animation; it must be zeroed here or an N-instruction
    // program takes N seconds to run.
    manager.setStepDelay(0);
    if (bicameralRequested)
    {
        manager.getMemoryController()->setBicameralEnabled(true);
    }
    manager.loadProgram(program);
    // See the REPL branch above: drop the end-of-program breakpoint that LoadProgram plants.
    manager.setBreakpoints({});

    Kites::cli::RunStatsCollector stats(program, *manager.getRegisterFile(),
                                        Kites::vm_config::config.getVlen());
    const bool singleCycle = processorTypeIt->second == Kites::ProcessorType::RVSS;
    if (statsRequested && singleCycle)
    {
        QObject::connect(&manager, &Kites::ProcessorManager::processorClockedSignal, &manager,
                         [&stats](const Kites::ProcessorState &processorState)
                         { stats.onClock(processorState); });
    }

    // Known limitation: a program hitting a blocking read syscall will hang here
    // forever, since nothing in this CLI feeds terminal stdin into the VM's input
    // queue yet (that's separate, future work).
    manager.run();

    if (runFailed)
    {
        std::cerr << "Run failed: " << runErrorMsg << std::endl;
    }

    Kites::DumpRegisters(Kites::globals::registers_dump_file_path, *manager.getRegisterFile());

    if (bicameralRequested)
    {
        // The Bicameral Cache is write-back: put what the program wrote into main memory, which
        // is what --mem-dump reads.
        manager.getMemoryController()->writeBackAllCaches();
    }

    if (!memDumpArgs.empty())
    {
        try
        {
            manager.getMemoryController()->dumpMemory(memDumpArgs);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Memory dump failed: " << e.what() << std::endl;
            return 3;
        }
    }

    if (statsRequested)
    {
        std::cout << stats.write(Kites::globals::vm_state_directory / "stats_dump.json",
                                 processorTypeStr, manager.getCycles(),
                                 manager.getInstructionsRetired(), singleCycle);
        if (bicameralRequested)
        {
            Kites::MemoryController *memory = manager.getMemoryController();
            const Kites::BicameralStats b = memory->getBicameralCache()->getStats();
            const Kites::BicameralConfig &config = memory->getBicameralCache()->getConfig();
            std::cout << "Bicameral Cache (L1 " << config.totalBytes() / 1024 << " KB = SC "
                      << config.scalarBytes() / 1024 << " KB + VC " << config.vectorBytes() / 1024
                      << " KB; L2 " << memory->getL2Cache()->getCacheSizeInBytes() / 1024
                      << " KB)\n"
                      << "  Geometry     : SC " << config.scalar.lineCount << " indices x "
                      << config.scalar.wayCount << " ways x " << config.sectorSize
                      << " B; VC fully associative, " << config.vectorLineCount << " lines x "
                      << config.sectorsPerLine << " sectors x " << config.sectorSize << " B\n"
                      << "  Scalar Cache : " << b.scalarHits << " hits, " << b.scalarMisses
                      << " misses, " << b.scalarWriteBacks << " write-backs\n"
                      << "  Vector Cache : " << b.vectorHits << " hits, " << b.vectorSectorMisses
                      << " sector misses, " << b.vectorLineMisses << " line misses, "
                      << b.vectorWriteBacks << " sector write-backs\n"
                      << "  Cross lookup : " << b.migrations << " SC->VC migrations, "
                      << b.crossHits << " scalar cross-hits in the VC\n";
        }
    }

    std::cout << "Assembled '" << assemblyFile << "': " << program.text_buffer.size()
               << " instruction word(s), " << program.data_buffer.size() << " data item(s).\n"
               << "Run " << (runFailed ? "failed" : "completed") << ".\n"
               << "Registers   -> " << Kites::globals::registers_dump_file_path.string() << "\n"
               << "Disassembly -> " << Kites::globals::disassembly_file_path.string() << "\n"
               << "Errors      -> " << Kites::globals::errors_dump_file_path.string();
    if (statsRequested)
    {
        std::cout << "\nStatistics  -> "
                  << (Kites::globals::vm_state_directory / "stats_dump.json").string();
    }
    if (!memDumpArgs.empty())
    {
        std::cout << "\nMemory      -> " << Kites::globals::memory_dump_file_path.string();
    }
    std::cout << std::endl;

    return runFailed ? 2 : 0;
}
