param(
    [string]$CompilerBinPath = "C:\msys64\ucrt64\bin",
    [string]$EnvPath = ".env"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$exePath = Join-Path $projectRoot "build/afis.exe"
$samplesDir = Join-Path $projectRoot "samples"

function Ensure-Binary {
    if (-not (Test-Path $exePath)) {
        Write-Host "Executable not found. Building first..." -ForegroundColor Yellow
        & $buildScript -CompilerBinPath $CompilerBinPath
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed."
        }
    }
}

function Read-Default([string]$prompt, [string]$defaultValue) {
    $value = Read-Host "$prompt [$defaultValue]"
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $defaultValue
    }
    return $value.Trim()
}

function Read-YesNo([string]$prompt, [bool]$defaultYes) {
    $suffix = if ($defaultYes) { "Y/n" } else { "y/N" }
    while ($true) {
        $value = Read-Host "$prompt ($suffix)"
        if ([string]::IsNullOrWhiteSpace($value)) {
            return $defaultYes
        }

        $v = $value.Trim().ToLowerInvariant()
        if ($v -in @("y", "yes", "1", "true")) {
            return $true
        }
        if ($v -in @("n", "no", "0", "false")) {
            return $false
        }

        Write-Host "Please answer yes or no." -ForegroundColor Yellow
    }
}

function Read-PositiveInt([string]$prompt, [int]$defaultValue) {
    while ($true) {
        $value = Read-Host "$prompt [$defaultValue]"
        if ([string]::IsNullOrWhiteSpace($value)) {
            return $defaultValue
        }

        $parsed = 0
        if ([int]::TryParse($value.Trim(), [ref]$parsed) -and $parsed -gt 0) {
            return $parsed
        }

        Write-Host "Please enter a positive integer." -ForegroundColor Yellow
    }
}

function Get-SampleFiles {
    if (-not (Test-Path $samplesDir)) {
        throw "Samples directory not found: $samplesDir"
    }

    $extensions = @(".ir", ".cpp", ".cc", ".cxx", ".c++", ".cp")
    $files = Get-ChildItem -Path $samplesDir -File |
        Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object Name |
        ForEach-Object { "samples/$($_.Name)" }

    return @($files)
}

function Show-SampleFiles {
    $sampleFiles = Get-SampleFiles
    if ($sampleFiles.Count -eq 0) {
        Write-Host "No sample files found." -ForegroundColor Yellow
        return
    }

    Write-Host ""
    Write-Host "Available sample files:" -ForegroundColor Cyan
    for ($i = 0; $i -lt $sampleFiles.Count; $i++) {
        Write-Host ("{0}) {1}" -f ($i + 1), $sampleFiles[$i])
    }
}

function Select-SampleOrManual {
    $sampleFiles = Get-SampleFiles
    if ($sampleFiles.Count -eq 0) {
        throw "No sample files found in $samplesDir"
    }

    Show-SampleFiles
    Write-Host "M) Enter custom path manually"

    while ($true) {
        $choice = Read-Host "Choose sample number"
        if ([string]::IsNullOrWhiteSpace($choice)) {
            return $sampleFiles[0]
        }

        $trimmed = $choice.Trim()
        if ($trimmed.ToLowerInvariant() -eq "m") {
            return Read-Default "Manual input path" $sampleFiles[0]
        }

        $parsed = 0
        if ([int]::TryParse($trimmed, [ref]$parsed)) {
            if ($parsed -ge 1 -and $parsed -le $sampleFiles.Count) {
                return $sampleFiles[$parsed - 1]
            }
        }

        Write-Host "Invalid choice. Select a valid number or M." -ForegroundColor Yellow
    }
}

function ConvertTo-SafeName([string]$text) {
    $safe = $text -replace '[^a-zA-Z0-9]+', '_'
    $safe = $safe.Trim('_')
    if ([string]::IsNullOrWhiteSpace($safe)) {
        return "sample"
    }
    return $safe.ToLowerInvariant()
}

function Is-CppInputFile([string]$path) {
    $ext = [System.IO.Path]::GetExtension($path)
    if ([string]::IsNullOrWhiteSpace($ext)) {
        return $false
    }

    return @(".cpp", ".cc", ".cxx", ".c++", ".cp") -contains $ext.ToLowerInvariant()
}

function Resolve-ProjectPath([string]$pathValue) {
    if ([string]::IsNullOrWhiteSpace($pathValue)) {
        return ""
    }

    if ([System.IO.Path]::IsPathRooted($pathValue)) {
        return $pathValue
    }

    return Join-Path $projectRoot $pathValue
}

function Get-DotEnvValue([string]$FilePath, [string]$Key) {
    if (-not (Test-Path $FilePath)) {
        return ""
    }

    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$'
    foreach ($line in (Get-Content -Path $FilePath -ErrorAction SilentlyContinue)) {
        $text = [string]$line
        if ($text -match $pattern) {
            $value = $Matches[1].Trim()
            if ($value.StartsWith('"') -and $value.EndsWith('"') -and $value.Length -ge 2) {
                $value = $value.Substring(1, $value.Length - 2)
            }
            if ($value.StartsWith("'") -and $value.EndsWith("'") -and $value.Length -ge 2) {
                $value = $value.Substring(1, $value.Length - 2)
            }
            return $value.Trim()
        }
    }

    return ""
}

function Test-LlmConfigured {
    $envKey = [Environment]::GetEnvironmentVariable("GEMINI_API_KEY")
    if (-not [string]::IsNullOrWhiteSpace($envKey)) {
        return $true
    }

    $resolvedEnvPath = Resolve-ProjectPath $EnvPath
    $fileKey = Get-DotEnvValue -FilePath $resolvedEnvPath -Key "GEMINI_API_KEY"
    if ([string]::IsNullOrWhiteSpace($fileKey)) {
        return $false
    }

    $normalized = $fileKey.Trim().ToLowerInvariant()
    if ($normalized -in @("your_api_key_here", "replace_me", "changeme", "none", "null")) {
        return $false
    }

    return $true
}

function Get-AutoBestProfile {
    $llmConfigured = Test-LlmConfigured

    return [PSCustomObject]@{
        Verify = $true
        ShowMap = $false
        EnableSub = $false
        EnableExplain = $false
        UseSeed = $false
        LlmConfigured = $llmConfigured
    }
}

function Invoke-Afis([string[]]$Arguments, [string]$ConsoleSummaryPath = "") {
    $hasNativePreference = Test-Path variable:PSNativeCommandUseErrorActionPreference
    $oldErrorPreference = $ErrorActionPreference
    if ($hasNativePreference) {
        $oldNativePreference = $PSNativeCommandUseErrorActionPreference
        $PSNativeCommandUseErrorActionPreference = $false
    }

    try {
        $ErrorActionPreference = "Continue"
        $output = & $exePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorPreference
        if ($hasNativePreference) {
            $PSNativeCommandUseErrorActionPreference = $oldNativePreference
        }
    }

    $output | ForEach-Object { Write-Host $_ }

    if (-not [string]::IsNullOrWhiteSpace($ConsoleSummaryPath)) {
        $output | Out-File -FilePath $ConsoleSummaryPath -Encoding utf8
    }

    return [PSCustomObject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Build-CppDeterministicFallbackArgs([string[]]$Arguments) {
    $filtered = New-Object 'System.Collections.Generic.List[string]'
    foreach ($arg in $Arguments) {
        if ($arg -in @("--no-llm-cpp", "--llm-substitute", "--llm-explain")) {
            continue
        }
        $filtered.Add($arg) | Out-Null
    }
    $filtered.Add("--no-llm-cpp") | Out-Null
    return $filtered.ToArray()
}

function Invoke-AfisAuto(
    [string[]]$Arguments,
    [bool]$IsCppInput,
    [bool]$LlmConfigured,
    [string]$ConsoleSummaryPath = ""
) {
    $result = Invoke-Afis -Arguments $Arguments -ConsoleSummaryPath $ConsoleSummaryPath
    $usedCppFallback = $false

    if ($result.ExitCode -ne 0 -and $IsCppInput -and $LlmConfigured) {
        Write-Host "Primary C++ LLM path failed; retrying with deterministic --no-llm-cpp fallback..." -ForegroundColor Yellow
        $fallbackArgs = Build-CppDeterministicFallbackArgs -Arguments $Arguments
        $result = Invoke-Afis -Arguments $fallbackArgs -ConsoleSummaryPath $ConsoleSummaryPath
        $usedCppFallback = $true
    }

    return [PSCustomObject]@{
        Result = $result
        UsedCppFallback = $usedCppFallback
    }
}

function Get-SemanticEquivalence([object[]]$OutputLines) {
    foreach ($line in $OutputLines) {
        $text = [string]$line
        if ($text -match 'Semantic equivalence\s*:\s*(PASS|FAIL)') {
            return $Matches[1]
        }
    }

    return "UNKNOWN"
}

function Get-FieldValueFromOutput([object[]]$OutputLines, [string]$FieldName) {
    $pattern = '^\s*' + [regex]::Escape($FieldName) + '\s*:\s*(.+)$'
    foreach ($line in $OutputLines) {
        $text = [string]$line
        if ($text -match $pattern) {
            return $Matches[1].Trim()
        }
    }

    return ""
}

function Get-VerificationDetails([object[]]$OutputLines) {
    $originalLines = New-Object 'System.Collections.Generic.List[string]'
    $transformedLines = New-Object 'System.Collections.Generic.List[string]'
    $capture = ""
    $semantic = "UNKNOWN"

    foreach ($lineObj in $OutputLines) {
        $line = [string]$lineObj

        if ($line -match '^\s*Original output\s*:\s*$') {
            $capture = "original"
            continue
        }

        if ($line -match '^\s*Transformed output\s*:\s*$') {
            $capture = "transformed"
            continue
        }

        if ($line -match 'Semantic equivalence\s*:\s*(PASS|FAIL)') {
            $semantic = $Matches[1]
            $capture = ""
            continue
        }

        if ($line -match '^\s*Report markdown\s*:') {
            $capture = ""
        }

        if ($line -match '^\s*Report html\s*:') {
            $capture = ""
        }

        if ($capture -eq "original") {
            $originalLines.Add($line) | Out-Null
            continue
        }

        if ($capture -eq "transformed") {
            $transformedLines.Add($line) | Out-Null
            continue
        }
    }

    while ($originalLines.Count -gt 0 -and [string]::IsNullOrWhiteSpace($originalLines[$originalLines.Count - 1])) {
        $originalLines.RemoveAt($originalLines.Count - 1)
    }

    while ($transformedLines.Count -gt 0 -and [string]::IsNullOrWhiteSpace($transformedLines[$transformedLines.Count - 1])) {
        $transformedLines.RemoveAt($transformedLines.Count - 1)
    }

    $originalText = ($originalLines -join [Environment]::NewLine).TrimEnd()
    $transformedText = ($transformedLines -join [Environment]::NewLine).TrimEnd()

    return [PSCustomObject]@{
        SemanticEquivalence = $semantic
        OriginalOutput = $originalText
        TransformedOutput = $transformedText
    }
}

function Write-ValidationReasoningReport(
    [string]$ReasoningPath,
    [string]$SourceFile,
    [string]$TransformedFile,
    [string]$ConsoleSummaryPath,
    [string]$ReportPath,
    [string]$ReportHtmlPath,
    [object[]]$OutputLines
) {
    $verification = Get-VerificationDetails -OutputLines $OutputLines
    $semantic = $verification.SemanticEquivalence
    $originalOutput = $verification.OriginalOutput
    $transformedOutput = $verification.TransformedOutput

    $inputMode = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Input mode"
    $cfgMode = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "CFG mode"
    $instructionCount = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Instruction count"
    $originalHash = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Original IR hash"
    $transformedHash = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Transformed IR hash"
    $movedInstructionSlots = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Moved instruction slots"
    $reorderRatio = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Reorder ratio"
    $renamedSymbols = Get-FieldValueFromOutput -OutputLines $OutputLines -FieldName "Renamed symbols"

    $hashChanged = "UNKNOWN"
    if (-not [string]::IsNullOrWhiteSpace($originalHash) -and -not [string]::IsNullOrWhiteSpace($transformedHash)) {
        if ($originalHash -ne $transformedHash) {
            $hashChanged = "YES"
        } else {
            $hashChanged = "NO"
        }
    }

    $outputMatch = "UNKNOWN"
    if ($semantic -eq "PASS") {
        $outputMatch = "YES"
    } elseif ($semantic -eq "FAIL") {
        $outputMatch = "NO"
    }

    $reasoningLines = New-Object 'System.Collections.Generic.List[string]'
    $reasoningLines.Add("AFIS Validation Reasoning Report") | Out-Null
    $reasoningLines.Add("Generated at: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')") | Out-Null
    $reasoningLines.Add("") | Out-Null
    $reasoningLines.Add("Result") | Out-Null
    $reasoningLines.Add("- Semantic equivalence: $semantic") | Out-Null
    $reasoningLines.Add("- Output match from --verify comparison: $outputMatch") | Out-Null
    $reasoningLines.Add("- Structural hash changed: $hashChanged") | Out-Null
    $reasoningLines.Add("") | Out-Null
    $reasoningLines.Add("Ground Reasoning") | Out-Null

    if ($semantic -eq "PASS") {
        $reasoningLines.Add("1) Validation mode forced --verify, so AFIS executed both original and transformed programs.") | Out-Null
        $reasoningLines.Add("2) AFIS printed both outputs and marked semantic equivalence as PASS.") | Out-Null
        $reasoningLines.Add("3) PASS means the transformed program produced exactly the same output text as the original program.") | Out-Null
        if ($hashChanged -eq "YES") {
            $reasoningLines.Add("4) IR hash changed, which is evidence that representation changed while behavior stayed the same.") | Out-Null
        }
    } elseif ($semantic -eq "FAIL") {
        $reasoningLines.Add("1) Validation mode forced --verify, so AFIS executed both original and transformed programs.") | Out-Null
        $reasoningLines.Add("2) AFIS marked semantic equivalence as FAIL.") | Out-Null
        $reasoningLines.Add("3) FAIL means the output text differed between original and transformed execution.") | Out-Null
    } else {
        $reasoningLines.Add("1) Validation run completed, but semantic equivalence line could not be parsed from console output.") | Out-Null
        $reasoningLines.Add("2) Check console summary and reports for the raw verification section.") | Out-Null
    }

    $reasoningLines.Add("") | Out-Null
    $reasoningLines.Add("Verification Output Snapshot") | Out-Null
    $reasoningLines.Add("Original output:") | Out-Null
    if ([string]::IsNullOrWhiteSpace($originalOutput)) {
        $reasoningLines.Add("<empty>") | Out-Null
    } else {
        foreach ($line in ($originalOutput -split '\r?\n')) {
            $reasoningLines.Add($line) | Out-Null
        }
    }
    $reasoningLines.Add("") | Out-Null
    $reasoningLines.Add("Transformed output:") | Out-Null
    if ([string]::IsNullOrWhiteSpace($transformedOutput)) {
        $reasoningLines.Add("<empty>") | Out-Null
    } else {
        foreach ($line in ($transformedOutput -split '\r?\n')) {
            $reasoningLines.Add($line) | Out-Null
        }
    }

    $reasoningLines.Add("") | Out-Null
    $reasoningLines.Add("Run Facts") | Out-Null
    $reasoningLines.Add("- Input file: $SourceFile") | Out-Null
    $reasoningLines.Add("- Transformed file: $TransformedFile") | Out-Null
    if (-not [string]::IsNullOrWhiteSpace($inputMode)) { $reasoningLines.Add("- Input mode: $inputMode") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($cfgMode)) { $reasoningLines.Add("- CFG mode: $cfgMode") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($instructionCount)) { $reasoningLines.Add("- Instruction count: $instructionCount") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($movedInstructionSlots)) { $reasoningLines.Add("- Moved instruction slots: $movedInstructionSlots") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($reorderRatio)) { $reasoningLines.Add("- Reorder ratio: $reorderRatio") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($renamedSymbols)) { $reasoningLines.Add("- Renamed symbols: $renamedSymbols") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($originalHash)) { $reasoningLines.Add("- Original IR hash: $originalHash") | Out-Null }
    if (-not [string]::IsNullOrWhiteSpace($transformedHash)) { $reasoningLines.Add("- Transformed IR hash: $transformedHash") | Out-Null }
    $reasoningLines.Add("- Console summary: $ConsoleSummaryPath") | Out-Null
    $reasoningLines.Add("- Markdown report: $ReportPath") | Out-Null
    $reasoningLines.Add("- HTML report: $ReportHtmlPath") | Out-Null

    $reasoningDir = Split-Path -Parent $ReasoningPath
    if (-not [string]::IsNullOrWhiteSpace($reasoningDir)) {
        New-Item -ItemType Directory -Path $reasoningDir -Force | Out-Null
    }

    $reasoningLines | Out-File -FilePath $ReasoningPath -Encoding utf8

    return [PSCustomObject]@{
        SemanticEquivalence = $semantic
        OutputMatch = $outputMatch
        HashChanged = $hashChanged
        OriginalOutput = $originalOutput
        TransformedOutput = $transformedOutput
    }
}

function Start-ValidationRun {
    Ensure-Binary

    $sourceFile = Select-SampleOrManual
    $validationKey = ConvertTo-SafeName $sourceFile
    $validationDir = Join-Path "build/validation" $validationKey
    New-Item -ItemType Directory -Path $validationDir -Force | Out-Null

    $outputPath = Join-Path $validationDir "transformed.ir"
    $reportPath = Join-Path $validationDir "validation_summary.md"
    $reportHtmlPath = Join-Path $validationDir "validation_summary.html"
    $summaryPath = Join-Path $validationDir "validation_console_summary.txt"
    $reasoningPath = Join-Path $validationDir "validation_reasoning.txt"
    $opts = Get-AutoBestProfile

    $args = New-Object 'System.Collections.Generic.List[string]'
    if (Is-CppInputFile $sourceFile) {
        $args.Add("--input-cpp") | Out-Null
    } else {
        $args.Add("--input") | Out-Null
    }
    $args.Add($sourceFile) | Out-Null
    $args.Add("--output") | Out-Null
    $args.Add($outputPath) | Out-Null
    $args.Add("--env") | Out-Null
    $args.Add($EnvPath) | Out-Null
    $args.Add("--verify") | Out-Null
    $args.Add("--report") | Out-Null
    $args.Add($reportPath) | Out-Null
    $args.Add("--report-html") | Out-Null
    $args.Add($reportHtmlPath) | Out-Null
    if (-not $opts.LlmConfigured) {
        $args.Add("--no-llm-cpp") | Out-Null
    }
    if ($opts.ShowMap) { $args.Add("--show-map") | Out-Null }
    if ($opts.EnableSub) { $args.Add("--llm-substitute") | Out-Null }
    if ($opts.EnableExplain) { $args.Add("--llm-explain") | Out-Null }
    if ($opts.UseSeed) {
        $args.Add("--seed") | Out-Null
        $args.Add("123456") | Out-Null
    }

    Write-Host ""
    Write-Host "Running validation proof mode..." -ForegroundColor Cyan
    Write-Host "Auto profile: verify=ON, llmConfigured=$($opts.LlmConfigured), cppConversion=$(if ($opts.LlmConfigured) { 'LLM with auto fallback' } else { 'deterministic' })" -ForegroundColor DarkCyan
    Write-Host "Command: afis.exe $($args -join ' ')" -ForegroundColor DarkCyan

    $autoInvoke = Invoke-AfisAuto -Arguments $args.ToArray() -IsCppInput (Is-CppInputFile $sourceFile) -LlmConfigured $opts.LlmConfigured -ConsoleSummaryPath $summaryPath
    $result = $autoInvoke.Result
    if ($result.ExitCode -ne 0) {
        throw "Validation run failed with exit code $($result.ExitCode)."
    }

    $reasoning = Write-ValidationReasoningReport `
        -ReasoningPath $reasoningPath `
        -SourceFile $sourceFile `
        -TransformedFile $outputPath `
        -ConsoleSummaryPath $summaryPath `
        -ReportPath $reportPath `
        -ReportHtmlPath $reportHtmlPath `
        -OutputLines $result.Output

    Write-Host ""
    Write-Host "Validation Evidence" -ForegroundColor Cyan
    Write-Host "- Console summary : $summaryPath"
    Write-Host "- Markdown report : $reportPath"
    Write-Host "- HTML report     : $reportHtmlPath"
    Write-Host "- Reasoning report: $reasoningPath"
    Write-Host "- C++ fallback used: $($autoInvoke.UsedCppFallback)"

    if ($reasoning.SemanticEquivalence -eq "PASS") {
        Write-Host "- Semantic equivalence: PASS (original and transformed outputs match)" -ForegroundColor Green
        Write-Host "- Why PASS: --verify executed both programs and their outputs were exactly identical." -ForegroundColor Green
    } elseif ($reasoning.SemanticEquivalence -eq "FAIL") {
        Write-Host "- Semantic equivalence: FAIL (outputs differ)" -ForegroundColor Red
        Write-Host "- Why FAIL: --verify detected different output text between original and transformed runs." -ForegroundColor Red
    } else {
        Write-Host "- Semantic equivalence: UNKNOWN (could not parse result, see console summary)" -ForegroundColor Yellow
    }
}

function Get-FeatureOptions([bool]$includeRunsPrompt) {
    $verify = Read-YesNo "Run semantic verification" $true
    $showMap = Read-YesNo "Show rename map" $false
    $forceCpp = Read-YesNo "Force C++ input mode (--input-cpp)" $false
    $allowLlmCpp = Read-YesNo "Allow LLM for C++ conversion" $false
    $enableSub = Read-YesNo "Enable LLM substitution" $false
    $enableExplain = Read-YesNo "Enable LLM explanation" $false

    $runs = 1
    if ($includeRunsPrompt) {
        $runs = Read-PositiveInt "Number of runs" 1
    }

    $useSeed = Read-YesNo "Use fixed seed" $false
    $seedValue = "123456"
    $fixedSeedAcrossRuns = $false
    if ($useSeed) {
        $seedValue = Read-Default "Seed value" "123456"
        if ($runs -gt 1) {
            $fixedSeedAcrossRuns = Read-YesNo "Reuse same seed across all runs" $false
        }
    }

    $batchOutputDir = ""
    $cleanBatchOutputDir = $false
    if ($runs -gt 1) {
        $batchOutputDir = Read-Default "Batch output directory" "build/batch_runs"
        $cleanBatchOutputDir = Read-YesNo "Clean batch output directory before run" $true
    }

    return [PSCustomObject]@{
        Verify = $verify
        ShowMap = $showMap
        ForceCpp = $forceCpp
        AllowLlmCpp = $allowLlmCpp
        EnableSub = $enableSub
        EnableExplain = $enableExplain
        Runs = $runs
        UseSeed = $useSeed
        SeedValue = $seedValue
        FixedSeedAcrossRuns = $fixedSeedAcrossRuns
        BatchOutputDir = $batchOutputDir
        CleanBatchOutputDir = $cleanBatchOutputDir
    }
}

function Start-FullRun {
    Ensure-Binary

    $sourceFile = Select-SampleOrManual
    $runKey = ConvertTo-SafeName $sourceFile
    $runDir = Join-Path "build/full_run" $runKey
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null

    $outputPath = Join-Path $runDir "transformed.ir"
    $reportPath = Join-Path $runDir "summary.md"
    $reportHtmlPath = Join-Path $runDir "summary.html"
    $summaryPath = Join-Path $runDir "console_summary.txt"
    $reasoningPath = Join-Path $runDir "validation_reasoning.txt"
    $opts = Get-AutoBestProfile

    $args = New-Object 'System.Collections.Generic.List[string]'

    if (Is-CppInputFile $sourceFile) {
        $args.Add("--input-cpp") | Out-Null
    } else {
        $args.Add("--input") | Out-Null
    }
    $args.Add($sourceFile) | Out-Null

    $args.Add("--output") | Out-Null
    $args.Add($outputPath) | Out-Null
    $args.Add("--env") | Out-Null
    $args.Add($EnvPath) | Out-Null

    if ($opts.Verify) { $args.Add("--verify") | Out-Null }
    if ($opts.ShowMap) { $args.Add("--show-map") | Out-Null }
    if (-not $opts.LlmConfigured) { $args.Add("--no-llm-cpp") | Out-Null }
    if ($opts.EnableSub) { $args.Add("--llm-substitute") | Out-Null }
    if ($opts.EnableExplain) { $args.Add("--llm-explain") | Out-Null }
    $args.Add("--report") | Out-Null
    $args.Add($reportPath) | Out-Null
    $args.Add("--report-html") | Out-Null
    $args.Add($reportHtmlPath) | Out-Null
    if ($opts.UseSeed) {
        $args.Add("--seed") | Out-Null
        $args.Add("123456") | Out-Null
    }

    Write-Host ""
    Write-Host "Auto profile: verify=ON, llmConfigured=$($opts.LlmConfigured), cppAutoDetect=ON" -ForegroundColor DarkCyan
    Write-Host "Running: afis.exe $($args -join ' ')" -ForegroundColor Cyan
    $autoInvoke = Invoke-AfisAuto -Arguments $args.ToArray() -IsCppInput (Is-CppInputFile $sourceFile) -LlmConfigured $opts.LlmConfigured -ConsoleSummaryPath $summaryPath
    $result = $autoInvoke.Result
    if ($result.ExitCode -ne 0) {
        throw "Run failed with exit code $($result.ExitCode)."
    }

    $reasoning = Write-ValidationReasoningReport `
        -ReasoningPath $reasoningPath `
        -SourceFile $sourceFile `
        -TransformedFile $outputPath `
        -ConsoleSummaryPath $summaryPath `
        -ReportPath $reportPath `
        -ReportHtmlPath $reportHtmlPath `
        -OutputLines $result.Output

    Write-Host ""
    Write-Host "Run completed." -ForegroundColor Green
    Write-Host "Console summary: $summaryPath"
    Write-Host "Reasoning report: $reasoningPath"
    Write-Host "C++ fallback used: $($autoInvoke.UsedCppFallback)"
    Write-Host "Semantic equivalence: $($reasoning.SemanticEquivalence)"
}

function Start-RunAllSamples {
    Ensure-Binary

    $sampleFiles = Get-SampleFiles
    if ($sampleFiles.Count -eq 0) {
        throw "No sample files found in $samplesDir"
    }

    $rootDir = "build/all_samples"
    if (Test-Path $rootDir) {
        Remove-Item -Path $rootDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $rootDir -Force | Out-Null

    $opts = Get-AutoBestProfile
    $runsPerSample = Read-PositiveInt "Number of runs for each sample" 1

    Write-Host ""
    Write-Host "Running all samples..." -ForegroundColor Cyan
    Write-Host "Auto profile: verify=ON, llmConfigured=$($opts.LlmConfigured), llmSubstitute=$($opts.EnableSub), llmExplain=$($opts.EnableExplain), cppAutoDetect=ON, cppFallback=ON, runsPerSample=$runsPerSample" -ForegroundColor DarkCyan

    $results = New-Object 'System.Collections.Generic.List[object]'

    foreach ($sampleFile in $sampleFiles) {
        $sampleKey = ConvertTo-SafeName $sampleFile
        $sampleDir = Join-Path $rootDir $sampleKey
        New-Item -ItemType Directory -Path $sampleDir -Force | Out-Null

        for ($runIndex = 1; $runIndex -le $runsPerSample; $runIndex++) {
            $runDir = $sampleDir
            if ($runsPerSample -gt 1) {
                $runFolderName = ("run_{0:d3}" -f $runIndex)
                $runDir = Join-Path (Join-Path $sampleDir "runs") $runFolderName
                New-Item -ItemType Directory -Path $runDir -Force | Out-Null
            }

            $outputPath = Join-Path $runDir "transformed.ir"
            $reportPath = Join-Path $runDir "summary.md"
            $reportHtmlPath = Join-Path $runDir "summary.html"
            $consoleSummaryPath = Join-Path $runDir "console_summary.txt"
            $reasoningPath = Join-Path $runDir "validation_reasoning.txt"

            $args = New-Object 'System.Collections.Generic.List[string]'
            if (Is-CppInputFile $sampleFile) {
                $args.Add("--input-cpp") | Out-Null
            } else {
                $args.Add("--input") | Out-Null
            }
            $args.Add($sampleFile) | Out-Null

            $args.Add("--output") | Out-Null
            $args.Add($outputPath) | Out-Null
            $args.Add("--env") | Out-Null
            $args.Add($EnvPath) | Out-Null
            $args.Add("--report") | Out-Null
            $args.Add($reportPath) | Out-Null
            $args.Add("--report-html") | Out-Null
            $args.Add($reportHtmlPath) | Out-Null

            if ($opts.Verify) { $args.Add("--verify") | Out-Null }
            if ($opts.ShowMap) { $args.Add("--show-map") | Out-Null }
            if (-not $opts.LlmConfigured) { $args.Add("--no-llm-cpp") | Out-Null }
            if ($opts.EnableSub) { $args.Add("--llm-substitute") | Out-Null }
            if ($opts.EnableExplain) { $args.Add("--llm-explain") | Out-Null }
            if ($opts.UseSeed) {
                $args.Add("--seed") | Out-Null
                $args.Add("123456") | Out-Null
            }

            Write-Host ""
            Write-Host "Sample: $sampleFile (run $runIndex/$runsPerSample)" -ForegroundColor Yellow
            $autoInvoke = Invoke-AfisAuto -Arguments $args.ToArray() -IsCppInput (Is-CppInputFile $sampleFile) -LlmConfigured $opts.LlmConfigured -ConsoleSummaryPath $consoleSummaryPath
            $runResult = $autoInvoke.Result

            $reasoning = Write-ValidationReasoningReport `
                -ReasoningPath $reasoningPath `
                -SourceFile $sampleFile `
                -TransformedFile $outputPath `
                -ConsoleSummaryPath $consoleSummaryPath `
                -ReportPath $reportPath `
                -ReportHtmlPath $reportHtmlPath `
                -OutputLines $runResult.Output

            $status = if ($runResult.ExitCode -eq 0 -and $reasoning.SemanticEquivalence -eq "PASS") { "PASS" } else { "FAIL" }
            $results.Add([PSCustomObject]@{
                Sample = $sampleFile
                Run = $runIndex
                Status = $status
                Semantic = $reasoning.SemanticEquivalence
                Folder = $runDir
                Reasoning = $reasoningPath
                UsedCppFallback = $autoInvoke.UsedCppFallback
                ExitCode = $runResult.ExitCode
            }) | Out-Null
        }
    }

    $passCount = ($results | Where-Object { $_.Status -eq "PASS" }).Count
    $total = $results.Count

    Write-Host ""
    Write-Host "Run-all summary: $passCount/$total passed" -ForegroundColor Cyan
    foreach ($item in $results) {
        Write-Host ("[{0}] {1} run {2} (semantic={3})" -f $item.Status, $item.Sample, $item.Run, $item.Semantic)
        Write-Host ("  Folder: {0}" -f $item.Folder)
        Write-Host ("  Reasoning: {0}" -f $item.Reasoning)
        Write-Host ("  C++ fallback used: {0}" -f $item.UsedCppFallback)
    }

    $combinedSummary = Join-Path $rootDir "_run_all_summary.txt"
    $results | Format-Table -AutoSize | Out-String | Out-File -FilePath $combinedSummary -Encoding utf8

    Write-Host ""
    Write-Host "Per-sample folder artifacts:" -ForegroundColor Green
    Write-Host "  transformed.ir"
    Write-Host "  summary.md"
    Write-Host "  summary.html"
    Write-Host "  console_summary.txt"
    Write-Host "  validation_reasoning.txt"
    if ($runsPerSample -gt 1) {
        Write-Host "  runs/run_XXX/ (one folder per run)"
    }
    Write-Host "Combined summary: $combinedSummary"
}

while ($true) {
    Write-Host ""
    Write-Host "================ AFIS Unified CLI ================" -ForegroundColor Cyan
    Write-Host "1) Run one file (AUTO best mode)"
    Write-Host "2) Run ALL sample files (AUTO best mode)"
    Write-Host "3) Validation proof run (AUTO + reasoning)"
    Write-Host "4) List sample files"
    Write-Host "5) Clean + rebuild"
    Write-Host "6) Exit"

    $choice = Read-Host "Choose option"

    switch ($choice.Trim()) {
        "1" {
            Start-FullRun
        }
        "2" {
            Start-RunAllSamples
        }
        "3" {
            Start-ValidationRun
        }
        "4" {
            Show-SampleFiles
        }
        "5" {
            & $buildScript -Clean -CompilerBinPath $CompilerBinPath
            Write-Host "Clean build complete." -ForegroundColor Green
        }
        "6" {
            Write-Host "Exiting CLI."
            return
        }
        default {
            Write-Host "Invalid choice. Pick 1-6." -ForegroundColor Yellow
        }
    }
}
