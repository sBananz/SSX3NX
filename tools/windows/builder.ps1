# SPDX-License-Identifier: GPL-3.0-only
# SSX3NX Builder: a window that sets up the tools, builds SSX3NX.nro from your
# own SSX 3 (PAL) Xbox game and puts it on the SD card. Started by
# "SSX3NX Builder.bat" in the top folder. Adapted from the OpenTrickyNX Builder
# (DevCrashDay/OpenTrickyNX, port/tools/windows/builder.ps1, GPL-3.0-only).
#
# Without -Task it shows the window. Each button runs this same script again
# with -Task (setup, build, sdcard) in the background, writing to a log file
# the window shows as it grows. Windows PowerShell 5.1, nothing to install.
# Keep this file ASCII: PowerShell 5.1 reads it in the ANSI code page.
param(
    [string]$Task = "",
    [string]$Game = "",
    [string]$Drive = ""
)

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Work = Join-Path $Root "build\windows"
$Downloads = Join-Path $Work "downloads"
$Toolkit = Join-Path $Root "build\xboxrecomp"
$Extracted = Join-Path $Work "game"
$MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$Bash = Join-Path $MsysRoot "usr\bin\bash.exe"
$Python = Join-Path $MsysRoot "ucrt64\bin\python.exe"
# The Vulkan files: the builder's own download, or one already unpacked by hand
# (NVK_SDK, or a portlibs\switch folder in the top folder).
$NvkDir = Join-Path $Work "nvk-sdk\portlibs\switch"
foreach ($d in @($env:NVK_SDK, (Join-Path $Root "portlibs\switch"))) {
    if ($d -and (Test-Path (Join-Path $d "lib\libvulkan.a"))) { $NvkDir = $d; break }
}
$DockerBin = Join-Path $env:ProgramFiles "Docker\Docker\resources\bin"
$DockerDesktop = Join-Path $env:ProgramFiles "Docker\Docker\Docker Desktop.exe"
$Settings = Join-Path $Work "settings.json"
$Dist = Join-Path $Root "dist\switch\ssx3nx"
$Nro = Join-Path $Dist "SSX3NX.nro"

$MsysUrl = "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe"
$DockerUrl = "https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe"
$NvkUrl = "https://github.com/DevCrashDay/OpenTrickyNX/releases/download/nvk-sdk/mesa-switch-nvk-sdk.zip"
$Packages = "mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone git"
# SHA-1 of the PAL Xbox SSX 3 default.xbe the translation is made for.
$PalXbe = "BA876CC11AD38650EAC9831B1C7F8C4DBF7C5BA9"
$Title = "SSX3NX Builder"

New-Item -ItemType Directory -Force -Path $Work, $Downloads | Out-Null
if (Test-Path $DockerBin) { $env:PATH = "$DockerBin;$env:PATH" }

function Has-File([string]$path) { $path -and (Test-Path -LiteralPath $path -PathType Leaf) }
function Has-Dir([string]$path) { $path -and (Test-Path -LiteralPath $path -PathType Container) }
function Test-Msys { (Test-Path $Bash) -and (Test-Path $Python) }
function Test-DockerInstalled { [bool](Get-Command docker.exe -ErrorAction SilentlyContinue) }
function Test-Nvk { Test-Path (Join-Path $NvkDir "lib\libvulkan.a") }
function Test-Toolkit {
    $h = Join-Path $Toolkit "src\kernel\guest_ptr.h"
    (Test-Path $h) -and [bool](Select-String -LiteralPath $h -Pattern "XBOX_PAGEMAP" -Quiet)
}
function Test-Nro { Test-Path $Nro }

# ---------------------------------------------------------------------------
# The background tasks. They print to the log; the last line says how it ended.

function Say([string]$text) { [Console]::Out.WriteLine($text) }
function Fail([string]$text) { Say ""; Say "PROBLEM: $text"; exit 1 }

# Runs a program, its output (both streams) into the log; returns the exit code.
function Run([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = "Continue"
    & $exe @arguments 2>&1 | ForEach-Object { Say "$_" }
    return $LASTEXITCODE
}

# Runs a bash script in MSYS2's UCRT64 shell, from the top folder. The script
# goes through a file: Windows PowerShell mangles quotes in program arguments.
# Paths go in as environment variables (SSX3NX_*), turned into MSYS2 paths there.
function Run-Bash([string]$script) {
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    $env:MSYS2_PATH_TYPE = "inherit"      # keeps docker.exe reachable
    $env:SSX3NX_ROOT = $Root
    $file = Join-Path $Work "step.sh"
    $text = "set -e`ncd `"`$(cygpath -u `"`$SSX3NX_ROOT`")`"`n" + $script + "`n"
    [IO.File]::WriteAllText($file, $text.Replace("`r", ""))
    return (Run $Bash @("-l", $file.Replace("\", "/")))
}

# Returns $false when the file isn't there (404); other failures stop the task.
function Download([string]$url, [string]$file, [switch]$Optional) {
    Say "   downloading $url"
    $ProgressPreference = "SilentlyContinue"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$file.part" }
    catch {
        $code = 0
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($Optional -and $code -eq 404) { return $false }
        Fail "the download failed ($($_.Exception.Message)). Check the internet connection and try again."
    }
    Move-Item -Force "$file.part" $file
    return $true
}

function Test-DockerRunning {
    $ErrorActionPreference = "Continue"
    & docker.exe info *> $null
    return ($LASTEXITCODE -eq 0)
}

function Start-Docker {
    if (Test-DockerRunning) { return }
    Say "   starting Docker Desktop (it can take a few minutes)"
    if (Test-Path $DockerDesktop) { Start-Process $DockerDesktop }
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 5
        if (Test-DockerRunning) { Say "   Docker is running"; return }
    }
    Fail "Docker Desktop did not start. Open it from the Start menu, accept its terms (you can skip signing in), wait until it says it is running, then press the button again."
}

# The game folder (default.xbe + data\): the folder you picked, or your disc
# image extracted once into build\windows\game. Checks it is the PAL game.
function Get-GameFolder {
    if (-not $Game) { Fail "pick your game first (step 2)." }
    if (Has-Dir $Game) {
        $dir = $Game
        if (-not (Has-File (Join-Path $dir "default.xbe"))) { Fail "there is no default.xbe in $dir. Pick the folder that holds default.xbe and the data folder." }
    } elseif (Has-File $Game) {
        $dir = $Extracted
        $item = Get-Item -LiteralPath $Game
        $stamp = "$($item.FullName)|$($item.Length)|$($item.LastWriteTimeUtc.Ticks)"
        $marker = Join-Path $dir ".from-disc-image"
        if ((Has-File $marker) -and ((Get-Content -Raw -LiteralPath $marker).Trim() -eq $stamp)) {
            Say "   your disc image is already extracted ($dir)"
        } else {
            Say "   extracting your disc image to $dir (about 2 GB, a few minutes)"
            if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
            if ((Run $Python @((Join-Path $Root "tools\extract_iso.py"), $Game, $dir)) -ne 0) { Fail "the disc image could not be read." }
            Set-Content -LiteralPath $marker -Value $stamp -Encoding ASCII
        }
    } else {
        Fail "$Game was not found. Pick your game again (step 2)."
    }
    if (-not (Has-Dir (Join-Path $dir "data"))) { Fail "there is no data folder next to default.xbe in ${dir}: the game files are incomplete." }
    $sha = (Get-FileHash -LiteralPath (Join-Path $dir "default.xbe") -Algorithm SHA1).Hash
    if ($sha -ne $PalXbe -and $env:SSX3NX_ANY_XBE -ne "1") {
        Fail "this default.xbe is not the PAL (European) Xbox release of SSX 3, which is the only version SSX3NX is made for (SHA-1 $sha)."
    }
    Say "   game: $dir (PAL SSX 3)"
    return $dir
}

function Task-Setup {
    Say "== [1/5] MSYS2 (Python and tools for translating the game)"
    if (-not (Test-Path $Bash)) {
        $sfx = Join-Path $Downloads "msys2-base.sfx.exe"
        Download $MsysUrl $sfx | Out-Null
        $parent = Split-Path $MsysRoot -Parent
        if ((Split-Path $MsysRoot -Leaf) -ne "msys64") { Fail "MSYS2_ROOT has to end in \msys64 for the automatic install." }
        Say "   unpacking to $MsysRoot"
        if ((Run $sfx @("-y", "-o$parent")) -ne 0 -or -not (Test-Path $Bash)) { Fail "MSYS2 could not be unpacked to $MsysRoot." }
        Say "   first start of MSYS2"
        Run-Bash "true" | Out-Null
    } else {
        Say "   MSYS2 is in $MsysRoot"
    }
    if (-not (Test-Path $Python) -or (Run-Bash "`"`$MINGW_PREFIX/bin/python.exe`" -c 'import capstone' && command -v git >/dev/null") -ne 0) {
        Say "   installing Python, capstone and git"
        # twice: the first run may stop after updating pacman itself
        Run-Bash "pacman -Syu --noconfirm || true" | Out-Null
        Run-Bash "pacman -Syu --noconfirm || true" | Out-Null
        if ((Run-Bash "pacman -S --needed --noconfirm $Packages") -ne 0) { Fail "the MSYS2 packages could not be installed." }
    }
    Say "   MSYS2 is ready"

    Say "== [2/5] Docker Desktop (the Switch compiler runs in it)"
    if (-not (Test-DockerInstalled)) {
        $ErrorActionPreference = "Continue"
        $wsl = $false
        if (Get-Command wsl.exe -ErrorAction SilentlyContinue) { & wsl.exe --status *> $null; $wsl = ($LASTEXITCODE -eq 0) }
        if (-not $wsl) {
            Say "   turning on WSL, which Docker needs (Windows asks for permission)"
            Start-Process wsl.exe -ArgumentList "--install --no-distribution" -Verb RunAs -Wait | Out-Null
        }
        $installer = Join-Path $Downloads "Docker Desktop Installer.exe"
        Download $DockerUrl $installer | Out-Null
        Say "   installing Docker Desktop (Windows asks for permission)"
        $p = Start-Process $installer -ArgumentList "install --quiet --accept-license --backend=wsl-2" -Verb RunAs -Wait -PassThru
        if ($p.ExitCode -ne 0) { Fail "the Docker Desktop installer stopped with code $($p.ExitCode)." }
        Say ""
        Say "RESTART: Docker Desktop is installed. Restart the computer, then open SSX3NX Builder again and press Install / check once more."
        exit 3
    }
    Start-Docker

    Say "== [3/5] the Switch compiler (about 3 GB the first time)"
    if ((Run-Bash "docker image inspect ssx3nx-build >/dev/null 2>&1 || docker build -t ssx3nx-build - < Dockerfile") -ne 0) {
        Fail "the Switch compiler (devkitpro/devkita64) could not be downloaded."
    }
    Say "   the Switch compiler is ready"

    Say "== [4/5] Vulkan driver files (Mesa NVK)"
    if (-not (Test-Nvk)) {
        $zip = Join-Path $Downloads "mesa-switch-nvk-sdk.zip"
        if (-not (Download $NvkUrl $zip -Optional)) {
            Fail "the Vulkan driver files are not available for download right now. Get mesa-switch-nvk-sdk from OpenTrickyNX's GitHub (Actions, 'Mesa NVK SDK') and unpack it so that portlibs\switch\lib\libvulkan.a is in the SSX3NX folder, then press Install / check again."
        }
        $dir = Join-Path $Work "nvk-sdk"
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        Expand-Archive -Path $zip -DestinationPath $dir
        if (-not (Test-Nvk)) { Fail "the download has no portlibs\switch\lib\libvulkan.a in it." }
    }
    Say "   the Vulkan files are in $NvkDir"

    Say "== [5/5] the recompiler (xboxrecomp, with the SSX 3 and Switch changes)"
    $env:SSX3NX_TK = $Toolkit
    if ((Run-Bash "TK=`"`$(cygpath -u `"`$SSX3NX_TK`")`" bash scripts/setup.sh") -ne 0) { Fail "the recompiler could not be set up. The messages above say why." }
    if (-not (Test-Toolkit)) { Fail "the recompiler in $Toolkit is missing the Switch changes. Delete that folder and press Install / check again." }

    Say "OK: everything is installed. Pick your game (step 2), then build (step 3)."
}

function Task-Build {
    if (-not (Test-Msys)) { Fail "press Install / check first (MSYS2 is missing)." }
    if (-not (Test-DockerInstalled)) { Fail "press Install / check first (Docker Desktop is missing)." }
    if (-not (Test-Nvk)) { Fail "press Install / check first (the Vulkan files are missing)." }
    if (-not (Test-Toolkit)) { Fail "press Install / check first (the recompiler is missing)." }
    Say "== [1/3] your game"
    $dir = Get-GameFolder
    $free = (Get-PSDrive -Name $Root.Substring(0, 1)).Free
    if ($free -lt 6GB) { Fail "the build needs about 6 GB free on drive $($Root.Substring(0, 2)); there are $([Math]::Floor($free / 1GB)) GB." }
    Say "== [2/3] starting Docker"
    Start-Docker
    # Each translated file takes a couple of GB to compile, and Docker gets half
    # the memory: more at once than that stalls the build.
    $gb = [Math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [Math]::Floor($gb / 4)))
    $env:SWITCH_JOBS = "$jobs"
    $env:NVK_SDK = $NvkDir
    $env:SSX3NX_TK = $Toolkit
    $env:SSX3NX_XBE = Join-Path $dir "default.xbe"
    Say "   $gb GB of memory: compiling $jobs files at once"
    Say "== [3/3] translating and compiling (20 to 40 minutes the first time)"
    if ((Run-Bash "export TK=`"`$(cygpath -u `"`$SSX3NX_TK`")`"; export XBE=`"`$(cygpath -u `"`$SSX3NX_XBE`")`"; ./build_switch.sh") -ne 0) {
        Fail "the build stopped. The messages above say why."
    }
    if (-not (Test-Nro)) { Fail "the build finished but SSX3NX.nro is missing." }
    Say "OK: SSX3NX.nro is built. Put the SD card in and copy it over (step 4)."
}

function Task-SdCard {
    if (-not (Test-Nro)) { Fail "build the game first (step 3)." }
    if (-not $Drive -or -not (Test-Path "$Drive\")) { Fail "pick the SD card's drive." }
    $dest = "$Drive\switch\ssx3nx"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Say "== [1/2] the game program"
    Copy-Item -Force $Nro $dest
    Copy-Item -Force (Join-Path $Dist "README.txt") $dest
    if (Test-Path (Join-Path $dest "SSX 3 env.txt")) {
        Say "   kept your SSX 3 env.txt (your settings)"
    } else {
        Copy-Item (Join-Path $Dist "SSX 3 env.txt") $dest
    }
    Say "   $dest\SSX3NX.nro"
    Say "== [2/2] the game's files"
    $cardGame = Join-Path $dest "game"      # not $game: PowerShell names ignore case ($Game)
    if (Has-File (Join-Path $cardGame "default.xbe")) {
        Say "   already on the card ($cardGame): kept"
    } else {
        $dir = Get-GameFolder
        Say "   copying about 2 GB to $cardGame (several minutes on most SD cards)"
        $ErrorActionPreference = "Continue"
        & robocopy.exe $dir $cardGame /E /XD TDATA UDATA /XF ".from-disc-image" /NFL /NDL /NJH /NP /R:2 /W:2 2>&1 | ForEach-Object { if ("$_".Trim()) { Say "   $_" } }
        if ($LASTEXITCODE -ge 8) { Fail "copying the game files to the SD card failed (robocopy code $LASTEXITCODE). Is the card full or write-protected?" }
    }
    Say "OK: the SD card is ready. On the Switch, hold R while starting any game, then pick SSX 3 (SSX3NX) in the Homebrew Menu. The first start takes a few minutes longer."
}

if ($Task) {
    $ErrorActionPreference = "Stop"
    try {
        switch ($Task) {
            "setup"  { Task-Setup }
            "build"  { Task-Build }
            "sdcard" { Task-SdCard }
            default  { Fail "unknown task $Task" }
        }
    } catch {
        Fail $_.Exception.Message
    }
    exit 0
}

# ---------------------------------------------------------------------------
# The window.

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$saved = @{ game = "" }
if (Test-Path $Settings) {
    try { (Get-Content -Raw $Settings | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $saved[$_.Name] = $_.Value } } catch { }
}
function Save-Settings {
    $saved.game = $gameBox.Text
    try { $saved | ConvertTo-Json | Set-Content -Encoding UTF8 $Settings } catch { }
}

$font = New-Object Drawing.Font("Segoe UI", 10)
$bold = New-Object Drawing.Font("Segoe UI", 10, [Drawing.FontStyle]::Bold)
$form = New-Object Windows.Forms.Form
$form.Text = $Title
$form.Font = $font
$form.ClientSize = New-Object Drawing.Size(720, 680)
$form.FormBorderStyle = "FixedSingle"
$form.MaximizeBox = $false
$form.StartPosition = "CenterScreen"

function Add-Control($parent, $control, [int]$x, [int]$y, [int]$w, [int]$h) {
    $control.Location = New-Object Drawing.Point($x, $y)
    $control.Size = New-Object Drawing.Size($w, $h)
    $parent.Controls.Add($control)
    return $control
}
function New-Label([string]$text) { $l = New-Object Windows.Forms.Label; $l.Text = $text; return $l }
function New-Button([string]$text) { $b = New-Object Windows.Forms.Button; $b.Text = $text; return $b }
function New-Group($text, [int]$y, [int]$h) {
    $g = New-Object Windows.Forms.GroupBox
    $g.Text = $text
    $g.Font = $bold
    return (Add-Control $form $g 12 $y 696 $h)
}
function Set-Plain($group) { foreach ($c in $group.Controls) { $c.Font = $font } }

# 1. Tools
$g1 = New-Group "1. Install the tools (once)" 8 124
$toolsLabel = Add-Control $g1 (New-Label "") 14 26 460 92
$setupButton = Add-Control $g1 (New-Button "Install / check") 500 40 180 44

# 2. Game
$g2 = New-Group "2. Your game" 140 112
Add-Control $g2 (New-Label "Your own SSX 3, PAL Xbox version: the folder with default.xbe and data, or a disc image.") 14 26 670 22 | Out-Null
$gameBox = Add-Control $g2 (New-Object Windows.Forms.TextBox) 14 52 440 26
$folderPick = Add-Control $g2 (New-Button "Folder...") 464 50 104 30
$isoPick = Add-Control $g2 (New-Button "Disc image...") 576 50 104 30
$gameLabel = Add-Control $g2 (New-Label "") 14 82 670 24

# 3. Build
$g3 = New-Group "3. Build the game" 260 84
$buildButton = Add-Control $g3 (New-Button "Build SSX3NX.nro") 14 30 220 40
$buildLabel = Add-Control $g3 (New-Label "The first build takes 20 to 40 minutes.") 250 38 430 24

# 4. SD card
$g4 = New-Group "4. Copy to the SD card" 352 84
Add-Control $g4 (New-Label "SD card drive:") 14 38 110 24 | Out-Null
$driveBox = Add-Control $g4 (New-Object Windows.Forms.ComboBox) 126 35 90 28
$driveBox.DropDownStyle = "DropDownList"
$refreshButton = Add-Control $g4 (New-Button "Refresh") 222 33 84 32
$sdButton = Add-Control $g4 (New-Button "Copy to SD card") 320 30 180 40
$sdLabel = Add-Control $g4 (New-Label "Copies the game program and your game files.") 510 28 180 48

# Messages
$log = Add-Control $form (New-Object Windows.Forms.TextBox) 12 446 696 186
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = "Vertical"
$log.Font = New-Object Drawing.Font("Consolas", 9)
$status = Add-Control $form (New-Label "Ready.") 12 644 530 24
$folderButton = Add-Control $form (New-Button "Open output folder") 548 640 160 30

foreach ($g in $g1, $g2, $g3, $g4) { Set-Plain $g }
$gameBox.Text = $saved.game

function Mark([bool]$ok) { if ($ok) { "[OK]     " } else { "[needed] " } }
function Update-Tools {
    $toolsLabel.Text = (Mark (Test-Msys)) + "MSYS2 with Python`r`n" +
        (Mark (Test-DockerInstalled)) + "Docker Desktop`r`n" +
        (Mark (Test-Nvk)) + "Vulkan driver files`r`n" +
        (Mark (Test-Toolkit)) + "The recompiler (xboxrecomp)"
    $buildLabel.Text = if (Test-Nro) { "Built: $Nro" } else { "The first build takes 20 to 40 minutes." }
}
function Update-Game {
    $t = $gameBox.Text
    $gameLabel.Text = if (-not $t) { "Not picked yet." }
        elseif ((Has-Dir $t) -and (Has-File (Join-Path $t "default.xbe"))) { "A game folder: OK (the version is checked when you build)." }
        elseif (Has-Dir $t) { "No default.xbe in this folder: pick the folder that holds it." }
        elseif (Has-File $t) { "A disc image: it is extracted when you build." }
        else { "Not found." }
}
function Update-Drives {
    $driveBox.Items.Clear()
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and $d.DriveType -eq "Removable") { [void]$driveBox.Items.Add($d.Name.TrimEnd("\")) }
    }
    if ($driveBox.Items.Count -gt 0) { $driveBox.SelectedIndex = 0 }
}
Update-Tools
Update-Game
Update-Drives

$folderPick.Add_Click({
    $d = New-Object Windows.Forms.FolderBrowserDialog
    $d.Description = "Pick the folder that holds SSX 3's default.xbe and data folder"
    if ($d.ShowDialog() -eq "OK") { $gameBox.Text = $d.SelectedPath; Save-Settings; Update-Game }
})
$isoPick.Add_Click({
    $d = New-Object Windows.Forms.OpenFileDialog
    $d.Filter = "Xbox disc image (*.iso;*.xiso)|*.iso;*.xiso|All files|*.*"
    if ($d.ShowDialog() -eq "OK") { $gameBox.Text = $d.FileName; Save-Settings; Update-Game }
})
$gameBox.Add_TextChanged({ Update-Game })
$refreshButton.Add_Click({ Update-Drives })
$folderButton.Add_Click({ New-Item -ItemType Directory -Force -Path $Dist | Out-Null; Start-Process explorer.exe $Dist })

# One task at a time, in the background; a timer copies its log into the window.
$script:proc = $null
$script:logFile = ""
$script:logPos = 0
$buttons = @($setupButton, $buildButton, $sdButton)

function Start-Task([string]$name, [string]$what, [string[]]$extra) {
    if ($script:proc -and -not $script:proc.HasExited) { return }
    Save-Settings
    $script:logFile = Join-Path $Work "$name.log"
    Set-Content -Path $script:logFile -Value "" -Encoding ASCII
    $script:logPos = 0
    $log.Text = ""
    # parameter names bare, values quoted (paths may have spaces)
    $quoted = ($extra | ForEach-Object { if ($_ -like "-*") { $_ } else { "`"$_`"" } }) -join " "
    $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    # cmd joins the two output streams into the log
    $line = "/c `"`"$ps`" -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Task $name $quoted > `"$($script:logFile)`" 2>&1`""
    $script:proc = Start-Process cmd.exe -ArgumentList $line -WindowStyle Hidden -PassThru
    foreach ($b in $buttons) { $b.Enabled = $false }
    $status.Text = "$what ... (you can leave this window open and do something else)"
    $timer.Start()
}

function Read-Log {
    if (-not $script:logFile -or -not (Test-Path $script:logFile)) { return }
    try {
        $fs = [IO.File]::Open($script:logFile, "Open", "Read", "ReadWrite")
        $fs.Seek($script:logPos, "Begin") | Out-Null
        $reader = New-Object IO.StreamReader($fs)
        $new = $reader.ReadToEnd()
        $script:logPos = $fs.Position
        $reader.Close()
        if ($new) {
            $new = ($new -replace "`r(?!`n)", "`r`n") -replace "(?<!`r)`n", "`r`n"
            $log.AppendText($new)
            if ($log.TextLength -gt 200000) { $log.Text = $log.Text.Substring($log.TextLength - 100000) }
            $m = [regex]::Matches($new, "== \[(\d)/(\d)\] ([^\r\n]*)")
            if ($m.Count -gt 0) { $status.Text = "Step $($m[$m.Count-1].Groups[1].Value) of $($m[$m.Count-1].Groups[2].Value): $($m[$m.Count-1].Groups[3].Value)" }
        }
    } catch { }
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    Read-Log
    if ($script:proc -and $script:proc.HasExited) {
        $timer.Stop()
        Read-Log
        foreach ($b in $buttons) { $b.Enabled = $true }
        Update-Tools
        $text = $log.Text
        $problem = [regex]::Match($text, "PROBLEM: ([^\r\n]*)")
        $restart = [regex]::Match($text, "RESTART: ([^\r\n]*)")
        $ok = [regex]::Match($text, "OK: ([^\r\n]*)")
        if ($restart.Success) {
            $status.Text = "Restart the computer."
            [void][Windows.Forms.MessageBox]::Show($restart.Groups[1].Value, $Title, "OK", "Information")
        } elseif ($problem.Success) {
            $status.Text = "Stopped: see the messages."
            [void][Windows.Forms.MessageBox]::Show($problem.Groups[1].Value + "`r`n`r`nThe full messages are in $($script:logFile)", $Title, "OK", "Warning")
        } elseif ($ok.Success) {
            $status.Text = $ok.Groups[1].Value
        } else {
            $status.Text = "Stopped. See the messages above."
        }
    }
})

$setupButton.Add_Click({ Start-Task "setup" "Installing the tools" @() })
$buildButton.Add_Click({
    if (-not $gameBox.Text) { [void][Windows.Forms.MessageBox]::Show("Pick your game first (step 2).", $Title); return }
    Start-Task "build" "Building" @("-Game", $gameBox.Text)
})
$sdButton.Add_Click({
    if (-not $driveBox.SelectedItem) { [void][Windows.Forms.MessageBox]::Show("Put the SD card in the computer, press Refresh and pick its drive.", $Title); return }
    Start-Task "sdcard" "Copying to the SD card" @("-Drive", $driveBox.SelectedItem, "-Game", $gameBox.Text)
})
$form.Add_FormClosing({
    Save-Settings
    if ($script:proc -and -not $script:proc.HasExited) {
        $a = [Windows.Forms.MessageBox]::Show("A task is still running. Close anyway? It keeps running in the background.", $Title, "YesNo", "Question")
        if ($a -ne "Yes") { $_.Cancel = $true }
    }
})

if (-not (Test-Path (Join-Path $Root "build_switch.sh"))) {
    [void][Windows.Forms.MessageBox]::Show("Start the builder from the SSX3NX folder (unpack the whole download first).", $Title)
}
[void]$form.ShowDialog()
