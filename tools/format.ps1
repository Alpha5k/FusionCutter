[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$required_major = 22
$project_root = Split-Path -Parent $PSScriptRoot

function Find-ClangFormat {
    if ($ClangFormat) {
        if (-not (Test-Path -LiteralPath $ClangFormat -PathType Leaf)) {
            throw "The requested clang-format executable does not exist: $ClangFormat"
        }

        return (Resolve-Path -LiteralPath $ClangFormat).Path
    }

    $path_command = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($path_command) {
        return $path_command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $visual_studio_matches = @(
            & $vswhere -latest -products * -find "VC\Tools\Llvm\**\bin\clang-format.exe"
        )

        $native_match = $visual_studio_matches | Where-Object { $_ -match "[\\/]x64[\\/]bin[\\/]clang-format\.exe$" } |
            Select-Object -First 1
        if ($native_match) {
            return $native_match
        }

        if ($visual_studio_matches.Count -gt 0) {
            return $visual_studio_matches[0]
        }
    }

    throw "clang-format was not found. Install major version $required_major or pass -ClangFormat <path>."
}

function Get-ProjectSourceFiles {
    $source_roots = @("include", "src", "tests", "tools")
    $source_extensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inl")
    $files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()

    foreach ($source_root in $source_roots) {
        $absolute_root = Join-Path $project_root $source_root
        if (-not (Test-Path -LiteralPath $absolute_root -PathType Container)) {
            continue
        }

        foreach ($file in Get-ChildItem -LiteralPath $absolute_root -Recurse -File) {
            if ($source_extensions -contains $file.Extension.ToLowerInvariant()) {
                $files.Add($file)
            }
        }
    }

    return @($files | Sort-Object FullName -Unique)
}

$clang_format = Find-ClangFormat
$version_text = & $clang_format --version
if ($LASTEXITCODE -ne 0 -or $version_text -notmatch "version\s+(\d+)\.") {
    throw "Unable to determine the clang-format version from: $clang_format"
}

$actual_major = [int]$Matches[1]
if ($actual_major -ne $required_major) {
    throw "Fusion Cutter requires clang-format major version $required_major; found $actual_major at $clang_format."
}

$source_files = @(Get-ProjectSourceFiles)
if ($source_files.Count -eq 0) {
    Write-Host "No project-owned C/C++ files were found."
    exit 0
}

$format_arguments = if ($Check) {
    @("--dry-run", "--Werror", "--style=file")
} else {
    @("-i", "--style=file")
}

$failed_files = [System.Collections.Generic.List[string]]::new()
foreach ($source_file in $source_files) {
    $saved_error_action = $ErrorActionPreference
    $ErrorActionPreference = "Continue"

    $format_output = & $clang_format @format_arguments $source_file.FullName 2>&1

    $format_exit_code = $LASTEXITCODE
    $ErrorActionPreference = $saved_error_action

    if ($format_output) {
        $format_output | ForEach-Object { Write-Host $_ }
    }

    if ($format_exit_code -ne 0) {
        $failed_files.Add($source_file.FullName)
    }
}

if ($failed_files.Count -gt 0) {
    Write-Host "clang-format failed for $($failed_files.Count) file(s)." -ForegroundColor Red
    exit 1
}

$mode = if ($Check) { "checked" } else { "formatted" }
Write-Host "$($source_files.Count) project-owned C/C++ file(s) $mode with clang-format $actual_major."
