# Anti-Fingerprint Instruction Shuffler

Anti-Fingerprint Instruction Shuffler is a C++ compiler-backend style project that transforms a simple three-address intermediate representation (IR) while preserving runtime behavior.

Core objective:
- Same input program -> same runtime output
- Same input program -> different instruction layout across runs (without fixed seed)
- Same input program -> different register/variable names across runs (without fixed seed)

This project now supports two input paths:
- Direct IR input (`.ir`)
- Experimental C++ source input (`.cpp`) that is converted into IR first

---

## 1) End-to-End Workflow

Transformation pipeline:
1. Input loader reads IR or converts C++ to IR.
2. Parser validates IR syntax.
3. Branch and label validation runs.
4. CFG build happens when control flow is present.
5. Dependency graph (RAW/WAR/WAW + side-effect barriers) is built.
6. Randomized safe scheduling is applied.
7. CFG block reorder + branch fixups happen in CFG mode.
8. Optional LLM-assisted substitution candidates are validated and applied.
9. Register renaming is applied consistently.
10. Transformed IR is emitted.
11. Optional semantic verification executes original and transformed programs.
12. Optional LLM explanation/report is generated.

---

## 2) Key Features

### A) Dependency-Safe Instruction Shuffling
- Uses hazard constraints:
  - RAW
  - WAR
  - WAW
- Uses randomized topological scheduling.

### B) CFG-Aware Basic Block Reordering
- Builds basic blocks from leaders.
- Reorders non-entry blocks.
- Inserts branch fixups to preserve fallthrough semantics.
- Validates branch targets before and after transform.

### C) Side-Effect Protection
- Treats `load`, `store`, `print`, and `call` as conservative barriers.
- Prevents unsafe reordering across side-effect boundaries.

### D) Register Renaming
- Renames identifiers consistently in a run.
- Produces different names across different random runs.

### E) LLM-Assisted Instruction Substitution (Validated)
- Candidate pattern is conservative and deterministic:
  - `dst = a OP b` -> `dst = a` then `dst = dst OP b`
- LLM only selects candidate IDs.
- Deterministic validator applies only safe candidates.

### F) LLM Explanation and Report Generation
- Generates human-readable explanations of:
  - why transformations are safe
  - which constraints blocked movement
  - branch fixup effects
- Generates per-run markdown report.
- Optional HTML report output.

### G) Experimental C++ to IR Conversion
- Accepts `.cpp` input directly.
- Path 1: LLM-assisted conversion (if Gemini key configured).
- Path 2: deterministic fallback converter for straight-line code.
- Designed for demo convenience, not as a full production frontend.

### H) Interactive CLI Loop
- Menu-like loop for repeated runs.
- Prompts for input path, verification, seed, LLM options, and report paths.

---

## 3) Supported IR Syntax

### Arithmetic and Assignment
- `a = b`
- `a = b + c`
- `a = b - c`
- `a = b * c`
- `a = b / c`
- `a = b % c`
- `a = b & c`
- `a = b | c`
- `a = b ^ c`
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
- Supported conditions: `==`, `!=`, `<`, `>`, `<=`, `>=`

### Utility
- `nop`
- Comments start with `#` or `//`

---

## 4) C++ Input Mode (Experimental)

When `--input` points to `.cpp` (or `--input-cpp` is used), the tool converts C++ to IR first.

Current deterministic converter support:
- variable declarations and assignments
- arithmetic expressions (`+ - * / %` with parentheses)
- unary minus
- function calls in assignment/statement form
- increment/decrement
- `cout`/`std::cout` printing expressions

Current deterministic converter limits:
- control-flow statements (`if`, `while`, `for`, `switch`) are not handled by deterministic fallback
- for control-flow C++, configure Gemini API so LLM conversion can be attempted first

Interpreter supported builtins:
- `max`, `min`, `abs`, `inc`

---

## 5) LLM Integration (`.env` + Gemini)

### Environment Variables
Use `.env` in project root:
- `GEMINI_API_KEY`
- `GEMINI_MODEL` (default: `gemini-2.0-flash`)
- `GEMINI_CURL_PATH` (default: `curl`)
- `GEMINI_TIMEOUT_SECONDS` (default: `30`)

Template file:
- `.env.example`

### Safety Model
- LLM is advisory for candidate selection and explanations.
- Deterministic validation and runtime verification remain the source of truth.
- If LLM is unavailable, deterministic fallback behavior is used where possible.

---

## 6) CLI Usage

Executable format:

`afis --input <file> [--output <file>] [--seed <u64>] [--verify] [--show-map]`

### Core options
- `--input`, `-i` (required unless interactive)
- `--input-cpp` (force C++ mode)
- `--output`, `-o`
- `--seed`
- `--verify`
- `--show-map`
- `--interactive`

### LLM/report options
- `--env <file>`
- `--llm-substitute`
- `--llm-explain`
- `--report <file.md>`
- `--report-html <file.html>`
- `--no-llm-cpp`

### Example commands

```powershell
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify
./build/afis.exe --input samples/example_cfg.ir --output build/cfg_out.ir --verify --seed 123456 --show-map
./build/afis.exe --input samples/example_cpp_basic.cpp --output build/from_cpp.ir --verify
./build/afis.exe --input samples/example_full.ir --output build/substituted.ir --verify --llm-substitute
./build/afis.exe --input samples/example_cpp_calls.cpp --output build/cpp_report.ir --verify --llm-explain --report build/cpp_report.md --report-html build/cpp_report.html
./build/afis.exe --interactive
```

---

## 7) Metrics

Per-run summary includes:
- `Moved instruction slots`
- `Reorder ratio`
- `Moved block slots`
- `Block reorder ratio`
- `Branch fixups inserted`
- `Side-effect violations`
- `Shuffle fallbacks used`
- `Renamed symbols`
- `Original IR hash`
- `Transformed IR hash`
- LLM substitution counters (when enabled)

Verification section includes:
- Original output
- Transformed output
- `Semantic equivalence: PASS/FAIL`

---

## 8) Project Structure

### Core modules
- `src/ir.h`, `src/ir.cpp`
- `src/parser.h`, `src/parser.cpp`
- `src/dependency.h`, `src/dependency.cpp`
- `src/shuffler.h`, `src/shuffler.cpp`
- `src/cfg.h`, `src/cfg.cpp`
- `src/renamer.h`, `src/renamer.cpp`
- `src/interpreter.h`, `src/interpreter.cpp`

### New modules
- `src/llm.h`, `src/llm.cpp` (Gemini integration)
- `src/substitution.h`, `src/substitution.cpp` (validated substitution layer)
- `src/cpp_frontend.h`, `src/cpp_frontend.cpp` (experimental C++ conversion)
- `src/main.cpp` (interactive and non-interactive orchestration)

### Scripts
- `scripts/build.ps1`
- `scripts/demo.ps1`

### Samples
- `samples/example_pure.ir`
- `samples/example_side_effects.ir`
- `samples/example_full.ir`
- `samples/example_cfg.ir`
- `samples/example_branch_heavy.ir`
- `samples/example_cpp_basic.cpp`
- `samples/example_cpp_calls.cpp`

---

## 9) Demo Flow (Teacher/Viva Friendly)

1. Build:

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

2. Show IR diversification:

```powershell
./scripts/demo.ps1 -Input "samples/example_full.ir" -Runs 3 -CleanRunDir -CompilerBinPath "C:\msys64\ucrt64\bin"
```

3. Show C++ input conversion run:

```powershell
./build/afis.exe --input "samples/example_cpp_basic.cpp" --output "build/from_cpp.ir" --verify
```

4. Show LLM explanation/report (after `.env` setup):

```powershell
./build/afis.exe --input "samples/example_cfg.ir" --output "build/llm_cfg.ir" --verify --llm-explain --report "build/llm_cfg_report.md"
```

5. Optional interactive mode:

```powershell
./build/afis.exe --interactive
```

---

## 10) Troubleshooting

### Build fails
- Re-run clean build:

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

### LLM features say not configured
- Ensure `.env` exists and has valid `GEMINI_API_KEY`.
- Check `curl` availability or set `GEMINI_CURL_PATH`.

### C++ conversion fails on control flow
- Deterministic converter supports straight-line subset only.
- Configure Gemini key to enable LLM conversion attempt.

### Verification fails
- Compare original and transformed output blocks.
- Inspect branch targets and side-effect operations.

---

## 11) Future Extensions

- Clang-based robust frontend for production C++ to IR lowering
- Additional validated substitution patterns
- Stronger memory alias analysis
- Batch benchmark harness and result dashboards

---

## 12) Installation (End)

Prerequisites:
- C++ compiler: `g++` or `clang++`
- PowerShell (Windows scripts)
- Optional: `curl` and Gemini API key for LLM features

### Windows (PowerShell)

1. Build:

```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

2. Basic IR run:

```powershell
./build/afis.exe --input "samples/example_full.ir" --output "build/out.ir" --verify
```

3. C++ run:

```powershell
./build/afis.exe --input "samples/example_cpp_basic.cpp" --output "build/out_cpp.ir" --verify
```

4. LLM setup:
- Copy `.env.example` to `.env`
- Fill `GEMINI_API_KEY`

5. LLM report run:

```powershell
./build/afis.exe --input "samples/example_cfg.ir" --output "build/out_cfg.ir" --verify --llm-substitute --llm-explain --report "build/report.md" --report-html "build/report.html"
```

### Linux/macOS (manual)

```bash
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/*.cpp -o build/afis
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```

### Optional `make`

```bash
make
./build/afis --input samples/example_full.ir --output build/out.ir --verify
```
