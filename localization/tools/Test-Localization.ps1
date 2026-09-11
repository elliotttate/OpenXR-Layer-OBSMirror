[CmdletBinding()]
param(
    [string]$RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolved here rather than in the parameter default: with [CmdletBinding()],
# Windows PowerShell 5.1 evaluates parameter defaults before $PSScriptRoot is set,
# which made the documented "powershell -File <script>" invocation fail.
if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }

# Compile-free consistency checks:
#   1. translation table: unique resource names, non-empty keys, coverage;
#   2. resw files match the table in both directions and carry exactly the
#      table's values (so a value cannot drift while the names still match);
#   3. every x:Uid in XAML exists in the table;
#   4. every Loc.S / Loc.F key in C# exists in the table, and every "code"
#      resource is actually referenced by some C# file;
#   5. OBS plugin locale files expose exactly the same keys as en-US.ini.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 parses scripts as ANSI.
# All data files are read as UTF-8 explicitly.

. (Join-Path $PSScriptRoot 'TranslationTable.ps1')

$problems = [System.Collections.Generic.List[string]]::new()
$tablePath = Join-Path $RepoRoot 'localization\control-center-strings.tsv'
$stringsRoot = Join-Path $RepoRoot 'ControlCenter\Strings'
$controlCenterRoot = Join-Path $RepoRoot 'ControlCenter'
$pluginLocaleRoot = Join-Path $RepoRoot 'OBSPlugin\win-openxr\data\locale'

# The main table plus every fragment in localization/fragments/*.tsv are merged.
$tablePaths = @($tablePath)
$fragmentRoot = Join-Path $RepoRoot 'localization\fragments'
if (Test-Path -LiteralPath $fragmentRoot -PathType Container) {
    $tablePaths += @(Get-ChildItem -LiteralPath $fragmentRoot -File -Filter *.tsv | ForEach-Object FullName)
}

$rows = @()
foreach ($path in $tablePaths) {
    $rows += @(Import-TranslationTable -Path $path)
}

# --- 1. translation table ---
foreach ($row in $rows) {
    if ([string]::IsNullOrWhiteSpace($row.key)) {
        $problems.Add('A row has an empty key.')
    }
}

$duplicateRows = @($rows | Group-Object { Get-TranslationResourceName -Row $_ } | Where-Object { $_.Count -gt 1 })
foreach ($duplicate in $duplicateRows) {
    $problems.Add("Duplicate resource name '$($duplicate.Name)' appears $($duplicate.Count) times in the table.")
}

$tableNames = [System.Collections.Generic.HashSet[string]]::new()
$knownKeys = [System.Collections.Generic.HashSet[string]]::new()
$rowByName = @{}
foreach ($row in $rows) {
    $name = Get-TranslationResourceName -Row $row
    [void]$tableNames.Add($name)
    [void]$knownKeys.Add($row.key)
    $rowByName[$name] = $row
}

$untranslated = @($rows | Where-Object { [string]::IsNullOrWhiteSpace($_.'zh-CN') })
Write-Host "Translation table: $($rows.Count) rows, $($untranslated.Count) still falling back to English."

# --- 2. resw ---
$languageKeys = @{}
foreach ($languageDirectory in (Get-ChildItem -LiteralPath $stringsRoot -Directory)) {
    $language = $languageDirectory.Name
    $reswPath = Join-Path $languageDirectory.FullName 'Resources.resw'
    if (-not (Test-Path -LiteralPath $reswPath -PathType Leaf)) {
        $problems.Add("Missing $reswPath")
        continue
    }

    $sourceColumn = 'english'
    if ($language -ne 'en-US') { $sourceColumn = $language }
    $hasColumn = $rows.Count -gt 0 -and ($rows[0].PSObject.Properties.Name -contains $sourceColumn)
    if (-not $hasColumn) {
        $problems.Add("Translation table has no column '$sourceColumn' for language directory '$language'.")
    }

    [xml]$document = Get-Content -LiteralPath $reswPath -Raw -Encoding utf8
    $names = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($data in $document.root.data) {
        $name = [string]$data.name
        if (-not $names.Add($name)) {
            $problems.Add("$reswPath contains duplicate resource '$name'.")
            continue
        }

        # The resw files are generated but checked in, so they are compared with
        # the table value by value: matching names alone would let an outdated
        # string ship (a value edited in the table but never regenerated).
        if ($hasColumn -and $rowByName.ContainsKey($name)) {
            $expected = Get-TranslationValue -Row $rowByName[$name] -Language $language
            $actual = [string]$data.value
            if ($actual -cne $expected) {
                $problems.Add("$reswPath value for '$name' does not match the translation table (table: [$expected], resw: [$actual]).")
            }
        }
    }
    $languageKeys[$language] = $names

    foreach ($missing in ($tableNames | Where-Object { -not $names.Contains($_) })) {
        $problems.Add("$reswPath is missing '$missing' from the translation table.")
    }
    foreach ($stale in ($names | Where-Object { -not $tableNames.Contains($_) })) {
        $problems.Add("$reswPath contains '$stale', which is no longer in the translation table.")
    }
}

$referenceLanguage = 'en-US'
if (-not $languageKeys.ContainsKey($referenceLanguage)) {
    $problems.Add("Reference language '$referenceLanguage' has no resources.")
}
else {
    foreach ($language in $languageKeys.Keys) {
        if ($language -eq $referenceLanguage) { continue }
        foreach ($name in ($languageKeys[$language] | Where-Object { -not $languageKeys[$referenceLanguage].Contains($_) })) {
            $problems.Add("$language has '$name' but $referenceLanguage does not.")
        }
        foreach ($name in ($languageKeys[$referenceLanguage] | Where-Object { -not $languageKeys[$language].Contains($_) })) {
            $problems.Add("$referenceLanguage has '$name' but $language does not.")
        }
    }
}

# --- 3. XAML x:Uid ---
foreach ($xamlFile in (Get-ChildItem -LiteralPath $controlCenterRoot -Recurse -File -Filter *.xaml | Where-Object { $_.FullName -notmatch '\\(obj|bin)\\' })) {
    $content = Get-Content -LiteralPath $xamlFile.FullName -Raw -Encoding utf8
    foreach ($match in [regex]::Matches($content, 'x:Uid="(?<key>[^"]+)"')) {
        $key = $match.Groups['key'].Value
        if (-not $knownKeys.Contains($key)) {
            $problems.Add("$($xamlFile.Name) uses x:Uid=`"$key`" which is not in the translation table.")
        }
    }
}

# --- 4. C# Loc calls ---
# Loc takes a *resource name*, not an x:Uid key. A XAML-derived key therefore
# has to carry its property suffix (Foo.Text), otherwise the lookup misses and
# the UI silently falls back to English. Whitespace (a line break included) is
# allowed between the parenthesis and the key so a wrapped call is still checked.
$csharpFiles = @(Get-ChildItem -LiteralPath $controlCenterRoot -Recurse -File -Filter *.cs | Where-Object { $_.FullName -notmatch '\\(obj|bin)\\' })
$csharpText = ''
foreach ($sourceFile in $csharpFiles) {
    $content = Get-Content -LiteralPath $sourceFile.FullName -Raw -Encoding utf8
    $csharpText += $content
    foreach ($match in [regex]::Matches($content, 'Loc\.(?:S|F)\(\s*"(?<key>[^"]+)"')) {
        $key = $match.Groups['key'].Value
        if (-not $tableNames.Contains($key)) {
            $hint = ''
            if ($knownKeys.Contains($key)) {
                $hint = " ('$key' is an x:Uid key; use its resource name with the property suffix, e.g. $key.Text)"
            }
            $problems.Add("$($sourceFile.Name) calls Loc with '$key' which is not a resource name in the translation table.$hint")
        }
    }
}

# --- 4b. code resources are actually referenced ---
# A "code" resource exists only to be looked up from C#. One that no C# file
# mentions is either a leftover row or a missed localization: the English string
# is still hard-coded at the call site while its translation goes unused.
$codeRows = @($rows | Where-Object { ($_.prop -replace '[#*]\d*$', '') -eq 'code' })
foreach ($row in $codeRows) {
    if (-not $csharpText.Contains($row.key)) {
        $problems.Add("Resource '$($row.key)' is a code resource, but no C# file references it.")
    }
}

# --- 5. OBS plugin locale files ---
# The parser is strict on purpose: a duplicate assignment is reported instead of
# being collapsed by the HashSet, which would hide a malformed locale file.
function Get-IniKeys {
    param([string]$Path)
    $keys = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in (Get-Content -LiteralPath $Path -Encoding utf8)) {
        if ($line -match '^\s*(?<key>[A-Za-z0-9_]+)\s*=') {
            if (-not $keys.Add($Matches['key'])) {
                $problems.Add("$(Split-Path -Path $Path -Leaf) defines '$($Matches['key'])' more than once.")
            }
        }
    }
    return $keys
}

$pluginFiles = @(Get-ChildItem -LiteralPath $pluginLocaleRoot -File -Filter *.ini)
# Without the English baseline every comparison below would be skipped, so a
# missing en-US.ini has to fail loudly rather than pass as "nothing to compare".
if (-not ($pluginFiles | Where-Object { $_.BaseName -eq 'en-US' })) {
    $problems.Add("$pluginLocaleRoot has no en-US.ini, so the other locale files cannot be compared.")
}
$englishKeys = $null
foreach ($file in $pluginFiles) {
    $keys = Get-IniKeys -Path $file.FullName
    if ($file.BaseName -eq 'en-US') { $englishKeys = $keys; continue }
    if ($null -eq $englishKeys) { continue }
    foreach ($key in $keys) {
        if (-not $englishKeys.Contains($key)) { $problems.Add("$($file.Name) defines '$key' which en-US.ini does not.") }
    }
    foreach ($key in $englishKeys) {
        if (-not $keys.Contains($key)) { $problems.Add("$($file.Name) is missing key '$key' from en-US.ini.") }
    }
}
Write-Host "Plugin locale files checked: $(($pluginFiles | ForEach-Object Name) -join ', ')"

# --- 6. x:Uid property applicability ---
# WinUI applies EVERY resource named "<Uid>.<Property>" to EVERY element that
# carries x:Uid="<Uid>". If one of those elements has no such property (a
# TextBlock receiving ".Content", for example) the app throws
# XamlParseException while loading the XAML and never shows a window.
# So each localized property must be an attribute present on every element that
# carries the same x:Uid.
$propertiesByUid = @{}
foreach ($row in $rows) {
    if ($row.prop -eq 'code') { continue }
    $property = $row.prop -replace '[#*]\d*$', ''
    if (-not $propertiesByUid.ContainsKey($row.key)) {
        $propertiesByUid[$row.key] = [System.Collections.Generic.HashSet[string]]::new()
    }
    [void]$propertiesByUid[$row.key].Add($property)
}

$uidElementCount = @{}
foreach ($xamlFile in (Get-ChildItem -LiteralPath $controlCenterRoot -Recurse -File -Filter *.xaml | Where-Object { $_.FullName -notmatch '\\(obj|bin)\\' })) {
    $content = Get-Content -LiteralPath $xamlFile.FullName -Raw -Encoding utf8
    foreach ($tagMatch in [regex]::Matches($content, '<[A-Za-z][^<>]*>')) {
        $tag = $tagMatch.Value
        $uidMatch = [regex]::Match($tag, 'x:Uid="(?<uid>[^"]+)"')
        if (-not $uidMatch.Success) { continue }
        $uid = $uidMatch.Groups['uid'].Value

        if ($uidElementCount.ContainsKey($uid)) { $uidElementCount[$uid]++ } else { $uidElementCount[$uid] = 1 }

        if (-not $propertiesByUid.ContainsKey($uid)) { continue }
        $attributes = [System.Collections.Generic.HashSet[string]]::new()
        foreach ($attributeMatch in [regex]::Matches($tag, '(?<name>[A-Za-z][A-Za-z0-9.]*)="')) {
            [void]$attributes.Add($attributeMatch.Groups['name'].Value)
        }
        foreach ($property in $propertiesByUid[$uid]) {
            if (-not $attributes.Contains($property)) {
                $problems.Add("$($xamlFile.Name): x:Uid=`"$uid`" owns resource '$uid.$property' but this element has no '$property' attribute, so WinUI throws XamlParseException at load time.")
            }
        }
    }
}

# An x:Uid shared by several elements is only safe when it owns exactly one
# property; otherwise every element must support all of them.
foreach ($uid in $uidElementCount.Keys) {
    if ($uidElementCount[$uid] -le 1) { continue }
    if (-not $propertiesByUid.ContainsKey($uid)) { continue }
    if ($propertiesByUid[$uid].Count -gt 1) {
        $problems.Add("x:Uid=`"$uid`" is used by $($uidElementCount[$uid]) elements and owns $($propertiesByUid[$uid].Count) properties ($(($propertiesByUid[$uid]) -join ', ')); split it into one Uid per element.")
    }
}

# --- result ---
if ($problems.Count -gt 0) {
    Write-Host ''
    Write-Host "Localization check FAILED with $($problems.Count) problem(s):" -ForegroundColor Red
    foreach ($problem in $problems) { Write-Host "  - $problem" -ForegroundColor Red }
    exit 1
}

Write-Host ''
Write-Host 'Localization check passed.' -ForegroundColor Green
