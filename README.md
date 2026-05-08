# Anti-Fingerprint Instruction Shuffler

Anti-Fingerprint Instruction Shuffler, or AFIS, is a C++17 compiler-backend style project that transforms a simple three-address intermediate representation while preserving program behavior. It is built to demonstrate code diversification: the same program should still compute the same result, but its instruction layout, symbol names, block order, and structural fingerprint can change between runs.

The project supports:
- Direct `.ir` input
- Experimental `.cpp` input converted into IR
- Optimization passes before diversification
- Dependency-safe instruction shuffling
- CFG-aware block reordering
- Register renaming
- Runtime semantic verification
- Fingerprint metrics and variant ranking
- Markdown and HTML reports
- Optional Gemini summaries for better human-readable explanations

## Why This Project Exists

Many reverse-engineering, clone-detection, or fingerprinting systems rely on stable code structure. If two builds keep the same instruction order, same temporary names, same block order, and same structural hash, they are easy to match.

AFIS tries to reduce that structural similarity while keeping the observable behavior unchanged. It does this conservatively:

1. Parse or convert the input into IR.
2. Simplify the IR with safe optimization passes.
3. Analyze dependencies and side effects.
4. Shuffle only instructions that are legal to move.
5. Reorder CFG blocks while fixing branches.
6. Rename registers and temporaries consistently.
7. Verify that original and transformed outputs still match.

## Current Status

The current project has been rebuilt and tested with the comprehensive test harness.

Latest local verification result:

```text
PASS=22 FAIL=0 WARN=0
```

The generated HTML report no longer embeds the workflow diagram in the main page. The workflow diagram is still produced as a separate artifact when artifact generation is enabled. The HTML report now includes a fingerprint comparison table so the original and transformed program shapes can be compared directly.

## Requirements

### Windows

- PowerShell
- C++17 compiler
- Recommended compiler path: `C:\msys64\ucrt64\bin`
- Optional: `curl` for Gemini API calls
- Optional: Gemini API key for LLM summaries and C++ conversion assistance

### Linux/macOS

- C++17 compiler such as `g++` or `clang++`
- `make` is optional
- Optional: `curl` and Gemini API key

## Build

### Windows PowerShell

```powershell
./scripts/build.ps1
```

Clean rebuild:

```powershell
./scripts/build.ps1 -Clean
```

With explicit compiler directory:

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

Output:

```text
build/afis.exe
```

### Linux/macOS

```bash
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o build/afis
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```

Or:

```bash
make
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```

## Quick Start

Run one IR sample and verify semantic equivalence:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify
```

Run a CFG-heavy sample with a fixed seed:

```powershell
./build/afis.exe --input samples/example_cfg.ir --output build/cfg_out.ir --verify --seed 123456 --show-map
```

Run a C++ sample using deterministic fallback conversion:

```powershell
./build/afis.exe --input-cpp samples/example_cpp_basic.cpp --output build/from_cpp.ir --verify --no-llm-cpp
```

Generate a full artifact set:

```powershell
./build/afis.exe --input samples/example_cfg.ir --output build/demo/transformed.ir --verify --dump-passes --emit-cfg-dot --emit-dep-dot --emit-trace --fingerprint-report --artifact-dir build/demo --report-html build/demo/summary.html
```

Generate multiple variants and select the strongest diversification result:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/variant.ir --verify --variants 3 --select-best-variant --artifact-dir build/variants
```

Use the guided CLI:

```powershell
./scripts/run.ps1
```

## Full Run Workflow

This is the complete project workflow from start to finish.

### Step 1: Input Loading

AFIS starts from either an IR file or a C++ file.

IR input:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/out.ir
```

C++ input:

```powershell
./build/afis.exe --input-cpp samples/example_cpp_basic.cpp --output build/out.ir
```

For C++ files, AFIS converts C++ into the internal IR first. The deterministic C++ converter supports a small straight-line subset. If Gemini is configured, the LLM path can be tried for more flexible C++ to IR conversion.

### Step 2: IR Parsing

The parser reads three-address IR and validates the syntax. Supported IR includes assignments, arithmetic operations, labels, branches, loads, stores, prints, and function calls.

Example IR:

```text
a = 10
b = 20
c = a + b
print c
```

If parsing fails, the run stops before transformation.

### Step 3: Optimization

Before shuffling, AFIS simplifies the IR using optimization passes:

- Constant folding
- Constant propagation
- Copy propagation
- Dead code elimination

These passes reduce unnecessary instructions before the diversification stage. When `--dump-passes` is enabled, AFIS writes the intermediate files:

```text
01_original.ir
02_constant_fold.ir
03_constant_propagation.ir
04_copy_propagation.ir
05_dead_code_elimination.ir
```

### Step 4: Branch and CFG Analysis

AFIS validates labels and branch targets. If the program contains control flow, it builds a control-flow graph.

CFG mode lets AFIS reason about:

- Basic blocks
- Entry block
- Branch targets
- Fallthrough behavior
- Block reorder safety

When graph artifacts are enabled, AFIS can emit:

```text
cfg.dot
cfg.svg
```

### Step 5: Dependency Analysis

AFIS builds dependency constraints before moving instructions. It tracks:

- RAW: read after write
- WAR: write after read
- WAW: write after write
- Side-effect barriers

Side-effecting instructions are handled conservatively. Examples:

- `load`
- `store`
- `print`
- `call`

This prevents unsafe transformations such as moving a print before the value it prints is computed, or moving memory operations across each other in a way the project cannot prove safe.

When dependency artifacts are enabled, AFIS can emit:

```text
dependency.dot
dependency.svg
```

### Step 6: Safe Instruction Shuffling

AFIS performs randomized topological scheduling over the dependency graph. That means it can choose different legal orders across runs, but only among orders that satisfy the dependency constraints.

With no fixed seed, repeated runs can produce different transformed IR. With `--seed`, the transformation becomes reproducible.

Example:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/run_a.ir --verify
./build/afis.exe --input samples/example_full.ir --output build/run_b.ir --verify
```

Fixed-seed example:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/fixed_a.ir --verify --seed 123456
./build/afis.exe --input samples/example_full.ir --output build/fixed_b.ir --verify --seed 123456
```

### Step 7: CFG Block Reordering

In CFG mode, AFIS can reorder basic blocks while preserving valid control flow. When block reordering changes fallthrough behavior, AFIS inserts branch fixups so execution still reaches the correct block.

The report shows:

- Moved block slots
- Block reorder ratio
- Branch fixups inserted

### Step 8: Register Renaming

After shuffling, AFIS renames registers and temporaries consistently. This changes the textual fingerprint without changing data flow.

Example idea:

```text
t1 = a + b
print t1
```

can become:

```text
r7 = x3 + x4
print r7
```

The exact names depend on the run seed.

### Step 9: Fingerprint Metrics

AFIS computes metrics that describe how much the transformed program differs from the original structure.

The HTML report includes:

- Original instruction count
- Transformed instruction count
- Original symbol count
- Transformed symbol count
- Original label count
- Transformed label count
- Original branch count
- Transformed branch count
- Original side-effect count
- Transformed side-effect count
- Original hash
- Transformed hash
- Diversification score

The fingerprint comparison table is useful because it shows the original and transformed program side by side, instead of only showing a single final score.

### Step 10: Semantic Verification

When `--verify` is enabled, AFIS executes both the original and transformed IR through the interpreter and compares their printed outputs.

Verification passes only if the observable output matches exactly.

Example output:

```text
Semantic equivalence: PASS
```

This is the correctness check. Fingerprint metrics are not a correctness proof; they only measure structural change.

### Step 11: Reports and Artifacts

AFIS can generate a complete artifact folder for inspection:

```text
01_original.ir
02_constant_fold.ir
03_constant_propagation.ir
04_copy_propagation.ir
05_dead_code_elimination.ir
06_shuffled.ir
07_final.ir
cfg.dot
cfg.svg
dependency.dot
dependency.svg
trace.md
fingerprint.md
verification.txt
workflow.dot
workflow.svg
summary.html
```

The HTML report is intended to be the first file to open. The separate diagrams and markdown files are useful for deeper inspection or presentation.

## Diversification Score

The Diversification Score is a numeric summary of how much the transformed IR differs from the original IR.

Current formula:

```text
score =
  movedInstructionSlots
  + (2.0 * movedBlockSlots)
  + (0.25 * reorderRatio)
  + (0.50 * blockReorderRatio)
  + renamedSymbols
  + hashComponent
```

`hashComponent` is derived from the difference between the original and transformed IR hashes:

```text
hashComponent = ((originalHash XOR transformedHash) & 0xFFFF) / 65535.0
```

What the score is useful for:

- Comparing multiple transformed variants
- Ranking generated outputs
- Demonstrating that the transformed IR is structurally different
- Selecting the best variant with `--select-best-variant`

What the score does not prove:

- It does not prove semantic correctness
- It does not prove security
- It does not prove resistance against every fingerprinting method

Correctness comes from `--verify`. Diversification Score only describes structural change.

## LLM and Gemini Integration

Gemini is optional. The compiler pipeline does not depend on Gemini for correctness.

Gemini can be used for:

- Better human-readable summaries in HTML and markdown reports
- Explaining why transformations are safe
- Summarizing dependency and fingerprint metrics
- Assisting C++ to IR conversion for inputs beyond the deterministic fallback subset

Gemini is not used as the source of truth for semantic correctness. The project still relies on deterministic parsing, transformation logic, and runtime verification.

If Gemini is unavailable:

- IR input still works
- Deterministic C++ fallback still works for supported C++ samples
- Reports still generate
- HTML uses a deterministic summary instead of a Gemini summary

### Configure Gemini

Copy `.env.example` to `.env` and fill in your key:

```text
GEMINI_API_KEY=your_key_here
GEMINI_MODEL=gemini-2.5-flash
GEMINI_CURL_PATH=curl
GEMINI_TIMEOUT_SECONDS=30
```

Run with Gemini explanations:

```powershell
./build/afis.exe --input samples/example_cfg.ir --output build/llm/transformed.ir --verify --llm-explain --report build/llm/report.md --report-html build/llm/summary.html --artifact-dir build/llm --dump-passes
```

In `summary.html`, the explanation section is labeled either:

- `Gemini Summary`
- `Deterministic Summary`

## Supported IR Syntax

### Assignment and Arithmetic

```text
a = b
a = b + c
a = b - c
a = b * c
a = b / c
a = b % c
a = b & c
a = b | c
a = b ^ c
a = - b
```

### Side Effects

```text
load dst, addr
store addr, src
print x
call fn arg1 arg2 -> dst
call fn arg1 arg2
```

### Control Flow

```text
L0:
goto L0
if x goto L0
if a < b goto L0
if a >= b goto L0
```

Supported comparisons:

```text
== != < > <= >=
```

### Utility

```text
nop
```

Comments start with `#` or `//`.

## C++ Input Mode

C++ support is experimental. The deterministic converter is designed for demo-friendly straight-line code, not full C++.

Currently supported by deterministic conversion:

- Variable declarations
- Assignments
- Arithmetic expressions
- Parentheses
- Unary minus
- Increment and decrement
- Function calls
- `cout` and `std::cout` prints

Current deterministic limitations:

- `if`
- `while`
- `for`
- `switch`
- Complex classes/templates
- General pointer-heavy C++

For unsupported C++ syntax, configure Gemini and allow LLM-assisted conversion. Always use `--verify` after C++ conversion.

Interpreter builtins include:

- `max`
- `min`
- `abs`
- `inc`

## Command-Line Options

General shape:

```text
afis --input <file> [--output <file>] [options]
```

Core options:

| Option | Meaning |
|---|---|
| `--input`, `-i <file>` | Input file, usually `.ir` or auto-detected `.cpp` |
| `--input-cpp <file>` | Force C++ input mode |
| `--output`, `-o <file>` | Output transformed IR path |
| `--output-dir <dir>` | Batch output directory when `--runs > 1` |
| `--seed <u64>` | Fixed seed for reproducible output |
| `--runs <n>` | Run the transformation multiple times |
| `--fixed-seed-runs` | Reuse the same seed for each batch run |
| `--clean-output-dir` | Clear `--output-dir` before a batch run |
| `--verify` | Execute original and transformed programs and compare output |
| `--show-map` | Print the rename map |
| `--interactive` | Start the interactive CLI |
| `--artifact-dir <dir>` | Directory for generated artifacts |
| `--dump-passes` | Save IR after each major stage |
| `--emit-cfg-dot` | Export CFG graph |
| `--emit-dep-dot` | Export dependency graph |
| `--emit-trace` | Save transformation trace markdown |
| `--fingerprint-report` | Save fingerprint metrics markdown |
| `--variants <n>` | Generate multiple transformed variants |
| `--select-best-variant` | Pick the valid variant with the highest diversification score |

LLM and report options:

| Option | Meaning |
|---|---|
| `--env <file>` | `.env` file path, default `.env` |
| `--llm-explain` | Enable LLM explanation/report mode |
| `--report <file.md>` | Write markdown report |
| `--report-html <file.html>` | Write HTML report |
| `--no-llm-cpp` | Disable LLM assistance for C++ conversion |

## Scripts

### `scripts/build.ps1`

Builds the project on Windows.

```powershell
./scripts/build.ps1 -Clean
```

### `scripts/run.ps1`

Interactive menu for common workflows:

1. Run one file in AUTO best mode
2. Run all sample files in AUTO best mode
3. Validation proof run
4. List sample files
5. Clean and rebuild
6. Exit

AUTO best mode enables verification by default. If a usable Gemini key is configured, it also enables LLM summaries. If Gemini or LLM C++ conversion fails, C++ runs can fall back to deterministic conversion when possible.

```powershell
./scripts/run.ps1
```

### `scripts/demo.ps1`

Runs multiple demo variants and reports whether outputs changed structurally while verification still passes.

```powershell
./scripts/demo.ps1 -Input "samples/example_full.ir" -Runs 3 -CleanRunDir
```

### `scripts/comprehensive_test.ps1`

Runs the full test and artifact validation suite.

```powershell
./scripts/comprehensive_test.ps1
```

If the project is already built:

```powershell
./scripts/comprehensive_test.ps1 -SkipCleanBuild
```

Expected current result:

```text
PASS=22 FAIL=0 WARN=0
```

## Project Structure

```text
src/
  cfg.*             CFG construction and block reordering helpers
  cpp_frontend.*    Experimental C++ to IR conversion
  dependency.*      Dependency graph and hazard tracking
  diagram.*         DOT/SVG artifact generation
  fingerprint.*     Fingerprint metrics and diversification score
  interpreter.*     IR interpreter used by --verify
  ir.*              IR model and formatting
  llm.*             Gemini integration
  main.cpp          CLI orchestration and report generation
  optimizer.*       Constant folding/propagation, copy propagation, DCE
  parser.*          IR parser
  renamer.*         Register/symbol renaming
  shuffler.*        Dependency-safe instruction scheduling
  substitution.*    Substitution helpers

samples/
  example_*.ir      IR examples
  example_*.cpp     C++ examples

scripts/
  build.ps1
  run.ps1
  demo.ps1
  comprehensive_test.ps1

build/
  Generated binaries, reports, and artifacts
```

## Sample Files

Important sample inputs:

| Sample | Purpose |
|---|---|
| `samples/example_full.ir` | General IR transformation demo |
| `samples/example_pure.ir` | Pure arithmetic and assignment movement |
| `samples/example_side_effects.ir` | Side-effect barrier behavior |
| `samples/example_cfg.ir` | Basic CFG and branch handling |
| `samples/example_cfg_mixed.ir` | Mixed CFG and data dependencies |
| `samples/example_branch_heavy.ir` | More branch-heavy control flow |
| `samples/example_calls_memory.ir` | Calls, loads, stores, and memory-like effects |
| `samples/example_dependency_blocked.ir` | Cases where dependencies block movement |
| `samples/example_optimizable.ir` | Optimization pass demonstration |
| `samples/example_loop.ir` | Loop-style CFG sample |
| `samples/example_cpp_basic.cpp` | Basic C++ conversion |
| `samples/example_cpp_calls.cpp` | C++ calls and output |
| `samples/example_cpp_chain.cpp` | Chained C++ expressions |
| `samples/example_cpp_optimizable.cpp` | C++ input with optimization opportunities |

## HTML Report

`summary.html` is the main human-readable report.

It includes:

- Run overview
- Verification result
- Gemini or deterministic summary
- Fingerprint comparison
- Diversification metrics
- Optimization trace
- Hazard/dependency summary
- Artifact links
- Original, optimized, shuffled, and final IR previews

The workflow diagram is generated separately as `workflow.svg` but is intentionally not embedded directly inside the HTML page. Open it separately when presenting the pipeline visually.

## Recommended Demo Flow

1. Build the project:

```powershell
./scripts/build.ps1 -Clean
```

2. Run one full artifact demo:

```powershell
./build/afis.exe --input samples/example_cfg.ir --output build/demo/transformed.ir --verify --dump-passes --emit-cfg-dot --emit-dep-dot --emit-trace --fingerprint-report --artifact-dir build/demo --report-html build/demo/summary.html
```

3. Open:

```text
build/demo/summary.html
```

4. Explain the pass files in order:

```text
01_original.ir
02_constant_fold.ir
03_constant_propagation.ir
04_copy_propagation.ir
05_dead_code_elimination.ir
06_shuffled.ir
07_final.ir
```

5. Show correctness:

```text
verification.txt
```

6. Show safety reasoning:

```text
cfg.svg
dependency.svg
trace.md
fingerprint.md
```

7. Explain that the transformation is accepted only when semantic verification passes.

## Testing

Run the full suite:

```powershell
./scripts/comprehensive_test.ps1
```

What the suite checks:

- IR verification cases
- CFG verification
- Side-effect safety verification
- Deterministic C++ fallback conversion
- Multi-variant generation
- Batch mode
- Fixed-seed determinism
- Unseeded variability
- Interactive-menu workflows
- Generated artifact presence
- Variant summary generation

## Troubleshooting

### Build fails

Use a clean build and provide the compiler path:

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

### Gemini summary is missing

Check:

- `.env` exists
- `GEMINI_API_KEY` is valid
- `curl` is available
- `GEMINI_MODEL` is valid
- Network access is available

If Gemini cannot be reached, the report should still contain a deterministic summary.

### C++ conversion fails

Try deterministic fallback:

```powershell
./build/afis.exe --input-cpp samples/example_cpp_basic.cpp --output build/out.ir --verify --no-llm-cpp
```

For more complex C++, configure Gemini and retry without `--no-llm-cpp`.

### Verification fails

Inspect:

- Console output
- `verification.txt`
- `trace.md`
- `dependency.svg`
- `cfg.svg`
- `01_original.ir` through `07_final.ir`

A valid transformation should preserve printed output exactly.

### Two unseeded runs look the same

Some programs have limited legal movement because dependencies or side effects block reordering. Try:

- A larger sample
- More variants
- A sample with more independent instructions

Example:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/variant.ir --verify --variants 5 --select-best-variant --artifact-dir build/variants
```

## Notes on Correctness

AFIS is intentionally conservative. If the dependency graph cannot prove movement is safe, the instruction remains constrained. This may reduce diversification on tightly dependent programs, but it is better than producing an unsafe reorder.

The recommended correctness signal is:

```text
Semantic equivalence: PASS
```

The recommended diversification signal is:

```text
Diversification score: <number>
```

Use both together: verification for correctness, score for structural change.

## Future Work

Possible extensions:

- Clang-based C++ frontend
- Stronger alias analysis
- More advanced block scheduling
- Additional verified substitution rules
- Larger benchmark harness
- Report dashboards across many samples
- More configurable diversification scoring
