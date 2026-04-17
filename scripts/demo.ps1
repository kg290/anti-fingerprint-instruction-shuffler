param(
    [Alias("Input")]
    [string]$InputFile = "samples/example_full.ir",
    [int]$Runs = 3,
    [string]$CompilerBinPath = "C:\msys64\ucrt64\bin",
    [switch]$FixedSeed,
    [switch]$ShowMap,
    [switch]$CleanRunDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"

Write-Host "Step 1/5: Building project"
& $buildScript -CompilerBinPath $CompilerBinPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$exePath = Join-Path $projectRoot "build/afis.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "Executable not found: $exePath"
    exit 1
}

$inputPath = Join-Path $projectRoot $InputFile
if (-not (Test-Path $inputPath)) {
    Write-Error "Input file not found: $inputPath"
    exit 1
}

$runDir = Join-Path $projectRoot "build/demo_runs"
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
if ($CleanRunDir) {
    Get-ChildItem -Path $runDir -Filter "run_*.ir" -ErrorAction SilentlyContinue | Remove-Item -Force
}

Write-Host "Step 2/5: Running transformed builds"
$outputFiles = New-Object 'System.Collections.Generic.List[string]'
$semanticPassCount = 0
$reorderRatios = New-Object 'System.Collections.Generic.List[double]'
$blockReorderRatios = New-Object 'System.Collections.Generic.List[double]'
$branchFixups = New-Object 'System.Collections.Generic.List[int]'
$cfgEnabledCount = 0
$sideEffectViolationTotal = 0

for ($i = 1; $i -le $Runs; $i++) {
    $outFile = Join-Path $runDir ("run_" + $i + ".ir")
    $runParams = New-Object 'System.Collections.Generic.List[string]'
    $runParams.Add("--input") | Out-Null
    $runParams.Add($inputPath) | Out-Null
    $runParams.Add("--output") | Out-Null
    $runParams.Add($outFile) | Out-Null
    $runParams.Add("--verify") | Out-Null

    if ($FixedSeed) {
        $runParams.Add("--seed") | Out-Null
        $runParams.Add("123456") | Out-Null
    } else {
        $randomSeed = [uint64](Get-Random -Minimum 1 -Maximum 2147483647)
        $runParams.Add("--seed") | Out-Null
        $runParams.Add($randomSeed.ToString()) | Out-Null
    }
    if ($ShowMap) {
        $runParams.Add("--show-map") | Out-Null
    }

    Write-Host ""
    Write-Host ("Run {0}/{1}" -f $i, $Runs)
    $runOutput = & $exePath $runParams.ToArray() 2>&1
    $runExitCode = $LASTEXITCODE
    $runOutput | ForEach-Object { Write-Host $_ }

    if ($runExitCode -ne 0) {
        exit $runExitCode
    }

    if ($runOutput -match "Semantic equivalence\s*:\s*PASS") {
        $semanticPassCount += 1
    }

    $reorderMatch = $runOutput | Select-String -Pattern '^Reorder ratio\s*:\s*([0-9]+(?:\.[0-9]+)?)%' | Select-Object -First 1
    if ($reorderMatch) {
        $reorderRatios.Add([double]$reorderMatch.Matches[0].Groups[1].Value) | Out-Null
    }

    $blockReorderMatch = $runOutput | Select-String -Pattern '^Block reorder ratio\s*:\s*([0-9]+(?:\.[0-9]+)?)%' | Select-Object -First 1
    if ($blockReorderMatch) {
        $blockReorderRatios.Add([double]$blockReorderMatch.Matches[0].Groups[1].Value) | Out-Null
    }

    $branchFixupMatch = $runOutput | Select-String -Pattern '^Branch fixups inserted\s*:\s*([0-9]+)' | Select-Object -First 1
    if ($branchFixupMatch) {
        $branchFixups.Add([int]$branchFixupMatch.Matches[0].Groups[1].Value) | Out-Null
    }

    if ($runOutput -match "CFG mode\s*:\s*ENABLED") {
        $cfgEnabledCount += 1
    }

    $sideEffectMatch = $runOutput | Select-String -Pattern '^Side-effect violations\s*:\s*([0-9]+)' | Select-Object -First 1
    if ($sideEffectMatch) {
        $sideEffectViolationTotal += [int]$sideEffectMatch.Matches[0].Groups[1].Value
    }

    $hash = (Get-FileHash -Algorithm SHA256 $outFile).Hash
    Write-Host ("Transformed IR hash: {0}" -f $hash.Substring(0, 16))
    $outputFiles.Add($outFile) | Out-Null
}

Write-Host ""
Write-Host "Step 3/5: Diversity check"
if ($Runs -ge 2) {
    $diff = Compare-Object (Get-Content $outputFiles[0]) (Get-Content $outputFiles[1])
    $diffCount = if ($null -eq $diff) { 0 } else { ($diff | Measure-Object).Count }
    $hash1 = (Get-FileHash -Algorithm SHA256 $outputFiles[0]).Hash
    $hash2 = (Get-FileHash -Algorithm SHA256 $outputFiles[1]).Hash
    $hashChanged = ($hash1 -ne $hash2)

    Write-Host ("Line-level layout diff entries (run_1 vs run_2): {0}" -f $diffCount)
    Write-Host ("Hash changed between run_1 and run_2: {0}" -f ($(if ($hashChanged) { "YES" } else { "NO" })))

    if ($FixedSeed) {
        if ($diffCount -eq 0 -and -not $hashChanged) {
            Write-Host "PASS: Fixed seed produced identical transformed layout."
        } else {
            Write-Host "WARNING: Fixed seed produced different layouts."
        }
    } else {
        if ($diffCount -gt 0 -or $hashChanged) {
            Write-Host "PASS: Unseeded runs produced different transformed layouts."
        } else {
            Write-Host "WARNING: First two runs matched. Try increasing --Runs."
        }
    }
} else {
    Write-Host "Skipped (Runs < 2)"
}

Write-Host ""
Write-Host "Step 4/5: Metrics summary"
$semanticPassRate = if ($Runs -gt 0) { (100.0 * $semanticPassCount / $Runs) } else { 0.0 }
Write-Host ("Semantic pass rate        : {0}/{1} ({2:N2}%)" -f $semanticPassCount, $Runs, $semanticPassRate)

if ($reorderRatios.Count -gt 0) {
    $avgReorder = ($reorderRatios.ToArray() | Measure-Object -Average).Average
    Write-Host ("Average reorder ratio     : {0:N2}%" -f $avgReorder)
}

if ($blockReorderRatios.Count -gt 0) {
    $avgBlockReorder = ($blockReorderRatios.ToArray() | Measure-Object -Average).Average
    Write-Host ("Average block reorder ratio: {0:N2}%" -f $avgBlockReorder)
}

if ($branchFixups.Count -gt 0) {
    $totalFixups = ($branchFixups.ToArray() | Measure-Object -Sum).Sum
    Write-Host ("Total branch fixups       : {0}" -f $totalFixups)
}

Write-Host ("CFG-enabled runs          : {0}/{1}" -f $cfgEnabledCount, $Runs)
Write-Host ("Total side-effect violations across runs: {0}" -f $sideEffectViolationTotal)

Write-Host ""
Write-Host "Step 5/5: Demo artifacts"
Write-Host "Input IR        : $inputPath"
Write-Host "Output directory: $runDir"
Write-Host ""
Write-Host "Open run_1.ir and run_2.ir side-by-side to show instruction and register-level diversity."
