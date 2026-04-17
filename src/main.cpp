#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "cfg.h"
#include "dependency.h"
#include "interpreter.h"
#include "parser.h"
#include "renamer.h"
#include "shuffler.h"

namespace afis {
namespace {

struct Options {
    std::string inputPath;
    std::string outputPath = "transformed.ir";
    bool hasSeed = false;
    std::uint64_t seed = 0;
    bool verify = false;
    bool showMap = false;
};

void PrintUsage(const char* exe) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << exe << " --input <file> [--output <file>] [--seed <u64>] [--verify] [--show-map]\n\n";
    std::cerr << "Examples:\n";
    std::cerr << "  " << exe << " --input samples/example_full.ir --output build/run1.ir --verify\n";
    std::cerr << "  " << exe << " --input samples/example_full.ir --output build/run2.ir --seed 123 --verify --show-map\n";
}

bool ParseU64(const std::string& text, std::uint64_t& out) {
    try {
        std::size_t idx = 0;
        unsigned long long value = std::stoull(text, &idx, 10);
        if (idx != text.size()) {
            return false;
        }
        out = static_cast<std::uint64_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseArgs(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--input" || arg == "-i") {
            if (i + 1 >= argc) {
                error = "Missing value for --input";
                return false;
            }
            options.inputPath = argv[++i];
            continue;
        }

        if (arg == "--output" || arg == "-o") {
            if (i + 1 >= argc) {
                error = "Missing value for --output";
                return false;
            }
            options.outputPath = argv[++i];
            continue;
        }

        if (arg == "--seed") {
            if (i + 1 >= argc) {
                error = "Missing value for --seed";
                return false;
            }
            std::uint64_t parsed = 0;
            if (!ParseU64(argv[++i], parsed)) {
                error = "Invalid --seed value: " + std::string(argv[i]);
                return false;
            }
            options.hasSeed = true;
            options.seed = parsed;
            continue;
        }

        if (arg == "--verify") {
            options.verify = true;
            continue;
        }

        if (arg == "--show-map") {
            options.showMap = true;
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            return false;
        }

        error = "Unknown argument: " + arg;
        return false;
    }

    if (options.inputPath.empty()) {
        error = "--input is required";
        return false;
    }

    return true;
}

std::string ProgramToText(const Program& program) {
    std::ostringstream out;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        out << InstructionToString(program.instructions[i]);
        if (i + 1 < program.instructions.size()) {
            out << "\n";
        }
    }
    return out.str();
}

std::uint64_t HashText(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool WriteTextFile(const std::string& path, const std::string& content, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "Could not write output file: " + path;
        return false;
    }
    out << content;
    return true;
}

std::size_t CountMovedInstructions(const std::vector<std::size_t>& order) {
    std::size_t moved = 0;
    for (std::size_t newPos = 0; newPos < order.size(); ++newPos) {
        if (order[newPos] != newPos) {
            moved += 1;
        }
    }
    return moved;
}

std::size_t CountMovedOriginalInstructionSlots(const Program& original, const Program& transformed) {
    const std::size_t kSynthetic = std::numeric_limits<std::size_t>::max();

    std::vector<std::size_t> before;
    before.reserve(original.instructions.size());
    for (const Instruction& inst : original.instructions) {
        before.push_back(inst.originalIndex);
    }

    std::vector<std::size_t> after;
    after.reserve(original.instructions.size());
    for (const Instruction& inst : transformed.instructions) {
        if (inst.originalIndex != kSynthetic) {
            after.push_back(inst.originalIndex);
        }
    }

    const std::size_t comparable = std::min(before.size(), after.size());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < comparable; ++i) {
        if (before[i] != after[i]) {
            moved += 1;
        }
    }

    if (before.size() > comparable) {
        moved += before.size() - comparable;
    }
    if (after.size() > comparable) {
        moved += after.size() - comparable;
    }

    return moved;
}

std::size_t CountSideEffectOrderViolations(const std::vector<Instruction>& before,
                                           const std::vector<Instruction>& after) {
    std::vector<std::size_t> beforeOrder;
    std::vector<std::size_t> afterOrder;

    for (const Instruction& inst : before) {
        if (IsSideEffectOperation(inst)) {
            beforeOrder.push_back(inst.originalIndex);
        }
    }
    for (const Instruction& inst : after) {
        if (IsSideEffectOperation(inst)) {
            afterOrder.push_back(inst.originalIndex);
        }
    }

    const std::size_t comparable = std::min(beforeOrder.size(), afterOrder.size());
    std::size_t violations = 0;
    for (std::size_t i = 0; i < comparable; ++i) {
        if (beforeOrder[i] != afterOrder[i]) {
            violations += 1;
        }
    }
    if (beforeOrder.size() > comparable) {
        violations += beforeOrder.size() - comparable;
    }
    if (afterOrder.size() > comparable) {
        violations += afterOrder.size() - comparable;
    }

    return violations;
}

std::uint64_t DeriveSeed(std::uint64_t baseSeed, std::uint64_t salt) {
    std::uint64_t x = baseSeed ^ (salt + 0x9E3779B97F4A7C15ULL + (baseSeed << 6U) + (baseSeed >> 2U));
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return x;
}

void ShuffleBlockBody(BasicBlock& block,
                      std::uint64_t seed,
                      std::size_t& movedSlots,
                      std::size_t& sideEffectViolations,
                      std::size_t& fallbackCount) {
    std::size_t labelPrefix = 0;
    while (labelPrefix < block.instructions.size() &&
           block.instructions[labelPrefix].op == OpCode::Label) {
        labelPrefix += 1;
    }

    if (labelPrefix >= block.instructions.size()) {
        return;
    }

    bool hasTerminator = IsTerminator(block.instructions.back());
    std::size_t bodyEnd = hasTerminator ? block.instructions.size() - 1 : block.instructions.size();

    if (bodyEnd <= labelPrefix) {
        return;
    }

    Program body;
    for (std::size_t i = labelPrefix; i < bodyEnd; ++i) {
        body.instructions.push_back(block.instructions[i]);
    }

    DependencyGraph graph = BuildDependencyGraph(body);
    ShuffleResult shuffled = RandomizedTopologicalShuffle(body, graph, seed);

    movedSlots += CountMovedInstructions(shuffled.order);
    sideEffectViolations += CountSideEffectOrderViolations(body.instructions, shuffled.program.instructions);
    if (shuffled.fallbackUsed) {
        fallbackCount += 1;
    }

    std::vector<Instruction> rebuilt;
    rebuilt.reserve(block.instructions.size());
    for (std::size_t i = 0; i < labelPrefix; ++i) {
        rebuilt.push_back(block.instructions[i]);
    }
    rebuilt.insert(rebuilt.end(),
                   shuffled.program.instructions.begin(),
                   shuffled.program.instructions.end());
    if (hasTerminator) {
        rebuilt.push_back(block.instructions.back());
    }

    block.instructions = std::move(rebuilt);
}

void PrintRenameMap(const RenameMap& renameMap) {
    std::vector<std::pair<std::string, std::string>> pairs(renameMap.begin(), renameMap.end());
    std::sort(pairs.begin(), pairs.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) {
                  return a.first < b.first;
              });

    std::cout << "Rename map (old -> new):\n";
    for (const auto& kv : pairs) {
        std::cout << "  " << kv.first << " -> " << kv.second << "\n";
    }
}

}  // namespace
}  // namespace afis

int main(int argc, char** argv) {
    using namespace afis;

    Options options;
    std::string argError;
    if (!ParseArgs(argc, argv, options, argError)) {
        if (!argError.empty()) {
            std::cerr << "Argument error: " << argError << "\n\n";
        }
        PrintUsage(argv[0]);
        return 1;
    }

    ParseResult parsed = ParseIRFile(options.inputPath);
    if (!parsed.errors.empty()) {
        std::cerr << "Input parsing failed with " << parsed.errors.size() << " error(s):\n";
        for (const ParseError& err : parsed.errors) {
            if (err.line > 0) {
                std::cerr << "  line " << err.line << ": " << err.message << "\n";
            } else {
                std::cerr << "  " << err.message << "\n";
            }
        }
        return 1;
    }

    const Program& original = parsed.program;

    BranchValidationResult inputValidation = ValidateBranches(original);
    if (!inputValidation.success) {
        std::cerr << "Input control-flow validation failed: " << inputValidation.error << "\n";
        return 1;
    }

    std::uint64_t masterSeed = options.hasSeed ? options.seed : GenerateRandomSeed();
    std::uint64_t shuffleSeed = masterSeed ^ 0x9E3779B97F4A7C15ULL;
    std::uint64_t renameSeed = masterSeed ^ 0xD1B54A32D192ED03ULL;

    Program reordered;
    BlockReorderStats blockStats;
    std::size_t sideEffectViolations = 0;
    std::size_t shuffleFallbackCount = 0;
    bool cfgModeEnabled = HasExplicitControlFlow(original);

    if (cfgModeEnabled) {
        CFG cfg;
        std::string cfgError;
        if (!BuildCFG(original, cfg, cfgError)) {
            std::cerr << "CFG build failed: " << cfgError << "\n";
            return 1;
        }

        for (std::size_t i = 0; i < cfg.blocks.size(); ++i) {
            ShuffleBlockBody(cfg.blocks[i],
                             DeriveSeed(shuffleSeed, static_cast<std::uint64_t>(i + 1)),
                             blockStats.movedBlockSlots,
                             sideEffectViolations,
                             shuffleFallbackCount);
        }

        reordered = ReorderBasicBlocks(cfg, DeriveSeed(shuffleSeed, 0xC0FFEEULL), blockStats);
    } else {
        DependencyGraph graph = BuildDependencyGraph(original);
        ShuffleResult shuffled = RandomizedTopologicalShuffle(original, graph, shuffleSeed);

        if (!shuffled.fallbackUsed && CountMovedInstructions(shuffled.order) == 0 &&
            original.instructions.size() > 1) {
            std::uint64_t candidateSeed = shuffleSeed;
            for (int attempt = 0; attempt < 16; ++attempt) {
                candidateSeed += 0x9E3779B97F4A7C15ULL;
                ShuffleResult candidate = RandomizedTopologicalShuffle(original, graph, candidateSeed);
                if (!candidate.fallbackUsed && CountMovedInstructions(candidate.order) > 0) {
                    shuffled = std::move(candidate);
                    break;
                }
            }
        }

        reordered = shuffled.program;
        sideEffectViolations = CountSideEffectOrderViolations(original.instructions, reordered.instructions);
        if (shuffled.fallbackUsed) {
            shuffleFallbackCount = 1;
        }
        blockStats.blockCount = 1;
        blockStats.movedBlockSlots = 0;
        blockStats.branchFixupsInserted = 0;
        blockStats.order = {0};
    }

    {
        CFG transformedCfg;
        std::string cfgError;
        if (!BuildCFG(reordered, transformedCfg, cfgError)) {
            std::cerr << "Transformed CFG build failed: " << cfgError << "\n";
            return 1;
        }
    }

    BranchValidationResult outputValidation = ValidateBranches(reordered);
    if (!outputValidation.success) {
        std::cerr << "Output control-flow validation failed: " << outputValidation.error << "\n";
        return 1;
    }

    RenameResult renamed = RenameRegisters(reordered, renameSeed);

    Program transformed = renamed.program;
    std::string transformedText = ProgramToText(transformed);

    std::string writeError;
    if (!WriteTextFile(options.outputPath, transformedText, writeError)) {
        std::cerr << writeError << "\n";
        return 1;
    }

    std::size_t moved = CountMovedOriginalInstructionSlots(original, transformed);
    double reorderRatio = original.instructions.empty()
                              ? 0.0
                              : (100.0 * static_cast<double>(moved) /
                                 static_cast<double>(original.instructions.size()));
    double blockReorderRatio = blockStats.blockCount == 0
                                   ? 0.0
                                   : (100.0 * static_cast<double>(blockStats.movedBlockSlots) /
                                      static_cast<double>(blockStats.blockCount));

    std::string originalText = ProgramToText(original);
    std::uint64_t originalHash = HashText(originalText);
    std::uint64_t transformedHash = HashText(transformedText);

    std::cout << "Anti-Fingerprint Instruction Shuffler Summary\n";
    std::cout << "-------------------------------------------\n";
    std::cout << "Input file                : " << options.inputPath << "\n";
    std::cout << "Output file               : " << options.outputPath << "\n";
    std::cout << "CFG mode                  : " << (cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n";
    std::cout << "Input CFG validation      : PASS\n";
    std::cout << "Output CFG validation     : PASS\n";
    std::cout << "Instruction count         : " << original.instructions.size() << "\n";
    std::cout << "Basic block count         : " << blockStats.blockCount << "\n";
    std::cout << "Master seed               : " << masterSeed << "\n";
    std::cout << "Moved instruction slots   : " << moved << " / " << original.instructions.size() << "\n";
    std::cout << "Reorder ratio             : " << std::fixed << std::setprecision(2)
              << reorderRatio << "%\n";
    std::cout << "Moved block slots         : " << blockStats.movedBlockSlots
              << " / " << blockStats.blockCount << "\n";
    std::cout << "Block reorder ratio       : " << std::fixed << std::setprecision(2)
              << blockReorderRatio << "%\n";
    std::cout << std::defaultfloat;
    std::cout << "Branch fixups inserted    : " << blockStats.branchFixupsInserted << "\n";
    std::cout << "Side-effect violations    : " << sideEffectViolations << "\n";
    std::cout << "Shuffle fallbacks used    : " << shuffleFallbackCount << "\n";
    std::cout << "Renamed symbols           : " << renamed.renameMap.size() << "\n";
    std::cout << "Original IR hash          : 0x" << std::hex << originalHash << std::dec << "\n";
    std::cout << "Transformed IR hash       : 0x" << std::hex << transformedHash << std::dec << "\n";

    int exitCode = 0;

    if (options.verify) {
        ExecutionResult originalExec = ExecuteProgram(original);
        ExecutionResult transformedExec = ExecuteProgram(transformed);

        if (!originalExec.success) {
            std::cerr << "Verification failed while executing original program:\n";
            std::cerr << "  " << originalExec.error << "\n";
            exitCode = 2;
        }

        if (exitCode == 0 && !transformedExec.success) {
            std::cerr << "Verification failed while executing transformed program:\n";
            std::cerr << "  " << transformedExec.error << "\n";
            exitCode = 2;
        }

        if (exitCode == 0) {
            bool sameOutput = (originalExec.printedValues == transformedExec.printedValues);
            std::cout << "\nVerification\n";
            std::cout << "------------\n";
            std::cout << "Original output:\n" << PrintedOutputToText(originalExec.printedValues) << "\n";
            std::cout << "Transformed output:\n" << PrintedOutputToText(transformedExec.printedValues) << "\n";
            std::cout << "Semantic equivalence      : " << (sameOutput ? "PASS" : "FAIL") << "\n";

            if (!sameOutput) {
                exitCode = 3;
            }
        }
    }

    if (options.showMap) {
        std::cout << "\n";
        PrintRenameMap(renamed.renameMap);
    }

    if (exitCode != 0) {
        return exitCode;
    }

    return 0;
}