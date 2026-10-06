#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of build.sh: configure, build, test or install Gygax.

Gygax's C++ core is POSIX-only and is not ported to Win32 (see docs/WINDOWS.md).
This script runs the real build.sh unmodified inside the `gygax-dev` Docker
image (Docker Desktop, WSL2 backend), bind-mounting this checkout so build
output and any BUILD_DIR you set land back on the Windows side.

.EXAMPLE
    .\build.ps1                # build (default)
    .\build.ps1 test           # build and run ctest
    .\build.ps1 clean
    $env:BUILD_DIR = "build/win"; .\build.ps1 build
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'scripts\DevContainer.ps1')

$envVars = @{}
if ($env:BUILD_DIR) { $envVars['BUILD_DIR'] = $env:BUILD_DIR }
if ($env:BUILD_TYPE) { $envVars['BUILD_TYPE'] = $env:BUILD_TYPE }

Invoke-GygaxDevContainer -Command (@('./build.sh') + $RestArgs) -EnvVars $envVars
