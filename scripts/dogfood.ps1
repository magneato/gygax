#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of scripts\dogfood.sh: clean build, ctest, live
    daemon smoke test, Python bindings, retro toolchain, and (without --fast)
    the ASan/UBSan/TSan builds, all inside the gygax-dev Docker image.

.EXAMPLE
    .\scripts\dogfood.ps1 --fast
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'DevContainer.ps1')

Invoke-GygaxDevContainer -Command (@('./scripts/dogfood.sh') + $RestArgs)
