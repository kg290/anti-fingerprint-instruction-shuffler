#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cfg.h"
#include "cpp_frontend.h"
#include "dependency.h"
#include "interpreter.h"
#include "llm.h"
#include "parser.h"
#include "renamer.h"
#include "shuffler.h"
#include "substitution.h"

namespace afis {
namespace {

struct Options {
    std::string inputPath;
    std::string outputPath = "transformed.ir";
    bool hasSeed = false;
    std::uint64_t seed = 0;
    bool verify = false;
    bool showMap = false;

    bool interactive = false;

    bool enableLlmSubstitution = false;
    bool enableLlmExplanation = false;

    std::string reportPath;
    std::string reportHtmlPath;

    std::string envPath = ".env";

    bool forceCppInput = false;
    bool preferLlmCpp = true;
};

struct HazardBreakdown {
    std::size_t raw = 0;
    std::size_t war = 0;
    std::size_t waw = 0;
    std::size_t barrier = 0;
    std::vector<std::string> sampleDetails;
};

struct RunSummary {
    std::string inputPath;
    std::string outputPath;
    std::string inputMode = "IR";
    std::string inputModeNote;

    bool cfgModeEnabled = false;

    std::size_t instructionCount = 0;
    std::size_t blockCount = 0;

    std::uint64_t masterSeed = 0;

    std::size_t movedInstructionSlots = 0;
    double reorderRatio = 0.0;

    std::size_t movedBlockSlots = 0;
    double blockReorderRatio = 0.0;

    std::size_t branchFixupsInserted = 0;
    std::size_t sideEffectViolations = 0;
    std::size_t shuffleFallbacksUsed = 0;

    std::size_t renamedSymbols = 0;
    std::uint64_t originalHash = 0;
    std::uint64_t transformedHash = 0;

    bool verifyRequested = false;
    bool verifyPass = false;
    std::string verifyError;
    std::vector<long long> originalOutput;
    std::vector<long long> transformedOutput;

    bool llmConfigured = false;
    bool llmSubstitutionEnabled = false;
    bool llmExplanationEnabled = false;

    SubstitutionStats substitutionStats;
    std::string substitutionNote;

    HazardBreakdown hazards;

    std::string explanation;
    bool explanationFromLlm = false;
    std::string explanationError;

    RenameMap renameMap;

    std::string originalText;
    std::string transformedText;

    std::string reportPath;
    std::string reportHtmlPath;
};

std::string ToLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsCppFilePath(const std::string& path) {
    std::string lower = ToLower(path);
    return EndsWith(lower, ".cpp") || EndsWith(lower, ".cc") || EndsWith(lower, ".cxx") ||
           EndsWith(lower, ".c++") || EndsWith(lower, ".cp");
}

void PrintUsage(const char* exe) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << exe
              << " --input <file> [--output <file>] [--seed <u64>] [--verify] [--show-map]\n";
    std::cerr << "  " << exe << " --interactive [--env <file>]\n\n";

    std::cerr << "Core options:\n";
    std::cerr << "  --input, -i <file>      Input file (.ir or .cpp)\n";
    std::cerr << "  --input-cpp <file>      Force C++ input mode (experimental)\n";
    std::cerr << "  --output, -o <file>     Output transformed IR file\n";
    std::cerr << "  --seed <u64>            Fixed seed for deterministic runs\n";
    std::cerr << "  --verify                Execute original and transformed IR and compare output\n";
    std::cerr << "  --show-map              Print rename map\n";
    std::cerr << "  --interactive           Start loop-based interactive mode\n";

    std::cerr << "LLM options (.env Gemini):\n";
    std::cerr << "  --env <file>            Path to .env file (default: .env)\n";
    std::cerr << "  --llm-substitute        Enable LLM-assisted candidate selection for substitution\n";
    std::cerr << "  --llm-explain           Enable explanation mode and auto markdown report\n";
    std::cerr << "  --report <file.md>      Write markdown report\n";
    std::cerr << "  --report-html <file>    Write html report\n";
    std::cerr << "  --no-llm-cpp            Disable LLM assistance for C++ to IR conversion\n\n";

    std::cerr << "Examples:\n";
    std::cerr << "  " << exe
              << " --input samples/example_full.ir --output build/out.ir --verify\n";
    std::cerr << "  " << exe
              << " --input samples/example_cpp_basic.cpp --output build/from_cpp.ir --verify --llm-explain\n";
    std::cerr << "  " << exe
              << " --input samples/example_full.ir --output build/sub.ir --llm-substitute --verify\n";
    std::cerr << "  " << exe << " --interactive\n";
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

bool ParseArgs(int argc,
               char** argv,
               Options& options,
               std::string& error,
               bool& wantsHelp) {
    wantsHelp = false;

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

        if (arg == "--input-cpp") {
            if (i + 1 >= argc) {
                error = "Missing value for --input-cpp";
                return false;
            }
            options.inputPath = argv[++i];
            options.forceCppInput = true;
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

        if (arg == "--interactive") {
            options.interactive = true;
            continue;
        }

        if (arg == "--env") {
            if (i + 1 >= argc) {
                error = "Missing value for --env";
                return false;
            }
            options.envPath = argv[++i];
            continue;
        }

        if (arg == "--llm-substitute") {
            options.enableLlmSubstitution = true;
            continue;
        }

        if (arg == "--llm-explain") {
            options.enableLlmExplanation = true;
            continue;
        }

        if (arg == "--report") {
            if (i + 1 >= argc) {
                error = "Missing value for --report";
                return false;
            }
            options.reportPath = argv[++i];
            continue;
        }

        if (arg == "--report-html") {
            if (i + 1 >= argc) {
                error = "Missing value for --report-html";
                return false;
            }
            options.reportHtmlPath = argv[++i];
            continue;
        }

        if (arg == "--no-llm-cpp") {
            options.preferLlmCpp = false;
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            wantsHelp = true;
            return false;
        }

        error = "Unknown argument: " + arg;
        return false;
    }

    if (!options.interactive && options.inputPath.empty()) {
        error = "--input is required unless --interactive is used";
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

std::string FirstCommonToken(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const std::string& x : a) {
        for (const std::string& y : b) {
            if (x == y) {
                return x;
            }
        }
    }
    return "";
}

HazardBreakdown AnalyzeHazards(const Program& program) {
    HazardBreakdown hazards;

    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        std::vector<std::string> readI = ReadSet(program.instructions[i]);
        std::vector<std::string> writeI = WriteSet(program.instructions[i]);
        bool barrierI = IsSideEffectingBarrier(program.instructions[i]);

        for (std::size_t j = i + 1; j < program.instructions.size(); ++j) {
            std::vector<std::string> readJ = ReadSet(program.instructions[j]);
            std::vector<std::string> writeJ = WriteSet(program.instructions[j]);
            bool barrierJ = IsSideEffectingBarrier(program.instructions[j]);

            std::string rawTok = FirstCommonToken(writeI, readJ);
            if (!rawTok.empty()) {
                hazards.raw += 1;
                if (hazards.sampleDetails.size() < 6) {
                    std::ostringstream out;
                    out << i << " -> " << j << " RAW on " << rawTok;
                    hazards.sampleDetails.push_back(out.str());
                }
            }

            std::string warTok = FirstCommonToken(readI, writeJ);
            if (!warTok.empty()) {
                hazards.war += 1;
                if (hazards.sampleDetails.size() < 6) {
                    std::ostringstream out;
                    out << i << " -> " << j << " WAR on " << warTok;
                    hazards.sampleDetails.push_back(out.str());
                }
            }

            std::string wawTok = FirstCommonToken(writeI, writeJ);
            if (!wawTok.empty()) {
                hazards.waw += 1;
                if (hazards.sampleDetails.size() < 6) {
                    std::ostringstream out;
                    out << i << " -> " << j << " WAW on " << wawTok;
                    hazards.sampleDetails.push_back(out.str());
                }
            }

            if (barrierI || barrierJ) {
                hazards.barrier += 1;
                if (hazards.sampleDetails.size() < 6) {
                    std::ostringstream out;
                    out << i << " -> " << j << " side-effect barrier";
                    hazards.sampleDetails.push_back(out.str());
                }
            }
        }
    }

    return hazards;
}

std::string BuildDeterministicExplanation(const RunSummary& summary) {
    std::ostringstream out;

    out << "The transformation preserved semantics by enforcing dependency and control-flow safety. ";
    out << "Instruction movement was constrained by hazards (RAW=" << summary.hazards.raw
        << ", WAR=" << summary.hazards.war << ", WAW=" << summary.hazards.waw
        << ") and side-effect barriers (" << summary.hazards.barrier << "). ";

    if (summary.cfgModeEnabled) {
        out << "CFG mode reordered basic blocks and inserted " << summary.branchFixupsInserted
            << " branch fixups to preserve valid fallthrough behavior. ";
    } else {
        out << "Straight-line scheduling used randomized topological ordering under dependency constraints. ";
    }

    out << "Register renaming changed " << summary.renamedSymbols
        << " symbols consistently without changing data flow. ";

    if (summary.llmSubstitutionEnabled) {
        out << "LLM-assisted substitution considered " << summary.substitutionStats.candidateCount
            << " candidates and applied " << summary.substitutionStats.appliedCount
            << " validated rewrites. ";
    }

    if (summary.verifyRequested) {
        out << "Runtime verification result: " << (summary.verifyPass ? "PASS" : "FAIL") << ".";
    }

    return out.str();
}

std::string TruncateForPrompt(const std::string& text, std::size_t maxChars) {
    if (text.size() <= maxChars) {
        return text;
    }
    if (maxChars < 64) {
        return text.substr(0, maxChars);
    }

    std::ostringstream out;
    out << text.substr(0, maxChars - 48);
    out << "\n... [truncated " << (text.size() - (maxChars - 48)) << " chars]";
    return out.str();
}

std::string BuildExplanationPrompt(const RunSummary& summary) {
    std::ostringstream out;

    out << "Explain this compiler IR transformation in concise, viva-friendly language.\n";
    out << "Focus on why correctness is preserved and what changed.\n\n";

    out << "Metrics:\n";
    out << "- CFG mode: " << (summary.cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n";
    out << "- Reorder ratio: " << std::fixed << std::setprecision(2) << summary.reorderRatio << "%\n";
    out << "- Block reorder ratio: " << std::fixed << std::setprecision(2)
        << summary.blockReorderRatio << "%\n";
    out << std::defaultfloat;
    out << "- Branch fixups inserted: " << summary.branchFixupsInserted << "\n";
    out << "- Side-effect violations: " << summary.sideEffectViolations << "\n";
    out << "- Renamed symbols: " << summary.renamedSymbols << "\n";
    out << "- Hazard counts RAW/WAR/WAW/barrier: " << summary.hazards.raw << "/"
        << summary.hazards.war << "/" << summary.hazards.waw << "/" << summary.hazards.barrier << "\n";

    if (summary.llmSubstitutionEnabled) {
        out << "- Substitution candidates / applied: " << summary.substitutionStats.candidateCount << " / "
            << summary.substitutionStats.appliedCount << "\n";
    }

    out << "\nSample blocked edges:\n";
    if (summary.hazards.sampleDetails.empty()) {
        out << "- none\n";
    } else {
        for (const std::string& detail : summary.hazards.sampleDetails) {
            out << "- " << detail << "\n";
        }
    }

    out << "\nOriginal IR:\n" << TruncateForPrompt(summary.originalText, 3500) << "\n";
    out << "\nTransformed IR:\n" << TruncateForPrompt(summary.transformedText, 3500) << "\n";

    return out.str();
}

std::string EscapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 64);

    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }

    return out;
}

std::tm LocalTimeNow(std::time_t now) {
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    return tmBuf;
}

std::string TimestampStringHuman() {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf = LocalTimeNow(now);

    std::ostringstream out;
    out << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string FormatPercent(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value << "%";
    return out.str();
}

std::string FormatHex64(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string HtmlWithBreaks(const std::string& text) {
    std::string escaped = EscapeHtml(text);
    std::string out;
    out.reserve(escaped.size() + 32);

    for (char ch : escaped) {
        if (ch == '\n') {
            out += "<br/>\n";
        } else {
            out.push_back(ch);
        }
    }

    return out;
}

std::string BuildMarkdownReport(const RunSummary& summary) {
    std::ostringstream out;

    out << "# Anti-Fingerprint Transformation Report\n\n";
    out << "Generated: " << TimestampStringHuman() << "\n\n";

    out << "## Run Configuration\n\n";
    out << "- Input path: " << summary.inputPath << "\n";
    out << "- Input mode: " << summary.inputMode << "\n";
    if (!summary.inputModeNote.empty()) {
        out << "- Input mode note: " << summary.inputModeNote << "\n";
    }
    out << "- Output path: " << summary.outputPath << "\n";
    out << "- CFG mode: " << (summary.cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n";
    out << "- Master seed: " << summary.masterSeed << "\n";
    out << "- LLM configured: " << (summary.llmConfigured ? "YES" : "NO") << "\n";
    out << "\n";

    out << "## Metrics\n\n";
    out << "- Instruction count: " << summary.instructionCount << "\n";
    out << "- Basic block count: " << summary.blockCount << "\n";
    out << "- Moved instruction slots: " << summary.movedInstructionSlots << "\n";
    out << "- Reorder ratio: " << std::fixed << std::setprecision(2) << summary.reorderRatio << "%\n";
    out << "- Moved block slots: " << summary.movedBlockSlots << "\n";
    out << "- Block reorder ratio: " << std::fixed << std::setprecision(2)
        << summary.blockReorderRatio << "%\n";
    out << std::defaultfloat;
    out << "- Branch fixups inserted: " << summary.branchFixupsInserted << "\n";
    out << "- Side-effect violations: " << summary.sideEffectViolations << "\n";
    out << "- Shuffle fallbacks used: " << summary.shuffleFallbacksUsed << "\n";
    out << "- Renamed symbols: " << summary.renamedSymbols << "\n";
    out << "- Original IR hash: 0x" << std::hex << summary.originalHash << std::dec << "\n";
    out << "- Transformed IR hash: 0x" << std::hex << summary.transformedHash << std::dec << "\n";
    out << "\n";

    if (summary.llmSubstitutionEnabled) {
        out << "## LLM-Assisted Substitution\n\n";
        out << "- Candidates discovered: " << summary.substitutionStats.candidateCount << "\n";
        out << "- Candidate IDs approved by LLM: " << summary.substitutionStats.llmApprovedCount << "\n";
        out << "- Substitutions applied: " << summary.substitutionStats.appliedCount << "\n";
        if (!summary.substitutionNote.empty()) {
            out << "- Notes: " << summary.substitutionNote << "\n";
        }
        out << "\n";
    }

    out << "## Dependency/Hazard Snapshot\n\n";
    out << "- RAW edges: " << summary.hazards.raw << "\n";
    out << "- WAR edges: " << summary.hazards.war << "\n";
    out << "- WAW edges: " << summary.hazards.waw << "\n";
    out << "- Side-effect barrier edges: " << summary.hazards.barrier << "\n";
    out << "\n";

    if (!summary.hazards.sampleDetails.empty()) {
        out << "Sample blocked edges:\n";
        for (const std::string& detail : summary.hazards.sampleDetails) {
            out << "- " << detail << "\n";
        }
        out << "\n";
    }

    out << "## Explanation\n\n";
    out << (summary.explanation.empty() ? "(No explanation generated)" : summary.explanation) << "\n\n";
    if (!summary.explanationError.empty()) {
        out << "LLM explanation note: " << summary.explanationError << "\n\n";
    }

    out << "## Verification\n\n";
    if (!summary.verifyRequested) {
        out << "Verification not requested in this run.\n\n";
    } else if (!summary.verifyError.empty()) {
        out << "Verification failed to execute: " << summary.verifyError << "\n\n";
    } else {
        out << "Semantic equivalence: " << (summary.verifyPass ? "PASS" : "FAIL") << "\n\n";
        out << "Original output:\n\n```\n";
        out << PrintedOutputToText(summary.originalOutput) << "\n```\n\n";
        out << "Transformed output:\n\n```\n";
        out << PrintedOutputToText(summary.transformedOutput) << "\n```\n\n";
    }

    out << "## Rename Map\n\n";
    if (summary.renameMap.empty()) {
        out << "(No symbols renamed)\n";
    } else {
        std::vector<std::pair<std::string, std::string>> pairs(
            summary.renameMap.begin(), summary.renameMap.end());
        std::sort(pairs.begin(), pairs.end(),
                  [](const std::pair<std::string, std::string>& a,
                     const std::pair<std::string, std::string>& b) {
                      return a.first < b.first;
                  });

        for (const auto& kv : pairs) {
            out << "- " << kv.first << " -> " << kv.second << "\n";
        }
    }

    out << "\n## Transformed IR\n\n```\n";
    out << summary.transformedText << "\n```\n";

    return out.str();
}

std::string BuildHtmlReport(const RunSummary& summary, const std::string& markdownBody) {
    std::string verificationLabel = "NOT RUN";
    std::string verificationClass = "warn";
    if (summary.verifyRequested) {
       if (!summary.verifyError.empty()) {
          verificationLabel = "ERROR";
          verificationClass = "fail";
       } else if (summary.verifyPass) {
          verificationLabel = "PASS";
          verificationClass = "ok";
       } else {
          verificationLabel = "FAIL";
          verificationClass = "fail";
       }
    }

    std::vector<std::pair<std::string, std::string>> renamePairs(summary.renameMap.begin(),
                                                     summary.renameMap.end());
    std::sort(renamePairs.begin(), renamePairs.end(),
            [](const std::pair<std::string, std::string>& a,
              const std::pair<std::string, std::string>& b) {
               return a.first < b.first;
            });

    std::ostringstream html;
    html << "<!doctype html>\n";
    html << "<html><head><meta charset=\"utf-8\"/>";
    html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>";
    html << "<title>AFIS Transformation Report</title>";
    html << "<style>"
        << ":root{--bg:#f6f8fb;--panel:#ffffff;--ink:#13243b;--muted:#5f6f85;--ok:#0f7b6c;"
        << "--warn:#9a6700;--fail:#b42318;--line:#dde4ee;}"
        << "*{box-sizing:border-box;}"
        << "body{font-family:'Segoe UI',Arial,sans-serif;margin:0;background:var(--bg);color:var(--ink);}"
        << ".page{max-width:1120px;margin:0 auto;padding:28px 20px 36px 20px;}"
        << ".hero{background:linear-gradient(120deg,#0f4c81,#1e7cc2);color:#fff;padding:22px;border-radius:16px;}"
        << ".hero h1{margin:0 0 8px 0;font-size:1.7rem;}"
        << ".hero p{margin:0;opacity:0.95;}"
        << ".chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;}"
        << ".chip{padding:6px 10px;border-radius:999px;background:rgba(255,255,255,0.18);font-size:0.85rem;}"
        << ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px;margin-top:16px;}"
        << ".card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px;}"
        << ".label{font-size:0.78rem;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;}"
        << ".value{font-size:1.22rem;font-weight:700;margin-top:4px;}"
        << ".section{margin-top:16px;background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px;}"
        << ".section h2{margin:0 0 10px 0;font-size:1.05rem;}"
        << "table{width:100%;border-collapse:collapse;font-size:0.94rem;}"
        << "th,td{padding:8px 10px;border-bottom:1px solid var(--line);text-align:left;}"
        << "th{color:var(--muted);font-weight:600;background:#f8fafc;}"
        << ".ok{color:var(--ok);font-weight:700;}"
        << ".warn{color:var(--warn);font-weight:700;}"
        << ".fail{color:var(--fail);font-weight:700;}"
        << "pre{background:#f3f6fb;padding:12px;overflow:auto;border-radius:8px;border:1px solid var(--line);}"
        << "details summary{cursor:pointer;color:#0f4c81;font-weight:600;}"
        << "</style>";
    html << "</head><body>\n";
    html << "<div class=\"page\">";

    html << "<div class=\"hero\">";
    html << "<h1>AFIS Transformation Report</h1>";
    html << "<p>Generated: " << EscapeHtml(TimestampStringHuman()) << "</p>";
    html << "<div class=\"chips\">";
    html << "<span class=\"chip\">Input mode: " << EscapeHtml(summary.inputMode) << "</span>";
    html << "<span class=\"chip\">CFG: " << (summary.cfgModeEnabled ? "ENABLED" : "DISABLED") << "</span>";
    html << "<span class=\"chip\">Verification: <span class=\"" << verificationClass << "\">"
        << verificationLabel << "</span></span>";
    html << "<span class=\"chip\">Seed: " << summary.masterSeed << "</span>";
    html << "<span class=\"chip\">LLM configured: " << (summary.llmConfigured ? "YES" : "NO")
        << "</span>";
    html << "</div></div>";

    html << "<div class=\"grid\">";
    html << "<div class=\"card\"><div class=\"label\">Instructions</div><div class=\"value\">"
        << summary.instructionCount << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Basic Blocks</div><div class=\"value\">"
        << summary.blockCount << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Reorder Ratio</div><div class=\"value\">"
        << EscapeHtml(FormatPercent(summary.reorderRatio)) << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Block Reorder Ratio</div><div class=\"value\">"
        << EscapeHtml(FormatPercent(summary.blockReorderRatio)) << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Branch Fixups</div><div class=\"value\">"
        << summary.branchFixupsInserted << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Renamed Symbols</div><div class=\"value\">"
        << summary.renamedSymbols << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Original Hash</div><div class=\"value\">"
        << EscapeHtml(FormatHex64(summary.originalHash)) << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Transformed Hash</div><div class=\"value\">"
        << EscapeHtml(FormatHex64(summary.transformedHash)) << "</div></div>";
    html << "</div>";

    html << "<div class=\"section\"><h2>Input / Output</h2>";
    html << "<table><tbody>";
    html << "<tr><th>Input path</th><td>" << EscapeHtml(summary.inputPath) << "</td></tr>";
    html << "<tr><th>Output path</th><td>" << EscapeHtml(summary.outputPath) << "</td></tr>";
    if (!summary.inputModeNote.empty()) {
       html << "<tr><th>Input mode note</th><td>" << EscapeHtml(summary.inputModeNote) << "</td></tr>";
    }
    html << "</tbody></table></div>";

    html << "<div class=\"section\"><h2>Hazard Snapshot</h2>";
    html << "<table><thead><tr><th>Type</th><th>Count</th></tr></thead><tbody>";
    html << "<tr><td>RAW</td><td>" << summary.hazards.raw << "</td></tr>";
    html << "<tr><td>WAR</td><td>" << summary.hazards.war << "</td></tr>";
    html << "<tr><td>WAW</td><td>" << summary.hazards.waw << "</td></tr>";
    html << "<tr><td>Side-effect barrier edges</td><td>" << summary.hazards.barrier << "</td></tr>";
    html << "</tbody></table>";
    if (!summary.hazards.sampleDetails.empty()) {
       html << "<h3>Sample blocked edges</h3><ul>";
       for (const std::string& detail : summary.hazards.sampleDetails) {
          html << "<li>" << EscapeHtml(detail) << "</li>";
       }
       html << "</ul>";
    }
    html << "</div>";

    if (summary.llmSubstitutionEnabled) {
       html << "<div class=\"section\"><h2>LLM-Assisted Substitution</h2>";
       html << "<table><tbody>";
       html << "<tr><th>Candidates discovered</th><td>" << summary.substitutionStats.candidateCount
           << "</td></tr>";
       html << "<tr><th>IDs approved by LLM</th><td>" << summary.substitutionStats.llmApprovedCount
           << "</td></tr>";
       html << "<tr><th>Substitutions applied</th><td>" << summary.substitutionStats.appliedCount
           << "</td></tr>";
       if (!summary.substitutionNote.empty()) {
          html << "<tr><th>Note</th><td>" << EscapeHtml(summary.substitutionNote) << "</td></tr>";
       }
       html << "</tbody></table></div>";
    }

    html << "<div class=\"section\"><h2>Explanation</h2><p>";
    html << HtmlWithBreaks(summary.explanation.empty() ? "(No explanation generated)" : summary.explanation);
    html << "</p>";
    if (!summary.explanationError.empty()) {
       html << "<p><strong>LLM note:</strong> " << EscapeHtml(summary.explanationError) << "</p>";
    }
    html << "</div>";

    html << "<div class=\"section\"><h2>Verification Output</h2>";
    if (!summary.verifyRequested) {
       html << "<p>Verification not requested.</p>";
    } else if (!summary.verifyError.empty()) {
       html << "<p class=\"fail\">" << EscapeHtml(summary.verifyError) << "</p>";
    } else {
       html << "<p>Status: <span class=\"" << verificationClass << "\">"
           << verificationLabel << "</span></p>";
       html << "<h3>Original output</h3><pre>" << EscapeHtml(PrintedOutputToText(summary.originalOutput))
           << "</pre>";
       html << "<h3>Transformed output</h3><pre>"
           << EscapeHtml(PrintedOutputToText(summary.transformedOutput)) << "</pre>";
    }
    html << "</div>";

    html << "<div class=\"section\"><h2>Rename Map</h2>";
    if (renamePairs.empty()) {
       html << "<p>(No symbols renamed)</p>";
    } else {
       html << "<table><thead><tr><th>Original</th><th>Renamed</th></tr></thead><tbody>";
       for (const auto& kv : renamePairs) {
          html << "<tr><td>" << EscapeHtml(kv.first) << "</td><td>" << EscapeHtml(kv.second)
              << "</td></tr>";
       }
       html << "</tbody></table>";
    }
    html << "</div>";

    html << "<div class=\"section\"><h2>Transformed IR</h2><pre>"
        << EscapeHtml(summary.transformedText) << "</pre></div>";

    html << "<div class=\"section\"><details><summary>Raw Markdown Report</summary><pre>"
        << EscapeHtml(markdownBody) << "</pre></details></div>";

    html << "</div>";
    html << "</body></html>\n";
    return html.str();
}

bool ParseInputProgram(const Options& options,
                      const GeminiClient& gemini,
                      Program& original,
                      std::string& inputMode,
                      std::string& inputModeNote,
                      std::string& error) {
    bool useCpp = options.forceCppInput || IsCppFilePath(options.inputPath);
    if (useCpp) {
        CppConversionResult conversion =
            ConvertCppFileToIR(options.inputPath, &gemini, options.preferLlmCpp);
        if (!conversion.success) {
            error = conversion.error;
            return false;
        }

        original = std::move(conversion.program);
        inputMode = "CPP";
        inputModeNote = conversion.llmNote;
        return true;
    }

    ParseResult parsed = ParseIRFile(options.inputPath);
    if (!parsed.errors.empty()) {
        std::ostringstream out;
        out << "Input parsing failed with " << parsed.errors.size() << " error(s):\n";
        for (const ParseError& err : parsed.errors) {
            if (err.line > 0) {
                out << "  line " << err.line << ": " << err.message << "\n";
            } else {
                out << "  " << err.message << "\n";
            }
        }
        error = out.str();
        return false;
    }

    original = std::move(parsed.program);
    inputMode = "IR";
    inputModeNote = "";
    return true;
}

void MaybeRunLlmSubstitution(const Options& options,
                             const GeminiClient& gemini,
                             Program& reordered,
                             SubstitutionStats& substitutionStats,
                             std::string& substitutionNote) {
    substitutionStats = SubstitutionStats{};
    substitutionNote.clear();

    if (!options.enableLlmSubstitution) {
        return;
    }

    std::vector<SubstitutionCandidate> candidates = FindSafeSubstitutionCandidates(reordered);
    if (candidates.empty()) {
        substitutionStats.candidateCount = 0;
        substitutionNote = "No safe substitution candidates were found.";
        return;
    }

    std::vector<std::size_t> approvedIds;

    if (gemini.IsConfigured()) {
        const std::string systemPrompt =
            "You are a strict compiler safety checker. "
            "Pick only candidate IDs that are safe for exact integer semantic equivalence. "
            "Return only comma-separated integers.";

        std::string userPrompt = BuildSubstitutionSelectionPrompt(reordered, candidates);
        LlmResult llm = gemini.GenerateText(systemPrompt, userPrompt, 0.1);
        if (llm.ok) {
            std::string cleaned = StripMarkdownCodeFences(llm.text);
            approvedIds = ParseApprovedCandidateIds(cleaned);
            substitutionNote = "LLM candidate selection completed.";
        } else {
            substitutionNote = "LLM substitution selection failed: " + llm.error;
        }
    } else {
        substitutionNote = "LLM is not configured; substitution selection skipped.";
    }

    reordered = ApplyApprovedSubstitutions(reordered, candidates, approvedIds, substitutionStats);

    if (!approvedIds.empty() && substitutionStats.appliedCount == 0) {
        if (!substitutionNote.empty()) {
            substitutionNote += " ";
        }
        substitutionNote += "LLM-approved IDs did not pass deterministic validation.";
    }
}

void MaybeRunLlmExplanation(const Options& options,
                            const GeminiClient& gemini,
                            RunSummary& summary) {
    summary.explanation = BuildDeterministicExplanation(summary);
    summary.explanationFromLlm = false;
    summary.explanationError.clear();

    if (!options.enableLlmExplanation) {
        return;
    }

    if (!gemini.IsConfigured()) {
        summary.explanationError = "Gemini API key not configured; using deterministic explanation.";
        return;
    }

    const std::string systemPrompt =
        "You are a compiler design teaching assistant. "
        "Explain transformations clearly, accurately, and concisely for a viva. "
        "Do not invent metrics. Mention correctness constraints explicitly.";

    std::string userPrompt = BuildExplanationPrompt(summary);
    LlmResult llm = gemini.GenerateText(systemPrompt, userPrompt, 0.2);
    if (!llm.ok) {
        summary.explanationError = "LLM explanation failed: " + llm.error;
        return;
    }

    std::string cleaned = StripMarkdownCodeFences(llm.text);
    if (cleaned.empty()) {
        summary.explanationError = "LLM explanation was empty; using deterministic explanation.";
        return;
    }

    summary.explanation = cleaned;
    summary.explanationFromLlm = true;
}

bool SaveReportsIfRequested(const Options& options, RunSummary& summary, std::string& error) {
    bool wantsAny = !options.reportPath.empty() || !options.reportHtmlPath.empty() ||
                    options.enableLlmExplanation;
    if (!wantsAny) {
        return true;
    }

    std::string markdownPath = options.reportPath;
    if (markdownPath.empty()) {
        markdownPath = "build/llm_report_latest.md";
    }

    std::string markdown = BuildMarkdownReport(summary);
    if (!WriteTextFile(markdownPath, markdown, error)) {
        return false;
    }
    summary.reportPath = markdownPath;

    std::string htmlPath = options.reportHtmlPath;
    if (htmlPath.empty() && options.enableLlmExplanation) {
        htmlPath = "build/llm_report_latest.html";
    }
    if (!htmlPath.empty()) {
        std::string html = BuildHtmlReport(summary, markdown);
        if (!WriteTextFile(htmlPath, html, error)) {
            return false;
        }
        summary.reportHtmlPath = htmlPath;
    }

    return true;
}

int ExecuteSingleRun(const Options& options, RunSummary& summary) {
    summary = RunSummary{};
    summary.inputPath = options.inputPath;
    summary.outputPath = options.outputPath;
    summary.verifyRequested = options.verify;
    summary.llmSubstitutionEnabled = options.enableLlmSubstitution;
    summary.llmExplanationEnabled = options.enableLlmExplanation;

    GeminiConfig geminiConfig = LoadGeminiConfigFromEnvFile(options.envPath);
    GeminiClient gemini(geminiConfig);
    summary.llmConfigured = gemini.IsConfigured();

    Program original;
    std::string parseError;
    if (!ParseInputProgram(options,
                           gemini,
                           original,
                           summary.inputMode,
                           summary.inputModeNote,
                           parseError)) {
        std::cerr << parseError << "\n";
        return 1;
    }

    summary.originalText = ProgramToText(original);

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

        std::size_t movedInsideBlocks = 0;
        for (std::size_t i = 0; i < cfg.blocks.size(); ++i) {
            ShuffleBlockBody(cfg.blocks[i],
                             DeriveSeed(shuffleSeed, static_cast<std::uint64_t>(i + 1)),
                             movedInsideBlocks,
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

    SubstitutionStats substitutionStats;
    std::string substitutionNote;
    MaybeRunLlmSubstitution(options, gemini, reordered, substitutionStats, substitutionNote);

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

    summary.cfgModeEnabled = cfgModeEnabled;
    summary.instructionCount = original.instructions.size();
    summary.blockCount = blockStats.blockCount;
    summary.masterSeed = masterSeed;
    summary.movedInstructionSlots = moved;
    summary.reorderRatio = reorderRatio;
    summary.movedBlockSlots = blockStats.movedBlockSlots;
    summary.blockReorderRatio = blockReorderRatio;
    summary.branchFixupsInserted = blockStats.branchFixupsInserted;
    summary.sideEffectViolations = sideEffectViolations;
    summary.shuffleFallbacksUsed = shuffleFallbackCount;
    summary.renamedSymbols = renamed.renameMap.size();
    summary.originalHash = originalHash;
    summary.transformedHash = transformedHash;
    summary.renameMap = renamed.renameMap;
    summary.transformedText = transformedText;
    summary.substitutionStats = substitutionStats;
    summary.substitutionNote = substitutionNote;
    summary.hazards = AnalyzeHazards(original);

    std::cout << "Anti-Fingerprint Instruction Shuffler Summary\n";
    std::cout << "-------------------------------------------\n";
    std::cout << "Input file                : " << options.inputPath << "\n";
    std::cout << "Input mode                : " << summary.inputMode << "\n";
    if (!summary.inputModeNote.empty()) {
        std::cout << "Input mode note           : " << summary.inputModeNote << "\n";
    }
    std::cout << "Output file               : " << options.outputPath << "\n";
    std::cout << "CFG mode                  : " << (cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n";
    std::cout << "Input CFG validation      : PASS\n";
    std::cout << "Output CFG validation     : PASS\n";
    std::cout << "Instruction count         : " << original.instructions.size() << "\n";
    std::cout << "Basic block count         : " << blockStats.blockCount << "\n";
    std::cout << "Master seed               : " << masterSeed << "\n";
    std::cout << "Moved instruction slots   : " << moved << " / " << original.instructions.size()
              << "\n";
    std::cout << "Reorder ratio             : " << std::fixed << std::setprecision(2)
              << reorderRatio << "%\n";
    std::cout << "Moved block slots         : " << blockStats.movedBlockSlots << " / "
              << blockStats.blockCount << "\n";
    std::cout << "Block reorder ratio       : " << std::fixed << std::setprecision(2)
              << blockReorderRatio << "%\n";
    std::cout << std::defaultfloat;
    std::cout << "Branch fixups inserted    : " << blockStats.branchFixupsInserted << "\n";
    std::cout << "Side-effect violations    : " << sideEffectViolations << "\n";
    std::cout << "Shuffle fallbacks used    : " << shuffleFallbackCount << "\n";
    std::cout << "Renamed symbols           : " << renamed.renameMap.size() << "\n";
    std::cout << "Original IR hash          : 0x" << std::hex << originalHash << std::dec << "\n";
    std::cout << "Transformed IR hash       : 0x" << std::hex << transformedHash << std::dec << "\n";

    std::cout << "LLM configured            : " << (summary.llmConfigured ? "YES" : "NO") << "\n";
    std::cout << "LLM substitution enabled  : "
              << (options.enableLlmSubstitution ? "YES" : "NO") << "\n";
    if (options.enableLlmSubstitution) {
        std::cout << "Substitution candidates   : " << substitutionStats.candidateCount << "\n";
        std::cout << "Substitution approved IDs : " << substitutionStats.llmApprovedCount << "\n";
        std::cout << "Substitutions applied     : " << substitutionStats.appliedCount << "\n";
        if (!substitutionNote.empty()) {
            std::cout << "Substitution note         : " << substitutionNote << "\n";
        }
    }

    int exitCode = 0;

    if (options.verify) {
        ExecutionResult originalExec = ExecuteProgram(original);
        ExecutionResult transformedExec = ExecuteProgram(transformed);

        if (!originalExec.success) {
            std::cerr << "Verification failed while executing original program:\n";
            std::cerr << "  " << originalExec.error << "\n";
            summary.verifyError = originalExec.error;
            exitCode = 2;
        }

        if (exitCode == 0 && !transformedExec.success) {
            std::cerr << "Verification failed while executing transformed program:\n";
            std::cerr << "  " << transformedExec.error << "\n";
            summary.verifyError = transformedExec.error;
            exitCode = 2;
        }

        if (exitCode == 0) {
            bool sameOutput = (originalExec.printedValues == transformedExec.printedValues);
            std::cout << "\nVerification\n";
            std::cout << "------------\n";
            std::cout << "Original output:\n" << PrintedOutputToText(originalExec.printedValues) << "\n";
            std::cout << "Transformed output:\n" << PrintedOutputToText(transformedExec.printedValues) << "\n";
            std::cout << "Semantic equivalence      : " << (sameOutput ? "PASS" : "FAIL") << "\n";

            summary.originalOutput = originalExec.printedValues;
            summary.transformedOutput = transformedExec.printedValues;
            summary.verifyPass = sameOutput;

            if (!sameOutput) {
                exitCode = 3;
            }
        }
    }

    MaybeRunLlmExplanation(options, gemini, summary);

    if (options.enableLlmExplanation) {
        std::cout << "\nExplanation\n";
        std::cout << "-----------\n";
        std::cout << summary.explanation << "\n";
        if (!summary.explanationError.empty()) {
            std::cout << "Explanation note          : " << summary.explanationError << "\n";
        }
    }

    std::string reportError;
    if (!SaveReportsIfRequested(options, summary, reportError)) {
        std::cerr << "Report generation failed: " << reportError << "\n";
        if (exitCode == 0) {
            exitCode = 4;
        }
    } else {
        if (!summary.reportPath.empty()) {
            std::cout << "Report markdown           : " << summary.reportPath << "\n";
        }
        if (!summary.reportHtmlPath.empty()) {
            std::cout << "Report html               : " << summary.reportHtmlPath << "\n";
        }
    }

    if (options.showMap) {
        std::cout << "\n";
        PrintRenameMap(renamed.renameMap);
    }

    return exitCode;
}

std::string PromptLine(const std::string& prompt, const std::string& defaultValue = "") {
    std::cout << prompt;
    if (!defaultValue.empty()) {
        std::cout << " [" << defaultValue << "]";
    }
    std::cout << ": ";
    std::cout.flush();

    std::string line;
    std::getline(std::cin, line);
    line = Trim(line);
    if (line.empty()) {
        return defaultValue;
    }
    return line;
}

bool ParseYesNoAnswer(const std::string& text, bool& value) {
    std::string lower = ToLower(Trim(text));
    if (lower == "y" || lower == "yes" || lower == "1" || lower == "true") {
        value = true;
        return true;
    }
    if (lower == "n" || lower == "no" || lower == "0" || lower == "false") {
        value = false;
        return true;
    }
    return false;
}

bool PromptYesNo(const std::string& prompt, bool defaultValue) {
    std::string def = defaultValue ? "Y/n" : "y/N";
    while (true) {
        std::string line = PromptLine(prompt + " (" + def + ")", "");
        if (line.empty()) {
            return defaultValue;
        }
        bool parsed = false;
        if (ParseYesNoAnswer(line, parsed)) {
            return parsed;
        }
        std::cout << "Please answer yes or no.\n";
    }
}

int RunInteractiveSession(const Options& baseOptions) {
    std::cout << "Interactive mode started. Enter q to quit.\n";

    while (true) {
        Options run = baseOptions;
        run.interactive = false;

        std::string inputPath = PromptLine("Input file (.ir or .cpp, q to quit)", "samples/example_full.ir");
        if (ToLower(inputPath) == "q" || ToLower(inputPath) == "quit") {
            std::cout << "Exiting interactive mode.\n";
            return 0;
        }

        run.inputPath = inputPath;

        std::string defaultOutput = IsCppFilePath(run.inputPath) ? "build/interactive_cpp.ir"
                                                                 : "build/interactive_ir.ir";
        run.outputPath = PromptLine("Output IR path", defaultOutput);

        run.verify = PromptYesNo("Run semantic verification", true);
        run.showMap = PromptYesNo("Show rename map", false);

        bool useFixedSeed = PromptYesNo("Use fixed seed", false);
        if (useFixedSeed) {
            while (true) {
                std::string seedText = PromptLine("Seed value", "123456");
                std::uint64_t parsed = 0;
                if (ParseU64(seedText, parsed)) {
                    run.hasSeed = true;
                    run.seed = parsed;
                    break;
                }
                std::cout << "Seed must be an unsigned integer.\n";
            }
        } else {
            run.hasSeed = false;
            run.seed = 0;
        }

        run.enableLlmSubstitution = PromptYesNo("Enable LLM-assisted substitution", false);
        run.enableLlmExplanation = PromptYesNo("Enable LLM explanation/report", false);

        bool saveMarkdown = PromptYesNo("Write markdown report", run.enableLlmExplanation);
        if (saveMarkdown) {
            run.reportPath = PromptLine("Markdown report path", "build/interactive_report.md");
        } else {
            run.reportPath.clear();
        }

        bool saveHtml = PromptYesNo("Write html report", false);
        if (saveHtml) {
            run.reportHtmlPath = PromptLine("HTML report path", "build/interactive_report.html");
        } else {
            run.reportHtmlPath.clear();
        }

        std::cout << "\nRunning transformation...\n";
        RunSummary summary;
        int exitCode = ExecuteSingleRun(run, summary);
        if (exitCode != 0) {
            std::cout << "Run finished with non-zero exit code: " << exitCode << "\n";
        } else {
            std::cout << "Run completed successfully.\n";
        }

        bool runAgain = PromptYesNo("Run another transformation", true);
        std::cout << "\n";
        if (!runAgain) {
            std::cout << "Exiting interactive mode.\n";
            return 0;
        }
    }
}

}  // namespace
}  // namespace afis

int main(int argc, char** argv) {
    using namespace afis;

    Options options;
    std::string argError;
    bool wantsHelp = false;

    if (!ParseArgs(argc, argv, options, argError, wantsHelp)) {
        if (!argError.empty()) {
            std::cerr << "Argument error: " << argError << "\n\n";
        }
        PrintUsage(argv[0]);
        return wantsHelp ? 0 : 1;
    }

    if (options.interactive) {
        return RunInteractiveSession(options);
    }

    RunSummary summary;
    return ExecuteSingleRun(options, summary);
}