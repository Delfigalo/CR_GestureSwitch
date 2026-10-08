# Builds the translated menu: reads menu\gestureswitch.menu.json (English) and the menu\<lang>.tsv files, and writes
# the output file with every label and description as {"en": ..., "<lang>": ...}. Text with no translation stays
# plain English (the menu shows English then). Run by build.bat.
# A .tsv line is: English TAB translation. Templates use {w} (each weapon), {t}/{T} (each kind, lower case /
# capitalised) and {z} (the kind translated), from the "@kind" lines. All translated text lives in the .tsv files
# (this script has none: Windows PowerShell would misread it).
param([string]$Src, [string]$Out)
$dir = Split-Path $Src
$weapons = "Strike", "Glaive", "Cleave", "Extend", "Crush", "Drill", "Concussive Beatdown", "Uppercut Surge", "Spin Edge", "Wide Edge", "Split Pivot", "Split Orbit"
$langs = @{}
foreach ($f in Get-ChildItem $dir -Filter *.tsv) {
    $lang = [IO.Path]::GetFileNameWithoutExtension($f.Name)
    $lines = [IO.File]::ReadAllLines($f.FullName, [Text.Encoding]::UTF8)
    $kinds = @()
    foreach ($line in $lines) { if ($line.StartsWith("@kind`t")) { $kinds += ,($line.Split("`t")) } }
    $map = @{}
    foreach ($line in $lines) {
        if (-not $line -or $line.StartsWith("#") -or $line.StartsWith("@")) { continue }
        $p = $line.Split("`t")
        if ($p.Count -ne 2) { continue }
        if ($p[0].Contains("{w}")) {
            foreach ($w in $weapons) { $map[$p[0].Replace("{w}", $w)] = $p[1].Replace("{w}", $w) }
        } elseif ($p[0].Contains("{t}") -or $p[0].Contains("{T}")) {
            foreach ($k in $kinds) { $map[$p[0].Replace("{t}", $k[1]).Replace("{T}", $k[2])] = $p[1].Replace("{z}", $k[3]) }
        } else {
            $map[$p[0]] = $p[1]
        }
    }
    $langs[$lang] = $map
}

$t = [IO.File]::ReadAllText($Src, [Text.Encoding]::UTF8)
$missing = @{}
$re = [regex]'"(label|description)": "((?:[^"\\]|\\.)*)"'
$t = $re.Replace($t, {
    param($mt)
    $key = $mt.Groups[1].Value
    $en = $mt.Groups[2].Value
    $parts = @('"en": "' + $en + '"')
    foreach ($lang in ($langs.Keys | Sort-Object)) {
        if ($langs[$lang].ContainsKey($en)) { $parts += '"' + $lang + '": "' + $langs[$lang][$en] + '"' }
        elseif ($en -notmatch '^[0-9]$' -and $en -notmatch '^[LR][TB]/[LR][12]$' -and $weapons -notcontains $en) { $missing["$lang`t$en"] = 1 }   # (weapon names stay English)
    }
    if ($parts.Count -eq 1) { return $mt.Value }
    return '"' + $key + '": {' + ($parts -join ', ') + '}'
})
foreach ($k in $missing.Keys) { Write-Output "translate: not translated: $($k.Substring(0, [Math]::Min(100, $k.Length)))" }
[IO.File]::WriteAllText($Out, $t, (New-Object Text.UTF8Encoding($false)))
exit 0
