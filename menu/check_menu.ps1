# Strict check of the menu descriptor, as CRModMenu reads it: a raw line break (or any control character)
# inside a string makes CRModMenu reject the whole file, and the MODS tab disappears. PowerShell's own JSON
# reader lets those through, so this looks for them itself, then parses the file.
param([string]$Path)
try { $t = [IO.File]::ReadAllText($Path) } catch { Write-Output "menu: cannot read $Path"; exit 1 }
$line = 1; $inStr = $false; $esc = $false
foreach ($ch in $t.ToCharArray()) {
    if ($ch -eq "`n") { if ($inStr) { Write-Output "menu: line break inside a string at line $line (use \n)"; exit 1 }; $line++; continue }
    if ($inStr) {
        if ($esc) { $esc = $false } elseif ($ch -eq '\') { $esc = $true } elseif ($ch -eq '"') { $inStr = $false }
        elseif ([int]$ch -lt 32) { Write-Output "menu: control character inside a string at line $line"; exit 1 }
    } elseif ($ch -eq '"') { $inStr = $true }
}
try { $t | ConvertFrom-Json | Out-Null } catch { Write-Output "menu: not valid JSON: $_"; exit 1 }
exit 0
