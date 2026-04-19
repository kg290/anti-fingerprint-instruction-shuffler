param(
    [string]$CompilerBinPath = "C:\msys64\ucrt64\bin",
    [switch]$SkipCleanBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$exePath = Join-Path $projectRoot "build/afis.exe"
$samplesDir = Join-Path $projectRoot "samples"

$testRoot = Join-Path $projectRoot "build/comprehensive_test"
$logDir = Join-Path $testRoot "logs"

Set-Location $projectRoot

function Add-Result(
    [System.Collections.Generic.List[object]]$ResultList,
    [string]$Suite,
    [string]$Name,
    [string]$Status,
    [string]$Reason,
    [string]$Artifact
) {
    $ResultList.Add([PSCustomObject]@{
        Suite = $Suite
        Name = $Name
        Status = $Status
        Reason = $Reason
        Artifact = $Artifact
    }) | Out-Null
}

function Test-SemanticPass([object[]]$OutputLines) {
    foreach ($line in $OutputLines) {
        if (([string]$line) -match 'Semantic equivalence\s*:\s*PASS') {
            return $true
        }
    }
    return $false
}

function ConvertTo-SafeName([string]$text) {
    $safe = $text -replace '[^a-zA-Z0-9]+', '_'
    $safe = $safe.Trim('_')
    if ([string]::IsNullOrWhiteSpace($safe)) {
        return "sample"
    }
    return $safe.ToLowerInvariant()
}

function Get-SampleFiles {
    $extensions = @(".ir", ".cpp", ".cc", ".cxx", ".c++", ".cp")
    $files = Get-ChildItem -Path $samplesDir -File |
        Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object Name |
        ForEach-Object { "samples/$($_.Name)" }

    return @($files)
}

function Invoke-AfisCase(
    [System.Collections.Generic.List[object]]$ResultList,
    [string]$Name,
    [string[]]$CommandArgs,
    [bool]$ExpectSemanticPass = $true
) {
    $logPath = Join-Path $logDir ("direct_{0}.log" -f $Name)
    New-Item -ItemType Directory -Path (Split-Path -Parent $logPath) -Force | Out-Null

    Write-Host "Running direct case: $Name" -ForegroundColor Cyan
    $hasNativePreference = Test-Path variable:PSNativeCommandUseErrorActionPreference
    $oldErrorPreference = $ErrorActionPreference
    if ($hasNativePreference) {
        $oldNativePreference = $PSNativeCommandUseErrorActionPreference
        $PSNativeCommandUseErrorActionPreference = $false
    }

    try {
        $ErrorActionPreference = "Continue"
        $output = & $exePath @CommandArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorPreference
        if ($hasNativePreference) {
            $PSNativeCommandUseErrorActionPreference = $oldNativePreference
        }
    }

    $output | Out-File -FilePath $logPath -Encoding utf8

    if ($exitCode -ne 0) {
        Add-Result $ResultList "direct" $Name "FAIL" ("ExitCode={0}" -f $exitCode) $logPath
        return
    }

    if ($ExpectSemanticPass -and -not (Test-SemanticPass -OutputLines $output)) {
        Add-Result $ResultList "direct" $Name "FAIL" "Missing semantic PASS marker" $logPath
        return
    }

    Add-Result $ResultList "direct" $Name "PASS" "ok" $logPath
}

function Invoke-RunMenuCase(
    [System.Collections.Generic.List[object]]$ResultList,
    [string]$Name,
    [string]$InputSequence,
    [string]$ExpectedPattern
) {
    $logPath = Join-Path $logDir ("menu_{0}.log" -f $Name)
    New-Item -ItemType Directory -Path (Split-Path -Parent $logPath) -Force | Out-Null
    $command = "({0}) | powershell -ExecutionPolicy Bypass -File scripts/run.ps1" -f $InputSequence

    Write-Host "Running menu case: $Name" -ForegroundColor Cyan
    $output = & cmd /c $command 2>&1
    $exitCode = $LASTEXITCODE
    $output | Out-File -FilePath $logPath -Encoding utf8

    if ($exitCode -ne 0) {
        Add-Result $ResultList "menu" $Name "FAIL" ("ExitCode={0}" -f $exitCode) $logPath
        return [PSCustomObject]@{ Success = $false; LogPath = $logPath; Output = $output }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedPattern)) {
        $matched = $false
        foreach ($line in $output) {
            if (([string]$line) -match $ExpectedPattern) {
                $matched = $true
                break
            }
        }

        if (-not $matched) {
            Add-Result $ResultList "menu" $Name "FAIL" ("Missing expected pattern: {0}" -f $ExpectedPattern) $logPath
            return [PSCustomObject]@{ Success = $false; LogPath = $logPath; Output = $output }
        }
    }

    Add-Result $ResultList "menu" $Name "PASS" "ok" $logPath
    return [PSCustomObject]@{ Success = $true; LogPath = $logPath; Output = $output }
}

$results = New-Object 'System.Collections.Generic.List[object]'

if (-not $SkipCleanBuild) {
    Write-Host "Running clean build..." -ForegroundColor Cyan
    & $buildScript -Clean -CompilerBinPath $CompilerBinPath
    if ($LASTEXITCODE -ne 0) {
        throw "Clean build failed."
    }
}

if (-not (Test-Path $exePath)) {
    throw "Executable missing after build: $exePath"
}

if (Test-Path $testRoot) {
    Remove-Item -Path $testRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

$sampleFiles = Get-SampleFiles
if ($sampleFiles.Count -eq 0) {
    throw "No sample files found in $samplesDir"
}

# Direct executable matrix
Invoke-AfisCase -ResultList $results -Name "ir_full_verify" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/ir_full_verify.ir","--verify","--env",".env")
Invoke-AfisCase -ResultList $results -Name "ir_cfg_verify" -CommandArgs @("--input","samples/example_cfg.ir","--output","build/comprehensive_test/ir_cfg_verify.ir","--verify","--env",".env")
Invoke-AfisCase -ResultList $results -Name "ir_side_effects_verify" -CommandArgs @("--input","samples/example_side_effects.ir","--output","build/comprehensive_test/ir_side_effects_verify.ir","--verify","--env",".env")
Invoke-AfisCase -ResultList $results -Name "cpp_basic_verify_deterministic" -CommandArgs @("--input-cpp","samples/example_cpp_basic.cpp","--output","build/comprehensive_test/cpp_basic_verify.ir","--verify","--env",".env","--no-llm-cpp")
Invoke-AfisCase -ResultList $results -Name "cpp_calls_verify_deterministic" -CommandArgs @("--input-cpp","samples/example_cpp_calls.cpp","--output","build/comprehensive_test/cpp_calls_verify.ir","--verify","--env",".env","--no-llm-cpp")
Invoke-AfisCase -ResultList $results -Name "batch_unseeded_runs2_verify" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/batch_unseeded_out.ir","--runs","2","--output-dir","build/comprehensive_test/batch_unseeded","--clean-output-dir","--verify","--env",".env")
Invoke-AfisCase -ResultList $results -Name "batch_fixed_seed_runs2_verify" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/batch_fixed_out.ir","--runs","2","--output-dir","build/comprehensive_test/batch_fixed","--clean-output-dir","--verify","--seed","123456","--fixed-seed-runs","--env",".env")

# Determinism check with same seed
Invoke-AfisCase -ResultList $results -Name "determinism_seed_run_a" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/determinism_seed_a.ir","--verify","--seed","123456","--env",".env")
Invoke-AfisCase -ResultList $results -Name "determinism_seed_run_b" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/determinism_seed_b.ir","--verify","--seed","123456","--env",".env")

$detAPath = Join-Path $projectRoot "build/comprehensive_test/determinism_seed_a.ir"
$detBPath = Join-Path $projectRoot "build/comprehensive_test/determinism_seed_b.ir"
if ((Test-Path $detAPath) -and (Test-Path $detBPath)) {
    $h1 = (Get-FileHash $detAPath -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $detBPath -Algorithm SHA256).Hash
    if ($h1 -eq $h2) {
        Add-Result $results "direct" "determinism_seed_hash_match" "PASS" "same hash as expected" "N/A"
    } else {
        Add-Result $results "direct" "determinism_seed_hash_match" "FAIL" ("hash mismatch: {0} vs {1}" -f $h1, $h2) "N/A"
    }
}

# Variability check without fixed seed (warn only if equal)
Invoke-AfisCase -ResultList $results -Name "variability_unseeded_run_a" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/variability_a.ir","--verify","--env",".env")
Invoke-AfisCase -ResultList $results -Name "variability_unseeded_run_b" -CommandArgs @("--input","samples/example_full.ir","--output","build/comprehensive_test/variability_b.ir","--verify","--env",".env")

$varAPath = Join-Path $projectRoot "build/comprehensive_test/variability_a.ir"
$varBPath = Join-Path $projectRoot "build/comprehensive_test/variability_b.ir"
if ((Test-Path $varAPath) -and (Test-Path $varBPath)) {
    $vh1 = (Get-FileHash $varAPath -Algorithm SHA256).Hash
    $vh2 = (Get-FileHash $varBPath -Algorithm SHA256).Hash
    if ($vh1 -ne $vh2) {
        Add-Result $results "direct" "variability_unseeded_hash_diff" "PASS" "different hashes as expected" "N/A"
    } else {
        Add-Result $results "direct" "variability_unseeded_hash_diff" "WARN" "hashes equal in this trial (possible but uncommon)" "N/A"
    }
}

# Unified CLI menu automation cases
$null = Invoke-RunMenuCase $results "auto_one_file" "echo 1&echo 1&echo 6" "Run completed\."
$null = Invoke-RunMenuCase $results "auto_validation" "echo 3&echo 1&echo 6" "Validation Evidence"
$runAll = Invoke-RunMenuCase $results "auto_run_all" "echo 2&echo 1&echo 6" "Run-all summary:\s*\d+/\d+ passed"

# Validate run-all summary counts and artifacts
if ($runAll.Success) {
    $passed = -1
    $total = -1
    foreach ($lineObj in $runAll.Output) {
        $line = [string]$lineObj
        if ($line -match 'Run-all summary:\s*(\d+)/(\d+)\s+passed') {
            $passed = [int]$Matches[1]
            $total = [int]$Matches[2]
            break
        }
    }

    if ($passed -eq $sampleFiles.Count -and $total -eq $sampleFiles.Count) {
        Add-Result $results "artifacts" "run_all_count_match" "PASS" "all samples reported pass" "build/all_samples/_run_all_summary.txt"
    } else {
        Add-Result $results "artifacts" "run_all_count_match" "FAIL" ("expected {0}/{0}, got {1}/{2}" -f $sampleFiles.Count, $passed, $total) "build/all_samples/_run_all_summary.txt"
    }

    foreach ($sampleFile in $sampleFiles) {
        $sampleKey = ConvertTo-SafeName $sampleFile
        $sampleDir = Join-Path $projectRoot ("build/all_samples/{0}" -f $sampleKey)

        $requiredFiles = @(
            "transformed.ir",
            "summary.md",
            "summary.html",
            "console_summary.txt",
            "validation_reasoning.txt"
        )

        $missing = New-Object 'System.Collections.Generic.List[string]'
        foreach ($rf in $requiredFiles) {
            $path = Join-Path $sampleDir $rf
            if (-not (Test-Path $path)) {
                $missing.Add($rf) | Out-Null
            }
        }

        if ($missing.Count -gt 0) {
            Add-Result $results "artifacts" ("artifact_set_{0}" -f $sampleKey) "FAIL" ("missing files: {0}" -f ($missing -join ', ')) $sampleDir
            continue
        }

        $reasoningPath = Join-Path $sampleDir "validation_reasoning.txt"
        $reasoningText = Get-Content -Path $reasoningPath -Raw
        if ($reasoningText -match 'Semantic equivalence:\s*PASS') {
            Add-Result $results "artifacts" ("reasoning_pass_{0}" -f $sampleKey) "PASS" "reasoning confirms semantic PASS" $reasoningPath
        } else {
            Add-Result $results "artifacts" ("reasoning_pass_{0}" -f $sampleKey) "FAIL" "reasoning missing semantic PASS" $reasoningPath
        }
    }
}

$summaryPath = Join-Path $testRoot "summary.txt"
$summaryText = $results | Sort-Object Suite, Name | Format-Table -AutoSize | Out-String
$summaryText | Set-Content -Path $summaryPath -Encoding utf8
Write-Host $summaryText

$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count
$warnCount = @($results | Where-Object { $_.Status -eq "WARN" }).Count
$passCount = @($results | Where-Object { $_.Status -eq "PASS" }).Count

Write-Host ""
Write-Host "Comprehensive test complete: PASS=$passCount FAIL=$failCount WARN=$warnCount" -ForegroundColor Cyan
Write-Host "Summary: $summaryPath"

if ($failCount -gt 0) {
    exit 2
}
