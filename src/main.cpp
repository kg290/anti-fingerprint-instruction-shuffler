#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "cfg.h"
#include "cpp_frontend.h"
#include "dependency.h"
#include "diagram.h"
#include "fingerprint.h"
#include "interpreter.h"
#include "llm.h"
#include "optimizer.h"
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
    bool cleanOutputDir = false;
    bool fixedSeedAcrossRuns = false;

    bool interactive = false;

    bool enableLlmExplanation = false;

    std::string reportPath;
    std::string reportHtmlPath;
    std::string outputDir;

    std::string envPath = ".env";

    bool forceCppInput = false;
    int runs = 1;
    bool preferLlmCpp = true;
    std::string artifactDir;
    bool dumpPasses = false;
    bool emitCfgDot = false;
    bool emitDependencyDot = false;
    bool emitTrace = false;
    bool emitFingerprintReport = false;
    int variants = 1;
    bool selectBestVariant = false;
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
    std::size_t transformedInstructionCount = 0;
    std::size_t transformedBlockCount = 0;
    std::size_t originalSymbolCount = 0;
    std::size_t transformedSymbolCount = 0;
    std::size_t originalLabelCount = 0;
    std::size_t transformedLabelCount = 0;
    std::size_t originalBranchCount = 0;
    std::size_t transformedBranchCount = 0;
    std::size_t originalSideEffectCount = 0;
    std::size_t transformedSideEffectCount = 0;

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
    FingerprintMetrics fingerprintMetrics;

    bool verifyRequested = false;
    bool verifyPass = false;
    std::string verifyError;
    std::vector<long long> originalOutput;
    std::vector<long long> transformedOutput;

    bool llmConfigured = false;
    bool llmExplanationEnabled = false;

    HazardBreakdown hazards;
    OptimizationTrace optimizationTrace;

    std::string explanation;
    bool explanationFromLlm = false;
    std::string explanationError;

    RenameMap renameMap;

    std::string originalText;
    std::string optimizedText;
    std::string shuffledText;
    std::string transformedText;
    std::string artifactDir;
    std::string originalDumpPath;
    std::string constantFoldDumpPath;
    std::string constantPropDumpPath;
    std::string copyPropDumpPath;
    std::string dceDumpPath;
    std::string optimizedDumpPath;
    std::string shuffledDumpPath;
    std::string finalDumpPath;
    std::string cfgDotPath;
    std::string cfgSvgPath;
    std::string dependencyDotPath;
    std::string dependencySvgPath;
    std::string tracePath;
    std::string fingerprintReportPath;
    std::string verificationPath;
    std::string workflowDotPath;
    std::string workflowSvgPath;

    std::string reportPath;
    std::string reportHtmlPath;
};

std::string FormatPercent(double value);
std::string FormatHex64(std::uint64_t value);

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
              << " --input <file> [--output <file>] [--seed <u64>] [--verify] [--show-map] [--runs <n>]\n";
    std::cerr << "  " << exe << " --interactive [--env <file>]\n\n";

    std::cerr << "Core options:\n";
    std::cerr << "  --input, -i <file>      Input file (.ir or .cpp)\n";
    std::cerr << "  --input-cpp <file>      Force C++ input mode (experimental)\n";
    std::cerr << "  --output, -o <file>     Output transformed IR file\n";
    std::cerr << "  --output-dir <dir>      Output directory for batch mode (--runs > 1)\n";
    std::cerr << "  --seed <u64>            Fixed seed for deterministic runs\n";
    std::cerr << "  --runs <n>              Run transformation n times in one command\n";
    std::cerr << "  --fixed-seed-runs       Reuse exactly the same seed for each batch run\n";
    std::cerr << "  --clean-output-dir      Clear --output-dir before batch run\n";
    std::cerr << "  --verify                Execute original and transformed IR and compare output\n";
    std::cerr << "  --show-map              Print rename map\n";
    std::cerr << "  --interactive           Start loop-based interactive mode\n";
    std::cerr << "  --artifact-dir <dir>    Directory for pass dumps, diagrams, traces, and metrics\n";
    std::cerr << "  --dump-passes           Save IR after each major compiler stage\n";
    std::cerr << "  --emit-cfg-dot          Export CFG diagram as .dot\n";
    std::cerr << "  --emit-dep-dot          Export dependency graph as .dot\n";
    std::cerr << "  --emit-trace            Save transformation trace markdown\n";
    std::cerr << "  --fingerprint-report    Save fingerprint metrics markdown\n";
    std::cerr << "  --variants <n>          Generate n transformed variants for one input\n";
    std::cerr << "  --select-best-variant   Rank generated variants and pick the best one\n";

    std::cerr << "LLM options (.env Gemini):\n";
    std::cerr << "  --env <file>            Path to .env file (default: .env)\n";
    std::cerr << "  --llm-explain           Enable explanation mode and auto markdown report\n";
    std::cerr << "  --report <file.md>      Write markdown report\n";
    std::cerr << "  --report-html <file>    Write html report\n";
    std::cerr << "  --no-llm-cpp            Disable LLM assistance for C++ to IR conversion\n\n";

    std::cerr << "Examples:\n";
    std::cerr << "  " << exe
              << " --input samples/example_full.ir --output build/out.ir --verify\n";
    std::cerr << "  " << exe
              << " --input samples/example_full.ir --output build/out.ir --runs 2 --verify\n";
    std::cerr << "  " << exe
              << " --input samples/example_full.ir --output-dir build/demo_runs --runs 3 --verify --clean-output-dir\n";
    std::cerr << "  " << exe
              << " --input samples/example_cpp_basic.cpp --output build/from_cpp.ir --verify --llm-explain\n";
    std::cerr << "  " << exe
              << " --input samples/example_cfg.ir --output build/final.ir --verify --dump-passes --emit-cfg-dot --emit-dep-dot --emit-trace --fingerprint-report --artifact-dir build/demo_case\n";
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

bool ParsePositiveInt(const std::string& text, int& out) {
    try {
        std::size_t idx = 0;
        long value = std::stol(text, &idx, 10);
        if (idx != text.size() || value <= 0 || value > std::numeric_limits<int>::max()) {
            return false;
        }
        out = static_cast<int>(value);
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

        if (arg == "--output-dir") {
            if (i + 1 >= argc) {
                error = "Missing value for --output-dir";
                return false;
            }
            options.outputDir = argv[++i];
            continue;
        }

        if (arg == "--artifact-dir") {
            if (i + 1 >= argc) {
                error = "Missing value for --artifact-dir";
                return false;
            }
            options.artifactDir = argv[++i];
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

        if (arg == "--runs") {
            if (i + 1 >= argc) {
                error = "Missing value for --runs";
                return false;
            }
            int parsedRuns = 1;
            if (!ParsePositiveInt(argv[++i], parsedRuns)) {
                error = "Invalid --runs value: " + std::string(argv[i]);
                return false;
            }
            options.runs = parsedRuns;
            continue;
        }

        if (arg == "--fixed-seed-runs") {
            options.fixedSeedAcrossRuns = true;
            continue;
        }

        if (arg == "--clean-output-dir") {
            options.cleanOutputDir = true;
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

        if (arg == "--dump-passes") {
            options.dumpPasses = true;
            continue;
        }

        if (arg == "--emit-cfg-dot") {
            options.emitCfgDot = true;
            continue;
        }

        if (arg == "--emit-dep-dot") {
            options.emitDependencyDot = true;
            continue;
        }

        if (arg == "--emit-trace") {
            options.emitTrace = true;
            continue;
        }

        if (arg == "--fingerprint-report") {
            options.emitFingerprintReport = true;
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

        if (arg == "--variants") {
            if (i + 1 >= argc) {
                error = "Missing value for --variants";
                return false;
            }
            int parsedVariants = 1;
            if (!ParsePositiveInt(argv[++i], parsedVariants)) {
                error = "Invalid --variants value: " + std::string(argv[i]);
                return false;
            }
            options.variants = parsedVariants;
            continue;
        }

        if (arg == "--select-best-variant") {
            options.selectBestVariant = true;
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

    if (options.runs <= 0) {
        error = "--runs must be >= 1";
        return false;
    }

    if (options.interactive && options.runs > 1) {
        error = "--runs cannot be combined with --interactive";
        return false;
    }

    if (options.interactive && options.variants > 1) {
        error = "--variants cannot be combined with --interactive";
        return false;
    }

    if (options.cleanOutputDir && options.outputDir.empty()) {
        error = "--clean-output-dir requires --output-dir";
        return false;
    }

    if (options.runs > 1 && options.variants > 1) {
        error = "--runs and --variants cannot both be greater than 1";
        return false;
    }

    return true;
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
    try {
        std::filesystem::path outPath(path);
        std::filesystem::path parent = outPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
    } catch (const std::exception& ex) {
        error = std::string("Could not prepare output directory for file: ") + path + " (" +
                ex.what() + ")";
        return false;
    } catch (...) {
        error = "Could not prepare output directory for file: " + path;
        return false;
    }

    std::ofstream out(path);
    if (!out) {
        error = "Could not write output file: " + path;
        return false;
    }

    out << content;
    return true;
}

std::string SafeFileStem(const std::string& path) {
    std::filesystem::path p(path);
    std::string stem = p.stem().string();
    if (stem.empty()) {
        stem = "run";
    }
    for (char& ch : stem) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    return stem;
}

std::string ResolveArtifactDir(const Options& options) {
    if (!options.artifactDir.empty()) {
        return options.artifactDir;
    }
    std::filesystem::path outputPath(options.outputPath);
    std::filesystem::path parent = outputPath.parent_path();
    std::string dirName = SafeFileStem(options.outputPath) + "_artifacts";
    if (parent.empty()) {
        return dirName;
    }
    return (parent / dirName).string();
}

bool EnsureDirectory(const std::string& path, std::string& error) {
    try {
        if (!path.empty()) {
            std::filesystem::create_directories(path);
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not create directory: ") + path + " (" + ex.what() + ")";
        return false;
    } catch (...) {
        error = "Could not create directory: " + path;
        return false;
    }
}

bool MaybeWriteProgramArtifact(const std::string& artifactDir,
                               const std::string& fileName,
                               const Program& program,
                               std::string& savedPath,
                               std::string& error) {
    if (artifactDir.empty()) {
        savedPath.clear();
        return true;
    }
    std::filesystem::path path = std::filesystem::path(artifactDir) / fileName;
    if (!WriteTextFile(path.string(), ProgramToText(program), error)) {
        return false;
    }
    savedPath = path.string();
    return true;
}

std::string BuildTransformationTraceMarkdown(const RunSummary& summary) {
    std::ostringstream out;
    out << "# AFIS Transformation Trace\n\n";
    out << "- Input: " << summary.inputPath << "\n";
    out << "- Output: " << summary.outputPath << "\n";
    out << "- Seed: " << summary.masterSeed << "\n";
    out << "- CFG mode: " << (summary.cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n\n";

    out << "## Optimization Totals\n\n";
    out << "- Constant folds: " << summary.optimizationTrace.constantFolds << "\n";
    out << "- Constant propagations: " << summary.optimizationTrace.constantPropagations << "\n";
    out << "- Copy propagations: " << summary.optimizationTrace.copyPropagations << "\n";
    out << "- Dead instructions removed: " << summary.optimizationTrace.deadInstructionsRemoved << "\n";
    out << "- Moved instruction slots: " << summary.movedInstructionSlots << "\n";
    out << "- Moved block slots: " << summary.movedBlockSlots << "\n";
    out << "- Verification: ";
    if (!summary.verifyRequested) {
        out << "NOT RUN\n\n";
    } else if (!summary.verifyError.empty()) {
        out << "ERROR\n\n";
    } else {
        out << (summary.verifyPass ? "PASS" : "FAIL") << "\n\n";
    }

    out << "## Stage Artifacts\n\n";
    if (!summary.originalDumpPath.empty()) {
        out << "- Original IR: " << summary.originalDumpPath << "\n";
    }
    if (!summary.constantFoldDumpPath.empty()) {
        out << "- After constant folding: " << summary.constantFoldDumpPath << "\n";
    }
    if (!summary.constantPropDumpPath.empty()) {
        out << "- After constant propagation: " << summary.constantPropDumpPath << "\n";
    }
    if (!summary.copyPropDumpPath.empty()) {
        out << "- After copy propagation: " << summary.copyPropDumpPath << "\n";
    }
    if (!summary.dceDumpPath.empty()) {
        out << "- After dead code elimination: " << summary.dceDumpPath << "\n";
    }
    if (!summary.optimizedDumpPath.empty()) {
        out << "- Optimized IR: " << summary.optimizedDumpPath << "\n";
    }
    if (!summary.shuffledDumpPath.empty()) {
        out << "- Shuffled IR: " << summary.shuffledDumpPath << "\n";
    }
    if (!summary.finalDumpPath.empty()) {
        out << "- Final IR: " << summary.finalDumpPath << "\n";
    }
    if (!summary.cfgDotPath.empty()) {
        out << "- CFG dot: " << summary.cfgDotPath << "\n";
    }
    if (!summary.dependencyDotPath.empty()) {
        out << "- Dependency dot: " << summary.dependencyDotPath << "\n";
    }
    if (!summary.cfgSvgPath.empty()) {
        out << "- CFG SVG: " << summary.cfgSvgPath << "\n";
    }
    if (!summary.dependencySvgPath.empty()) {
        out << "- Dependency SVG: " << summary.dependencySvgPath << "\n";
    }
    if (!summary.tracePath.empty()) {
        out << "- Transformation trace: " << summary.tracePath << "\n";
    }
    if (!summary.fingerprintReportPath.empty()) {
        out << "- Fingerprint report: " << summary.fingerprintReportPath << "\n";
    }
    if (!summary.verificationPath.empty()) {
        out << "- Verification text: " << summary.verificationPath << "\n";
    }
    if (!summary.workflowDotPath.empty()) {
        out << "- Workflow dot: " << summary.workflowDotPath << "\n";
    }
    if (!summary.workflowSvgPath.empty()) {
        out << "- Workflow SVG: " << summary.workflowSvgPath << "\n";
    }
    out << "\n";

    out << "## Detailed Changes\n\n";
    if (summary.optimizationTrace.changes.empty()) {
        out << "No optimization changes were recorded.\n";
    } else {
        for (const OptimizationChange& change : summary.optimizationTrace.changes) {
            out << "### " << change.passName << "\n\n";
            out << "- Note: " << change.note << "\n";
            out << "- Before: `" << change.before << "`\n";
            if (change.removed) {
                out << "- After: `(removed)`\n\n";
            } else {
                out << "- After: `" << change.after << "`\n\n";
            }
        }
    }

    return out.str();
}

std::string BuildFingerprintMarkdown(const RunSummary& summary) {
    std::ostringstream out;
    out << "# AFIS Fingerprint Metrics\n\n";
    out << "- Original hash: " << FormatHex64(summary.fingerprintMetrics.originalHash) << "\n";
    out << "- Transformed hash: " << FormatHex64(summary.fingerprintMetrics.transformedHash) << "\n";
    out << "- Moved instruction slots: " << summary.fingerprintMetrics.movedInstructionSlots << "\n";
    out << "- Reorder ratio: " << FormatPercent(summary.fingerprintMetrics.reorderRatio) << "\n";
    out << "- Moved block slots: " << summary.fingerprintMetrics.movedBlockSlots << "\n";
    out << "- Block reorder ratio: " << FormatPercent(summary.fingerprintMetrics.blockReorderRatio) << "\n";
    out << "- Renamed symbols: " << summary.fingerprintMetrics.renamedSymbols << "\n";
    out << "- Diversification score: " << std::fixed << std::setprecision(2)
        << summary.fingerprintMetrics.diversificationScore << "\n";
    return out.str();
}

std::string BuildVerificationText(const RunSummary& summary) {
    std::ostringstream out;
    out << "AFIS Verification Result\n";
    out << "Input: " << summary.inputPath << "\n";
    out << "Output: " << summary.outputPath << "\n";
    out << "Verification requested: " << (summary.verifyRequested ? "YES" : "NO") << "\n";
    if (!summary.verifyRequested) {
        out << "Status: NOT RUN\n";
        return out.str();
    }
    if (!summary.verifyError.empty()) {
        out << "Status: ERROR\n";
        out << "Error: " << summary.verifyError << "\n";
        return out.str();
    }
    out << "Status: " << (summary.verifyPass ? "PASS" : "FAIL") << "\n";
    out << "Original output:\n" << PrintedOutputToText(summary.originalOutput) << "\n";
    out << "Transformed output:\n" << PrintedOutputToText(summary.transformedOutput) << "\n";
    return out.str();
}

std::vector<WorkflowStepDiagram> BuildWorkflowSteps(const RunSummary& summary) {
    std::vector<WorkflowStepDiagram> steps;
    steps.push_back({"Input Parsing", summary.originalDumpPath.empty()
                                          ? "Parse input IR or convert C++ to IR"
                                          : "Output: " + summary.originalDumpPath});
    std::ostringstream optimizationDetail;
    if (summary.constantFoldDumpPath.empty() && summary.constantPropDumpPath.empty() &&
        summary.copyPropDumpPath.empty() && summary.dceDumpPath.empty()) {
        optimizationDetail << "Constant fold, constant propagate, copy propagate, DCE";
    } else {
        optimizationDetail << "Fold: "
                           << (summary.constantFoldDumpPath.empty() ? "(not saved)"
                                                                   : summary.constantFoldDumpPath)
                           << "\n";
        optimizationDetail << "Const-prop: "
                           << (summary.constantPropDumpPath.empty() ? "(not saved)"
                                                                   : summary.constantPropDumpPath)
                           << "\n";
        optimizationDetail << "Copy-prop: "
                           << (summary.copyPropDumpPath.empty() ? "(not saved)"
                                                                : summary.copyPropDumpPath)
                           << "\n";
        optimizationDetail << "DCE: "
                           << (summary.dceDumpPath.empty() ? "(not saved)" : summary.dceDumpPath);
    }
    steps.push_back({"Optimization", optimizationDetail.str()});
    steps.push_back({"CFG / Dependency Analysis",
                     summary.cfgDotPath.empty() && summary.dependencyDotPath.empty()
                         ? "Build CFG and dependency graph"
                         : "CFG: " + summary.cfgDotPath +
                               (summary.dependencyDotPath.empty()
                                    ? ""
                                    : "\nDependency: " + summary.dependencyDotPath)});
    steps.push_back({"Safe Shuffling / Reordering",
                     summary.shuffledDumpPath.empty()
                         ? "Reorder only when hazards and barriers allow it"
                         : "Output: " + summary.shuffledDumpPath});
    steps.push_back({"Register Renaming",
                     summary.finalDumpPath.empty() ? "Rename identifiers consistently"
                                                   : "Output: " + summary.finalDumpPath});
    steps.push_back({"Fingerprint Metrics",
                     summary.fingerprintReportPath.empty()
                         ? "Compare structural change before vs after"
                         : "Output: " + summary.fingerprintReportPath});
    steps.push_back({"Verification",
                     summary.verificationPath.empty()
                         ? "Compare original and transformed outputs"
                         : "Output: " + summary.verificationPath});
    return steps;
}

bool SaveAuxiliaryArtifactsIfRequested(const Options& options,
                                       RunSummary& summary,
                                       std::string& error) {
    if (summary.artifactDir.empty()) {
        return true;
    }

    std::filesystem::path root(summary.artifactDir);

    if (options.emitTrace) {
        std::filesystem::path tracePath = root / "trace.md";
        summary.tracePath = tracePath.string();
    }

    if (options.emitFingerprintReport) {
        std::filesystem::path fingerprintPath = root / "fingerprint.md";
        summary.fingerprintReportPath = fingerprintPath.string();
        if (!WriteTextFile(summary.fingerprintReportPath, BuildFingerprintMarkdown(summary), error)) {
            return false;
        }
    }

    if (summary.verifyRequested) {
        std::filesystem::path verificationPath = root / "verification.txt";
        summary.verificationPath = verificationPath.string();
        if (!WriteTextFile(summary.verificationPath, BuildVerificationText(summary), error)) {
            return false;
        }
    }

    std::filesystem::path workflowDotPath = root / "workflow.dot";
    summary.workflowDotPath = workflowDotPath.string();
    std::filesystem::path workflowSvgPath = root / "workflow.svg";
    summary.workflowSvgPath = workflowSvgPath.string();

    std::vector<WorkflowStepDiagram> workflowSteps = BuildWorkflowSteps(summary);

    if (!WriteWorkflowDot(workflowSteps, workflowDotPath.string(), error)) {
        return false;
    }

    if (!WriteWorkflowSvg(workflowSteps, workflowSvgPath.string(), error)) {
        return false;
    }

    if (options.emitTrace) {
        if (!WriteTextFile(summary.tracePath, BuildTransformationTraceMarkdown(summary), error)) {
            return false;
        }
    }

    return true;
}

struct StageExplanation {
    std::string title;
    std::string purpose;
    std::string runEvidence;
    std::string safetyReason;
    std::string artifactPath;
};

std::vector<StageExplanation> BuildStageExplanations(const RunSummary& summary) {
    std::vector<StageExplanation> stages;
    stages.push_back({"Step 1: Input Parsing",
                      "Convert the input into AFIS IR so later analyses work on one simple internal format.",
                      summary.originalDumpPath.empty() ? "Original IR artifact was not saved in this run."
                                                       : "Original IR saved at " + summary.originalDumpPath,
                      "This stage is safe because it only reads and normalizes the input program before any transformation.",
                      summary.originalDumpPath});

    std::ostringstream optimizationEvidence;
    optimizationEvidence << "Folds=" << summary.optimizationTrace.constantFolds
                         << ", const-prop=" << summary.optimizationTrace.constantPropagations
                         << ", copy-prop=" << summary.optimizationTrace.copyPropagations
                         << ", dead removed=" << summary.optimizationTrace.deadInstructionsRemoved;
    if (!summary.dceDumpPath.empty()) {
        optimizationEvidence << ". Final optimized IR: " << summary.dceDumpPath;
    }
    stages.push_back({"Step 2: Optimization",
                      "Simplify the IR before shuffling so the later transformation works on cleaner code.",
                      optimizationEvidence.str(),
                      "Only local, conservative optimizations are applied, and side-effecting or control-flow instructions are not removed unsafely.",
                      summary.dceDumpPath});

    std::ostringstream analysisEvidence;
    analysisEvidence << "RAW=" << summary.hazards.raw << ", WAR=" << summary.hazards.war
                     << ", WAW=" << summary.hazards.waw << ", barriers=" << summary.hazards.barrier;
    if (!summary.cfgDotPath.empty()) {
        analysisEvidence << ". CFG diagram: " << summary.cfgDotPath;
    }
    if (!summary.dependencyDotPath.empty()) {
        analysisEvidence << ". Dependency diagram: " << summary.dependencyDotPath;
    }
    stages.push_back({"Step 3: CFG / Dependency Analysis",
                      "Find which instructions and blocks depend on each other, so AFIS knows what is movable and what must stay ordered.",
                      analysisEvidence.str(),
                      "This stage builds legality constraints. Later shuffling is allowed only when these dependency and barrier constraints are respected.",
                      summary.cfgDotPath.empty() ? summary.dependencyDotPath : summary.cfgDotPath});

    std::ostringstream shuffleEvidence;
    shuffleEvidence << "Moved instruction slots=" << summary.movedInstructionSlots
                    << ", moved block slots=" << summary.movedBlockSlots
                    << ", branch fixups=" << summary.branchFixupsInserted;
    if (!summary.shuffledDumpPath.empty()) {
        shuffleEvidence << ". Shuffled IR: " << summary.shuffledDumpPath;
    }
    stages.push_back({"Step 4: Safe Shuffling / Reordering",
                      "Change program layout to reduce fingerprint similarity while preserving valid execution order.",
                      shuffleEvidence.str(),
                      "Reordering is restricted by hazards, side-effect barriers, and CFG fixups. The report also tracks side-effect violations, which should stay 0.",
                      summary.shuffledDumpPath});

    std::ostringstream renameEvidence;
    renameEvidence << "Renamed symbols=" << summary.renamedSymbols;
    if (!summary.finalDumpPath.empty()) {
        renameEvidence << ". Final IR: " << summary.finalDumpPath;
    }
    stages.push_back({"Step 5: Register Renaming",
                      "Rename symbols so the transformed IR looks different even when logic stays the same.",
                      renameEvidence.str(),
                      "The renaming map is consistent, so every use and definition of a symbol is rewritten together without changing data flow.",
                      summary.finalDumpPath});

    std::ostringstream fingerprintEvidence;
    fingerprintEvidence << "Reorder ratio=" << FormatPercent(summary.reorderRatio)
                        << ", block reorder ratio=" << FormatPercent(summary.blockReorderRatio)
                        << ", diversification score=" << std::fixed << std::setprecision(2)
                        << summary.fingerprintMetrics.diversificationScore;
    stages.push_back({"Step 6: Fingerprint Metrics",
                      "Measure how much the transformed program structurally differs from the original one.",
                      fingerprintEvidence.str(),
                      "These metrics do not change the program. They help explain anti-fingerprint impact using visible numbers.",
                      summary.fingerprintReportPath.empty() ? summary.reportHtmlPath
                                                            : summary.fingerprintReportPath});

    std::string verificationEvidence;
    if (!summary.verifyRequested) {
        verificationEvidence = "Verification was not requested in this run.";
    } else if (!summary.verifyError.empty()) {
        verificationEvidence = "Verification error: " + summary.verifyError;
    } else {
        verificationEvidence = std::string("Verification result: ") +
                               (summary.verifyPass ? "PASS" : "FAIL");
    }
    stages.push_back({"Step 7: Verification",
                      "Prove that the transformed IR still behaves like the original program.",
                      verificationEvidence,
                      "AFIS executes both original and transformed programs and compares their printed outputs directly.",
                      summary.verificationPath.empty() ? summary.reportHtmlPath : summary.verificationPath});

    return stages;
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

std::size_t CountUniqueDataSymbols(const Program& program) {
    std::unordered_set<std::string> symbols;
    for (const Instruction& inst : program.instructions) {
        for (const std::string& token : ReadSet(inst)) {
            if (IsIdentifier(token)) {
                symbols.insert(token);
            }
        }
        for (const std::string& token : WriteSet(inst)) {
            if (IsIdentifier(token)) {
                symbols.insert(token);
            }
        }
    }
    return symbols.size();
}

std::size_t CountInstructionsWithOp(const Program& program, OpCode op) {
    return static_cast<std::size_t>(
        std::count_if(program.instructions.begin(),
                      program.instructions.end(),
                      [op](const Instruction& inst) { return inst.op == op; }));
}

std::size_t CountBranchInstructions(const Program& program) {
    return static_cast<std::size_t>(
        std::count_if(program.instructions.begin(),
                      program.instructions.end(),
                      [](const Instruction& inst) {
                          return inst.op == OpCode::Goto || inst.op == OpCode::IfGoto;
                      }));
}

std::size_t CountSideEffectInstructions(const Program& program) {
    return static_cast<std::size_t>(
        std::count_if(program.instructions.begin(),
                      program.instructions.end(),
                      [](const Instruction& inst) { return IsSideEffectOperation(inst); }));
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

    out << "This run started from ";
    out << (summary.inputMode == "CPP" ? "C++ input converted into IR" : "IR input");
    out << " and produced a semantics-preserving transformed IR. ";
    out << "Optimization changed the program with "
        << summary.optimizationTrace.constantFolds << " folds, "
        << summary.optimizationTrace.constantPropagations << " constant propagations, "
        << summary.optimizationTrace.copyPropagations << " copy propagations, and "
        << summary.optimizationTrace.deadInstructionsRemoved << " dead instructions removed. ";
    out << "AFIS then reordered " << summary.movedInstructionSlots
        << " instruction slots";
    if (summary.cfgModeEnabled) {
        out << " and " << summary.movedBlockSlots << " block slots";
    }
    out << ", renamed " << summary.renamedSymbols << " symbols, and kept legality under RAW/WAR/WAW/barrier counts of "
        << summary.hazards.raw << "/" << summary.hazards.war << "/" << summary.hazards.waw
        << "/" << summary.hazards.barrier << ". ";
    if (summary.cfgModeEnabled) {
        out << "CFG-aware reordering inserted " << summary.branchFixupsInserted
            << " branch fixups where needed. ";
    }
    out << "The transformed program hash changed from " << FormatHex64(summary.originalHash)
        << " to " << FormatHex64(summary.transformedHash) << ". ";
    if (summary.verifyRequested) {
        out << "Verification result: " << (summary.verifyPass ? "PASS" : "FAIL");
        if (!summary.verifyError.empty()) {
            out << " (" << summary.verifyError << ")";
        }
        out << ".";
    } else {
        out << "Verification was not requested.";
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

    out << "Write the executive summary paragraph for an AFIS compiler transformation HTML report.\n";
    out << "Use 5 concise sentences in one paragraph.\n";
    out << "Sentence 1: give the verdict, input mode, and verification result.\n";
    out << "Sentence 2: summarize optimizer effects using the exact optimization counts.\n";
    out << "Sentence 3: compare the original and transformed fingerprint shape using instruction count, symbol count, and hashes.\n";
    out << "Sentence 4: summarize movement/diversification using moved instruction slots, moved block slots, branch fixups, renamed symbols, and diversification score.\n";
    out << "Sentence 5: explain why the transformation stayed safe using hazard counts, side-effect violations, and branch validation.\n";
    out << "Do not use bullet points, markdown headings, generic theory, or marketing language.\n";
    out << "Use the exact metric names below. Do not describe instruction reorder ratio as edge movement.\n";
    out << "Do not invent graph edge counts, performance claims, security claims, or validation methods not listed here.\n";
    out << "Mention that Gemini is only summarizing run evidence if it fits naturally, but do not make that the main point.\n\n";

    out << "Metrics:\n";
    out << "- Input mode: " << summary.inputMode << "\n";
    out << "- CFG mode: " << (summary.cfgModeEnabled ? "ENABLED" : "DISABLED") << "\n";
    out << "- Verification requested: " << (summary.verifyRequested ? "YES" : "NO") << "\n";
    out << "- Verification result: ";
    if (!summary.verifyRequested) {
        out << "NOT RUN\n";
    } else if (!summary.verifyError.empty()) {
        out << "ERROR (" << summary.verifyError << ")\n";
    } else {
        out << (summary.verifyPass ? "PASS" : "FAIL") << "\n";
    }
    out << "- Input branch validation: PASS\n";
    out << "- Output branch validation: PASS\n";
    out << "- Constant folds: " << summary.optimizationTrace.constantFolds << "\n";
    out << "- Constant propagations: " << summary.optimizationTrace.constantPropagations << "\n";
    out << "- Copy propagations: " << summary.optimizationTrace.copyPropagations << "\n";
    out << "- Dead instructions removed: "
        << summary.optimizationTrace.deadInstructionsRemoved << "\n";
    out << "- Original instruction count: " << summary.instructionCount << "\n";
    out << "- Transformed instruction count: " << summary.transformedInstructionCount << "\n";
    out << "- Original data symbols: " << summary.originalSymbolCount << "\n";
    out << "- Transformed data symbols: " << summary.transformedSymbolCount << "\n";
    out << "- Original side-effect operations: " << summary.originalSideEffectCount << "\n";
    out << "- Transformed side-effect operations: " << summary.transformedSideEffectCount
        << "\n";
    out << "- Original hash: " << FormatHex64(summary.originalHash) << "\n";
    out << "- Transformed hash: " << FormatHex64(summary.transformedHash) << "\n";
    out << "- Moved instruction slots: " << summary.movedInstructionSlots << " of "
        << summary.instructionCount << "\n";
    out << "- Instruction-slot reorder ratio: " << std::fixed << std::setprecision(2)
        << summary.reorderRatio << "%\n";
    out << "- Moved block slots: " << summary.movedBlockSlots << " of " << summary.blockCount
        << "\n";
    out << "- Block-slot reorder ratio: " << std::fixed << std::setprecision(2)
        << summary.blockReorderRatio << "%\n";
    out << std::defaultfloat;
    out << "- Branch fixups inserted: " << summary.branchFixupsInserted << "\n";
    out << "- Side-effect violations: " << summary.sideEffectViolations << "\n";
    out << "- Renamed symbols: " << summary.renamedSymbols << "\n";
    out << "- Diversification score: " << std::fixed << std::setprecision(2)
        << summary.fingerprintMetrics.diversificationScore << "\n";
    out << std::defaultfloat;
    out << "- Hazard counts RAW/WAR/WAW/barrier: " << summary.hazards.raw << "/"
        << summary.hazards.war << "/" << summary.hazards.waw << "/" << summary.hazards.barrier << "\n";

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
    out << "- Summary source: "
        << (summary.explanationFromLlm ? "Gemini" : "deterministic fallback") << "\n";
    if (!summary.artifactDir.empty()) {
        out << "- Artifact directory: " << summary.artifactDir << "\n";
    }
    out << "\n";

    out << "## Optimization Summary\n\n";
    out << "- Constant folds: " << summary.optimizationTrace.constantFolds << "\n";
    out << "- Constant propagations: " << summary.optimizationTrace.constantPropagations << "\n";
    out << "- Copy propagations: " << summary.optimizationTrace.copyPropagations << "\n";
    out << "- Dead instructions removed: " << summary.optimizationTrace.deadInstructionsRemoved << "\n";
    out << "\n";

    out << "## Gemini Role\n\n";
    out << "- Compiler core: deterministic\n";
    out << "- Gemini role: explanation/report assistance";
    if (summary.inputMode == "CPP") {
        out << " and optional C++ to IR frontend help";
    }
    out << "\n";
    out << "- Verification: performed by AFIS itself through original vs transformed output comparison\n\n";

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
    out << "- Diversification score: " << std::fixed << std::setprecision(2)
        << summary.fingerprintMetrics.diversificationScore << "\n";
    out << std::defaultfloat;
    out << "\n";

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

    out << "## Demo Artifacts\n\n";
    if (!summary.originalDumpPath.empty()) {
        out << "- Original IR: " << summary.originalDumpPath << "\n";
    }
    if (!summary.constantFoldDumpPath.empty()) {
        out << "- After constant folding: " << summary.constantFoldDumpPath << "\n";
    }
    if (!summary.constantPropDumpPath.empty()) {
        out << "- After constant propagation: " << summary.constantPropDumpPath << "\n";
    }
    if (!summary.copyPropDumpPath.empty()) {
        out << "- After copy propagation: " << summary.copyPropDumpPath << "\n";
    }
    if (!summary.dceDumpPath.empty()) {
        out << "- After dead code elimination: " << summary.dceDumpPath << "\n";
    }
    if (!summary.optimizedDumpPath.empty()) {
        out << "- Optimized IR: " << summary.optimizedDumpPath << "\n";
    }
    if (!summary.shuffledDumpPath.empty()) {
        out << "- Shuffled IR: " << summary.shuffledDumpPath << "\n";
    }
    if (!summary.finalDumpPath.empty()) {
        out << "- Final IR: " << summary.finalDumpPath << "\n";
    }
    if (!summary.cfgDotPath.empty()) {
        out << "- CFG dot: " << summary.cfgDotPath << "\n";
    }
    if (!summary.dependencyDotPath.empty()) {
        out << "- Dependency dot: " << summary.dependencyDotPath << "\n";
    }
    if (!summary.cfgSvgPath.empty()) {
        out << "- CFG SVG: " << summary.cfgSvgPath << "\n";
    }
    if (!summary.dependencySvgPath.empty()) {
        out << "- Dependency SVG: " << summary.dependencySvgPath << "\n";
    }
    if (!summary.tracePath.empty()) {
        out << "- Transformation trace: " << summary.tracePath << "\n";
    }
    if (!summary.fingerprintReportPath.empty()) {
        out << "- Fingerprint report: " << summary.fingerprintReportPath << "\n";
    }
    if (!summary.verificationPath.empty()) {
        out << "- Verification text: " << summary.verificationPath << "\n";
    }
    if (!summary.workflowDotPath.empty()) {
        out << "- Workflow dot: " << summary.workflowDotPath << "\n";
    }
    if (!summary.workflowSvgPath.empty()) {
        out << "- Workflow SVG: " << summary.workflowSvgPath << "\n";
    }
    if (summary.originalDumpPath.empty() && summary.constantFoldDumpPath.empty() &&
        summary.constantPropDumpPath.empty() && summary.copyPropDumpPath.empty() &&
        summary.dceDumpPath.empty() && summary.optimizedDumpPath.empty() &&
        summary.shuffledDumpPath.empty() && summary.finalDumpPath.empty() &&
        summary.cfgDotPath.empty() && summary.dependencyDotPath.empty() &&
        summary.cfgSvgPath.empty() && summary.dependencySvgPath.empty() &&
        summary.tracePath.empty() && summary.fingerprintReportPath.empty() &&
        summary.verificationPath.empty() && summary.workflowDotPath.empty() &&
        summary.workflowSvgPath.empty()) {
        out << "(No demo artifacts were requested)\n";
    }
    out << "\n";

    out << "## Stage-By-Stage Walkthrough\n\n";
    std::vector<StageExplanation> stageExplanations = BuildStageExplanations(summary);
    for (const StageExplanation& stage : stageExplanations) {
        out << "### " << stage.title << "\n\n";
        out << "- What this step does: " << stage.purpose << "\n";
        out << "- What happened in this run: " << stage.runEvidence << "\n";
        out << "- Why this step is safe / useful: " << stage.safetyReason << "\n";
        if (!stage.artifactPath.empty()) {
            out << "- Best file to show in demo: " << stage.artifactPath << "\n";
        }
        out << "\n";
    }

    out << "## Explanation\n\n";
    out << "Source: " << (summary.explanationFromLlm ? "Gemini" : "deterministic fallback")
        << "\n\n";
    out << (summary.explanation.empty() ? "(No explanation generated)" : summary.explanation) << "\n\n";
    if (!summary.explanationError.empty()) {
        out << "Explanation note: " << summary.explanationError << "\n\n";
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

    out << "## IR Snapshots\n\n";
    out << "### Original IR\n\n```\n" << summary.originalText << "\n```\n\n";
    out << "### After Optimization\n\n```\n" << summary.optimizedText << "\n```\n\n";
    out << "### After Safe Shuffling / Reordering\n\n```\n" << summary.shuffledText << "\n```\n\n";

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

std::string BuildHtmlReport(const RunSummary& summary, [[maybe_unused]] const std::string& markdownBody) {
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

    auto fileNameOnly = [](const std::string& path) -> std::string {
        if (path.empty()) {
            return "";
        }
        return std::filesystem::path(path).filename().string();
    };

    auto appendArtifactRow = [&](std::ostringstream& out,
                                 const std::string& label,
                                 const std::string& path) {
        if (path.empty()) {
            return;
        }
        const std::string name = fileNameOnly(path);
        out << "<tr><th>" << EscapeHtml(label) << "</th><td><a href=\""
            << EscapeHtml(name) << "\">" << EscapeHtml(name) << "</a></td></tr>";
    };

    auto signedDelta = [](std::size_t before, std::size_t after) -> std::string {
        if (after == before) {
            return "0";
        }
        if (after > before) {
            return "+" + std::to_string(after - before);
        }
        return "-" + std::to_string(before - after);
    };

    auto appendComparisonRow = [&](std::ostringstream& out,
                                   const std::string& label,
                                   const std::string& before,
                                   const std::string& after,
                                   const std::string& change) {
        out << "<tr><th>" << EscapeHtml(label) << "</th><td>" << EscapeHtml(before)
            << "</td><td>" << EscapeHtml(after) << "</td><td>" << EscapeHtml(change)
            << "</td></tr>";
    };

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
        << ".diagram-wrap{background:#fbfdff;border:1px solid var(--line);border-radius:12px;padding:10px;overflow:auto;}"
        << ".summary{line-height:1.55;font-size:1rem;}"
        << ".artifact-table a{color:#0f4c81;text-decoration:none;font-weight:600;}"
        << ".artifact-table a:hover{text-decoration:underline;}"
        << ".stack{display:grid;gap:10px;}"
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
    html << "<span class=\"chip\">Summary: "
         << (summary.explanationFromLlm ? "Gemini" : "deterministic")
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
    html << "<div class=\"card\"><div class=\"label\">Diversification Score</div><div class=\"value\">"
        << std::fixed << std::setprecision(2) << summary.fingerprintMetrics.diversificationScore
        << "</div></div>";
    html << std::defaultfloat;
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
    html << "<tr><th>Artifact folder</th><td>" << EscapeHtml(summary.artifactDir) << "</td></tr>";
    html << "</tbody></table></div>";

    html << "<div class=\"section\"><h2>"
         << (summary.explanationFromLlm ? "Gemini Summary" : "Deterministic Summary")
         << "</h2>";
    html << "<div class=\"summary\">" << HtmlWithBreaks(summary.explanation.empty()
                                                         ? "(No summary generated)"
                                                         : summary.explanation)
         << "</div>";
    if (!summary.explanationError.empty()) {
        html << "<p><strong>Explanation note:</strong> "
             << EscapeHtml(summary.explanationError) << "</p>";
    }
    html << "</div>";

    html << "<div class=\"section\"><h2>What Changed</h2>";
    html << "<div class=\"grid\">";
    html << "<div class=\"card\"><div class=\"label\">Constant Folds</div><div class=\"value\">"
        << summary.optimizationTrace.constantFolds << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Constant Propagations</div><div class=\"value\">"
        << summary.optimizationTrace.constantPropagations << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Copy Propagations</div><div class=\"value\">"
        << summary.optimizationTrace.copyPropagations << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Dead Instructions Removed</div><div class=\"value\">"
        << summary.optimizationTrace.deadInstructionsRemoved << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Moved Instruction Slots</div><div class=\"value\">"
        << summary.movedInstructionSlots << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Moved Block Slots</div><div class=\"value\">"
        << summary.movedBlockSlots << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Branch Fixups</div><div class=\"value\">"
        << summary.branchFixupsInserted << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Renamed Symbols</div><div class=\"value\">"
        << summary.renamedSymbols << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Side-Effect Violations</div><div class=\"value\">"
        << summary.sideEffectViolations << "</div></div>";
    html << "<div class=\"card\"><div class=\"label\">Shuffle Fallbacks</div><div class=\"value\">"
        << summary.shuffleFallbacksUsed << "</div></div>";
    html << "</div></div>";

    html << "<div class=\"section\"><h2>Fingerprint Comparison</h2>";
    html << "<table><thead><tr><th>Metric</th><th>Original</th><th>Transformed</th><th>Change</th></tr></thead><tbody>";
    appendComparisonRow(html,
                        "IR hash",
                        FormatHex64(summary.originalHash),
                        FormatHex64(summary.transformedHash),
                        summary.originalHash == summary.transformedHash ? "same" : "changed");
    appendComparisonRow(html,
                        "Instruction count",
                        std::to_string(summary.instructionCount),
                        std::to_string(summary.transformedInstructionCount),
                        signedDelta(summary.instructionCount, summary.transformedInstructionCount));
    appendComparisonRow(html,
                        "Basic blocks",
                        std::to_string(summary.blockCount),
                        std::to_string(summary.transformedBlockCount),
                        signedDelta(summary.blockCount, summary.transformedBlockCount));
    appendComparisonRow(html,
                        "Data symbols",
                        std::to_string(summary.originalSymbolCount),
                        std::to_string(summary.transformedSymbolCount),
                        signedDelta(summary.originalSymbolCount, summary.transformedSymbolCount));
    appendComparisonRow(html,
                        "Labels",
                        std::to_string(summary.originalLabelCount),
                        std::to_string(summary.transformedLabelCount),
                        signedDelta(summary.originalLabelCount, summary.transformedLabelCount));
    appendComparisonRow(html,
                        "Branch instructions",
                        std::to_string(summary.originalBranchCount),
                        std::to_string(summary.transformedBranchCount),
                        signedDelta(summary.originalBranchCount, summary.transformedBranchCount));
    appendComparisonRow(html,
                        "Side-effect operations",
                        std::to_string(summary.originalSideEffectCount),
                        std::to_string(summary.transformedSideEffectCount),
                        signedDelta(summary.originalSideEffectCount, summary.transformedSideEffectCount));
    html << "</tbody></table></div>";

    html << "<div class=\"section\"><h2>Diversification Metrics</h2>";
    html << "<table><tbody>";
    html << "<tr><th>Moved instruction slots</th><td>" << summary.fingerprintMetrics.movedInstructionSlots
        << " / " << summary.instructionCount << "</td></tr>";
    html << "<tr><th>Reorder ratio</th><td>" << EscapeHtml(FormatPercent(summary.fingerprintMetrics.reorderRatio))
        << "</td></tr>";
    html << "<tr><th>Moved block slots</th><td>" << summary.fingerprintMetrics.movedBlockSlots
        << " / " << summary.blockCount << "</td></tr>";
    html << "<tr><th>Block reorder ratio</th><td>"
        << EscapeHtml(FormatPercent(summary.fingerprintMetrics.blockReorderRatio)) << "</td></tr>";
    html << "<tr><th>Renamed symbols</th><td>" << summary.fingerprintMetrics.renamedSymbols << "</td></tr>";
    html << "<tr><th>Original hash</th><td>" << EscapeHtml(FormatHex64(summary.fingerprintMetrics.originalHash))
        << "</td></tr>";
    html << "<tr><th>Transformed hash</th><td>"
        << EscapeHtml(FormatHex64(summary.fingerprintMetrics.transformedHash)) << "</td></tr>";
    html << "<tr><th>Diversification score</th><td>" << std::fixed << std::setprecision(2)
        << summary.fingerprintMetrics.diversificationScore << "</td></tr>";
    html << std::defaultfloat;
    html << "</tbody></table></div>";

    html << "<div class=\"section\"><h2>Artifacts</h2>";
    html << "<table class=\"artifact-table\"><tbody>";
    appendArtifactRow(html, "Original IR", summary.originalDumpPath);
    appendArtifactRow(html, "After Constant Fold", summary.constantFoldDumpPath);
    appendArtifactRow(html, "After Constant Propagation", summary.constantPropDumpPath);
    appendArtifactRow(html, "After Copy Propagation", summary.copyPropDumpPath);
    appendArtifactRow(html, "After Dead Code Elimination", summary.dceDumpPath);
    appendArtifactRow(html, "After Shuffling", summary.shuffledDumpPath);
    appendArtifactRow(html, "Final IR", summary.finalDumpPath);
    appendArtifactRow(html, "CFG Graph", summary.cfgSvgPath);
    appendArtifactRow(html, "Dependency Graph", summary.dependencySvgPath);
    appendArtifactRow(html, "Transformation Trace", summary.tracePath);
    appendArtifactRow(html, "Fingerprint Report", summary.fingerprintReportPath);
    appendArtifactRow(html, "Verification Text", summary.verificationPath);
    appendArtifactRow(html, "Transformed IR Output", summary.outputPath);
    appendArtifactRow(html, "This HTML Report", summary.reportHtmlPath);
    html << "</tbody></table></div>";

    html << "<div class=\"section\"><h2>Compiler Graphs</h2>";
    if (!summary.cfgSvgPath.empty()) {
        html << "<h3>Control Flow Graph</h3><div class=\"diagram-wrap\"><img src=\""
             << EscapeHtml(fileNameOnly(summary.cfgSvgPath))
             << "\" alt=\"CFG diagram\" style=\"max-width:100%;height:auto;\"/></div>";
    }
    if (!summary.dependencySvgPath.empty()) {
        html << "<h3>Dependency Graph</h3><div class=\"diagram-wrap\"><img src=\""
             << EscapeHtml(fileNameOnly(summary.dependencySvgPath))
             << "\" alt=\"Dependency diagram\" style=\"max-width:100%;height:auto;\"/></div>";
    }
    html << "</div>";

    html << "<div class=\"section\"><h2>IR Snapshots</h2>";
    html << "<div class=\"stack\">";
    html << "<div><h3>Original IR</h3><pre>" << EscapeHtml(summary.originalText) << "</pre></div>";
    html << "<div><h3>After Optimization</h3><pre>" << EscapeHtml(summary.optimizedText) << "</pre></div>";
    html << "<div><h3>After Safe Shuffling / Reordering</h3><pre>" << EscapeHtml(summary.shuffledText) << "</pre></div>";
    html << "<div><h3>Final IR After Renaming</h3><pre>" << EscapeHtml(summary.transformedText) << "</pre></div>";
    html << "</div>";
    html << "</div>";

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

    summary.reportPath = options.reportPath;
    summary.reportHtmlPath =
        options.reportHtmlPath.empty() && options.enableLlmExplanation
            ? "build/llm_report_latest.html"
            : options.reportHtmlPath;

    std::string markdown = BuildMarkdownReport(summary);
    if (!options.reportPath.empty()) {
        if (!WriteTextFile(options.reportPath, markdown, error)) {
            return false;
        }
    } else {
        summary.reportPath.clear();
    }

    std::string htmlPath = summary.reportHtmlPath;
    if (!htmlPath.empty()) {
        std::string html = BuildHtmlReport(summary, markdown);
        if (!WriteTextFile(htmlPath, html, error)) {
            return false;
        }
        summary.reportHtmlPath = htmlPath;
    }

    return true;
}

bool WantsArtifactOutput(const Options& options) {
    return options.dumpPasses || options.emitCfgDot || options.emitDependencyDot ||
           options.emitTrace || options.emitFingerprintReport || !options.artifactDir.empty();
}

int ExecuteSingleRun(const Options& options, RunSummary& summary) {
    summary = RunSummary{};
    summary.inputPath = options.inputPath;
    summary.outputPath = options.outputPath;
    summary.verifyRequested = options.verify;
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

    if (WantsArtifactOutput(options)) {
        summary.artifactDir = ResolveArtifactDir(options);
        std::string dirError;
        if (!EnsureDirectory(summary.artifactDir, dirError)) {
            std::cerr << dirError << "\n";
            return 1;
        }
    }

    BranchValidationResult inputValidation = ValidateBranches(original);
    if (!inputValidation.success) {
        std::cerr << "Input control-flow validation failed: " << inputValidation.error << "\n";
        return 1;
    }

    OptimizationPipelineResult optimization = RunOptimizationPipeline(original);
    summary.optimizationTrace = optimization.trace;
    Program optimized = optimization.afterDeadCodeElimination;
    summary.optimizedText = ProgramToText(optimized);

    std::string artifactError;
    if (options.dumpPasses) {
        if (!MaybeWriteProgramArtifact(summary.artifactDir,
                                       "01_original.ir",
                                       original,
                                       summary.originalDumpPath,
                                       artifactError) ||
            !MaybeWriteProgramArtifact(summary.artifactDir,
                                       "02_constant_fold.ir",
                                       optimization.afterConstantFolding,
                                       summary.constantFoldDumpPath,
                                       artifactError) ||
            !MaybeWriteProgramArtifact(summary.artifactDir,
                                       "03_constant_propagation.ir",
                                       optimization.afterConstantPropagation,
                                       summary.constantPropDumpPath,
                                       artifactError) ||
            !MaybeWriteProgramArtifact(summary.artifactDir,
                                       "04_copy_propagation.ir",
                                       optimization.afterCopyPropagation,
                                       summary.copyPropDumpPath,
                                       artifactError) ||
            !MaybeWriteProgramArtifact(summary.artifactDir,
                                       "05_dead_code_elimination.ir",
                                       optimization.afterDeadCodeElimination,
                                       summary.dceDumpPath,
                                       artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
        summary.optimizedDumpPath = summary.dceDumpPath;
    }

    std::uint64_t masterSeed = options.hasSeed ? options.seed : GenerateRandomSeed();
    std::uint64_t shuffleSeed = masterSeed ^ 0x9E3779B97F4A7C15ULL;
    std::uint64_t renameSeed = masterSeed ^ 0xD1B54A32D192ED03ULL;

    Program reordered;
    BlockReorderStats blockStats;
    std::size_t sideEffectViolations = 0;
    std::size_t shuffleFallbackCount = 0;
    bool cfgModeEnabled = HasExplicitControlFlow(optimized);

    CFG optimizedCfg;
    std::string optimizedCfgError;
    if (!BuildCFG(optimized, optimizedCfg, optimizedCfgError)) {
        std::cerr << "Optimized CFG build failed: " << optimizedCfgError << "\n";
        return 1;
    }

    if (options.emitCfgDot) {
        std::filesystem::path cfgPath = std::filesystem::path(summary.artifactDir) / "cfg.dot";
        if (!WriteCfgDot(optimizedCfg, cfgPath.string(), artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
        summary.cfgDotPath = cfgPath.string();
    }
    if (WantsArtifactOutput(options)) {
        std::filesystem::path cfgSvgPath = std::filesystem::path(summary.artifactDir) / "cfg.svg";
        if (!WriteCfgSvg(optimizedCfg, cfgSvgPath.string(), artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
        summary.cfgSvgPath = cfgSvgPath.string();
    }

    if (options.emitDependencyDot) {
        DependencyGraph optimizedGraph = BuildDependencyGraph(optimized);
        std::filesystem::path depPath = std::filesystem::path(summary.artifactDir) / "dependency.dot";
        if (!WriteDependencyDot(optimized, optimizedGraph, depPath.string(), artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
        summary.dependencyDotPath = depPath.string();
        if (WantsArtifactOutput(options)) {
            std::filesystem::path depSvgPath = std::filesystem::path(summary.artifactDir) / "dependency.svg";
            if (!WriteDependencySvg(optimized, optimizedGraph, depSvgPath.string(), artifactError)) {
                std::cerr << artifactError << "\n";
                return 1;
            }
            summary.dependencySvgPath = depSvgPath.string();
        }
    } else if (WantsArtifactOutput(options)) {
        DependencyGraph optimizedGraph = BuildDependencyGraph(optimized);
        std::filesystem::path depSvgPath = std::filesystem::path(summary.artifactDir) / "dependency.svg";
        if (!WriteDependencySvg(optimized, optimizedGraph, depSvgPath.string(), artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
        summary.dependencySvgPath = depSvgPath.string();
    }

    if (cfgModeEnabled) {
        std::size_t movedInsideBlocks = 0;
        for (std::size_t i = 0; i < optimizedCfg.blocks.size(); ++i) {
            ShuffleBlockBody(optimizedCfg.blocks[i],
                             DeriveSeed(shuffleSeed, static_cast<std::uint64_t>(i + 1)),
                             movedInsideBlocks,
                             sideEffectViolations,
                             shuffleFallbackCount);
        }

        reordered =
            ReorderBasicBlocks(optimizedCfg, DeriveSeed(shuffleSeed, 0xC0FFEEULL), blockStats);
    } else {
        DependencyGraph graph = BuildDependencyGraph(optimized);
        ShuffleResult shuffled = RandomizedTopologicalShuffle(optimized, graph, shuffleSeed);

        if (!shuffled.fallbackUsed && CountMovedInstructions(shuffled.order) == 0 &&
            optimized.instructions.size() > 1) {
            std::uint64_t candidateSeed = shuffleSeed;
            for (int attempt = 0; attempt < 16; ++attempt) {
                candidateSeed += 0x9E3779B97F4A7C15ULL;
                ShuffleResult candidate =
                    RandomizedTopologicalShuffle(optimized, graph, candidateSeed);
                if (!candidate.fallbackUsed && CountMovedInstructions(candidate.order) > 0) {
                    shuffled = std::move(candidate);
                    break;
                }
            }
        }

        reordered = shuffled.program;
        sideEffectViolations =
            CountSideEffectOrderViolations(optimized.instructions, reordered.instructions);
        if (shuffled.fallbackUsed) {
            shuffleFallbackCount = 1;
        }
        blockStats.blockCount = 1;
        blockStats.movedBlockSlots = 0;
        blockStats.branchFixupsInserted = 0;
        blockStats.order = {0};
    }

    summary.shuffledText = ProgramToText(reordered);
    if (options.dumpPasses) {
        if (!MaybeWriteProgramArtifact(summary.artifactDir,
                                       "06_shuffled.ir",
                                       reordered,
                                       summary.shuffledDumpPath,
                                       artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
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
    summary.transformedText = transformedText;

    CFG finalTransformedCfg;
    std::string finalTransformedCfgError;
    if (!BuildCFG(transformed, finalTransformedCfg, finalTransformedCfgError)) {
        std::cerr << "Final transformed CFG build failed: " << finalTransformedCfgError << "\n";
        return 1;
    }

    std::string writeError;
    if (!WriteTextFile(options.outputPath, transformedText, writeError)) {
        std::cerr << writeError << "\n";
        return 1;
    }

    if (options.dumpPasses) {
        if (!MaybeWriteProgramArtifact(summary.artifactDir,
                                       "07_final.ir",
                                       transformed,
                                       summary.finalDumpPath,
                                       artifactError)) {
            std::cerr << artifactError << "\n";
            return 1;
        }
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
    CFG originalMetricCfg;
    std::string originalMetricCfgError;
    if (!BuildCFG(original, originalMetricCfg, originalMetricCfgError)) {
        std::cerr << "Original CFG metrics build failed: " << originalMetricCfgError << "\n";
        return 1;
    }

    summary.cfgModeEnabled = cfgModeEnabled;
    summary.instructionCount = original.instructions.size();
    summary.blockCount = originalMetricCfg.blocks.size();
    summary.transformedInstructionCount = transformed.instructions.size();
    summary.transformedBlockCount = finalTransformedCfg.blocks.size();
    summary.originalSymbolCount = CountUniqueDataSymbols(original);
    summary.transformedSymbolCount = CountUniqueDataSymbols(transformed);
    summary.originalLabelCount = CountInstructionsWithOp(original, OpCode::Label);
    summary.transformedLabelCount = CountInstructionsWithOp(transformed, OpCode::Label);
    summary.originalBranchCount = CountBranchInstructions(original);
    summary.transformedBranchCount = CountBranchInstructions(transformed);
    summary.originalSideEffectCount = CountSideEffectInstructions(original);
    summary.transformedSideEffectCount = CountSideEffectInstructions(transformed);
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
    summary.fingerprintMetrics = BuildFingerprintMetrics(moved,
                                                         reorderRatio,
                                                         blockStats.movedBlockSlots,
                                                         blockReorderRatio,
                                                         renamed.renameMap.size(),
                                                         originalHash,
                                                         transformedHash);
    summary.renameMap = renamed.renameMap;
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
    std::cout << "Diversification score     : " << std::fixed << std::setprecision(2)
              << summary.fingerprintMetrics.diversificationScore << "\n";
    std::cout << std::defaultfloat;
    std::cout << "Optimization summary      : folds=" << summary.optimizationTrace.constantFolds
              << ", const-prop=" << summary.optimizationTrace.constantPropagations
              << ", copy-prop=" << summary.optimizationTrace.copyPropagations
              << ", dce-removed=" << summary.optimizationTrace.deadInstructionsRemoved << "\n";

    std::cout << "LLM configured            : " << (summary.llmConfigured ? "YES" : "NO") << "\n";
    std::cout << "LLM explanation enabled   : "
              << (options.enableLlmExplanation ? "YES" : "NO") << "\n";
    std::cout << "LLM C++ assistance        : "
              << (options.preferLlmCpp ? "ENABLED" : "DISABLED") << "\n";

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
        std::cout << "Explanation source        : "
                  << (summary.explanationFromLlm ? "Gemini" : "deterministic fallback")
                  << "\n";
        std::cout << summary.explanation << "\n";
        if (!summary.explanationError.empty()) {
            std::cout << "Explanation note          : " << summary.explanationError << "\n";
        }
    }

    std::string auxiliaryArtifactError;
    if (!SaveAuxiliaryArtifactsIfRequested(options, summary, auxiliaryArtifactError)) {
        std::cerr << "Artifact generation failed: " << auxiliaryArtifactError << "\n";
        if (exitCode == 0) {
            exitCode = 4;
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

    if (!summary.tracePath.empty()) {
        std::cout << "Transformation trace      : " << summary.tracePath << "\n";
    }
    if (!summary.fingerprintReportPath.empty()) {
        std::cout << "Fingerprint report        : " << summary.fingerprintReportPath << "\n";
    }
    if (!summary.verificationPath.empty()) {
        std::cout << "Verification artifact     : " << summary.verificationPath << "\n";
    }
    if (!summary.workflowSvgPath.empty()) {
        std::cout << "Workflow diagram          : " << summary.workflowSvgPath << "\n";
    }

    if (options.showMap) {
        std::cout << "\n";
        PrintRenameMap(renamed.renameMap);
    }

    if (!summary.artifactDir.empty()) {
        std::cout << "Artifact directory        : " << summary.artifactDir << "\n";
    }

    return exitCode;
}

std::string BuildBatchOutputPath(const Options& options, int runIndex) {
    if (options.runs <= 1) {
        return options.outputPath;
    }

    if (!options.outputDir.empty()) {
        std::filesystem::path dir(options.outputDir);
        std::ostringstream fileName;
        fileName << "run_" << runIndex << ".ir";
        return (dir / fileName.str()).string();
    }

    std::filesystem::path outputPath(options.outputPath);
    std::filesystem::path parent = outputPath.parent_path();
    std::string stem = outputPath.stem().string();
    std::string ext = outputPath.extension().string();

    if (stem.empty()) {
        stem = "transformed";
    }
    if (ext.empty()) {
        ext = ".ir";
    }

    std::ostringstream fileName;
    fileName << stem << "_run_" << runIndex << ext;
    if (parent.empty()) {
        return fileName.str();
    }
    return (parent / fileName.str()).string();
}

bool PrepareBatchOutputArea(const Options& options, std::string& error) {
    if (options.runs <= 1 || options.outputDir.empty()) {
        return true;
    }

    try {
        std::filesystem::path dir(options.outputDir);
        if (options.cleanOutputDir && std::filesystem::exists(dir)) {
            std::filesystem::remove_all(dir);
        }
        std::filesystem::create_directories(dir);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Could not prepare output directory: ") + options.outputDir + " (" +
                ex.what() + ")";
        return false;
    } catch (...) {
        error = "Could not prepare output directory: " + options.outputDir;
        return false;
    }
}

int ExecuteBatchRuns(const Options& options) {
    std::string prepError;
    if (!PrepareBatchOutputArea(options, prepError)) {
        std::cerr << prepError << "\n";
        return 1;
    }

    int semanticPassCount = 0;
    double reorderSum = 0.0;
    double blockReorderSum = 0.0;
    std::size_t totalBranchFixups = 0;
    std::size_t totalSideEffectViolations = 0;
    int cfgEnabledRuns = 0;

    std::vector<std::uint64_t> transformedHashes;
    std::vector<std::string> outputPaths;
    transformedHashes.reserve(options.runs);
    outputPaths.reserve(options.runs);

    for (int runIndex = 1; runIndex <= options.runs; ++runIndex) {
        Options runOptions = options;
        runOptions.runs = 1;
        runOptions.interactive = false;
        runOptions.cleanOutputDir = false;
        runOptions.outputPath = BuildBatchOutputPath(options, runIndex);
        if (WantsArtifactOutput(options)) {
            std::ostringstream artifactName;
            artifactName << "run_" << runIndex;
            if (!options.artifactDir.empty()) {
                runOptions.artifactDir =
                    (std::filesystem::path(options.artifactDir) / artifactName.str()).string();
            } else {
                runOptions.artifactDir.clear();
            }
        }

        if (options.hasSeed) {
            runOptions.hasSeed = true;
            runOptions.seed = options.fixedSeedAcrossRuns
                                  ? options.seed
                                  : DeriveSeed(options.seed, static_cast<std::uint64_t>(runIndex));
        } else {
            runOptions.hasSeed = false;
            runOptions.seed = 0;
        }

        std::cout << "\nRun " << runIndex << "/" << options.runs << "\n";
        RunSummary summary;
        int exitCode = ExecuteSingleRun(runOptions, summary);
        if (exitCode != 0) {
            return exitCode;
        }

        reorderSum += summary.reorderRatio;
        blockReorderSum += summary.blockReorderRatio;
        totalBranchFixups += summary.branchFixupsInserted;
        totalSideEffectViolations += summary.sideEffectViolations;
        if (summary.cfgModeEnabled) {
            cfgEnabledRuns += 1;
        }
        if (options.verify && summary.verifyRequested && summary.verifyPass) {
            semanticPassCount += 1;
        }

        transformedHashes.push_back(summary.transformedHash);
        outputPaths.push_back(summary.outputPath);
    }

    std::cout << "\nBatch Summary\n";
    std::cout << "-------------\n";

    if (options.verify) {
        double passRate = options.runs == 0 ? 0.0
                                            : (100.0 * static_cast<double>(semanticPassCount) /
                                               static_cast<double>(options.runs));
        std::cout << "Semantic pass rate        : " << semanticPassCount << "/" << options.runs
                  << " (" << std::fixed << std::setprecision(2) << passRate << "%)\n";
        std::cout << std::defaultfloat;
    }

    double avgReorder = options.runs == 0 ? 0.0 : reorderSum / static_cast<double>(options.runs);
    double avgBlockReorder =
        options.runs == 0 ? 0.0 : blockReorderSum / static_cast<double>(options.runs);

    std::cout << "Average reorder ratio     : " << std::fixed << std::setprecision(2) << avgReorder
              << "%\n";
    std::cout << "Average block reorder ratio: " << std::fixed << std::setprecision(2)
              << avgBlockReorder << "%\n";
    std::cout << std::defaultfloat;
    std::cout << "Total branch fixups       : " << totalBranchFixups << "\n";
    std::cout << "CFG-enabled runs          : " << cfgEnabledRuns << "/" << options.runs << "\n";
    std::cout << "Total side-effect violations across runs: " << totalSideEffectViolations << "\n";

    if (transformedHashes.size() >= 2) {
        bool hashChanged = transformedHashes[0] != transformedHashes[1];
        std::cout << "First two run hashes differ: " << (hashChanged ? "YES" : "NO") << "\n";
    }

    std::cout << "Batch artifacts:\n";
    for (const std::string& path : outputPaths) {
        std::cout << "  " << path << "\n";
    }

    return 0;
}

std::string BuildVariantOutputPath(const Options& options, int variantIndex) {
    std::filesystem::path outputPath(options.outputPath);
    std::filesystem::path parent = outputPath.parent_path();
    std::string stem = outputPath.stem().string();
    std::string ext = outputPath.extension().string();
    if (stem.empty()) {
        stem = "transformed";
    }
    if (ext.empty()) {
        ext = ".ir";
    }
    std::ostringstream name;
    name << stem << "_variant_" << variantIndex << ext;
    if (parent.empty()) {
        return name.str();
    }
    return (parent / name.str()).string();
}

std::string BuildVariantArtifactDir(const Options& options, int variantIndex) {
    std::filesystem::path root = options.artifactDir.empty()
                                     ? std::filesystem::path("build") / "variants" / SafeFileStem(options.outputPath)
                                     : std::filesystem::path(options.artifactDir);
    std::ostringstream name;
    name << "variant_" << variantIndex;
    return (root / name.str()).string();
}

std::string BuildVariantSummaryMarkdown(const std::vector<RunSummary>& variants,
                                        int bestIndex,
                                        bool chooseBest) {
    std::ostringstream out;
    out << "# AFIS Variant Summary\n\n";
    out << "| Variant | Output | Verification | Reorder % | Block Reorder % | Renamed | Score |\n";
    out << "|---|---|---|---:|---:|---:|---:|\n";
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const RunSummary& summary = variants[i];
        std::string verification = "NOT RUN";
        if (summary.verifyRequested) {
            verification = summary.verifyError.empty() ? (summary.verifyPass ? "PASS" : "FAIL") : "ERROR";
        }
        out << "| " << (i + 1) << " | " << summary.outputPath << " | " << verification << " | "
            << std::fixed << std::setprecision(2) << summary.reorderRatio << " | "
            << std::fixed << std::setprecision(2) << summary.blockReorderRatio << " | "
            << summary.renamedSymbols << " | "
            << std::fixed << std::setprecision(2) << summary.fingerprintMetrics.diversificationScore
            << " |\n";
    }
    out << "\n";
    if (chooseBest && bestIndex >= 0) {
        const RunSummary& best = variants[bestIndex];
        out << "Selected best variant: " << (bestIndex + 1) << "\n";
        out << "- Output path: " << best.outputPath << "\n";
        out << "- Artifact directory: " << best.artifactDir << "\n";
        out << "- Verification: " << (best.verifyRequested ? (best.verifyPass ? "PASS" : "FAIL")
                                                          : "NOT RUN")
            << "\n";
        out << "- Diversification score: " << std::fixed << std::setprecision(2)
            << best.fingerprintMetrics.diversificationScore << "\n";
        out << "- Reason: highest valid diversification score among generated variants.\n";
    } else {
        out << "Best-variant selection not requested.\n";
    }
    return out.str();
}

int ExecuteVariantRuns(const Options& options) {
    std::vector<RunSummary> variantSummaries;
    variantSummaries.reserve(options.variants);

    int bestIndex = -1;
    double bestScore = -1.0;

    for (int variantIndex = 1; variantIndex <= options.variants; ++variantIndex) {
        Options variantOptions = options;
        variantOptions.variants = 1;
        variantOptions.runs = 1;
        variantOptions.interactive = false;
        variantOptions.cleanOutputDir = false;
        variantOptions.dumpPasses = true;
        variantOptions.outputPath = BuildVariantOutputPath(options, variantIndex);
        variantOptions.artifactDir = BuildVariantArtifactDir(options, variantIndex);

        if (options.hasSeed) {
            variantOptions.hasSeed = true;
            variantOptions.seed = DeriveSeed(options.seed, static_cast<std::uint64_t>(variantIndex));
        } else {
            variantOptions.hasSeed = false;
            variantOptions.seed = 0;
        }

        std::cout << "\nVariant " << variantIndex << "/" << options.variants << "\n";
        RunSummary summary;
        int exitCode = ExecuteSingleRun(variantOptions, summary);
        if (exitCode != 0) {
            return exitCode;
        }

        if ((!summary.verifyRequested || summary.verifyPass) &&
            summary.fingerprintMetrics.diversificationScore > bestScore) {
            bestScore = summary.fingerprintMetrics.diversificationScore;
            bestIndex = variantIndex - 1;
        }

        variantSummaries.push_back(std::move(summary));
    }

    std::string summaryError;
    std::string summaryRoot = options.artifactDir.empty()
                                  ? (std::filesystem::path("build") / "variants" / SafeFileStem(options.outputPath)).string()
                                  : options.artifactDir;
    if (!EnsureDirectory(summaryRoot, summaryError)) {
        std::cerr << summaryError << "\n";
        return 1;
    }

    std::filesystem::path summaryPath = std::filesystem::path(summaryRoot) / "variant_summary.md";
    bool chooseBest = options.selectBestVariant;
    if (!WriteTextFile(summaryPath.string(),
                       BuildVariantSummaryMarkdown(variantSummaries, bestIndex, chooseBest),
                       summaryError)) {
        std::cerr << summaryError << "\n";
        return 1;
    }

    std::cout << "\nVariant Summary\n";
    std::cout << "---------------\n";
    for (std::size_t i = 0; i < variantSummaries.size(); ++i) {
        std::cout << "Variant " << (i + 1) << " score        : "
                  << std::fixed << std::setprecision(2)
                  << variantSummaries[i].fingerprintMetrics.diversificationScore << "\n";
        std::cout << "Variant " << (i + 1) << " artifacts    : "
                  << variantSummaries[i].artifactDir << "\n";
    }
    std::cout << std::defaultfloat;
    if (chooseBest && bestIndex >= 0) {
        std::cout << "Selected best variant    : " << (bestIndex + 1) << "\n";
        std::cout << "Selected output path     : " << variantSummaries[bestIndex].outputPath << "\n";
    }
    std::cout << "Variant summary file     : " << summaryPath.string() << "\n";

    return 0;
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

    if (options.variants > 1) {
        return ExecuteVariantRuns(options);
    }

    if (options.runs > 1) {
        return ExecuteBatchRuns(options);
    }

    RunSummary summary;
    return ExecuteSingleRun(options, summary);
}
