param(
    [string]$OutputName = "afis.exe",
    [string]$Compiler = "",
    [string]$CompilerBinPath = "C:\msys64\ucrt64\bin",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"
$srcDir = Join-Path $projectRoot "src"

if ($CompilerBinPath) {
    if (Test-Path -Path $CompilerBinPath -PathType Container) {
        $pathSep = [System.IO.Path]::PathSeparator
        $pathItems = @($env:Path -split [regex]::Escape($pathSep))
        if ($pathItems -notcontains $CompilerBinPath) {
            $env:Path = $CompilerBinPath + $pathSep + $env:Path
        }
        Write-Host "Compiler bin path prefixed: $CompilerBinPath"
    } else {
        Write-Warning "CompilerBinPath not found: $CompilerBinPath. Falling back to system PATH."
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Path $buildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

if ($Compiler) {
    if (Test-Path -Path $Compiler -PathType Leaf) {
        $compilerPath = (Resolve-Path $Compiler).Path
        $compilerCommand = $null
    } else {
        $compilerMatch = Get-Command $Compiler -ErrorAction SilentlyContinue
        if (-not $compilerMatch) {
            Write-Error "Requested compiler not found: $Compiler"
            exit 1
        }
        $compilerCommand = @($compilerMatch)[0]
        $compilerPath = ""
    }
} else {
    $compilerCommand = @(Get-Command g++.exe -ErrorAction SilentlyContinue)[0]
    if (-not $compilerCommand) {
        $compilerCommand = @(Get-Command g++ -ErrorAction SilentlyContinue)[0]
    }
    if (-not $compilerCommand) {
        $compilerCommand = @(Get-Command clang++.exe -ErrorAction SilentlyContinue)[0]
    }
    if (-not $compilerCommand) {
        $compilerCommand = @(Get-Command clang++ -ErrorAction SilentlyContinue)[0]
    }

    $compilerPath = ""
}

if (-not $compilerCommand -and -not $compilerPath) {
    Write-Error "No C++ compiler found. Install g++ (MinGW/MSYS2) or clang++."
    exit 1
}

if (-not $compilerPath -and $compilerCommand) {
    if ($compilerCommand.PSObject.Properties["Path"]) {
        $compilerPath = $compilerCommand.Path
    } elseif ($compilerCommand.PSObject.Properties["Source"]) {
        $compilerPath = $compilerCommand.Source
    } elseif ($compilerCommand.PSObject.Properties["Definition"]) {
        $compilerPath = $compilerCommand.Definition
    }
}

if (-not $compilerPath) {
    Write-Error "Could not resolve compiler executable path."
    exit 1
}

$sourceFiles = Get-ChildItem -Path $srcDir -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName }
if (-not $sourceFiles) {
    Write-Error "No source files found in $srcDir"
    exit 1
}

$outputPath = Join-Path $buildDir $OutputName
$flags = @("-std=c++17", "-O2", "-Wall", "-Wextra", "-pedantic")

Write-Host "Compiling with $compilerPath"
Write-Host "Source files: $($sourceFiles.Count)"

& $compilerPath @flags @sourceFiles "-o" $outputPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "Compilation failed"
    exit $LASTEXITCODE
}

Write-Host "Build successful: $outputPath"
