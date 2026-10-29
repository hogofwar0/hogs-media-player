# Hogs media player — per-user Explorer integration (no admin needed; undo with unregister_thumbnails.ps1)
#  * thumbnail provider hogs_thumb.dll (FFmpeg-based: HEVC/x265, RMVB, VOB, ... get real picture thumbnails)
#  * the "Applications\hogofwar.exe" association (what Windows uses when Hogs is your chosen default app)
#    gets a proper name, the hog logo icon, and the thumbnail provider
#  * extensions Windows has NO thumbnailer for at all also get it (only where nothing else is registered)
# Everything is written under HKCU\Software\Classes — nothing system-wide is touched.
$ErrorActionPreference = 'Stop'
$dir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe   = Join-Path $dir 'hogofwar.exe'
$dll   = Join-Path $dir 'hogs_thumb.dll'
$clsid = '{26687DFC-63C4-4F37-8139-5E958C287DB4}'
$thumb = '{e357fccd-a995-4576-b01f-234630154e96}'      # IThumbnailProvider handler slot
$C     = 'HKCU:\Software\Classes'
if(-not (Test-Path $dll)){ throw "hogs_thumb.dll not found next to this script" }

function SetKey($path, $default, $values = @{}){
    if(-not (Test-Path -LiteralPath $path)){ New-Item -Path $path -Force | Out-Null }
    if($null -ne $default){ Set-Item -LiteralPath $path -Value $default }
    foreach($k in $values.Keys){ New-ItemProperty -LiteralPath $path -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
}

# 1) the COM thumbnail provider (runs in Windows' isolated thumbnail process, never inside Explorer)
SetKey "$C\CLSID\$clsid" 'Hogs media player thumbnails'
SetKey "$C\CLSID\$clsid\InprocServer32" $dll @{ ThreadingModel = 'Apartment' }
# Windows' isolated thumbnail host ignores per-user COM classes, so the provider loads in-proc. That is safe:
# hogs_thumb.dll decodes nothing itself — it runs `hogofwar.exe --thumb` in a separate process.
New-ItemProperty -LiteralPath "$C\CLSID\$clsid" -Name DisableProcessIsolation -Value 1 -PropertyType DWord -Force | Out-Null

# 2) the association Windows uses when Hogs is the chosen default ("Applications\hogofwar.exe")
$app = "$C\Applications\hogofwar.exe"
SetKey $app $null @{ FriendlyAppName = 'Hogs media player' }
SetKey "$app\DefaultIcon" "`"$exe`",0"
SetKey "$app\shell\open\command" "`"$exe`" `"%1`""
SetKey "$app\ShellEx\$thumb" $clsid
$video = '.mp4','.mkv','.avi','.mov','.wmv','.webm','.m4v','.flv','.mpg','.mpeg','.ts','.m2ts','.mts','.vob',
         '.rmvb','.rm','.ogm','.ogv','.3gp','.divx','.f4v','.asf','.m2t'
SetKey "$app\SupportedTypes" $null ($video | ForEach-Object -Begin { $h = @{} } -Process { $h[$_] = '' } -End { $h })

# 3) extensions with NO Windows thumbnailer: give them ours (skipped wherever a handler already exists)
$added = @()
foreach($e in '.rmvb','.rm','.vob','.flv','.ogm','.ogv','.divx','.f4v','.m2t'){
    $existing = (Get-ItemProperty -LiteralPath "Registry::HKEY_CLASSES_ROOT\$e\ShellEx\$thumb" -ErrorAction SilentlyContinue).'(default)'
    if(-not $existing){ SetKey "$C\$e\ShellEx\$thumb" $clsid; $added += $e }
}
Set-Content -Path (Join-Path $dir 'hogs_thumb_extensions.txt') -Value $added   # so unregister removes exactly these

# 4) tell Explorer associations changed (refreshes icons)
Add-Type -Namespace Hogs -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, System.IntPtr a, System.IntPtr b);'
[Hogs.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
"Registered. Extension-level thumbnails added for: " + ($added -join ' ')
