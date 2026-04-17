param(
    [string]$CompilerBinPath = "C:\msys64\ucrt64\bin",
    [string]$EnvPath = ".env"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$demoScript = Join-Path $PSScriptRoot "demo.ps1"
$exePath = Join-Path $projectRoot "build/afis.exe"

function Start-BuildIfMissing {
    if (-not (Test-Path $exePath)) {
        Write-Host "Executable not found. Building first..."
        & $buildScript -CompilerBinPath $CompilerBinPath
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed."
        }
    }
}

function Read-DefaultValue([string]$prompt, [string]$defaultValue) {
    $value = Read-Host "$prompt [$defaultValue]"
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $defaultValue
    }
    return $value.Trim()
}

function Read-YesNoChoice([string]$prompt, [bool]$defaultYes) {
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

function Start-GuidedRun {
    Start-BuildIfMissing

    $sourceFile = Read-DefaultValue "Source file (.ir or .cpp)" "samples/example_full.ir"
    $outputPath = Read-DefaultValue "Output IR path" "build/manual_out.ir"

    $verify = Read-YesNoChoice "Run semantic verification" $true
    $showMap = Read-YesNoChoice "Show rename map" $false
    $useSeed = Read-YesNoChoice "Use fixed seed" $false
    $enableSub = Read-YesNoChoice "Enable LLM substitution" $false
    $enableExplain = Read-YesNoChoice "Enable LLM explanation" $false

    $optionList = New-Object 'System.Collections.Generic.List[string]'
    $optionList.Add("--input") | Out-Null
    $optionList.Add($sourceFile) | Out-Null
    $optionList.Add("--output") | Out-Null
    $optionList.Add($outputPath) | Out-Null
    $optionList.Add("--env") | Out-Null
    $optionList.Add($EnvPath) | Out-Null

    if ($verify) { $optionList.Add("--verify") | Out-Null }
    if ($showMap) { $optionList.Add("--show-map") | Out-Null }
    if ($enableSub) { $optionList.Add("--llm-substitute") | Out-Null }

    if ($enableExplain) {
        $optionList.Add("--llm-explain") | Out-Null
        $report = Read-DefaultValue "Markdown report path" "build/manual_report.md"
        $htmlReport = Read-DefaultValue "HTML report path" "build/manual_report.html"
        $optionList.Add("--report") | Out-Null
        $optionList.Add($report) | Out-Null
        $optionList.Add("--report-html") | Out-Null
        $optionList.Add($htmlReport) | Out-Null
    }

    if ($useSeed) {
        $seed = Read-DefaultValue "Seed value" "123456"
        $optionList.Add("--seed") | Out-Null
        $optionList.Add($seed) | Out-Null
    }

    Write-Host ""
    Write-Host "Running: afis.exe $($optionList -join ' ')" -ForegroundColor Cyan
    & $exePath $optionList.ToArray()
    Write-Host ""
}

while ($true) {
    Write-Host ""
    Write-Host "================ AFIS Project Menu ================" -ForegroundColor Cyan
    Write-Host "1) Clean build"
    Write-Host "2) Demo run (IR)"
    Write-Host "3) Single run (guided)"
    Write-Host "4) Binary interactive mode"
    Write-Host "5) Exit"

    $choice = Read-Host "Choose option"

    switch ($choice.Trim()) {
        "1" {
            & $buildScript -Clean -CompilerBinPath $CompilerBinPath
            Write-Host "Clean build complete." -ForegroundColor Green
        }
        "2" {
            $demoFile = Read-DefaultValue "Demo source" "samples/example_full.ir"
            $runs = Read-DefaultValue "Number of runs" "3"
            & $demoScript -InputFile $demoFile -Runs ([int]$runs) -CleanRunDir -CompilerBinPath $CompilerBinPath
        }
        "3" {
            Start-GuidedRun
        }
        "4" {
            Start-BuildIfMissing
            & $exePath --interactive --env $EnvPath
        }
        "5" {
            Write-Host "Exiting menu."
            return
        }
        default {
            Write-Host "Invalid choice. Pick 1-5." -ForegroundColor Yellow
        }
    }
}
