# The Windows packages start on a clean Windows: the setup installs (silently, per user), every .exe / .dll finds its
# imports, the launcher answers its command line, draws its window (an offscreen screenshot and the real window) and
# the games run their self-tests; then the portable zip, and the uninstaller removes the installation.
# -UpdateTest also updates both (the setup's folder and the portable one) in place from a local fake release of the
# same packages (scripts/test-launcher-update.py serve, Python), including a file held open by another program.
#
#   pwsh scripts/windows/smoke.ps1 -Dist dist [-Mesa <folder with Mesa's x64 opengl32.dll>] [-Shots <folder>] [-UpdateTest]
#
# -Mesa is for machines without an OpenGL 3.3 driver (GitHub's runners): Mesa's llvmpipe DLLs are copied beside the
# launcher for the test only. The packages never contain them.
param(
    [Parameter(Mandatory)] [string] $Dist,
    [string] $Mesa,
    [string] $Shots = (Join-Path ([IO.Path]::GetTempPath()) 'safehouse-smoke'),
    [switch] $UpdateTest
)
$ErrorActionPreference = 'Stop'
$Dist = (Resolve-Path $Dist).Path
New-Item -ItemType Directory -Force $Shots | Out-Null
$failures = [Collections.Generic.List[string]]::new()
function Check([bool] $ok, [string] $what) {
    if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $failures.Add($what) }
}

# Runs EXE with ARGS (the launcher is a GUI program: its output only reaches redirected handles), returns
# @{ Code; Output }, kills it after TIMEOUT seconds.
function Run([string] $exe, [string[]] $arguments, [int] $timeout = 120) {
    $out = Join-Path $Shots ([IO.Path]::GetRandomFileName())
    $start = @{ FilePath = $exe; RedirectStandardOutput = $out; RedirectStandardError = "$out.err"; PassThru = $true; NoNewWindow = $true }
    if ($arguments) { $start.ArgumentList = $arguments }
    $p = Start-Process @start
    $null = $p.Handle  # keeps the exit code readable after the process ends
    if (-not $p.WaitForExit($timeout * 1000)) { $p.Kill($true); $code = 'timeout' } else { $code = $p.ExitCode }
    $text = ((Get-Content -Raw $out, "$out.err" -ErrorAction SilentlyContinue) -join '').Trim()
    Write-Host "> $(Split-Path -Leaf $exe) $arguments  (exit $code)"
    if ($text) { Write-Host ($text -replace '(?m)^', '    ') }
    @{ Code = $code; Output = $text }
}

# A DLL a program imports is either beside it or a Windows system DLL (api-ms-win-* are API sets).
function CheckImports([string] $bin) {
    $dumpbin = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
    foreach ($file in Get-ChildItem $bin -File | Where-Object Extension -in '.exe', '.dll') {
        $imports = & $dumpbin /nologo /dependents $file.FullName | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[\w.+-]+\.dll$' }
        $missing = $imports | Where-Object {
            $_ -notmatch '^(api|ext)-ms-' -and -not (Test-Path (Join-Path $bin $_)) -and -not (Test-Path (Join-Path "$env:windir\System32" $_))
        }
        Check (-not $missing) "$($file.Name) finds its DLLs$(if ($missing) { ': missing ' + ($missing -join ', ') })"
    }
}

# The screenshot is the launcher's real frame: a blank or one-colour image means it did not draw.
function CheckImage([string] $png, [string] $what) {
    if (-not (Test-Path $png)) { Check $false "$what (no image)"; return }
    Add-Type -AssemblyName System.Drawing
    $image = [Drawing.Bitmap]::FromFile($png)
    $colours = [Collections.Generic.HashSet[int]]::new()
    for ($y = 0; $y -lt $image.Height; $y += 8) { for ($x = 0; $x -lt $image.Width; $x += 8) { $null = $colours.Add($image.GetPixel($x, $y).ToArgb()) } }
    Check ($colours.Count -ge 50) "$what ($($image.Width)x$($image.Height), $($colours.Count) colours sampled)"
    $image.Dispose()
}

function UseMesa([string] $bin) {
    if ($Mesa) {
        Copy-Item (Join-Path $Mesa '*.dll') $bin
        $env:GALLIUM_DRIVER = 'llvmpipe'
    }
}
function RemoveMesa([string] $bin) {
    if ($Mesa) { Get-ChildItem $Mesa -Filter *.dll | ForEach-Object { Remove-Item -Force (Join-Path $bin $_.Name) -ErrorAction SilentlyContinue } }
}

# --- The setup
$setup = Get-ChildItem $Dist -Filter '*-Setup.exe' | Select-Object -First 1
$installed = Join-Path $env:LOCALAPPDATA 'Programs\SafeHouse'
$bin = Join-Path $installed 'bin'
# (Start-Process -Wait would also wait for every process the program starts; WaitForExit waits for the program only.)
function StartAndWait([string] $exe, [string[]] $arguments) {
    $start = @{ FilePath = $exe; PassThru = $true }
    if ($arguments) { $start.ArgumentList = $arguments }
    $p = Start-Process @start
    $null = $p.Handle  # keeps the exit code readable after the process ends
    $p.WaitForExit()
    $p
}
$p = StartAndWait $setup.FullName '/S'
Check ($p.ExitCode -eq 0) "$($setup.Name) /S exits 0 (got $($p.ExitCode))"
foreach ($exe in 'safehouse.exe', 'Sly1-Release.exe', 'Sly2-Release.exe', 'Sly3-Release.exe') {
    Check (Test-Path (Join-Path $bin $exe)) "installed bin\$exe"
}
Check (Test-Path (Join-Path $installed 'SafeHouse.exe')) 'installed SafeHouse.exe at the top'
Check (-not (Get-ChildItem $installed -File | Where-Object Extension -in '.cmd', '.bat', '.ps1')) 'no launch scripts at the top'
Check (Test-Path (Join-Path $installed 'Uninstall.exe')) 'installed Uninstall.exe'
Check (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SafeHouse') 'listed in Apps & features'
CheckImports $bin

# --- The launcher's command line and the games' self-tests
$launcher = Join-Path $bin 'safehouse.exe'
$help = Run $launcher '--help'
Check ($help.Code -eq 0) 'safehouse --help'
Check ($help.Output -notmatch 'build-source') 'the launcher cannot build the games from source (--build-source)'
Check ((Run $launcher '--installs').Code -eq 0) 'safehouse --installs'
$steam = Run $launcher '--steam-status'
Check ($steam.Code -eq 0 -and $steam.Output -match '^Steam: ') 'safehouse --steam-status'
Check ((Run $launcher '--desktop-shortcut-status').Code -eq 0) 'safehouse --desktop-shortcut-status'
Check ((Run (Join-Path $bin 'Sly2-Release.exe') '--rumble-selftest').Code -eq 0) 'Sly2-Release --rumble-selftest'
Check ((Run (Join-Path $bin 'Sly3-Release.exe') '--splice-selftest').Code -eq 0) 'Sly3-Release --splice-selftest'

# --- The launcher's window: a frame rendered offscreen, then the real window stays up
UseMesa $bin
$front = Join-Path $Shots 'launcher-home.png'
Check ((Run $launcher '--screenshot', $front, '--size', '1280x800' -timeout 180).Code -eq 0) 'safehouse --screenshot exits 0'
CheckImage $front 'the launcher draws its home screen'
$sly2 = Join-Path $Shots 'launcher-sly2.png'
$null = Run $launcher '--screenshot', $sly2, '--size', '1280x800', '--game', 'sly2' -timeout 180
CheckImage $sly2 'the launcher draws the Sly 2 page'

# Opened the way a player does: SafeHouse.exe at the top starts bin\safehouse.exe.
$start = StartAndWait (Join-Path $installed 'SafeHouse.exe')
Check ($start.ExitCode -eq 0) "SafeHouse.exe exits 0 after starting the launcher (got $($start.ExitCode))"
$window = $null
$title = ''
for ($i = 0; $i -lt 60 -and -not $title; $i++) {
    Start-Sleep -Milliseconds 500
    $window = Get-Process -Name safehouse -ErrorAction SilentlyContinue | Where-Object Path -eq $launcher | Select-Object -First 1
    if ($window) { $window.Refresh(); $title = $window.MainWindowTitle }
}
Start-Sleep -Seconds 10
$alive = $window -and -not $window.HasExited
Check $alive "the launcher window stays open (title '$title')"
Check ($title -ne '') 'SafeHouse.exe opens the launcher window'
if ($alive) { Stop-Process -Id $window.Id -Force }
RemoveMesa $bin

# --- The portable zip (portable mode: user files under data/ beside bin/)
$zip = Get-ChildItem $Dist -Filter '*-Portable.zip' | Select-Object -First 1
$portable = Join-Path $Shots 'portable'
Remove-Item -Recurse -Force $portable -ErrorAction SilentlyContinue
Expand-Archive $zip.FullName $portable
# (bin\safehouse.exe explicitly: Windows file names ignore case, so a search for safehouse.exe also finds SafeHouse.exe
#  at the top)
$portableRoot = Get-ChildItem $portable -Directory | Select-Object -First 1 -ExpandProperty FullName
$portableBin = Join-Path $portableRoot 'bin'
Check (Test-Path (Join-Path $portableBin 'safehouse.exe')) 'portable zip has bin\safehouse.exe'
Check (Test-Path (Join-Path $portableRoot 'portable.txt')) 'portable zip has portable.txt'
Check (Test-Path (Join-Path $portableRoot 'SafeHouse.exe')) 'portable zip has SafeHouse.exe at the top'
Check ((Run (Join-Path $portableBin 'safehouse.exe') '--installs').Code -eq 0) 'portable safehouse --installs'
CheckImports $portableBin

# --- Updates in place (launcher/update.h): safehouse --update stages the package from the fake release; a helper
# copied to .update\helper swaps the files once the launcher has exited and writes update-result.json.
if ($UpdateTest) {
    $serverLog = Join-Path $Shots 'update-server.log'
    $server = Start-Process python -ArgumentList (Join-Path $PSScriptRoot '..\test-launcher-update.py'), 'serve', $Dist, '--version', '99.0.0', '--port', '8765' `
        -PassThru -NoNewWindow -RedirectStandardOutput $serverLog -RedirectStandardError "$serverLog.err"
    Start-Sleep -Seconds 3
    $env:SAFEHOUSE_UPDATE_URL = 'http://127.0.0.1:8765/releases/latest'
    $env:SAFEHOUSE_UPDATE_TEST_VERSION = '0.0.1'
    $env:SAFEHOUSE_UPDATE_TEST_DATE = '2026-01-01T00:00:00Z'

    # Runs safehouse --update in ROOT and waits for the helper's result.
    function UpdateIn([string] $root, [string] $config, [string] $what) {
        $result = Join-Path $config 'update-result.json'
        Remove-Item -Force $result -ErrorAction SilentlyContinue
        $run = Run (Join-Path $root 'bin\safehouse.exe') '--update' -timeout 600
        Check ($run.Code -eq 0 -and $run.Output -match 'staged') "${what}: safehouse --update downloads, checks and stages 99.0.0"
        for ($i = 0; $i -lt 240 -and -not (Test-Path $result); $i++) { Start-Sleep -Milliseconds 500 }
        Start-Sleep -Seconds 2  # (the helper exits after writing the result)
        $log = Join-Path $root '.update\update.log'
        if (Test-Path $log) { Write-Host ((Get-Content -Raw $log) -replace '(?m)^', '    ') }
        if (Test-Path $result) { Get-Content -Raw $result | ConvertFrom-Json } else { $null }
    }

    $check = Run $launcher '--check-update'
    Check ($check.Code -eq 0 -and $check.Output -match 'install windows-installed' -and $check.Output -match 'newer') 'installed: the check finds 99.0.0'
    $userData = Join-Path $env:LOCALAPPDATA 'ProjectCane'
    New-Item -ItemType Directory -Force (Join-Path $userData 'user') | Out-Null
    Set-Content (Join-Path $userData 'user\update-test.txt') 'my save'
    Remove-Item -Force (Join-Path $bin 'cane-extractor.exe')
    $r = UpdateIn $installed (Join-Path $userData 'config') 'installed'
    Check ($r -and $r.ok -and $r.version -eq '99.0.0') "installed: updated to 99.0.0 ($(if ($r) { $r.message }))"
    Check (Test-Path (Join-Path $bin 'cane-extractor.exe')) 'installed: the package''s files are back in place'
    Check ((Get-Content (Join-Path $userData 'user\update-test.txt')) -eq 'my save') 'installed: the profile is untouched'
    Check (Test-Path (Join-Path $installed 'Uninstall.exe')) 'installed: Uninstall.exe kept'
    Check (-not (Test-Path (Join-Path $installed 'portable.txt'))) 'installed: no portable.txt (still a setup installation)'
    $entry = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SafeHouse'
    Check ($entry.DisplayVersion -eq '99.0.0') "installed: Apps & features shows 99.0.0 (got $($entry.DisplayVersion))"
    Check ((Run $launcher '--installs').Code -eq 0) 'installed: the updated launcher runs'

    $portableConfig = Join-Path $portableRoot 'data\config'
    $portableExe = Join-Path $portableBin 'safehouse.exe'
    New-Item -ItemType Directory -Force (Join-Path $portableRoot 'data\user') | Out-Null
    Set-Content (Join-Path $portableRoot 'data\user\update-test.txt') 'my save'
    # A file another program holds open (no sharing): the helper retries, gives up and restores the old files.
    $locked = [IO.File]::Open((Join-Path $portableBin 'SDL2.dll'), 'Open', 'Read', 'None')
    $before = (Get-FileHash $portableExe).Hash
    $r = UpdateIn $portableRoot $portableConfig 'portable, a file in use'
    $locked.Dispose()
    Check ($r -and -not $r.ok -and $r.message -match 'in use') "portable, a file in use: the update fails cleanly ($(if ($r) { $r.message }))"
    Check ((Get-FileHash $portableExe).Hash -eq $before -and (Test-Path (Join-Path $portableRoot 'SafeHouse.exe'))) 'portable, a file in use: the old files are back'
    $r = UpdateIn $portableRoot $portableConfig 'portable'
    Check ($r -and $r.ok -and $r.version -eq '99.0.0') "portable: updated to 99.0.0 ($(if ($r) { $r.message }))"
    Check ((Get-Content (Join-Path $portableRoot 'portable.txt') -Raw) -match 'portable') 'portable: portable.txt kept'
    Check ((Get-Content (Join-Path $portableRoot 'data\user\update-test.txt')) -eq 'my save') 'portable: data\ untouched'
    Check ((Run $portableExe '--installs').Code -eq 0) 'portable: the updated launcher runs'

    Remove-Item Env:SAFEHOUSE_UPDATE_URL, Env:SAFEHOUSE_UPDATE_TEST_VERSION, Env:SAFEHOUSE_UPDATE_TEST_DATE
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}

# --- The uninstaller (it copies itself to %TEMP% and returns at once: wait for the folder to go)
$p = StartAndWait (Join-Path $installed 'Uninstall.exe') '/S'
for ($i = 0; $i -lt 60 -and (Test-Path $bin); $i++) { Start-Sleep -Milliseconds 500 }
Check ($p.ExitCode -eq 0 -and -not (Test-Path $bin)) 'Uninstall.exe /S removes the installation'
Check (-not (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SafeHouse')) 'removed from Apps & features'

if ($failures.Count) {
    Write-Host "`n$($failures.Count) check(s) failed:"; $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "`nall checks passed"
