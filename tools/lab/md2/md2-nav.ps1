<#
Minecraft Dungeons II navigation and measurement row (agent gp, 15.10.2026). Starts the game from a private run dir
and drives it with scripted, screenshot-checked controller input: boot -> title -> first-run screens -> main menu ->
game -> first playable area. Derived from claude-row.ps1 and keeps its contract:
- Gates before launch: AC online, >=12 GiB free RAM, no other app.exe. Hold the lab lock (lab-lock.sh) around a row.
- Only a private copy under lab/runs is written (never lab/runs/md2, never the original dump).
- Input only through APS5_PAD_INPUT_FILE (in-process pad file, tools/pad_input.py format), no OS input.
- _sd is moved away each row unless -KeepSave; -SaveSeed seeds it from lab/saves/<dir>; -SaveAs keeps the final _sd.

Phases (-Phase):
  Steps    : run only the -Steps program (exploration). Default program: wait 60 s, one shot.
  Title    : boot to the title screen, then hold -HoldSeconds there (measurement window "Title").
  MainMenu : navigate to the main menu and hold -HoldSeconds there (window "MainMenu").
  Gameplay : navigate, PLAY, wait out the loading screen, skip the intro cutscene (Circle) when there is one, wait for the in-game HUD
             (state 'hud': minimap frame + health/ability bar on two probes in a row; without a HUD the next scene must answer the left
             stick instead), then walk for -WalkSeconds (window "Gameplay"; "Gameplay-unconfirmed" when no playable scene was confirmed).
A -Steps program is appended after the phase's navigation path (so MainMenu + "shot:x" works as well).

-ExtraEnv "K=V;K2=V2": the game's APS5_* environment. {RESULT} in a value stands for this row's result directory (for example
  APS5_SAMPLE_THREADS={RESULT}\threads.txt, so a sampler writes its file next to nav.log).
-SnapFiles "a.txt;b.collapsed.txt": files of the result directory the game rewrites while it runs (the samplers write every 20 s, cumulative);
  every new version is copied to snap/<stem>-<s>.<ext> (s = seconds from the process start to the file's write time, a trailing .collapsed of
  the stem dropped), so a steady-state window can be cut out later: lab/scripts/cpu-steady.sh <result dir> <libs dir> <from-s> <to-s>.

Step language (semicolon separated, fields colon separated, buttons joined with +):
  wait:S                          sleep S seconds (screens every -ShotEvery s, machine.csv every 5 s)
  shot:NAME                       save a screenshot shot-NNN-tSSS-NAME.png
  press:BTN[+BTN]:[MS]            pad pulse (MS = hold, default 400), then wait MS+700 ms so the game sees the release
  stick:LX,LY[,RX,RY]:MS[:BTN]    hold sticks (0..255, 128 centre, Y grows downward), MS <= 5000
  walk:SECS:DIRS:[SEGMS]          cycle through DIRS (up,down,left,right,ul,ur,dl,dr) holding the left stick SEGMS ms each
  nav:TARGET:TIMEOUT              state-driven navigation to title|accessibility|brightness|tos|autosave|mode|mainmenu|entering|loading|gameplay
                                  (a still screen no template knows, inside the first-run flow, gets Square, then Cross, alternating, after
                                  ~14 s, up to 4 times, with a screenshot unknown-N; after 90 s without progress the row stops with nav-stuck)
  until:S1|S2:TIMEOUT             wait until the screen classifies as one of the named states (lab/nav/md2/states.json)
  pressuntil:BTN:S1|S2:TIMEOUT[:EVERY]   press BTN every EVERY s (default 6) until a state shows
  spam:BTN[,BTN]:SECS:[EVERY]     press the buttons in turn every EVERY s (default 6) for SECS s (exploration autopilot)
  leave:S1|S2:TIMEOUT[:N]         wait until the screen is none of the states for N probes in a row (default 8) and is a real
                                  picture (not black, not a failed capture)
  skip:TIMEOUT                    the intro cutscene after the loading screen (state 'cutscene': letterbox bars + "Skip" with the Circle
                                  glyph): press Circle every 10 s (Cross when a dialog seems to hold the picture; 8 presses at most) until it
                                  is over, and wait for the in-game HUD (state 'hud', two probes in a row; no cutscene at all is fine): sets
                                  playable. Without a HUD within 25 s of the cutscene's end a move test (stick up for 3 s must change the
                                  picture) decides instead
  mwin:start:NAME / mwin:end:NAME measurement window around any steps (marks with [present] line counters in end.json)
  hold:SECS[:NAME]                measurement window: wait SECS inside it (marks with [present] line counters)
  mark:NAME                       log a mark
Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options touchpad release. Bit values are
those of core/libs/prx/libScePad/include/PadInputTypes.hpp (checked against InputCommandFile.hpp, mask 0x10fffe).

Screen states come from lab/nav/md2/states.json (text masks of a fixed region, see lab/scripts/md2-shot.py). To add one
look at a shot-*.png with the Read tool (1168x687) and run
  python -I lab/scripts/md2-shot.py learn <png> <state> <x0> <y0> <x1> <y1> [white|bright|gray|dark] [thr] [min]
then check it with `md2-shot.py match <png>`. A state named _x is an attribute (scored in the probe result, never the state).
After a row: python -I lab/scripts/md2-fps.py <result-dir> [--states]   (FPS, GPU busy, idle gaps, [gputime] classes)
             python -I lab/scripts/md2-sheet.py <result-dir> --from N --count 8   (contact sheet to look at)

Results go to a fresh lab/results/<Tag>-<timestamp> directory: stdout/stderr, nav.log (timeline with [present]
counters), pad.log, machine.csv, shot-*.png, tail-*.txt, end.json. Exit code: 0 = program finished, 2 = navigation
failed (state timeout, process exit, stall), 3 = gate refused.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-z0-9][a-z0-9-]*$')][string]$Tag,
    [ValidateSet('Steps','Title','MainMenu','Gameplay')][string]$Phase = 'Steps',
    [string]$Steps = '',
    [string]$RunDir = '',
    [string]$LibsDir = 'lab/libs-md2-main2',
    [ValidateRange(30,570)][int]$Seconds = 540,
    [int]$HoldSeconds = 90,
    [int]$WalkSeconds = 150,
    [string]$ExtraEnv = 'APS5_TEXTURE_CACHE_MIB=3072;APS5_PROFILE_DRAW=1;APS5_PROFILE_GPU=1;APS5_NO_PAD_INPUT=1',
    [int]$ShotEvery = 5,
    [string]$UeCmd = '',
    [switch]$KeepSave,
    [string]$SaveSeed = '',
    [ValidatePattern('^(md2-[a-z0-9][a-z0-9-]*)?$')][string]$SaveAs = '',
    [string]$SnapFiles = ''
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $RunDir) { $RunDir = Join-Path $workspace 'lab/runs/md2-gp' }
$RunDir = [IO.Path]::GetFullPath($RunDir)
if (-not $RunDir.StartsWith((Join-Path $workspace 'lab\runs'), [StringComparison]::OrdinalIgnoreCase)) { throw "run dir must be under lab/runs: $RunDir" }
if ($RunDir.TrimEnd('\','/') -ieq (Join-Path $workspace 'lab\runs\md2')) { throw 'lab/runs/md2 is the shared original: use your own run dir' }
$exe = Join-Path $RunDir 'app.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "no app.exe in $RunDir (relink first)" }
$py = Join-Path $env:LOCALAPPDATA 'Python/pythoncore-3.14-64/python.exe'
$shotScript = Join-Path $PSScriptRoot 'md2-shot.py'

$stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
$resultDir = Join-Path $workspace ("lab/results/{0}-{1}" -f $Tag, $stamp)
$null = New-Item -ItemType Directory -Path $resultDir -ErrorAction Stop
$padFile = Join-Path $resultDir 'pad.txt'
[IO.File]::WriteAllText($padFile, '')
$navLog = Join-Path $resultDir 'nav.log'
$inv = [Globalization.CultureInfo]::InvariantCulture

# ---------------------------------------------------------------- helpers (gates and pad as in claude-row.ps1)
function FreeGiB { return [double](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1048576 }
function AcOnline {
    Add-Type -AssemblyName System.Windows.Forms
    return [Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus.ToString() -eq 'Online'
}
function Gate([string]$Where) {
    $free = FreeGiB; $ac = AcOnline; $apps = @(Get-Process app -ErrorAction SilentlyContinue).Count
    Write-Host ("gate[{0}]: AC={1} freeGiB={2:N1} app.exe={3}" -f $Where, $ac, $free, $apps)
    if (-not $ac) { throw "gate: not on AC ($Where)" }
    if ($free -lt 12) { throw "gate: free RAM below 12 GiB ($Where)" }
    if ($apps -gt 0) { throw "gate: another game is running ($Where)" }
}
$Buttons = @{
    l3 = 0x2; r3 = 0x4; options = 0x8; up = 0x10; right = 0x20; down = 0x40; left = 0x80
    l2 = 0x100; r2 = 0x200; l1 = 0x400; r1 = 0x800; triangle = 0x1000; circle = 0x2000; cross = 0x4000
    square = 0x8000; touchpad = 0x100000; release = 0
}
$DirTable = @{
    up = @(128, 0); down = @(128, 255); left = @(0, 128); right = @(255, 128)
    ul = @(0, 0); ur = @(255, 0); dl = @(0, 255); dr = @(255, 255); none = @(128, 128)
}
$watch = [Diagnostics.Stopwatch]::StartNew()
function Now { return $watch.Elapsed.TotalSeconds }
$script:presentLines = 0; $script:gpuLines = 0; $script:errPos = 0L; $script:errCarry = ''
$script:lastState = ''; $script:lastTitleFps = $null; $script:lastFrame = $null; $script:lastFg = '<none>'
$script:lastPresentCount = 0; $script:lastPresentAt = 0.0; $script:stallLogged = $false
$script:marks = New-Object System.Collections.ArrayList
function Log([string]$msg) {
    $line = [string]::Format($inv, "[{0,6:F1}s pl={1} gl={2}] {3}", (Now), $script:presentLines, $script:gpuLines, $msg)
    Add-Content -LiteralPath $navLog -Value $line
    Write-Host $line
}
function UpdateLineCounts {
    $p = Join-Path $resultDir 'stderr.log'
    try {
        $fs = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $len = $fs.Length
            if ($len -le $script:errPos) { return }
            $null = $fs.Seek($script:errPos, [IO.SeekOrigin]::Begin)
            $buf = New-Object byte[] ($len - $script:errPos)
            $n = $fs.Read($buf, 0, $buf.Length)
            $script:errPos += $n
        } finally { $fs.Dispose() }
        $text = $script:errCarry + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        $lastNl = $text.LastIndexOf("`n")
        if ($lastNl -lt 0) { $script:errCarry = $text; return }
        $script:errCarry = $text.Substring($lastNl + 1)
        foreach ($line in $text.Substring(0, $lastNl).Split("`n")) {
            if ($line.StartsWith('[present]')) { $script:presentLines++ }
            elseif ($line.StartsWith('[gputime]')) { $script:gpuLines++ }
        }
    } catch {}
}
# The game's own log (stdout.log) names the world-generation steps: one "Tiles for SW.Area.<name>" block per area, the
# last one is DeepDark.Tutorial; log those as milestones so the timeline shows how far a load got.
$script:outPos = 0L; $script:outCarry = ''; $script:areaBlocks = 0; $script:equipLines = 0; $script:saveOpens = 0
function UpdateStdoutMilestones {
    $p = Join-Path $resultDir 'stdout.log'
    try {
        $fs = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $len = $fs.Length
            if ($len -le $script:outPos) { return }
            $null = $fs.Seek($script:outPos, [IO.SeekOrigin]::Begin)
            $buf = New-Object byte[] ($len - $script:outPos)
            $n = $fs.Read($buf, 0, $buf.Length)
            $script:outPos += $n
        } finally { $fs.Dispose() }
        $text = $script:outCarry + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        $lastNl = $text.LastIndexOf("`n")
        if ($lastNl -lt 0) { $script:outCarry = $text; return }
        $script:outCarry = $text.Substring($lastNl + 1)
        foreach ($line in $text.Substring(0, $lastNl).Split("`n")) {
            if ($line.StartsWith('################ Tiles for ')) {
                $script:areaBlocks++
                $name = ($line -replace '^################ Tiles for (\S+).*$', '$1')
                if ($script:areaBlocks -eq 1 -or $script:areaBlocks % 10 -eq 0 -or $name -like '*Tutorial*') { Log "stdout: area block $($script:areaBlocks) $name" }
            }
            elseif ($line.Contains('UEquipmentDisplayComponent::SetOverridingPlayerMesh')) { $script:equipLines++; if ($script:equipLines -eq 1) { Log 'stdout: first equipment display component' } }
            elseif ($line.Contains('path=/savedata0/')) { $script:saveOpens++ }
        }
    } catch {}
}
$padSequence = 0
function SendPad([int]$ButtonBits, [int]$Ms, [int[]]$Sticks) {
    if ($Ms -lt 1 -or $Ms -gt 5000) { throw "pad hold must be 1..5000 ms: $Ms" }
    $script:padSequence++
    $line = "{0} {1} {2}" -f $script:padSequence, $ButtonBits, $Ms
    if ($Sticks) { $line += ' ' + ($Sticks -join ' ') }
    $tmp = "$padFile.tmp"
    [IO.File]::WriteAllText($tmp, $line + "`n")
    for ($try = 0; $try -lt 40; $try++) {
        try { Move-Item -LiteralPath $tmp -Destination $padFile -Force; break }
        catch { if ($try -eq 39) { throw }; Start-Sleep -Milliseconds 25 }   # the game may have the file open for a read
    }
    Add-Content -LiteralPath (Join-Path $resultDir 'pad.log') -Value ("t={0:F1} {1}" -f (Now), $line)
}
function ButtonBits([string]$spec) {
    $bits = 0
    foreach ($name in $spec.ToLowerInvariant().Split('+')) {
        if (-not $Buttons.ContainsKey($name)) { throw "unknown button '$name'" }
        $bits = $bits -bor $Buttons[$name]
    }
    return $bits
}
$script:shotN = 0
$script:probeTimeouts = 0
# One capture + classification in a child process with a watchdog: a hung window must not hang the row.
function RunShotHelper([string[]]$ArgList, [int]$TimeoutMs) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $py
    $psi.Arguments = (($ArgList | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutMs)) { try { $p.Kill() } catch {}; return $null }
    return $outTask.Result
}
function Probe([string]$ShotName, [switch]$NoMatch) {
    $out = '-'
    if ($ShotName) { $script:shotN++; $out = Join-Path $resultDir ("shot-{0:D3}-t{1}{2}.png" -f $script:shotN, [int](Now), $(if ($ShotName -ne '+') { "-$ShotName" } else { '' })) }
    try {
        $helperArgs = @('-I', $shotScript, 'shot', $exe, $out); if ($NoMatch) { $helperArgs += '--no-match' }
        $json = RunShotHelper $helperArgs 25000
        if ($null -eq $json) { $script:probeTimeouts++; Log "probe timed out (25 s), count $($script:probeTimeouts)"; throw 'probe timeout' }
        $r = $json | ConvertFrom-Json
    } catch { $r = [pscustomobject]@{ kind = 'probe-error'; title = ''; fps = $null; frame = $null; state = ''; scores = $null } }
    if ($r.PSObject.Properties['fg'] -and $r.fg -ne $script:lastFg) { Log ("foreground window: '{0}'" -f $r.fg); $script:lastFg = $r.fg }
    if ($r.fps -ne $null) { $script:lastTitleFps = $r.fps }
    if ($r.frame -ne $null) { $script:lastFrame = $r.frame }
    if ($r.state -ne $script:lastState) {
        Log ("screen state: '{0}' -> '{1}' (kind {2}, title '{3}')" -f $script:lastState, $r.state, $r.kind, $r.title)
        $script:lastState = $r.state
    }
    return $r
}

# ---------------------------------------------------------------- row state
$ueFile = Join-Path $RunDir 'app0/uecommandline.txt'
$ueBackup = $null
$process = $null
$machine = $null
$stopReason = 'unknown'
$exitCode = 0
$minFree = [double]::MaxValue
$ramStrikes = 0
$lastSample = [DateTime]::MinValue
$lastShot = [DateTime]::MinValue
$script:shotEvery = $ShotEvery
$startedUtc = $null

class NavStop : System.Exception { NavStop([string]$m) : base($m) {} }

# Versions of the files named by -SnapFiles (see the header): copied to snap/ when their write time changed.
$script:snapSeen = @{}
function SnapshotFiles {
    if (-not $SnapFiles) { return }
    foreach ($name in ($SnapFiles -split ';')) {
        $name = $name.Trim()
        if (-not $name) { continue }
        try {
            $src = Join-Path $resultDir $name
            if (-not (Test-Path -LiteralPath $src)) { continue }
            $item = Get-Item -LiteralPath $src
            $ticks = $item.LastWriteTimeUtc.Ticks
            if ($script:snapSeen[$name] -eq $ticks) { continue }
            $secs = [int][math]::Floor(($item.LastWriteTimeUtc - $startedUtc).TotalSeconds)
            $stem = [IO.Path]::GetFileNameWithoutExtension($name) -replace '\.collapsed$', ''
            $dir = Join-Path $resultDir 'snap'
            if (-not (Test-Path -LiteralPath $dir)) { $null = New-Item -ItemType Directory -Path $dir }
            Copy-Item -LiteralPath $src -Destination (Join-Path $dir ('{0}-{1}{2}' -f $stem, $secs, [IO.Path]::GetExtension($name))) -Force
            $script:snapSeen[$name] = $ticks
        } catch {}
    }
}

function Tick {
    if ($process.HasExited) { throw [NavStop]::new('process-exit') }
    $elapsed = Now
    if ($elapsed -ge $Seconds) { throw [NavStop]::new('time-cap') }
    $now = [DateTime]::UtcNow
    if (($now - $lastSample).TotalSeconds -ge 5) {
        $script:lastSample = $now
        UpdateLineCounts
        UpdateStdoutMilestones
        SnapshotFiles
        if ($script:presentLines -ne $script:lastPresentCount) { $script:lastPresentCount = $script:presentLines; $script:lastPresentAt = $elapsed; $script:stallLogged = $false }
        elseif ($script:presentLines -gt 0 -and ($elapsed - $script:lastPresentAt) -ge 60 -and -not $script:stallLogged) {
            $script:stallLogged = $true
            Log ("stall: no [present] line for {0:F0} s (title FPS {1}, frame {2})" -f ($elapsed - $script:lastPresentAt), $script:lastTitleFps, $script:lastFrame)
        }
        $free = FreeGiB; if ($free -lt $script:minFree) { $script:minFree = $free }
        $ac = AcOnline
        $process.Refresh(); $appWs = $process.WorkingSet64 / 1GB
        $avail = ''; $commit = ''; $vram = ''
        try { $pm = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory -ErrorAction Stop; $avail = ($pm.AvailableBytes / 1GB).ToString('F2', $inv); $commit = ($pm.CommittedBytes / 1GB).ToString('F2', $inv) } catch {}
        try { $vram = ((& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>$null) | Select-Object -First 1).Trim() } catch {}
        $machine.WriteLine(('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11}' -f $now.ToString('o'), $elapsed.ToString('F1', $inv), $free.ToString('F2', $inv), $ac, $script:presentLines, 'x', $appWs.ToString('F2', $inv), $script:lastState, $(if ($null -ne $script:lastTitleFps) { ([double]$script:lastTitleFps).ToString('F2', $inv) } else { '' }), $avail, $commit, $vram))
        if (-not $ac) { throw [NavStop]::new('ac-lost') }
        if ($free -lt 0.5) { $script:ramStrikes++ } else { $script:ramStrikes = 0 }
        if ($script:ramStrikes -ge 3) { throw [NavStop]::new('ram-critical') }
    }
    if (($now - $lastShot).TotalSeconds -ge $script:shotEvery) {
        $script:lastShot = $now
        $null = Probe '+'
    }
}
function WaitSec([double]$s) {
    $end = (Now) + $s
    while ((Now) -lt $end) { Tick; Start-Sleep -Milliseconds 200 }
}
function Press([string]$spec, [int]$ms) {
    $bits = ButtonBits $spec
    SendPad $bits $ms $null
    Log ("press {0} {1} ms" -f $spec, $ms)
    WaitSec (($ms + 700) / 1000.0)
}
function Stick([int[]]$axes, [int]$ms, [int]$bits) {
    if ($axes.Count -eq 2) { $axes = @($axes[0], $axes[1], 128, 128) }
    SendPad $bits $ms $axes
}
function WaitState([string[]]$states, [double]$timeout, [switch]$Negate, [int]$Confirm = 1) {
    $deadline = (Now) + $timeout
    $run = 0
    while ($true) {
        Tick
        $r = Probe ''
        $hit = $states -contains $r.state
        if ($Negate) {
            # Leaving a screen counts only for a real picture: a black frame or a failed capture between two loading
            # screens must not end the wait, and the new picture has to stay for $Confirm probes in a row.
            $hit = (-not $hit) -and ($r.kind -notin @('black', 'nowindow', 'unreadable', 'probe-error'))
        }
        if ($hit) { $run++ } else { $run = 0 }
        if ($run -ge $Confirm) { return $r }
        if ((Now) -ge $deadline) { throw [NavStop]::new(("nav-timeout: waiting {0}for [{1}], screen is '{2}'" -f $(if ($Negate) { 'to leave ' } else { '' }), ($states -join '|'), $r.state)) }
        Start-Sleep -Milliseconds 700
    }
}
# State-driven navigation: probe the screen, act per state (rules), stop when the target state shows (a later one is an overshoot).
$StateOrder = @('title', 'accessibility', 'brightness', 'tos', 'autosave', 'mode', 'mainmenu', 'entering', 'loading', 'gameplay')
$Rules = @{
    # Glyphs seen on screen: Accessibility CONTINUE, Brightness CONTINUE (default 100 % kept) and Terms "I AGREE TO ALL" are
    # Square, the rest Cross.
    'title'         = @{ keys = @('cross');          retry = 12 }
    'accessibility' = @{ keys = @('square');         retry = 10 }
    'brightness'    = @{ keys = @('square');         retry = 10 }
    'tos'           = @{ keys = @('square');         retry = 10 }
    'mode'          = @{ keys = @('right', 'cross'); retry = 16; gap = 3 }   # choose "Play offline", then Select
    'mainmenu'      = @{ keys = @('cross');          retry = 20; onlyBeyond = $true }   # PLAY, only when the target is later
}
function SigDiff([string]$a, [string]$b) {
    $n = 0
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $n++ } }
    return $n
}
function Navigate([string]$target, [double]$timeout) {
    $targetRank = [array]::IndexOf($StateOrder, $target)
    if ($targetRank -lt 0) { throw "unknown nav target '$target'" }
    $mainMenuRank = [array]::IndexOf($StateOrder, 'mainmenu')
    $deadline = (Now) + $timeout
    $lastAct = @{}
    $stable = $null; $stableCount = 0
    # An unknown still screen in the first-run flow (a screen no template knows, e.g. a settings page that needs CONTINUE):
    # after ~14 s without any change press Square (the first-run pages are Accessibility / Brightness / Terms style pages),
    # keep a screenshot, and give up with nav-stuck after 90 s so a row does not sit on it until its time cap.
    $lastKnownRank = -1; $lastKnownName = ''
    $prevSig = $null; $still = 0
    $unknownStart = $null; $unknownPresses = 0; $unknownLastPress = 0.0; $pressSig = $null; $blackStart = $null
    Log "nav to '$target' (timeout $timeout s)"
    while ($true) {
        Tick
        $r = Probe ''
        $s = [string]$r.state
        if ($s -eq $stable) { $stableCount++ } else { $stable = $s; $stableCount = 1 }
        $rank = [array]::IndexOf($StateOrder, $s)
        if ($s -and $rank -ge 0) { $lastKnownRank = [math]::Max($lastKnownRank, $rank); $lastKnownName = $s; $unknownStart = $null; $unknownPresses = 0 }
        $sig = [string]$r.sig
        if ($sig -and $prevSig -and $sig.Length -eq $prevSig.Length -and (SigDiff $sig $prevSig) -le 1) { $still++ } else { $still = 0 }
        $prevSig = $sig
        # the screen changed after a press of the fallback: progress, so the stuck clock starts again
        if ($pressSig -and $sig -and $sig.Length -eq $pressSig.Length -and (SigDiff $sig $pressSig) -gt 1) { $unknownStart = $null; $unknownPresses = 0; $pressSig = $null }
        if ($rank -eq $targetRank -and $s) { Log "nav reached '$s'"; return $r }
        # the loading screen can be too short to catch: the scene it leads to is already up
        if ($target -eq 'loading' -and $s -in @('cutscene', 'hud')) { Log "nav reached '$s' (no loading screen seen)"; return $r }
        if ($rank -gt $targetRank) { throw [NavStop]::new(("nav-overshoot: target '{0}' but the screen is '{1}'" -f $target, $s)) }
        if ((Now) -ge $deadline) { throw [NavStop]::new(("nav-timeout: target '{0}', screen is '{1}'" -f $target, $s)) }
        # a page with a black background (the Brightness page is 90 % black) is a picture; a fully black frame is not
        $realPicture = ($r.kind -notin @('nowindow', 'unreadable', 'probe-error')) -and -not ($r.PSObject.Properties['black'] -and [double]$r.black -ge 0.995)
        $inFlow = ($lastKnownRank -ge 0 -and $lastKnownRank -lt $mainMenuRank)
        if (-not $s -and -not $realPicture -and $r.kind -notin @('nowindow', 'unreadable', 'probe-error') -and $inFlow) {
            if ($null -eq $blackStart) { $blackStart = Now }
            if (((Now) - $blackStart) -ge 90) { throw [NavStop]::new(("nav-stuck: black screen for {0:F0} s after '{1}'" -f ((Now) - $blackStart), $lastKnownName)) }
        } else { $blackStart = $null }
        if (-not $s -and $realPicture -and $still -ge 6 -and $inFlow) {
            if ($null -eq $unknownStart) { $unknownStart = Now; $unknownPresses = 0; $unknownLastPress = 0.0 }
            if (((Now) - $unknownStart) -ge 90) {
                throw [NavStop]::new(("nav-stuck: still screen no template knows for {0:F0} s after '{1}' (shots unknown-*)" -f ((Now) - $unknownStart), $lastKnownName))
            }
            if ($unknownPresses -lt 4 -and ((Now) - $unknownLastPress) -ge 12) {
                $unknownPresses++; $unknownLastPress = Now; $pressSig = $sig
                $null = Probe ("unknown-{0}" -f $unknownPresses)
                # Square first (the first-run pages), then Cross (a main menu or a dialog the flow has not met), alternating
                $unknownKey = @('square', 'cross')[($unknownPresses - 1) % 2]
                Log ("unknown still screen after '{0}' ({1} probes unchanged): pressing {2}, attempt {3}" -f $lastKnownName, $still, $unknownKey, $unknownPresses)
                Press $unknownKey 400
                $prevSig = $null; $still = 0
                continue
            }
        }
        if ($s -and $Rules.ContainsKey($s) -and $stableCount -ge 2) {
            $rule = $Rules[$s]
            $due = (-not $lastAct.ContainsKey($s)) -or (((Now) - $lastAct[$s]) -ge $rule.retry)
            if ($due -and -not ($rule.onlyBeyond -and $rank -ge $targetRank)) {
                $lastAct[$s] = Now
                $keys = $rule.keys
                # "How would you like to play": the card with the focus has pure white title text (attribute states
                # _mode_on / _mode_off); move to "Play offline" only when it is not already there.
                if ($s -eq 'mode' -and $r.scores -and $r.scores.PSObject.Properties['_mode_off'] -and $r.scores._mode_off -ge 0.6) { $keys = @('cross') }
                foreach ($k in $keys) { Press $k 400; if ($rule.gap) { WaitSec ($rule.gap - 1.1) } }
                continue
            }
        }
        Start-Sleep -Milliseconds 800
    }
}
function Hold([int]$secs, [string]$name) {
    UpdateLineCounts
    $m = [ordered]@{ name = $name; startS = [math]::Round((Now), 1); startPresentLines = $script:presentLines; startGpuLines = $script:gpuLines; startState = $script:lastState }
    Log "hold-start $name for $secs s"
    $save = $script:shotEvery; $script:shotEvery = 20
    try { WaitSec $secs } finally { $script:shotEvery = $save }
    UpdateLineCounts
    $m.endS = [math]::Round((Now), 1); $m.endPresentLines = $script:presentLines; $m.endGpuLines = $script:gpuLines
    $null = Probe ''
    $m.endState = $script:lastState; $m.stateStable = ($m.startState -eq $m.endState)
    $null = $script:marks.Add($m)
    Log "hold-end $name (screen '$($m.startState)' -> '$($m.endState)')"
}
# Measurement window around any steps: mwin:start:NAME ... mwin:end:NAME (a window still open when the row ends is
# closed at the end and flagged truncated).
$script:openMarks = @{}
$script:shotEveryOutside = $ShotEvery
$script:playable = $true   # set by the skip step: false while the intro cutscene or a scene that ignores the stick is on screen
$script:playableBy = ''    # 'hud' (state hud on two probes in a row) or 'stick' (move test), set with playable
function WindowStart([string]$name) {
    UpdateLineCounts
    # fewer screenshots inside a measurement window (one capture and classification costs the game a little)
    if ($script:openMarks.Count -eq 0) { $script:shotEveryOutside = $script:shotEvery; $script:shotEvery = [math]::Max($script:shotEvery, 20) }
    # a "Gameplay" window only counts when the skip step confirmed a scene that answers the stick
    $effective = if ($name -eq 'Gameplay' -and -not $script:playable) { 'Gameplay-unconfirmed' } else { $name }
    $script:openMarks[$name] = [ordered]@{ name = $effective; startS = [math]::Round((Now), 1); startPresentLines = $script:presentLines; startGpuLines = $script:gpuLines; startState = $script:lastState }
    Log "window-start $effective"
}
# After the loading screen: the world opens with an in-engine intro cutscene (letterbox, "Skip" + Circle bottom right, state
# 'cutscene'). Press Circle until it is over (Cross when a confirmation dialog seems to hold the picture), then check that the
# next scene answers the left stick (the picture changes) before a measurement window starts. Sets $script:playable.
function MoveTest {
    $r0 = Probe ''
    WaitSec 2.5
    $rIdle = Probe ''
    SendPad 0 3500 @(128, 0, 128, 128)
    WaitSec 3.2
    $r1 = Probe 'move-test'
    SendPad 0 100 @(128, 128, 128, 128)
    $z = [string]$r0.sig; $a = [string]$rIdle.sig; $b = [string]$r1.sig
    $d = if ($a -and $b -and $a.Length -eq $b.Length) { SigDiff $a $b } else { -1 }
    # logged for calibration only: how much the picture changes by itself over the 2.5 s before the stick moves
    $idle = if ($z -and $a -and $z.Length -eq $a.Length) { SigDiff $z $a } else { -1 }
    $script:playable = ($d -ge 3) -and ([string]$r1.state -ne 'cutscene')
    Log ("move test: stick up for 3 s changed {0} of 36 picture cells (without the stick, 2.5 s before: {1}), screen '{2}': playable {3}" -f $d, $idle, $r1.state, $script:playable)
}
# The in-game HUD (state 'hud': minimap frame + health/ability bar) on two probes in a row within $timeout s.
function WaitHud([double]$timeout) {
    $deadline = (Now) + $timeout
    $run = 0
    while ((Now) -lt $deadline) {
        Tick
        $r = Probe ''
        if ([string]$r.state -eq 'hud') { $run++; if ($run -ge 2) { return $true } } else { $run = 0 }
        Start-Sleep -Milliseconds 700
    }
    return $false
}
# A scene with the HUD is the game. Only when no HUD shows up (another HUD layout, a template that no longer fits) the stick test decides.
function ConfirmPlayable {
    if (WaitHud 25) {
        $null = Probe 'hud-seen'
        $script:playable = $true; $script:playableBy = 'hud'
        Log 'HUD on screen: playable'
        return
    }
    Log 'no HUD within 25 s: testing the stick instead'
    MoveTest
    if (-not $script:playable) {
        $null = Probe 'move-test-failed'
        Log 'no response to the stick: pressing cross once (a tutorial prompt?) and testing again'
        Press 'cross' 600
        WaitSec 3
        MoveTest
    }
    if ($script:playable) { $script:playableBy = 'stick' }
}
function SkipCutscene([double]$timeout) {
    $deadline = (Now) + $timeout
    $sequence = @('circle', 'circle', 'cross', 'circle', 'circle', 'square', 'circle', 'circle')
    $presses = 0; $lastPress = -100.0; $sawCutscene = $false; $clear = 0; $dialogPresses = 0; $hudRun = 0
    $prevSig = $null; $still = 0
    Log "skip: looking for the intro cutscene (timeout $timeout s)"
    while ((Now) -lt $deadline) {
        Tick
        $r = Probe ''
        $s = [string]$r.state
        $sig = [string]$r.sig
        if ($sig -and $prevSig -and $sig.Length -eq $prevSig.Length -and (SigDiff $sig $prevSig) -le 1) { $still++ } else { $still = 0 }
        $prevSig = $sig
        $real = ($r.kind -notin @('nowindow', 'unreadable', 'probe-error')) -and -not ($r.PSObject.Properties['black'] -and [double]$r.black -ge 0.995)
        # the HUD on two probes in a row: the cutscene is over (or there was none) and the game is up
        if ($s -eq 'hud') {
            $hudRun++
            if ($hudRun -ge 2) {
                if ($sawCutscene) { Log ("cutscene over after {0} presses: HUD on screen" -f $presses) } else { Log 'no cutscene: HUD on screen' }
                $null = Probe 'hud-seen'
                $script:playable = $true; $script:playableBy = 'hud'
                return
            }
            Start-Sleep -Milliseconds 700
            continue
        }
        $hudRun = 0
        if ($s -eq 'cutscene') {
            if (-not $sawCutscene) { $sawCutscene = $true; $null = Probe 'cutscene-seen'; Log 'cutscene on screen' }
            $clear = 0
            if (((Now) - $lastPress) -ge 10 -and $presses -lt $sequence.Count) {
                $btn = $sequence[$presses]; $presses++; $lastPress = Now
                Log ("cutscene: pressing {0} (attempt {1})" -f $btn, $presses)
                Press $btn $(if ($btn -eq 'circle') { 2000 } else { 600 })
                $prevSig = $null; $still = 0
                continue
            }
        } elseif ($s -in @('loading', 'entering')) {
            $clear = 0
        } elseif ($real) {
            # no longer the cutscene: a picture that stays still right after a press may be a confirmation dialog
            if ($sawCutscene -and $presses -gt 0 -and $still -ge 3 -and $dialogPresses -lt 2 -and ((Now) - $lastPress) -ge 6) {
                $dialogPresses++; $lastPress = Now
                $null = Probe ("skip-dialog-{0}" -f $dialogPresses)
                Log 'still unknown picture after a skip press (confirmation dialog?): pressing cross'
                Press 'cross' 600
                $prevSig = $null; $still = 0
                continue
            }
            $clear++
            if ($sawCutscene -and $clear -ge 6) {
                Log ("cutscene over after {0} presses, screen '{1}'" -f $presses, $s)
                $null = Probe 'after-cutscene'
                WaitSec 5   # let a fade or a camera move after the cutscene settle before the stick test
                ConfirmPlayable
                return
            }
            if (-not $sawCutscene -and $clear -ge 14) {
                Log "no cutscene appeared; the scene stayed for $clear probes"
                ConfirmPlayable
                return
            }
        } else { $clear = 0 }
        Start-Sleep -Milliseconds 700
    }
    $script:playable = $false; $script:playableBy = ''
    Log ("skip: timeout after {0:F0} s, cutscene seen {1}, presses {2}: no confirmed playable scene" -f $timeout, $sawCutscene, $presses)
}
function WindowEnd([string]$name, [bool]$truncated = $false) {
    $m = $script:openMarks[$name]
    if ($null -eq $m) { throw "window '$name' was not started" }
    UpdateLineCounts
    $m.endS = [math]::Round((Now), 1); $m.endPresentLines = $script:presentLines; $m.endGpuLines = $script:gpuLines
    if (-not $truncated) { $null = Probe '' }
    $m.endState = $script:lastState; $m.stateStable = ($m.startState -eq $m.endState); $m.truncated = $truncated
    $null = $script:marks.Add($m)
    $script:openMarks.Remove($name)
    if ($script:openMarks.Count -eq 0) { $script:shotEvery = $script:shotEveryOutside }
    Log "window-end $name (screen '$($m.startState)' -> '$($m.endState)'$(if ($truncated) { ', truncated' }))"
}
function Walk([double]$secs, [string[]]$dirs, [int]$segMs) {
    $end = (Now) + $secs; $i = 0
    Log ("walk {0} s [{1}] segments {2} ms" -f $secs, ($dirs -join ','), $segMs)
    while ((Now) -lt $end) {
        $d = $dirs[$i % $dirs.Count]; $i++
        if (-not $DirTable.ContainsKey($d)) { throw "unknown direction '$d'" }
        Stick ($DirTable[$d] + @(128, 128)) $segMs 0
        $segEnd = (Now) + ($segMs - 300) / 1000.0
        while ((Now) -lt $segEnd -and (Now) -lt $end) { Tick; Start-Sleep -Milliseconds 200 }
    }
    SendPad 0 100 @(128, 128, 128, 128)
}
function RunStep([string]$step) {
    $step = $step.Trim()
    if (-not $step) { return }
    $p = $step.Split(':')
    switch ($p[0]) {
        'wait' { WaitSec ([double]::Parse($p[1], $inv)) }
        'shot' { Tick; $null = Probe $p[1]; Log "shot $($p[1])" }
        'press' { Press $p[1] $(if ($p.Count -gt 2 -and $p[2]) { [int]$p[2] } else { 400 }) }
        'stick' {
            $axes = [int[]]($p[1].Split(',') | ForEach-Object { [int]$_ })
            $bits = if ($p.Count -gt 3 -and $p[3]) { ButtonBits $p[3] } else { 0 }
            Stick $axes ([int]$p[2]) $bits
            Log ("stick {0} {1} ms" -f $p[1], $p[2])
            WaitSec (([int]$p[2]) / 1000.0)
        }
        'walk' { Walk ([double]::Parse($p[1], $inv)) ($p[2].Split(',')) $(if ($p.Count -gt 3 -and $p[3]) { [int]$p[3] } else { 3500 }) }
        'nav' { $r = Navigate $p[1] ([double]$p[2]) }
        'until' { $r = WaitState ($p[1].Split('|')) ([double]$p[2]); Log "reached [$($p[1])] as '$($r.state)'" }
        'pressuntil' {
            $every = if ($p.Count -gt 4 -and $p[4]) { [double]$p[4] } else { 6 }
            $states = $p[2].Split('|'); $deadline = (Now) + [double]$p[3]
            while ($true) {
                Tick; $r = Probe ''
                if ($states -contains $r.state) { Log "reached [$($p[2])] as '$($r.state)'"; break }
                if ((Now) -ge $deadline) { throw [NavStop]::new(("nav-timeout: pressing {0} until [{1}], screen is '{2}'" -f $p[1], $p[2], $r.state)) }
                Press $p[1] 400
                WaitSec $every
            }
        }
        'spam' {
            $btns = $p[1].Split(','); $end = (Now) + [double]$p[2]; $every = if ($p.Count -gt 3 -and $p[3]) { [double]$p[3] } else { 6 }; $i = 0
            while ((Now) -lt $end) { Press $btns[$i % $btns.Count] 400; $i++; WaitSec ([math]::Max(0.1, $every - 1.1)) }
        }
        'leave' {
            $confirm = if ($p.Count -gt 3 -and $p[3]) { [int]$p[3] } else { 8 }
            $t0 = Now
            $r = WaitState ($p[1].Split('|')) ([double]$p[2]) -Negate -Confirm $confirm
            Log ("left [{0}] after {1:F0} s: screen is '{2}' (kind {3})" -f $p[1], ((Now) - $t0), $r.state, $r.kind)
        }
        'mwin' { if ($p[1] -eq 'start') { WindowStart $p[2] } else { WindowEnd $p[2] } }
        'skip' { SkipCutscene ([double]$p[1]) }
        'hold' { Hold ([int]$p[1]) $(if ($p.Count -gt 2 -and $p[2]) { $p[2] } else { 'hold' }) }
        'mark' { UpdateLineCounts; Log "mark $($p[1])" }
        default { throw "unknown step '$($p[0])'" }
    }
}

# ---------------------------------------------------------------- programs (filled in from the exploration)
$programs = @{
    Steps    = 'wait:60;shot:boot'
    Title    = 'until:title:240;hold:{HOLD}:Title'
    MainMenu = 'nav:mainmenu:420;hold:{HOLD}:MainMenu'
    # Boot to the main menu, PLAY (the nav rule presses it because the target is later), wait for the loading screen and for
    # it to go away, let the scene settle, then walk with the left stick for -WalkSeconds inside the measurement window
    # "Gameplay". Whether the scene after the loading screen is playable is for the screenshots to say.
    Gameplay = 'nav:loading:420;mark:loading-started;leave:loading:420;mark:world-loaded;skip:200;wait:10;shot:after-skip;mwin:start:Gameplay;walk:{WALK}:up,right,up,left:4000;mwin:end:Gameplay;shot:gameplay-end'
}
$program = $programs[$Phase]
if ($Phase -eq 'Steps' -and $Steps) { $program = '' }
if ($Steps) { $program = (($program, $Steps) | Where-Object { $_ }) -join ';' }
$program = $program.Replace('{HOLD}', "$HoldSeconds").Replace('{WALK}', "$WalkSeconds")

# ---------------------------------------------------------------- the row
try {
    try {
        Gate 'admission'
        if (-not $KeepSave) {
            $sd = Join-Path $RunDir '_sd'
            $savedTo = Join-Path $resultDir 'previous-_sd'
            if (Test-Path $sd) { Move-Item -LiteralPath $sd -Destination $savedTo }
            $null = New-Item -ItemType Directory -Path $sd
            if ($SaveSeed) {
                $seed = Join-Path $workspace "lab/saves/$SaveSeed"
                if (-not (Test-Path $seed)) { throw "save seed not found: $seed" }
                Copy-Item -Path (Join-Path $seed '*') -Destination $sd -Recurse -Force
            }
        }
        if ($UeCmd) {
            $ueBackup = [IO.File]::ReadAllText($ueFile)
            Copy-Item -LiteralPath $ueFile -Destination (Join-Path $resultDir 'uecommandline.before.txt')
            [IO.File]::WriteAllText($ueFile, ($ueBackup.Trim() + ' ' + $UeCmd + "`r`n"))
        }
        $ftLog = Join-Path $RunDir 'frame-timing.log'
        if (Test-Path $ftLog) { Remove-Item -LiteralPath $ftLog -Force }

        # Scrub inherited APS5_* so a row is defined only by its declared env.
        foreach ($name in @([Environment]::GetEnvironmentVariables().Keys)) {
            if ($name -like 'APS5_*') { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        }
        $env:VK_LOADER_DRIVERS_DISABLE = '*amd*'
        $env:APS5_BATCH_WRITEBACK_WINDOWS = '0'
        $env:APS5_PAD_INPUT_FILE = $padFile
        if ($ExtraEnv) {
            foreach ($pair in $ExtraEnv -split ';') {
                if ($pair -match '^([^=]+)=(.*)$') { Set-Item "Env:$($Matches[1])" ($Matches[2].Replace('{RESULT}', $resultDir)) }
            }
        }
        Gate 'prelaunch'
    } catch {
        Write-Host "refused: $($_.Exception.Message)"
        $stopReason = "gate: $($_.Exception.Message)"; $exitCode = 3
        throw [NavStop]::new('gate')
    }
    $startedUtc = [DateTime]::UtcNow
    $process = Start-Process -FilePath $exe -WorkingDirectory $RunDir -PassThru `
        -RedirectStandardOutput (Join-Path $resultDir 'stdout.log') `
        -RedirectStandardError (Join-Path $resultDir 'stderr.log')
    $watch.Restart()
    Write-Host "started pid $($process.Id) tag $Tag phase $Phase seconds $Seconds"
    $machine = New-Object IO.StreamWriter((Join-Path $resultDir 'machine.csv'), $false, (New-Object Text.UTF8Encoding($false)))
    $machine.WriteLine('utc,elapsedSeconds,freeGiB,acOnline,flips,probeKind,appWsGiB,state,titleFps,availGiB,commitGiB,vramMiB'); $machine.AutoFlush = $true
    Log "program: $program"
    foreach ($step in $program.Split(';')) { RunStep $step }
    $stopReason = 'program-done'
} catch [NavStop] {
    $msg = $_.Exception.Message
    if ($msg -ne 'gate') {
        $stopReason = $msg
        if ($msg -notin @('time-cap')) { $exitCode = 2 }
        if ($msg -eq 'time-cap') { $exitCode = 2 }
    }
    Write-Host "stop: $stopReason"
} catch {
    $stopReason = "error: $($_.Exception.Message)"; $exitCode = 2
    Write-Host $stopReason
    Write-Host $_.ScriptStackTrace
} finally {
    try { if ($process) { Log "end: $stopReason" } } catch {}
    if ($process -and -not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) {
            try { $process.Kill() } catch { try { Stop-Process -Id $process.Id -Force -ErrorAction Stop } catch { & taskkill /F /T /PID $process.Id 2>$null | Out-Null } }
            try { $process.WaitForExit(10000) | Out-Null } catch {}
        }
    }
    if ($process) { foreach ($k in @($script:openMarks.Keys)) { try { WindowEnd $k $true } catch {} } }
    if ($process) { SnapshotFiles }   # the samplers write once more at an orderly exit
    if ($machine) { $machine.Dispose() }
    if ($null -ne $ueBackup) { [IO.File]::WriteAllText($ueFile, $ueBackup) }
}
if (-not $process) {
    Write-Host "no game started: $stopReason"
    try { Remove-Item -LiteralPath $resultDir -Recurse -Force } catch {}
    exit $exitCode
}

# ---------------------------------------------------------------- after the run
foreach ($name in 'stderr', 'stdout') {
    $f = Join-Path $resultDir "$name.log"
    if (Test-Path $f) {
        try { Get-Content -LiteralPath $f -Tail 80 | Set-Content -LiteralPath (Join-Path $resultDir "tail-$name.txt") } catch {}
    }
}
try {
    # Crash and stop evidence: layer-level fatal markers from both logs (first 40 hits).
    $hits = Select-String -Path (Join-Path $resultDir 'stderr.log'), (Join-Path $resultDir 'stdout.log') `
        -Pattern 'what\(\)|not implemented|FATAL|terminate called|Unhandled|access violation|Assertion|ACCESS_VIOLATION|Stack trace|backtrace' -ErrorAction SilentlyContinue |
        Select-Object -First 40
    if ($hits) { $hits | ForEach-Object { "{0}:{1}: {2}" -f (Split-Path $_.Path -Leaf), $_.LineNumber, $_.Line.Substring(0, [math]::Min(400, $_.Line.Length)) } | Set-Content -LiteralPath (Join-Path $resultDir 'errors.txt') }
} catch {}
$saved = ''
if ($SaveAs) {
    $dst = Join-Path $workspace "lab/saves/$SaveAs"
    if (Test-Path $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    $null = New-Item -ItemType Directory -Path $dst
    if (@(Get-ChildItem -LiteralPath (Join-Path $RunDir '_sd') -Force -ErrorAction SilentlyContinue).Count -gt 0) {
        Copy-Item -Path (Join-Path $RunDir '_sd/*') -Destination $dst -Recurse -Force
    }
    $saved = $dst
    Write-Host "save kept as lab/saves/$SaveAs"
}
$agcHash = ''
if ($LibsDir) {
    $agc = Join-Path ([IO.Path]::GetFullPath((Join-Path $workspace $LibsDir))) 'libSceAgcDriver.prx'
    if (Test-Path $agc) { $agcHash = (Get-FileHash -LiteralPath $agc -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$end = [ordered]@{
    tag = $Tag; phase = $Phase; steps = $Steps; program = $program; extraEnv = $ExtraEnv; ueCmd = $UeCmd
    startedUtc = $startedUtc.ToString('o'); endedUtc = [DateTime]::UtcNow.ToString('o')
    stopReason = $stopReason; exitCode = $process.ExitCode; lastState = $script:lastState
    presentLines = $script:presentLines; gpuLines = $script:gpuLines; playable = $script:playable; playableBy = $script:playableBy; marks = @($script:marks)
    minFreeGiB = [math]::Round($minFree, 2); saveSeed = $SaveSeed; savedAs = $saved
    appExeSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    libSceAgcDriverSha256 = $agcHash; libsDir = $LibsDir
}
[IO.File]::WriteAllText((Join-Path $resultDir 'end.json'), ($end | ConvertTo-Json -Depth 5))
Write-Host ("done: {0} stop={1} presentLines={2} resultDir={3}" -f $Tag, $stopReason, $script:presentLines, $resultDir)
exit $exitCode
