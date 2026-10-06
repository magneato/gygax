#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of scripts\lint.sh: clang-format check, clang-tidy
    and cppcheck, via the gygax-dev Docker image. Needs a build first (for
    compile_commands.json); .\build.ps1 writes it to build/dev by default.

.EXAMPLE
    .\scripts\lint.ps1 all
    .\scripts\lint.ps1 format
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'DevContainer.ps1')

$envVars = @{}
if ($env:BUILD_DIR) { $envVars['BUILD_DIR'] = $env:BUILD_DIR }

Invoke-GygaxDevContainer -Command (@('./scripts/lint.sh') + $RestArgs) -EnvVars $envVars
