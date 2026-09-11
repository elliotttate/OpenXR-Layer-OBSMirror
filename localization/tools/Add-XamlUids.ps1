[CmdletBinding()]
param(
    [string]$TablePath,
    [string]$ControlCenterRoot,
    [switch]$Reset
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolved here rather than in parameter defaults: with [CmdletBinding()],
# Windows PowerShell 5.1 evaluates defaults before $PSScriptRoot is set, which
# made the documented "powershell -File <script>" invocation fail.
if (-not $TablePath) { $TablePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'control-center-strings.tsv' }
if (-not $ControlCenterRoot) { $ControlCenterRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'ControlCenter' }

# Adds x:Uid="key" to XAML elements based on the translation table.
# The English literal stays on the element, so a missing language pack falls
# back to English instead of rendering an empty control.
#
# prop syntax:
#   Text     exactly one match required
#   Text#2   take the second match
#   Text*    tag every match with the same x:Uid
#
# An element may carry only ONE x:Uid, which is why every localized property of
# the same element must share the same key base: the resources for that element
# are "<key>.<property>". The script therefore looks at the enclosing tag and
# reuses an x:Uid that is already there, and fails if that x:Uid is a different
# key. -Reset removes previously injected x:Uid="Ui_*" attributes first.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 parses scripts as ANSI,
# and non-ASCII comments can swallow the following line break.

. (Join-Path $PSScriptRoot 'TranslationTable.ps1')

$rows = Import-TranslationTable -Path $TablePath
$targetsByFile = @{}
foreach ($group in ($rows | Where-Object { $_.target -like '*.xaml' } | Group-Object target)) {
    $targetsByFile[$group.Name] = $group
}

function Get-FileEncoding {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return [System.Text.UTF8Encoding]::new($true)
    }
    return [System.Text.UTF8Encoding]::new($false)
}

# Finds the start index of the element tag that contains the given index.
function Get-TagBounds {
    param([string]$Text, [int]$Index)
    $tagStart = $Text.LastIndexOf('<', $Index)
    if ($tagStart -lt 0) { return $null }
    $tagEnd = $Text.IndexOf('>', $Index)
    if ($tagEnd -lt 0) { return $null }
    return @{ Start = $tagStart; End = $tagEnd }
}

$failures = [System.Collections.Generic.List[string]]::new()
$injected = 0
$reused = 0
$resetCount = 0

foreach ($fileName in $targetsByFile.Keys) {
    $file = Join-Path $ControlCenterRoot $fileName
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        $failures.Add("XAML file not found: $file")
        continue
    }

    $encoding = Get-FileEncoding -Path $file
    $text = [System.IO.File]::ReadAllText($file)
    $original = $text

    if ($Reset) {
        # Only this project's own keys are removed, and only when they sit in
        # front of an attribute (the exact shape this script injects).
        $text = [regex]::Replace($text, 'x:Uid="Ui_[^"]*" (?=[A-Za-z])', '')
        $resetCount += ([regex]::Matches($original, 'x:Uid="Ui_[^"]*" (?=[A-Za-z])')).Count
        [System.IO.File]::WriteAllText($file, $text, $encoding)
        Write-Host "Reset $fileName"
        continue
    }

    foreach ($row in $targetsByFile[$fileName].Group) {
        $key = $row.key
        $property = $row.prop
        $all = $false
        $occurrence = 1

        if ($property -match '^(?<name>[A-Za-z.]+)\*$') {
            $property = $Matches['name']
            $all = $true
        }
        elseif ($property -match '^(?<name>[A-Za-z.]+)#(?<index>\d+)$') {
            $property = $Matches['name']
            $occurrence = [int]$Matches['index']
        }

        $literal = [System.Security.SecurityElement]::Escape([string]$row.english)
        $attribute = "$property=`"$literal`""
        $pattern = "(?<![\w.])$([regex]::Escape($attribute))"
        $found = [regex]::Matches($text, $pattern)

        if ($found.Count -eq 0) {
            $failures.Add("$fileName`: no match for $attribute (key $key)")
            continue
        }

        if (-not $all) {
            if ($found.Count -gt 1 -and $row.prop -notmatch '#\d+$') {
                $failures.Add("$fileName`: $attribute matches $($found.Count) places; disambiguate with $property#$occurrence (key $key)")
                continue
            }
            if ($occurrence -gt $found.Count) {
                $failures.Add("$fileName`: asked for occurrence $occurrence of $attribute but only $($found.Count) exist (key $key)")
                continue
            }
            $selected = @($found[$occurrence - 1])
        }
        else {
            $selected = @($found)
        }

        $injectedHere = 0
        for ($index = $selected.Count - 1; $index -ge 0; $index--) {
            $match = $selected[$index]
            $bounds = Get-TagBounds -Text $text -Index $match.Index
            if ($null -eq $bounds) {
                $failures.Add("$fileName`: cannot locate the element tag for $attribute (key $key)")
                continue
            }

            $tag = $text.Substring($bounds.Start, $bounds.End - $bounds.Start + 1)
            $existing = [regex]::Match($tag, 'x:Uid="(?<key>[^"]+)"')
            if ($existing.Success) {
                if ($existing.Groups['key'].Value -ne $key) {
                    $failures.Add("$fileName`: element already carries x:Uid=`"$($existing.Groups['key'].Value)`" but key $key also targets it; move $property onto the same key base")
                }
                else {
                    $reused++
                }
                continue
            }

            $text = $text.Remove($match.Index, $match.Length).Insert($match.Index, "x:Uid=`"$key`" $($match.Value)")
            $injectedHere++
        }

        $injected += $injectedHere
    }

    if ($text -ne $original) {
        [System.IO.File]::WriteAllText($file, $text, $encoding)
        Write-Host "Updated $fileName"
    }
}

# Failure reporting comes first: -Reset used to return before this block, so a
# missing XAML file (recorded above) was never reported and the script exited 0.
if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'The following rows need attention:' -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  - $failure" -ForegroundColor Red }
    throw "$($failures.Count) localization row(s) could not be applied."
}

if ($Reset) {
    Write-Host "Removed $resetCount injected x:Uid attribute(s)."
    return
}

Write-Host "Injected $injected x:Uid attribute(s); reused $reused existing element attribute(s)."
