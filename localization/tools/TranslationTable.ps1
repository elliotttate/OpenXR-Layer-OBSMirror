# Shared translation-table helpers for the localization tools.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 parses scripts as ANSI,
# and non-ASCII comments can swallow the following line break.
#
# The table is tab-separated with a "key / target / prop / english / <language>"
# header. It is parsed by hand instead of with Import-Csv, because Import-Csv
# strips leading and trailing whitespace from every field -- and some UI strings
# legitimately begin with spaces (the "  -  CPU fallback" suffix that is appended
# to a preview status line, for example). Losing those spaces silently changes
# the English UI, which is exactly what the localization must not do.

function Import-TranslationTable {
    param([Parameter(Mandatory = $true)][string]$Path)

    $lines = @(Get-Content -LiteralPath $Path -Encoding utf8)
    if ($lines.Count -eq 0) { throw "Translation table is empty: $Path" }

    $header = $lines[0] -split "`t"
    $rows = [System.Collections.Generic.List[object]]::new()

    for ($index = 1; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }

        $fields = $line -split "`t"
        $values = [ordered]@{}
        for ($column = 0; $column -lt $header.Count; $column++) {
            $values[$header[$column]] = if ($column -lt $fields.Count) { $fields[$column] } else { '' }
        }
        $rows.Add([pscustomobject]$values)
    }

    return $rows
}

# Resource names are "<key>.<property>" for XAML rows and "<key>" for code rows.
# A trailing "#n" (which occurrence) or "*" (every match) is part of the XAML
# match rule, not part of the resource name.
function Get-TranslationResourceName {
    param([Parameter(Mandatory = $true)]$Row)

    $property = $Row.prop -replace '[#*]\d*$', ''
    if ($property -eq 'code') { return $Row.key }
    return "$($Row.key).$property"
}

# The value a language must carry: its own column, or the English original while
# the translation is still missing.
function Get-TranslationValue {
    param(
        [Parameter(Mandatory = $true)]$Row,
        [Parameter(Mandatory = $true)][string]$Language
    )

    $column = 'english'
    if ($Language -ne 'en-US') { $column = $Language }

    $translated = [string]$Row.$column
    if ([string]::IsNullOrWhiteSpace($translated)) { return [string]$Row.english }
    return $translated
}
