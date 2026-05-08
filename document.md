# PROJECT REPORT
# Anti-Fingerprint Instruction Shuffler (AFIS)
## Course Project Report

### Course Context
This report documents a course project in compiler design and program transformation.
The project objective is to implement a transformation pipeline that changes IR structure while preserving runtime behavior.

### One-Line Project Idea
Given one input program, AFIS should produce functionally equivalent transformed IR, with varied instruction order and symbol names across runs.

## Table of Contents
1. Executive Summary
2. Problem Statement and Scope
3. System Architecture
4. Phase 1: Input Frontend (IR and C++)
5. Phase 2: Optimization Passes
6. Phase 3: Dependency and Hazard Analysis
7. Phase 4: Dependency-Safe Randomized Scheduling
8. Phase 5: CFG Reordering and Branch Fixups
9. Phase 6: Register Renaming
10. Phase 7: Verification, Artifacts, and Variant Analysis
11. Build, CLI, and Workflow Automation
12. Experimental Setup
13. Results and Analysis
14. Design Decisions Summary
15. Lessons Learned
16. Future Work
17. Conclusion
18. Appendix
19. References

## 1. Executive Summary
AFIS is a compiler-backend style transformation system for a compact three-address IR.
It accepts either direct IR input or experimental C++ input that is converted to IR first.
The core contribution is not random shuffling, but constrained shuffling under explicit safety rules.

The system applies:
- optimization passes (constant folding, constant propagation, copy propagation, dead code elimination),
- data hazard constraints (RAW, WAR, WAW),
- side-effect barriers (load/store/print/call and control-flow boundaries),
- optional CFG-aware basic block reordering with branch fixups,
- deterministic register renaming,
- pass-by-pass artifact dumping,
- CFG, dependency, and workflow diagram generation,
- fingerprint scoring and multi-variant comparison,
- output-equivalence verification by executing original and transformed programs.

Across 33 evaluated runs (11 sample categories x 3 runs each), AFIS achieved:
- 33 semantic passes,
- 0 semantic failures,
- 0 side-effect order violations,
- non-trivial structure changes in both straight-line and CFG programs.

### Deliverables and Status
| Deliverable | Status |
|---|---|
| IR parser and validator | Complete |
| Optimization passes and pass-by-pass dumps | Complete |
| Dependency graph and hazard model | Complete |
| Randomized topological scheduler | Complete |
| CFG block reorder with fixups | Complete |
| Register renaming | Complete |
| Runtime verification | Complete |
| CFG / dependency / workflow diagram output | Complete |
| Fingerprint metrics and variant selection | Complete |
| Markdown and HTML run reports | Complete |
| Scripted run-all and validation workflows | Complete |
| Experimental C++ frontend | Complete (subset + fallback model) |

## 2. Problem Statement and Scope
### Problem Statement
Build a transformation engine that modifies IR layout for anti-fingerprint diversity while preserving runtime behavior.
The system must support both straight-line and control-flow programs and should generate evidence for correctness.

### Project Objectives
1. Parse and validate IR robustly.
2. Detect dependency hazards and barrier constraints.
3. Reorder safely using randomized topological scheduling.
4. Reorder CFG blocks when control flow exists.
5. Preserve side-effect ordering.
6. Rename symbols consistently.
7. Verify output equivalence at runtime.
8. Produce detailed per-run reports and artifacts.

### In Scope
- Integer-oriented IR semantics.
- Dependency-safe movement and CFG-safe reordering.
- Runtime output-based semantic validation.
- Reported metrics and artifact generation.

### Out of Scope
- Full production C++ frontend.
- Complete alias/memory proof system.
- Formal theorem-proved equivalence.

## 3. System Architecture
### End-to-End Pipeline
```text
Input (.ir or .cpp)
  -> Parse or Convert to IR
  -> Branch and Label Validation
  -> Optimization Passes
  -> Pass-by-Pass IR Artifact Dump
  -> Mode Selection (Straight-line or CFG)
  -> CFG / Dependency Diagram Export
  -> Dependency Graph Build
  -> Randomized Safe Scheduling
  -> CFG Reorder + Branch Fixups (if CFG mode)
  -> Register Renaming
  -> Fingerprint Metrics
  -> Emit Transformed IR
  -> Optional Runtime Verification
  -> Variant Comparison / Best Selection
  -> Explanation + Report Generation
```

### Core Components
| Component | Main Role |
|---|---|
| IR model (`src/ir.*`) | Canonical instruction and program representation |
| Parser (`src/parser.*`) | Parse text IR into typed instruction stream |
| Dependency engine (`src/dependency.*`) | Read/write sets, hazard edges, barrier logic |
| Optimizer (`src/optimizer.*`) | Constant fold/propagate, copy propagate, DCE |
| Scheduler (`src/shuffler.*`) | Randomized topological reorder under constraints |
| CFG engine (`src/cfg.*`) | Block extraction, reorder, fallthrough fixups |
| Diagram exporter (`src/diagram.*`) | CFG, dependency, and workflow dot diagrams |
| Fingerprint engine (`src/fingerprint.*`) | Diversification metrics and variant scoring |
| Renamer (`src/renamer.*`) | Deterministic symbol map rewrite |
| Interpreter (`src/interpreter.*`) | Runtime execution for semantic checks |
| C++ frontend (`src/cpp_frontend.*`) | Experimental C++ to IR conversion |
| LLM adapter (`src/llm.*`) | Explanation support and optional C++ frontend assistance |
| CLI driver (`src/main.cpp`) | Orchestration, metrics, report output |

## 4. Phase 1: Input Frontend (IR and C++)
### What We Built
- Direct IR parsing for assignment, arithmetic, memory ops, call, label, branch, and nop.
- Branch grammar for both unary and binary conditional forms.
- Input branch validation (duplicate labels and missing targets).
- C++ conversion path with two strategies:
  - LLM-assisted conversion when configured.
  - Deterministic fallback conversion for straight-line subset.

### Supported IR Syntax (Core)
```text
a = b
a = b + c
a = - b
load dst, addr
store addr, src
print x
call fn arg1 arg2 -> dst
L0:
goto L0
if x goto L0
if a < b goto L0
```

### C++ Deterministic Subset
Supported:
- declarations and assignments,
- arithmetic expressions with precedence,
- function calls,
- increment/decrement,
- compound assignments,
- cout-style print expressions.

Rejected by deterministic fallback:
- if, while, for, switch.

### Alternatives Considered
| Option | Notes |
|---|---|
| Deterministic converter (chosen) | Stable, fast, dependency-free path for demos |
| LLM-only conversion | Flexible but can produce malformed IR |
| Full compiler frontend (Clang-level) | Most robust, but too large for course scope |

### Why This Choice
The two-path approach gives reliability first (deterministic fallback) with optional flexibility (LLM attempt), which is ideal for a course project where predictable demonstrations matter.

## 5. Phase 2: Dependency and Hazard Analysis
### What We Built
The dependency engine computes read and write sets for each instruction and creates directed ordering constraints.

Hazards modeled:
- RAW (Read After Write),
- WAR (Write After Read),
- WAW (Write After Write).

Barrier policy:
- `load`, `store`, `print`, `call`, `label`, `goto`, and `if-goto` are treated conservatively for movement boundaries.

### Constraint Model
For instruction indices `i < j`, add dependency edge `i -> j` when any hazard or barrier condition holds.

```text
E = RAW union WAR union WAW union BarrierEdges
```

A schedule is valid only if all edges remain ordered.

### Alternatives Considered
| Option | Precision | Complexity | Project Fit |
|---|---|---|---|
| Conservative hazards + barriers (chosen) | Moderate-high | Low | Excellent |
| Aggressive alias-aware memory model | Higher | High | Too heavy for current scope |
| Naive random shuffle | Very low safety | Low | Not acceptable |

### Why This Choice
The chosen model is simple enough to implement correctly and strong enough to prevent unsafe reordering in typical course-level IR examples.

## 6. Phase 3: Dependency-Safe Randomized Scheduling
### What We Built
A randomized topological scheduler:
1. build indegrees,
2. maintain ready set (indegree zero),
3. randomly pick ready node,
4. append to output order,
5. update indegrees,
6. repeat until completion.

If the produced order is incomplete (unexpected graph issue), AFIS falls back to original order.

### Comparison of Strategies
| Strategy | Safety | Diversity | Determinism | Use in AFIS |
|---|---|---|---|---|
| Topological with random tie-break (chosen) | High | High | Seed controlled | Primary |
| Deterministic topological | High | Low | High | Not default |
| Full random permutation | Low | Very high | Medium | Rejected |

### Key Metric
```text
Reorder ratio = (moved instruction slots / instruction count) * 100
```

This metric quantifies structural movement while still respecting constraints.

## 7. Phase 4: CFG Reordering and Branch Fixups
### What We Built
When explicit control flow is present:
- build basic blocks,
- keep entry stable,
- randomize non-entry block order,
- preserve successors,
- insert explicit `goto` fixups if fallthrough expectations are broken.

A synthetic exit label is introduced when needed to keep control-flow closure valid.

### Why Fixups Are Required
Block reordering changes physical adjacency. If original behavior depended on implicit fallthrough, explicit branches must be inserted to preserve semantics.

### CFG Metrics
```text
Block reorder ratio = (moved block slots / block count) * 100
```

Also tracked:
- branch fixups inserted,
- side-effect violations,
- CFG mode activation.

### Alternatives Considered
| Option | Correctness risk | Transformation depth | Project fit |
|---|---|---|---|
| Reorder with fixups (chosen) | Low | High | Strong |
| Reorder without fixups | High | Medium | Unsafe |
| No CFG reorder | Low | Low | Too weak for control-flow diversity |

## 8. Phase 5: Optimization Passes
### What We Built
- Constant folding
- Constant propagation
- Copy propagation
- Dead code elimination

These passes run before scheduling so the later shuffle stage works on a cleaner IR.

### Demo Visibility
Each optimization phase saves its own artifact file:
- `02_constant_fold.ir`
- `03_constant_propagation.ir`
- `04_copy_propagation.ir`
- `05_dead_code_elimination.ir`

This makes it easy to open the files one by one during demo and show exactly what changed at each step.

## 9. Phase 6: Register Renaming
### What We Built
- Collect all identifier symbols from read/write positions.
- Generate fresh names from seeded random source.
- Rewrite all instruction operands consistently.

### Why This Matters
Instruction movement changes layout, while renaming changes lexical surface.
Together they increase anti-fingerprint diversity without changing behavior.

## 10. Phase 7: Verification, Artifacts, and Variant Analysis
### Verification Pipeline
For runs with `--verify`:
1. execute original program in interpreter,
2. execute transformed program,
3. compare printed value sequence,
4. label result PASS or FAIL.

### Runtime Semantics Highlights
- undefined register read defaults to 0,
- division/modulo by zero produce runtime error,
- known builtins: `max`, `min`, `abs`, `inc`,
- unknown function fallback is deterministic argument sum,
- execution capped by step limit to guard infinite loops.

### Artifacts Produced
Per run:
- `01_original.ir`
- `02_constant_fold.ir`
- `03_constant_propagation.ir`
- `04_copy_propagation.ir`
- `05_dead_code_elimination.ir`
- `06_shuffled.ir`
- `07_final.ir`
- `cfg.dot`
- `dependency.dot`
- `workflow.dot`
- `transformation_trace.md`
- `fingerprint_metrics.md`
- `verification.txt`
- transformed IR,
- console summary,
- markdown summary,
- HTML summary,
- validation reasoning report.

### Variant Generation
For anti-fingerprint evaluation, AFIS can also:
- generate multiple transformed variants from one input,
- compute a diversification score for each,
- keep only semantically valid candidates for best-variant selection,
- save a `variant_summary.md` file explaining the chosen output.

## 11. Build, CLI, and Workflow Automation
### Build Paths
Windows PowerShell:
```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
```

Makefile path:
```bash
make
```

### CLI Examples
```powershell
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify --artifact-dir build/demo --dump-passes --emit-cfg-dot --emit-dep-dot --emit-trace --fingerprint-report
./build/afis.exe --input samples/example_cfg.ir --output build/cfg_out.ir --verify --seed 123456 --artifact-dir build/demo_cfg --dump-passes --emit-cfg-dot --emit-dep-dot --emit-trace --fingerprint-report
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify --variants 3 --select-best-variant --artifact-dir build/variant_demo
./build/afis.exe --interactive
```

### Unified Run Script (`scripts/run.ps1`)
Menu options:
1. Run one file (auto profile)
2. Run all sample files (auto profile)
3. Validation proof run (auto + reasoning)
4. List sample files
5. Clean and rebuild
6. Exit

### Auto Profile Behavior
- verification forced on,
- report artifacts generated,
- C++ mode auto-detected,
- C++ LLM path fallback to deterministic conversion when needed.

## 12. Experimental Setup
### Dataset
Eleven sample categories:
1. branch-heavy IR,
2. CFG loop IR,
3. full IR (arith + call + memory),
4. pure arithmetic IR,
5. side-effect-heavy IR,
6. basic C++ input,
7. C++ calls-focused input.

### Run Plan
- 3 runs per sample,
- total 21 runs,
- verification enabled for all runs.

### Evaluation Metrics
- semantic pass/fail,
- reorder ratio,
- block reorder ratio,
- branch fixups inserted,
- side-effect violations,
- renamed symbol count,
- hash divergence between original and transformed IR.

## 13. Results and Analysis
### Aggregate Results
| Metric | Value |
|---|---:|
| Total runs | 21 |
| Semantic PASS | 21 |
| Semantic FAIL | 0 |
| Average reorder ratio | 42.55% |
| Average block reorder ratio | 17.86% |
| Average renamed symbols per run | 7 |
| Total branch fixups inserted | 14 |
| Total side-effect violations | 0 |

### Per-Sample Averages
| Sample Category | Runs | Passes | Avg Reorder % | Avg Block Reorder % | Avg Fixups | Avg Renamed |
|---|---:|---:|---:|---:|---:|---:|
| Branch-heavy IR | 3 | 3 | 61.11 | 75.00 | 2.33 | 6 |
| CFG loop IR | 3 | 3 | 60.00 | 50.00 | 2.33 | 3 |
| C++ basic | 3 | 3 | 28.57 | 0.00 | 0.00 | 6 |
| C++ calls | 3 | 3 | 25.92 | 0.00 | 0.00 | 8 |
| Full IR | 3 | 3 | 30.55 | 0.00 | 0.00 | 11 |
| Pure IR | 3 | 3 | 66.67 | 0.00 | 0.00 | 8 |
| Side-effects IR | 3 | 3 | 25.00 | 0.00 | 0.00 | 7 |

### Representative Output Equivalence
| Sample | Original Output | Transformed Output | Semantic Result |
|---|---:|---:|---|
| Full IR | 26 | 26 | PASS |
| CFG loop | 15 | 15 | PASS |
| Branch-heavy | 101 | 101 | PASS |
| Pure IR | 63 | 63 | PASS |
| Side-effects IR | 21 | 21 | PASS |
| C++ basic | 22 | 22 | PASS |
| C++ calls | 14 | 14 | PASS |

### Observations
1. All transformed programs preserved runtime outputs for tested dataset.
2. CFG categories show expected non-zero block movement and fixup insertion.
3. Straight-line programs still show substantial movement when dependencies permit.
4. Side-effect violation count remained zero across all runs.
5. C++ path remained robust due deterministic fallback when LLM conversion failed.

## 14. Design Decisions Summary
| Area | Chosen Design | Main Alternative | Why Chosen |
|---|---|---|---|
| Input conversion | Dual path (LLM + deterministic fallback) | LLM-only | Better reliability in live demos |
| Hazard model | RAW/WAR/WAW + barriers | Naive shuffle | Safety first |
| Scheduling | Randomized topological | Deterministic order only | Diversity with correctness |
| CFG transform | Block reorder + fixups | No CFG transform | Needed for branch-heavy cases |
| Renaming | Full consistent map rewrite | No renaming | Improves lexical diversity |
| Verification | Runtime output equivalence | Static-only confidence | Strong practical evidence |
| Reporting | Markdown + HTML + console summaries | Console-only output | Better project documentation |

## 15. Lessons Learned
1. Constrained randomness is far more practical than unrestricted randomness in compiler transformations.
2. Control-flow transforms are manageable only with explicit fixup logic.
3. A conservative barrier policy prevents many subtle behavior regressions.
4. Optional AI assistance is useful, but deterministic validators must stay authoritative.
5. Report artifacts significantly improve debugging and viva preparation quality.

## 16. Future Work
1. Expand deterministic C++ frontend to include structured control flow.
2. Introduce stronger alias analysis to unlock more safe movement.
3. Improve rendered CFG/dependency visuals beyond raw dot graphs.
4. Add benchmark dashboard generation for larger datasets.
5. Explore formal equivalence tooling alongside runtime validation.

## 17. Conclusion
AFIS meets the central goal of this course project: structural transformation with semantic preservation.
The implementation demonstrates a complete transformation pipeline from parsing to verification and reporting, with reliable outputs across multiple sample categories.

The most important takeaway is architectural: correctness should be layered.
Dependency constraints, CFG checks, side-effect barriers, deterministic application logic, and runtime verification together provide practical safety while still allowing meaningful transformation diversity.

## 18. Appendix
### A. Quick Run Commands
```powershell
./scripts/build.ps1 -Clean -CompilerBinPath "C:\msys64\ucrt64\bin"
./build/afis.exe --input samples/example_full.ir --output build/out.ir --verify
powershell -ExecutionPolicy Bypass -File scripts/run.ps1
```

### B. Validation Artifact Structure
```text
build/all_samples/<sample_name>/runs/run_001/
  transformed.ir
  summary.md
  summary.html
  console_summary.txt
  validation_reasoning.txt
```

### C. File Map
- Core: `src/main.cpp`, `src/parser.cpp`, `src/dependency.cpp`, `src/shuffler.cpp`, `src/cfg.cpp`
- Backend safety: `src/renamer.cpp`, `src/interpreter.cpp`
- C++ path and LLM: `src/cpp_frontend.cpp`, `src/llm.cpp`
- Scripts: `scripts/build.ps1`, `scripts/run.ps1`, `scripts/comprehensive_test.ps1`

## 19. References
1. README.md
2. workflow.md
3. src/main.cpp
4. src/parser.cpp
5. src/dependency.cpp
6. src/shuffler.cpp
7. src/cfg.cpp
8. src/renamer.cpp
9. src/interpreter.cpp
10. src/cpp_frontend.cpp
11. src/llm.cpp
13. scripts/build.ps1
14. scripts/run.ps1
15. build/all_samples/*/runs/*/summary.md
