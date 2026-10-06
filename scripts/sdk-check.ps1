#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of scripts\sdk-check.sh: installs the built package
    and builds examples/plugin-consumer against it, via the gygax-dev image.
    Mainly used by ctest (the sdk_consumer test); paths are relative to /src.

.EXAMPLE
    .\scripts\sdk-check.ps1 build/dev build/dev/gygax
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'DevContainer.ps1')

Invoke-GygaxDevContainer -Command (@('./scripts/sdk-check.sh') + $RestArgs)
