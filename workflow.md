# AFIS Process Workflow (Layman Version)

This file explains what happens in simple language when you run this project.

Think of AFIS like this:

- You give it a program in small IR language (or C++).
- AFIS rearranges internals to make it look different.
- AFIS checks that result is still behaving the same.
- You get output files and reports.

The key idea is:

- The text of IR is supposed to change.
- The behavior/output is supposed to stay same.

---

## 1) What Happens First If You Run AFIS

Example command:

`build/afis.exe --input samples/example_full.ir --output build/out.ir --verify`

Step by step:

1. AFIS reads your command options.
2. AFIS checks that required values exist (like input file).
3. AFIS loads optional `.env` config (for Gemini/LLM features).
4. AFIS reads your input program.
5. AFIS validates labels and jumps so control flow is valid.

If anything is wrong in these first checks, it stops and shows an error.

---

## 2) How AFIS Changes The Program

After input is accepted, AFIS starts transformation.

### 2.1 Seed setup

1. If you gave `--seed`, AFIS uses it.
2. If not, AFIS creates a random seed.

This seed controls shuffle/rename randomness.

### 2.2 Decide mode

AFIS checks program style:

1. If input has labels/goto/if-goto, AFIS uses CFG mode.
2. If input is straight-line instructions, AFIS uses straight-line mode.

### 2.3 Dependency-safe shuffle

AFIS does not shuffle blindly.

It builds safety rules first:

- RAW
- WAR
- WAW
- side-effect barriers (print/call/load/store/branch boundaries)

Then it shuffles only where safe.

### 2.4 CFG block reorder (if CFG mode)

If CFG mode is active:

1. AFIS makes basic blocks.
2. AFIS can reorder non-entry blocks.
3. AFIS adds fixup jumps if needed to keep flow correct.

### 2.5 Optional LLM substitution

If `--llm-substitute` is enabled:

1. AFIS finds safe substitution candidates.
2. LLM may suggest candidate IDs.
3. AFIS still validates them deterministically before applying.

So LLM is advisory; final safety is still deterministic.

### 2.6 Register renaming

AFIS renames variables/register symbols consistently.

This makes transformed IR look more obfuscated/diverse.

### 2.7 Write transformed output file

AFIS writes transformed IR to your output path.

---

## 3) How AFIS Proves It Still Works

If you use `--verify`, AFIS runs both versions.

1. Run original program in built-in interpreter.
2. Run transformed program in built-in interpreter.
3. Compare printed outputs exactly.

Then AFIS prints:

- Original output
- Transformed output
- `Semantic equivalence: PASS/FAIL`

Meaning:

- PASS = behavior preserved.
- FAIL = behavior changed (or runtime error).

So yes, IR text changes, but AFIS checks behavior equality.

---

## 4) What You See In Console Summary

For each run AFIS prints:

1. Input/output file names.
2. CFG mode ON/OFF.
3. Moved instruction/block ratios.
4. Branch fixups count.
5. Hazard/barrier related stats.
6. Original hash and transformed hash.
7. Verification result (if enabled).
8. Report paths (if report options were given).

---

## 5) Report Files (Markdown + HTML)

If you pass report options:

- `--report <file.md>`
- `--report-html <file.html>`

AFIS writes detailed run reports containing:

1. run config,
2. transformation metrics,
3. hazard snapshot,
4. verification result,
5. rename map,
6. transformed IR.

---

## 6) C++ Input Path (If Input Is .cpp)

If you give C++ file:

1. AFIS tries to convert C++ to IR.
2. Depending on flags/config, it can use deterministic converter and/or LLM path.
3. After conversion, the same normal IR transformation pipeline runs.

So C++ path is: C++ -> IR -> transform -> verify/report.

---

## 7) Fast CLI Workflow (scripts/run.ps1)

Use this command:

`powershell -ExecutionPolicy Bypass -File scripts/run.ps1`

You get a clean menu:

1. Run one file (AUTO best mode).
2. Run ALL sample files (AUTO best mode).
3. Validation proof run (AUTO + reasoning).
4. List sample files.
5. Clean + rebuild.
6. Exit.

In AUTO modes, script does not ask repeated yes/no toggles.
It automatically picks a stable best profile:

- verification ON,
- C++/IR auto-detection by file extension,
- markdown/html reports ON,
- automatic reasoning report generation,
- automatic C++ fallback to deterministic conversion if LLM path fails.

---

## 8) Validation Proof Mode (Ground Truth)

When you choose Validation proof run:

1. Script shows all sample files from `samples/`.
2. You pick by number.
3. Script auto-runs validation with `--verify` so original and transformed outputs are compared.
4. For C++ input, script tries LLM path and auto-falls back to deterministic conversion if needed.
5. Script writes validation artifacts (console + markdown + html + reasoning evidence).
6. At the end, script prints semantic status and short reason:

- `PASS` means both outputs match.
- `FAIL` means outputs differ.

Default artifact folder:

`build/validation/<sample_name>/`

Inside that folder you get:

1. `transformed.ir`
2. `validation_console_summary.txt`
3. `validation_summary.md`
4. `validation_summary.html`
5. `validation_reasoning.txt`

`validation_reasoning.txt` explains:

- how `--verify` validated behavior,
- original output vs transformed output snapshot,
- why PASS/FAIL happened,
- run facts like hashes and movement metrics.

This mode is the easiest way to prove behavior is preserved with concrete evidence files.

---

## 9) Run ALL Samples Button (Menu Option)

When you choose Run ALL samples:

1. Script collects every sample file in `samples/`.
2. For each sample, it creates a separate folder in:

`build/all_samples/<sample_name>/`

3. Script asks for number of runs per sample.
4. It runs AFIS that many times for each sample.
4. It auto-detects `.cpp` vs `.ir` per file.
5. For C++ files, it uses LLM path when available and auto-falls back to deterministic conversion if needed.
6. It saves per-sample artifacts:

- `transformed.ir`
- `summary.md`
- `summary.html`
- `console_summary.txt`
- `validation_reasoning.txt`

If runs-per-sample is greater than 1, each sample folder contains:

- `runs/run_001/`
- `runs/run_002/`
- `runs/run_003/` ...

Each run folder contains the same artifact set.

7. At end, script prints PASS/FAIL summary for all samples with semantic status and reasoning file path.

This is your one-click project-wide run mode.

---

## 10) Why Output IR Looks Different Every Time

If you do not fix seed:

- AFIS uses random seed each run.
- Different safe shuffle/rename choices happen.
- Transformed hash often changes.

If you set seed:

- Same seed gives reproducible run behavior.

Useful commands:

Unseeded twice:

`build/afis.exe --input samples/example_full.ir --output build/unseeded.ir --runs 2 --verify`

Fixed seed:

`build/afis.exe --input samples/example_full.ir --output build/fixed.ir --runs 2 --verify --seed 123456 --fixed-seed-runs`

---

## 11) Final Mental Model

When you run AFIS, this is the real flow:

1. Read input.
2. Validate control flow.
3. Build safety dependencies.
4. Shuffle/reorder only where safe.
5. Rename symbols.
6. Verify outputs (if verify enabled).
7. Save transformed IR and reports.

So the project is not just "changing text".

It is: controlled transformation + behavior check.
