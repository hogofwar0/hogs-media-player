# Build the portable release ZIP for GitHub.  Run:  powershell -ExecutionPolicy Bypass -File make_release.ps1
# ALLOW-LIST packaging: only the files named below are copied — personal data (playlist, hide-words, search,
# theme, enhcache), sources, build/dev tools, test media, backups and notes can never slip in.
# Output: ..\Hogs media player V1.45 distribution\Hogs-media-player-V1.45-win64.zip
$ErrorActionPreference = 'Stop'
$src  = Split-Path -Parent $MyInvocation.MyCommand.Path
$name = 'Hogs-media-player-V1.45-win64'
$dist = Join-Path (Split-Path -Parent $src) 'Hogs media player V1.45 distribution'
$stage = Join-Path $dist $name
$zip  = Join-Path $dist "$name.zip"

if(Get-Process hogofwar -ErrorAction SilentlyContinue){ throw "Close Hogs media player first (its files must not be in use)." }
if(Test-Path $stage){ Remove-Item $stage -Recurse -Force }
if(Test-Path $zip){ Remove-Item $zip -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- programs + runtime DLLs (avdevice is not used by anything: left out) ---
$files = 'hogofwar.exe','hogs_enhance.exe','hogs_thumb.dll','SDL2.dll',
         'avcodec-61.dll','avformat-61.dll','avfilter-10.dll','avutil-59.dll',
         'swscale-8.dll','swresample-5.dll','postproc-58.dll',
         'unregister_thumbnails.ps1','register_thumbnails.ps1'
foreach($f in $files){ Copy-Item (Join-Path $src $f) $stage -ErrorAction Stop }

# --- AI models (only the .param/.bin pairs) and MilkDrop presets (only .milk) ---
New-Item -ItemType Directory -Force "$stage\vulkan\models" | Out-Null
Copy-Item "$src\vulkan\models\*.param","$src\vulkan\models\*.bin" "$stage\vulkan\models\"
New-Item -ItemType Directory -Force "$stage\presets" | Out-Null
Copy-Item -LiteralPath (Get-ChildItem -LiteralPath "$src\presets" -Filter *.milk).FullName -Destination "$stage\presets\"

# --- user docs + licences ---
Copy-Item "$src\release_docs\README.txt","$src\release_docs\THIRD-PARTY-NOTICES.txt","$src\donate_qr.png" $stage
Copy-Item "$src\LICENSE" "$stage\LICENSE.txt"     # GNU GPL v3 full text (the project licence)

# --- safety net: refuse to ship anything that looks personal or developer-only ---
$bad = Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Name -match '^hogs_(session|excludes|search|theme)' -or $_.Extension -match '^\.(c|cpp|h|o|md|bat|sh|json|log|tmp|mp3|py)$' -or
    $_.FullName -match '\\(enhcache|deps|enhancer_src|_backup|tools|milk_src|models_src)\\' -or $_.Name -match 'avdevice|_t\.(exe|dll)$' }
if($bad){ $bad.FullName; throw "Refusing to package the files above." }

# entries are added one by one with explicit "/" paths: Windows PowerShell's Compress-Archive (and its .NET
# ZipFile) write "\", which some unzip tools turn into files literally named "presets\x.milk"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach($f in Get-ChildItem $stage -Recurse -File){
        $rel = $name + '/' + $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
$n = (Get-ChildItem $stage -Recurse -File).Count
$u = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"{0}: {1} files, {2:N0} MB unpacked, {3:N0} MB zipped" -f $zip, $n, $u, ((Get-Item $zip).Length / 1MB)
