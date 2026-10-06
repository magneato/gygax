#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of assemble.sh: assemble and run a 6502/6809 program
    in the CoCo-style emulator, via the gygax-dev Docker image.

.EXAMPLE
    .\assemble.ps1 examples\coco\hello.asm -o out.dsk --cpu 6809
    .\assemble.ps1 examples\coco\hello.asm -o out.dsk --debug
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'scripts\DevContainer.ps1')

$interactive = $RestArgs -contains '--debug'
Invoke-GygaxDevContainer -Command (@('./assemble.sh') + $RestArgs) -Interactive:$interactive
