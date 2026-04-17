# Anti-Fingerprint Instruction Shuffler

Anti-Fingerprint Instruction Shuffler is a C++ compiler-backend style project that transforms a simple three-address intermediate representation (IR) while preserving runtime behavior.

Core objective:
- Same input IR -> same program output
- Same input IR -> different instruction layout across runs (without fixed seed)
- Same input IR -> different register/variable names across runs (without fixed seed)

---

## 1) Full Project Overview

This project implements a correctness-first transformation pipeline for IR diversification. It supports both straight-line programs and label/branch CFG programs.

End-to-end pipeline:
1. Parse IR into normalized instructions.
2. Validate syntax and control-flow targets.
3. Build CFG and basic blocks (when control flow exists).
4. Build dependency graph per scheduling scope.
5. Randomly shuffle only dependency-safe instructions.
6. Reorder basic blocks with branch fixups (CFG mode).
7. Apply register renaming consistently.
8. Emit transformed IR.
9. Optionally execute original and transformed IR and compare outputs.

---

## 2) Key Features

### A) Dependency-Safe Instruction Shuffling
- Uses data hazards to prevent semantic breakage:
  - RAW (read-after-write)
  - WAR (write-after-read)
  - WAW (write-after-write)
- Uses randomized topological scheduling so output order can vary each run.

### B) CFG-Aware Basic Block Reordering
- Splits program into basic blocks using leaders.
- Keeps entry block stable, reorders remaining blocks randomly.
- Inserts branch fixups when fallthrough edges would be invalid after reorder.
- Preserves behavior by validating branch targets before and after transform.

### C) Side-Effect Protection
- Treats side-effecting ops (`load`, `store`, `print`, `call`) conservatively.
- Prevents unsafe movement across side-effect barriers.

### D) Register Renaming
- Renames identifiers consistently within a run.
- Different runs produce different names (unless a fixed seed is used).
- Function names in `call` are not renamed.

### E) Seed Control
- Unseeded mode: diverse outputs for same input.
- Seeded mode: reproducible output for debugging and evaluation.

### F) Built-in Correctness Validation
- Optional `--verify` executes both original and transformed IR.
- Reports `Semantic equivalence: PASS/FAIL`.

### G) Run Metrics
- Instruction and block movement ratios.
- Branch fixup count.
- Side-effect violation count.
- Rename count.
- Hashes for quick output differentiation.

---

## 3) Supported IR Syntax

### Arithmetic and Assignment
- `a = b`
- `a = b + c`
- `a = b - c`
- `a = b * c`
- `a = b / c`
- `a = - b`

### Side-Effecting Instructions
- `load dst, addr`
- `store addr, src`
- `print x`
- `call fn arg1 arg2 -> dst`
- `call fn arg1 arg2`

### Control-Flow Instructions
- `L0:`
- `goto L0`
- `if x goto L0`
- `if a < b goto L0`
- Supported condition operators: `==`, `!=`, `<`, `>`, `<=`, `>=`

### Utility
- `nop`
- Comments start with `#` or `//`

Notes:
- Tokens are whitespace-separated.
- Unary negation format is spaced: `a = - b`.

---

## 4) Internal Architecture (By Module)

### Core IR Model
- `src/ir.h`, `src/ir.cpp`
- Defines opcodes, instruction model, utility token helpers, and instruction-to-text conversion.

### Parser
- `src/parser.h`, `src/parser.cpp`
- Parses text IR into typed instruction objects.
- Reports structured parse errors with line numbers.

### Dependency Analysis
- `src/dependency.h`, `src/dependency.cpp`
- Builds dependency graph and side-effect barriers.
- Exposes read/write sets and side-effect classification.

### CFG + Basic Blocks
- `src/cfg.h`, `src/cfg.cpp`
- Constructs basic blocks.
- Validates branch labels and targets.
- Reorders blocks and inserts fixup jumps.

### Instruction Shuffler
- `src/shuffler.h`, `src/shuffler.cpp`
- Runs randomized topological scheduling with seed support.

### Register Renamer
- `src/renamer.h`, `src/renamer.cpp`
- Collects rename candidates and rewrites operands consistently.

### IR Interpreter / Verifier
- `src/interpreter.h`, `src/interpreter.cpp`
- Executes IR to validate semantic equivalence.
- Supports deterministic builtin calls (`max`, `min`, `abs`, `inc`).

### Pipeline Driver
- `src/main.cpp`
- CLI parsing, transformation orchestration, metrics, verification output.

---

## 5) Correctness Guarantees and Validation Strategy

### Instruction-Level Safety
- Reordering is constrained by hazard edges.
- Side-effect instructions are handled conservatively.

### CFG-Level Safety
- Input branch targets validated before transformation.
- Output branch targets validated after transformation.
- Fallthrough-sensitive paths repaired with explicit fixups.

### Runtime Equivalence Check
- `--verify` compares printed outputs of original and transformed programs.
- Non-equivalent outputs fail the run.

---

## 6) Metrics Explained

Per-run executable metrics:
- `Moved instruction slots`
- `Reorder ratio`
- `Moved block slots`
- `Block reorder ratio`
- `Branch fixups inserted`
- `Side-effect violations`
- `Renamed symbols`
- `Original IR hash` and `Transformed IR hash`

Demo script aggregate metrics:
- Line-level run-to-run diff count
- Hash change status between run 1 and run 2
- Semantic pass rate over all runs
- Average instruction reorder ratio
- Average block reorder ratio
- Total branch fixups
- Total side-effect violations
- Number of CFG-enabled runs

---

## 7) CLI Usage

Executable format:

`afis --input <file> [--output <file>] [--seed <u64>] [--verify] [--show-map]`

Arguments:
- `--input` (required): input IR file
- `--output`: output transformed IR file
- `--seed`: fixed seed for deterministic runs
- `--verify`: execute and compare original/transformed output
- `--show-map`: print rename map

Examples:

```powershell
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify
./build/afis.exe --input samples/example_cfg.ir --output build/cfg_out.ir --verify --seed 123456 --show-map
```

---

## 8) Build Script and Demo Script

### Build Script
- File: `scripts/build.ps1`
- Supports:
  - optional compiler override (`-Compiler`)
  - compiler bin path prefix (`-CompilerBinPath`) for toolchain selection
  - optional clean rebuild (`-Clean`)
  - optional output filename (`-OutputName`)

Examples:

```powershell
./scripts/build.ps1
./scripts/build.ps1 -CompilerBinPath "C:\msys64\ucrt64\bin"
./scripts/build.ps1 -Clean
./scripts/build.ps1 -Compiler clang++ -OutputName afis.exe
```

### Demo Script
- File: `scripts/demo.ps1`
- Supports:
  - input selection (`-Input` or `-InputFile`)
  - run count (`-Runs`)
  - compiler bin path pass-through (`-CompilerBinPath`)
  - fixed seed mode (`-FixedSeed`)
  - rename map display (`-ShowMap`)
  - cleanup run directory (`-CleanRunDir`)

Examples:

```powershell
./scripts/demo.ps1 -Input "samples/example_full.ir" -Runs 3 -CleanRunDir
./scripts/demo.ps1 -Input "samples/example_cfg.ir" -Runs 2 -CompilerBinPath "C:\msys64\ucrt64\bin"
./scripts/demo.ps1 -Input "samples/example_cfg.ir" -Runs 2 -FixedSeed
./scripts/demo.ps1 -Input "samples/example_side_effects.ir" -Runs 3
```

---

## 9) Sample Inputs

Available sample IR files:
- `samples/example_pure.ir`
- `samples/example_side_effects.ir`
- `samples/example_full.ir`
- `samples/example_cfg.ir`

Suggested use:
- `example_pure.ir`: high instruction-level shuffling freedom
- `example_side_effects.ir`: side-effect barrier behavior
- `example_full.ir`: combined arithmetic/call/memory pipeline
- `example_cfg.ir`: control-flow and block-reorder behavior

---

## 10) Teacher / Viva Demonstration Flow

1. Build once:
   - `./scripts/build.ps1 -Clean`
2. Run diverse mode:
   - `./scripts/demo.ps1 -Input "samples/example_cfg.ir" -Runs 3 -CleanRunDir`
3. Show fixed-seed reproducibility:
   - `./scripts/demo.ps1 -Input "samples/example_cfg.ir" -Runs 2 -FixedSeed`
4. Open `build/demo_runs/run_1.ir` and `build/demo_runs/run_2.ir` side-by-side.
5. Highlight:
   - same semantic output
   - changed internal structure in unseeded mode
   - identical structure in fixed-seed mode

---

## 11) Scope and Current Limits

- IR is intentionally simple and educational.
- Side-effect policy is conservative for safety over aggressiveness.
- No deep alias analysis yet.
- Unknown function calls in interpreter use deterministic fallback behavior.

---

## 12) Troubleshooting

### Compiler not found
- Install either `g++` (MinGW/MSYS2) or `clang++`.
- Verify with:
  - `g++ --version` or `clang++ --version`

### Build fails after major edits
- Run clean build:

```powershell
./scripts/build.ps1 -Clean
```

### Demo script input path error
- Use workspace-relative path under `samples/`.
- Example:
  - `-Input "samples/example_full.ir"`

### Verification fails
- Compare original vs transformed IR and inspect:
  - branch targets
  - side-effect instruction placement
  - arithmetic corner cases

---

## 13) Future Extensions

- Lightweight safe instruction substitution rules
- Better alias modeling for memory operations
- Stronger randomization strategies with policy tuning
- Batch benchmarking harness for large IR sets

---

## 14) Installation (End)

Prerequisites:
- C++ compiler: `g++` or `clang++`
- PowerShell (for scripts on Windows)

### Windows (PowerShell)

1. Open terminal in project root.
2. Build (MSYS2 UCRT64 preferred path):

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

3. Quick validation run:

```powershell
./build/afis.exe --input "samples/example_full.ir" --output "build/out.ir" --verify
```

4. Demo run:

```powershell
./scripts/demo.ps1 -Input "samples/example_cfg.ir" -Runs 3 -CleanRunDir -CompilerBinPath "C:\msys64\ucrt64\bin"
```

### Linux or macOS (manual)

1. Ensure `g++` or `clang++` is available.
2. Build:

```bash
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o build/afis
```

3. Run:

```bash
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```

### Optional `make` build

```bash
make
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```
