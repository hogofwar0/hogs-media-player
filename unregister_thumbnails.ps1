# Undo register_thumbnails.ps1 (per-user only). Leaves Windows' own "open with Hogs" entry as it was before:
# just the open command.
$dir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$clsid = '{26687DFC-63C4-4F37-8139-5E958C287DB4}'
$thumb = '{e357fccd-a995-4576-b01f-234630154e96}'
$C     = 'HKCU:\Software\Classes'
Remove-Item -LiteralPath "$C\CLSID\$clsid" -Recurse -Force -ErrorAction SilentlyContinue
$app = "$C\Applications\hogofwar.exe"
foreach($k in 'DefaultIcon','SupportedTypes',"ShellEx"){ Remove-Item -LiteralPath "$app\$k" -Recurse -Force -ErrorAction SilentlyContinue }
Remove-ItemProperty -LiteralPath $app -Name FriendlyAppName -ErrorAction SilentlyContinue
$list = Join-Path $dir 'hogs_thumb_extensions.txt'
if(Test-Path $list){
    foreach($e in Get-Content $list){ if($e){ Remove-Item -LiteralPath "$C\$e\ShellEx\$thumb" -Recurse -Force -ErrorAction SilentlyContinue } }
    Remove-Item $list
}
Add-Type -Namespace Hogs -Name Shell2 -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, System.IntPtr a, System.IntPtr b);'
[Hogs.Shell2]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
"Unregistered."
