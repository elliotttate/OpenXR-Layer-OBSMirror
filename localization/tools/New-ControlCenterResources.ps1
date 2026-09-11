[CmdletBinding()]
param(
    [string]$TablePath,
    [string[]]$Languages = @('en-US', 'zh-CN'),
    [string]$StringsRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolved here rather than in parameter defaults: with [CmdletBinding()],
# Windows PowerShell 5.1 evaluates defaults before $PSScriptRoot is set, which
# made the documented "powershell -File <script>" invocation fail.
if (-not $TablePath) { $TablePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'control-center-strings.tsv' }
if (-not $StringsRoot) { $StringsRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'ControlCenter\Strings' }

# Compiles the translation table into ControlCenter/Strings/<lang>/Resources.resw.
# Resource names are "<key>.<property>" for XAML rows and "<key>" for code rows.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 parses scripts as ANSI,
# and non-ASCII comments can swallow the following line break.

. (Join-Path $PSScriptRoot 'TranslationTable.ps1')

if (-not (Test-Path -LiteralPath $TablePath -PathType Leaf)) {
    throw "Translation table not found: $TablePath"
}

# The main table plus every fragment in localization/fragments/*.tsv are merged,
# so several people (or agents) can add keys without touching a shared file.
$tablePaths = @($TablePath)
$fragmentRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'fragments'
if (Test-Path -LiteralPath $fragmentRoot -PathType Container) {
    $tablePaths += @(Get-ChildItem -LiteralPath $fragmentRoot -File -Filter *.tsv | ForEach-Object FullName)
}

$rows = @()
foreach ($path in $tablePaths) {
    $rows += @(Import-TranslationTable -Path $path)
}
if ($rows.Count -eq 0) { throw "Translation table has no rows: $TablePath" }
Write-Host "Merged $($tablePaths.Count) table file(s), $($rows.Count) rows."

# Reject duplicate resource names up front: MakePri fails the build on them, and
# two rows that normalize to the same name ("Text" and "Text#2") would otherwise
# silently overwrite each other.
$duplicates = @($rows |
    Group-Object { Get-TranslationResourceName -Row $_ } |
    Where-Object { $_.Count -gt 1 })
if ($duplicates.Count -gt 0) {
    throw "Duplicate resource names: $(($duplicates | ForEach-Object Name) -join ', ')"
}

function ConvertTo-ReswXml {
    param([System.Collections.Specialized.OrderedDictionary]$Entries)

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
    [void]$builder.AppendLine('<root>')
    [void]$builder.AppendLine('  <resheader name="resmimetype">')
    [void]$builder.AppendLine('    <value>text/microsoft-resx</value>')
    [void]$builder.AppendLine('  </resheader>')
    [void]$builder.AppendLine('  <resheader name="version">')
    [void]$builder.AppendLine('    <value>2.0</value>')
    [void]$builder.AppendLine('  </resheader>')
    [void]$builder.AppendLine('  <resheader name="reader">')
    [void]$builder.AppendLine('    <value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>')
    [void]$builder.AppendLine('  </resheader>')
    [void]$builder.AppendLine('  <resheader name="writer">')
    [void]$builder.AppendLine('    <value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>')
    [void]$builder.AppendLine('  </resheader>')

    foreach ($name in $Entries.Keys) {
        $escapedName = [System.Security.SecurityElement]::Escape([string]$name)
        $escapedValue = [System.Security.SecurityElement]::Escape([string]$Entries[$name])
        [void]$builder.AppendLine("  <data name=`"$escapedName`" xml:space=`"preserve`">")
        [void]$builder.AppendLine("    <value>$escapedValue</value>")
        [void]$builder.AppendLine('  </data>')
    }

    [void]$builder.AppendLine('</root>')
    return $builder.ToString()
}

foreach ($language in $Languages) {
    # en-US is authored in the 'english' column; every other language uses its own column.
    $sourceColumn = 'english'
    if ($language -ne 'en-US') { $sourceColumn = $language }

    if (-not ($rows[0].PSObject.Properties.Name -contains $sourceColumn)) {
        throw "Translation table has no column '$sourceColumn' for language '$language'."
    }

    $entries = [ordered]@{}
    $fallbacks = 0
    foreach ($row in $rows) {
        $name = Get-TranslationResourceName -Row $row
        $translated = [string]$row.$sourceColumn

        if ([string]::IsNullOrWhiteSpace($translated)) {
            if ($language -ne 'en-US') { $fallbacks++ }
        }
        $entries[$name] = Get-TranslationValue -Row $row -Language $language
    }

    $languageDirectory = Join-Path $StringsRoot $language
    New-Item -ItemType Directory -Path $languageDirectory -Force | Out-Null
    $targetPath = Join-Path $languageDirectory 'Resources.resw'
    $xml = ConvertTo-ReswXml -Entries $entries
    [System.IO.File]::WriteAllText($targetPath, $xml, [System.Text.UTF8Encoding]::new($true))

    $note = ''
    if ($language -ne 'en-US') { $note = " ($fallbacks entries fall back to English)" }
    Write-Host "Wrote $($entries.Count) resources -> $targetPath$note"
}
