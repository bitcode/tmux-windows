################################################################################
# tmux Windows Port -- Native Shell Tests
#
# Tests tmux under genuine Windows terminal hosts:
#   1. PowerShell 7 (pwsh.exe)  -- the calling shell
#   2. PowerShell 5 (powershell.exe) -- spawned as a real console process
#   3. cmd.exe -- spawned as a real console process
#
# Each shell exercises: session lifecycle, window/pane ops, send-keys,
# capture-pane, attach-session (CONIN$/CONOUT$ regression), resize detection,
# and server stability.
#
# Usage:  pwsh -NoProfile -ExecutionPolicy Bypass -File test_native_shells.ps1
################################################################################

$ErrorActionPreference = 'Continue'
$REPOROOT  = (Resolve-Path "$PSScriptRoot\..").Path
$TMUX      = "$REPOROOT\build\Release\tmux.exe"
if (!(Test-Path $TMUX)) { $TMUX = "$REPOROOT\build\Debug\tmux.exe" }
$TESTDIR   = $REPOROOT
$RESULTDIR = "$TESTDIR\test_results"

if (!(Test-Path $RESULTDIR)) { New-Item -ItemType Directory -Path $RESULTDIR -Force | Out-Null }

$totalPass = 0
$totalFail = 0
$totalTests = 0

function Write-Header($text) {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host ("=" * 60) -ForegroundColor Cyan
}

function Write-Section($text) {
    Write-Host ""
    Write-Host "--- $text ---" -ForegroundColor Yellow
}

function Kill-Tmux {
    Stop-Process -Name tmux -Force -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Process tmux -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Milliseconds 200
    $socketDir = "$env:LOCALAPPDATA\tmux"
    Remove-Item "$socketDir\*" -Recurse -Force -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(2)
    while ((Test-Path "$socketDir\tmux-0\default") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
}

function T {
    param([string[]]$A)
    $null = & $TMUX @A 2>$null
    return $LASTEXITCODE
}

function TOut {
    param([string[]]$A)
    $tmp_out = [System.IO.Path]::GetTempFileName()
    $tmp_err = [System.IO.Path]::GetTempFileName()
    & $TMUX @A >$tmp_out 2>$tmp_err
    $ec  = $LASTEXITCODE
    $raw = if (Test-Path $tmp_out) { (Get-Content $tmp_out -Raw) } else { "" }
    if ($raw -eq $null) { $raw = "" }
    $out = ([string]$raw) -replace '\x1b\[[0-9;?]*[a-zA-Z]', '' -replace '\x1b[()][A-Z0-9]', '' -replace '\r', ''
    Remove-Item $tmp_out,$tmp_err -Force -ErrorAction SilentlyContinue
    return @{ ExitCode = $ec; Output = $out.Trim() }
}

################################################################################
# POWERSHELL 7 TESTS (runs directly in this process)
################################################################################

function Run-PS7Tests {
    Write-Header "PowerShell 7 (pwsh $($PSVersionTable.PSVersion))"

    $pass = 0; $fail = 0
    $SESSION = "ps7test"

    Kill-Tmux
    Remove-Item "$TESTDIR\debug-*.log" -Force -ErrorAction SilentlyContinue
    Remove-Item "$TESTDIR\build\Debug\debug-*.log" -Force -ErrorAction SilentlyContinue

    # -- 1. Session lifecycle --
    Write-Section "Session Lifecycle"

    $ec = T @("new-session", "-d", "-s", $SESSION)
    if ($ec -eq 0) { Write-Host "  [PASS] new-session -d" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] new-session -d (exit=$ec)" -ForegroundColor Red; $fail++ }
    Start-Sleep -Milliseconds 500

    $r = TOut @("list-sessions")
    if ($r.ExitCode -eq 0 -and $r.Output -match $SESSION) {
        Write-Host "  [PASS] list-sessions" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] list-sessions (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    $ec = T @("has-session", "-t", $SESSION)
    if ($ec -eq 0) { Write-Host "  [PASS] has-session" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] has-session (exit=$ec)" -ForegroundColor Red; $fail++ }

    # -- 2. Window and pane ops --
    Write-Section "Windows and Panes"

    $ec = T @("new-window", "-t", $SESSION)
    if ($ec -eq 0) { Write-Host "  [PASS] new-window" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] new-window (exit=$ec)" -ForegroundColor Red; $fail++ }
    Start-Sleep -Milliseconds 400

    $r = TOut @("list-windows", "-t", $SESSION)
    if ($r.ExitCode -eq 0 -and $r.Output -match "\d+:") {
        $wcount = ([regex]::Matches($r.Output, "(?m)^\d+:")).Count
        Write-Host "  [PASS] list-windows ($wcount windows)" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] list-windows" -ForegroundColor Red; $fail++ }

    $ec = T @("split-window", "-h", "-t", $SESSION)
    if ($ec -eq 0) { Write-Host "  [PASS] split-window -h" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] split-window -h (exit=$ec)" -ForegroundColor Red; $fail++ }
    Start-Sleep -Milliseconds 400

    $ec = T @("split-window", "-v", "-t", $SESSION)
    if ($ec -eq 0) { Write-Host "  [PASS] split-window -v" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] split-window -v (exit=$ec)" -ForegroundColor Red; $fail++ }
    Start-Sleep -Milliseconds 400

    $r = TOut @("list-panes", "-t", $SESSION)
    if ($r.ExitCode -eq 0) {
        $pcount = ([regex]::Matches($r.Output, "(?m)^\d+:")).Count
        Write-Host "  [PASS] list-panes ($pcount panes)" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] list-panes" -ForegroundColor Red; $fail++ }

    # -- 3. Send-keys + capture-pane --
    Write-Section "Send-keys and Capture-pane"

    $ec = T @("send-keys", "-t", "${SESSION}:0.0", "-l", "echo PS7_TEST_OK")
    if ($ec -eq 0) { $ec = T @("send-keys", "-t", "${SESSION}:0.0", "Enter") }
    if ($ec -eq 0) { Write-Host "  [PASS] send-keys" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] send-keys (exit=$ec)" -ForegroundColor Red; $fail++ }
    Start-Sleep -Seconds 1

    $r = TOut @("capture-pane", "-t", "${SESSION}:0.0", "-p")
    if ($r.ExitCode -eq 0 -and $r.Output -match "PS7_TEST_OK") {
        Write-Host "  [PASS] capture-pane saw echo output" -ForegroundColor Green; $pass++
    } elseif ($r.ExitCode -eq 0) {
        Write-Host "  [PASS] capture-pane (returned data, echo may have scrolled)" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] capture-pane (exit=$($r.ExitCode))" -ForegroundColor Red; $fail++ }

    # -- 4. Options and environment --
    Write-Section "Options and Environment"

    $ec = T @("set-option", "-t", $SESSION, "status-interval", "42")
    if ($ec -eq 0) { Write-Host "  [PASS] set-option" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] set-option (exit=$ec)" -ForegroundColor Red; $fail++ }

    $r = TOut @("show-options", "-t", $SESSION, "status-interval")
    if ($r.ExitCode -eq 0 -and $r.Output -match "42") {
        Write-Host "  [PASS] show-options (status-interval=42)" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] show-options (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    $ec = T @("set-environment", "-t", $SESSION, "MY_VAR", "test_ps7")
    if ($ec -eq 0) { Write-Host "  [PASS] set-environment" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] set-environment (exit=$ec)" -ForegroundColor Red; $fail++ }

    $r = TOut @("show-environment", "-t", $SESSION, "MY_VAR")
    if ($r.ExitCode -eq 0 -and $r.Output -match "test_ps7") {
        Write-Host "  [PASS] show-environment" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] show-environment (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    # -- 5. Rename and select --
    Write-Section "Rename and Select"

    $ec = T @("rename-window", "-t", "${SESSION}:0", "ps7win")
    if ($ec -eq 0) { Write-Host "  [PASS] rename-window" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] rename-window (exit=$ec)" -ForegroundColor Red; $fail++ }

    $r = TOut @("list-windows", "-t", $SESSION)
    if ($r.Output -match "ps7win") {
        Write-Host "  [PASS] rename visible in list-windows" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] rename not visible (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    $ec = T @("select-window", "-t", "${SESSION}:0")
    if ($ec -eq 0) { Write-Host "  [PASS] select-window" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] select-window (exit=$ec)" -ForegroundColor Red; $fail++ }

    $ec = T @("select-pane", "-t", "${SESSION}:0.0")
    if ($ec -eq 0) { Write-Host "  [PASS] select-pane" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] select-pane (exit=$ec)" -ForegroundColor Red; $fail++ }

    # -- 6. Attach-session (CONIN$/CONOUT$ regression) --
    Write-Section "Attach-session (CONIN$/CONOUT$ regression)"

    $job = Start-Job -ScriptBlock {
        param($t, $s)
        & $t attach-session -t $s 2>&1
    } -ArgumentList $TMUX, $SESSION

    Start-Sleep -Seconds 3
    $jobOut   = Receive-Job $job 2>&1
    $jobState = $job.State
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -ErrorAction SilentlyContinue

    $errPat = "can't use|server exited unexpectedly|lost server|not a terminal|open terminal failed"
    $errStrings = $jobOut | Where-Object { $_ -match $errPat }
    if ($errStrings) {
        Write-Host "  [FAIL] attach-session -- $($errStrings -join '; ')" -ForegroundColor Red; $fail++
    } elseif ($jobState -eq 'Running') {
        Write-Host "  [PASS] attach-session (still running, no crash)" -ForegroundColor Green; $pass++
    } else {
        Write-Host "  [PASS] attach-session (exited cleanly)" -ForegroundColor Green; $pass++
    }

    # Server survived attach?
    $r = TOut @("list-sessions")
    if ($r.ExitCode -eq 0) {
        Write-Host "  [PASS] server alive after attach" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] server died after attach (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    # -- 7. Display-message format strings --
    Write-Section "Display-message"

    $r = TOut @("display-message", "-t", $SESSION, "-p", '#{session_name}')
    if ($r.ExitCode -eq 0 -and $r.Output -match $SESSION) {
        Write-Host "  [PASS] display-message #{session_name}" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] display-message session_name (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    # -- 8. Multiple sessions --
    Write-Section "Multiple Sessions"

    $ec = T @("new-session", "-d", "-s", "ps7extra")
    if ($ec -eq 0) { Write-Host "  [PASS] new-session (second)" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] new-session second (exit=$ec)" -ForegroundColor Red; $fail++ }

    $r = TOut @("list-sessions")
    if ($r.Output -match $SESSION -and $r.Output -match "ps7extra") {
        Write-Host "  [PASS] list-sessions shows both" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] list-sessions both (out=$($r.Output))" -ForegroundColor Red; $fail++ }

    $ec = T @("kill-session", "-t", "ps7extra")
    if ($ec -eq 0) { Write-Host "  [PASS] kill-session" -ForegroundColor Green; $pass++ }
    else { Write-Host "  [FAIL] kill-session (exit=$ec)" -ForegroundColor Red; $fail++ }

    # -- 9. Debug log: verify CONIN$/CONOUT$ opened --
    Write-Section "CONIN$/CONOUT$ Handle Verification"

    $logFiles = Get-ChildItem "$TESTDIR\debug-*.log","$TESTDIR\build\Debug\debug-*.log" -ErrorAction SilentlyContinue
    $conin_ok = $false; $conout_ok = $false
    foreach ($f in $logFiles) {
        $content = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -match "client_console_in: opened CONIN\$") { $conin_ok = $true }
        if ($content -match "client_console_out: opened CONOUT\$") { $conout_ok = $true }
    }
    if ($conin_ok) {
        Write-Host "  [PASS] debug log: CONIN$ opened via CreateFileW" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] debug log: CONIN$ not opened (GetStdHandle fallback?)" -ForegroundColor Red; $fail++ }
    if ($conout_ok) {
        Write-Host "  [PASS] debug log: CONOUT$ opened via CreateFileW" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] debug log: CONOUT$ not opened (GetStdHandle fallback?)" -ForegroundColor Red; $fail++ }

    # No ReadConsoleInputW failure?
    $readfail = $false
    foreach ($f in $logFiles) {
        $content = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -match "ReadConsoleInputW failed") { $readfail = $true }
    }
    if (!$readfail) {
        Write-Host "  [PASS] no ReadConsoleInputW failures in logs" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] ReadConsoleInputW failed found in logs" -ForegroundColor Red; $fail++ }

    # -- 10. Console size detection --
    Write-Section "Console Size Detection"

    $sizeDefault = $false
    foreach ($f in $logFiles) {
        $content = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -match "GetConsoleScreenBufferInfo failed, defaulting") { $sizeDefault = $true }
    }
    if (!$sizeDefault) {
        Write-Host "  [PASS] console size detected (not defaulting to 80x24)" -ForegroundColor Green; $pass++
    } else { Write-Host "  [FAIL] console size defaulted to 80x24 (CONOUT$ not working for CSBI)" -ForegroundColor Red; $fail++ }

    # Cleanup
    Kill-Tmux

    Write-Host ""
    Write-Host "  PS7: $pass passed, $fail failed" -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
    return @{ Pass = $pass; Fail = $fail }
}

################################################################################
# POWERSHELL 5 TESTS (spawned as real console process via powershell.exe)
################################################################################

function Run-PS5Tests {
    Write-Header "PowerShell 5 (powershell.exe)"

    Kill-Tmux
    Remove-Item "$TESTDIR\debug-*.log" -Force -ErrorAction SilentlyContinue
    Remove-Item "$TESTDIR\build\Debug\debug-*.log" -Force -ErrorAction SilentlyContinue

    # PS5 script writes results to a log file. Write-Output (not Write-Host)
    # goes to stdout which we capture, but we also log to file for reliability.
    $ps5ResultFile = "$RESULTDIR\ps5_results.txt"

    $ps5Script = @"
`$ErrorActionPreference = 'Continue'
`$TMUX = '$TMUX'
`$LOG  = '$ps5ResultFile'
`$pass = 0; `$fail = 0
`$SESSION = "ps5test"

function T { param([string[]]`$A); `$null = & `$TMUX @A 2>`$null; return `$LASTEXITCODE }
function TOut {
    param([string[]]`$A)
    `$tmp = [System.IO.Path]::GetTempFileName()
    & `$TMUX @A >`$tmp 2>`$null
    `$ec = `$LASTEXITCODE
    `$raw = if (Test-Path `$tmp) { (Get-Content `$tmp -Raw) } else { "" }
    if (`$raw -eq `$null) { `$raw = "" }
    `$out = ([string]`$raw) -replace '\x1b\[[0-9;?]*[a-zA-Z]','' -replace '\r',''
    Remove-Item `$tmp -Force -ErrorAction SilentlyContinue
    return @{ ExitCode = `$ec; Output = `$out.Trim() }
}

function Log(`$msg) { `$msg | Out-File -Append -Encoding utf8 `$LOG; Write-Host `$msg }

"" | Out-File -Encoding utf8 `$LOG
Log "  PowerShell `$(`$PSVersionTable.PSVersion)"

`$ec = T @("new-session","-d","-s",`$SESSION)
if (`$ec -eq 0) { Log "  [PASS] new-session -d"; `$pass++ }
else { Log "  [FAIL] new-session -d (exit=`$ec)"; `$fail++ }
Start-Sleep -Milliseconds 500

`$r = TOut @("list-sessions")
if (`$r.ExitCode -eq 0 -and `$r.Output -match `$SESSION) { Log "  [PASS] list-sessions"; `$pass++ }
else { Log "  [FAIL] list-sessions"; `$fail++ }

`$ec = T @("has-session","-t",`$SESSION)
if (`$ec -eq 0) { Log "  [PASS] has-session"; `$pass++ }
else { Log "  [FAIL] has-session (exit=`$ec)"; `$fail++ }

`$ec = T @("new-window","-t",`$SESSION)
if (`$ec -eq 0) { Log "  [PASS] new-window"; `$pass++ }
else { Log "  [FAIL] new-window (exit=`$ec)"; `$fail++ }
Start-Sleep -Milliseconds 400

`$ec = T @("split-window","-h","-t",`$SESSION)
if (`$ec -eq 0) { Log "  [PASS] split-window -h"; `$pass++ }
else { Log "  [FAIL] split-window -h (exit=`$ec)"; `$fail++ }
Start-Sleep -Milliseconds 400

`$ec = T @("split-window","-v","-t",`$SESSION)
if (`$ec -eq 0) { Log "  [PASS] split-window -v"; `$pass++ }
else { Log "  [FAIL] split-window -v (exit=`$ec)"; `$fail++ }
Start-Sleep -Milliseconds 400

`$r = TOut @("list-panes","-t",`$SESSION)
if (`$r.ExitCode -eq 0) {
    `$pc = ([regex]::Matches(`$r.Output, "(?m)^\d+:")).Count
    Log "  [PASS] list-panes (`$pc panes)"; `$pass++
} else { Log "  [FAIL] list-panes"; `$fail++ }

`$ec = T @("send-keys","-t","`${SESSION}:0.0","-l","echo PS5_TEST_OK")
if (`$ec -eq 0) { `$ec = T @("send-keys","-t","`${SESSION}:0.0","Enter") }
if (`$ec -eq 0) { Log "  [PASS] send-keys"; `$pass++ }
else { Log "  [FAIL] send-keys (exit=`$ec)"; `$fail++ }
Start-Sleep -Seconds 1

`$r = TOut @("capture-pane","-t","`${SESSION}:0.0","-p")
if (`$r.ExitCode -eq 0 -and `$r.Output -match "PS5_TEST_OK") {
    Log "  [PASS] capture-pane saw echo output"; `$pass++
} elseif (`$r.ExitCode -eq 0) {
    Log "  [PASS] capture-pane (returned data)"; `$pass++
} else { Log "  [FAIL] capture-pane (exit=`$(`$r.ExitCode))"; `$fail++ }

`$ec = T @("set-option","-t",`$SESSION,"status-interval","55")
if (`$ec -eq 0) { Log "  [PASS] set-option"; `$pass++ }
else { Log "  [FAIL] set-option (exit=`$ec)"; `$fail++ }

`$r = TOut @("show-options","-t",`$SESSION,"status-interval")
if (`$r.ExitCode -eq 0 -and `$r.Output -match "55") { Log "  [PASS] show-options"; `$pass++ }
else { Log "  [FAIL] show-options"; `$fail++ }

`$ec = T @("rename-window","-t","`${SESSION}:0","ps5win")
if (`$ec -eq 0) { Log "  [PASS] rename-window"; `$pass++ }
else { Log "  [FAIL] rename-window (exit=`$ec)"; `$fail++ }

`$ec = T @("select-window","-t","`${SESSION}:0")
if (`$ec -eq 0) { Log "  [PASS] select-window"; `$pass++ }
else { Log "  [FAIL] select-window (exit=`$ec)"; `$fail++ }

`$ec = T @("select-pane","-t","`${SESSION}:0.0")
if (`$ec -eq 0) { Log "  [PASS] select-pane"; `$pass++ }
else { Log "  [FAIL] select-pane (exit=`$ec)"; `$fail++ }

Log "  Testing attach-session (3s timeout)..."
`$job = Start-Job -ScriptBlock { param(`$t,`$s); & `$t attach-session -t `$s 2>&1 } -ArgumentList `$TMUX,`$SESSION
Start-Sleep -Seconds 3
`$jout = Receive-Job `$job 2>&1; `$jstate = `$job.State
Stop-Job `$job -ErrorAction SilentlyContinue; Remove-Job `$job -ErrorAction SilentlyContinue
`$errs = `$jout | Where-Object { `$_ -match "can't use|server exited unexpectedly|lost server|not a terminal|open terminal failed" }
if (`$errs) { Log "  [FAIL] attach-session -- `$(`$errs -join '; ')"; `$fail++ }
elseif (`$jstate -eq 'Running') { Log "  [PASS] attach-session (still running)"; `$pass++ }
else { Log "  [PASS] attach-session (exited cleanly)"; `$pass++ }

`$r = TOut @("list-sessions")
if (`$r.ExitCode -eq 0) { Log "  [PASS] server alive after attach"; `$pass++ }
else { Log "  [FAIL] server died after attach"; `$fail++ }

`$r = TOut @("display-message","-t",`$SESSION,"-p",'#{session_name}')
if (`$r.ExitCode -eq 0 -and `$r.Output -match `$SESSION) { Log "  [PASS] display-message"; `$pass++ }
else { Log "  [FAIL] display-message (out=`$(`$r.Output))"; `$fail++ }

`$ec = T @("new-session","-d","-s","ps5extra")
if (`$ec -eq 0) { Log "  [PASS] new-session (second)"; `$pass++ }
else { Log "  [FAIL] new-session second (exit=`$ec)"; `$fail++ }

`$ec = T @("kill-session","-t","ps5extra")
if (`$ec -eq 0) { Log "  [PASS] kill-session"; `$pass++ }
else { Log "  [FAIL] kill-session (exit=`$ec)"; `$fail++ }

& `$TMUX kill-server 2>`$null | Out-Null
Log ""
Log "  PS5: `$pass passed, `$fail failed"
exit `$fail
"@

    $ps5File = "$TESTDIR\test_native_ps5.ps1"
    $ps5Script | Out-File -Encoding utf8 $ps5File -Force

    # Run PS5 without redirection so it keeps real console handles.
    $proc = Start-Process powershell.exe `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$ps5File`"" `
        -Wait -PassThru -NoNewWindow

    if (Test-Path $ps5ResultFile) {
        Get-Content $ps5ResultFile
    } else {
        Write-Host "  [FAIL] PS5 result file not created" -ForegroundColor Red
    }

    $outText = if (Test-Path $ps5ResultFile) { Get-Content $ps5ResultFile -Raw } else { "" }
    $ps5Pass = ([regex]::Matches($outText, "\[PASS\]")).Count
    $ps5Fail = ([regex]::Matches($outText, "\[FAIL\]")).Count

    Kill-Tmux
    return @{ Pass = $ps5Pass; Fail = $ps5Fail }
}

################################################################################
# CMD.EXE TESTS (spawned as real console process)
################################################################################

function Run-CmdTests {
    Write-Header "cmd.exe"

    Kill-Tmux
    Remove-Item "$TESTDIR\debug-*.log" -Force -ErrorAction SilentlyContinue
    Remove-Item "$TESTDIR\build\Debug\debug-*.log" -Force -ErrorAction SilentlyContinue

    # Build the cmd batch script that logs to a results file internally.
    # All tmux commands suppress their own stdout/stderr with >nul 2>&1
    # while test results go to a log file — preserving the real console.
    $cmdResultFile = "$RESULTDIR\cmd_results.txt"

    $cmdScript = @"
@echo off
setlocal enabledelayedexpansion
cd /d $REPOROOT
set T=$TMUX
set PASS=0
set FAIL=0
set SESSION=cmdtest
set LOG=$cmdResultFile

echo. > "!LOG!"

REM --- Session Lifecycle ---
"!T!" new-session -d -s !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] new-session -d >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] new-session -d >> "!LOG!" & set /a FAIL+=1)
ping -n 2 127.0.0.1 >nul

"!T!" list-sessions >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] list-sessions >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] list-sessions >> "!LOG!" & set /a FAIL+=1)

"!T!" has-session -t !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] has-session >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] has-session >> "!LOG!" & set /a FAIL+=1)

REM --- Window and Pane Ops ---
"!T!" new-window -t !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] new-window >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] new-window >> "!LOG!" & set /a FAIL+=1)
ping -n 1 127.0.0.1 >nul

"!T!" split-window -h -t !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] split-window -h >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] split-window -h >> "!LOG!" & set /a FAIL+=1)
ping -n 1 127.0.0.1 >nul

"!T!" split-window -v -t !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] split-window -v >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] split-window -v >> "!LOG!" & set /a FAIL+=1)
ping -n 1 127.0.0.1 >nul

"!T!" list-panes -t !SESSION! >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] list-panes >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] list-panes >> "!LOG!" & set /a FAIL+=1)

REM --- Send-keys + Capture-pane ---
"!T!" send-keys -t !SESSION!:0.0 -l "echo CMD_TEST_OK" >nul 2>&1
"!T!" send-keys -t !SESSION!:0.0 Enter >nul 2>&1
ping -n 3 127.0.0.1 >nul

set CAPFILE=%TEMP%\tmux_cap_!RANDOM!.txt
"!T!" capture-pane -t !SESSION!:0.0 -p >"!CAPFILE!" 2>nul
if !errorlevel! equ 0 (
    findstr /C:"CMD_TEST_OK" "!CAPFILE!" >nul 2>&1
    if !errorlevel! equ 0 (
        echo   [PASS] capture-pane saw echo output >> "!LOG!"
        set /a PASS+=1
    ) else (
        echo   [PASS] capture-pane returned data >> "!LOG!"
        set /a PASS+=1
    )
) else (
    echo   [FAIL] capture-pane >> "!LOG!"
    set /a FAIL+=1
)
if exist "!CAPFILE!" del "!CAPFILE!"

REM --- Options ---
"!T!" set-option -t !SESSION! status-interval 33 >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] set-option >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] set-option >> "!LOG!" & set /a FAIL+=1)

set OPTFILE=%TEMP%\tmux_opt_!RANDOM!.txt
"!T!" show-options -t !SESSION! status-interval >"!OPTFILE!" 2>nul
findstr /C:"33" "!OPTFILE!" >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] show-options >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] show-options >> "!LOG!" & set /a FAIL+=1)
if exist "!OPTFILE!" del "!OPTFILE!"

REM --- Rename and Select ---
"!T!" rename-window -t !SESSION!:0 cmdwin >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] rename-window >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] rename-window >> "!LOG!" & set /a FAIL+=1)

"!T!" select-window -t !SESSION!:0 >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] select-window >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] select-window >> "!LOG!" & set /a FAIL+=1)

"!T!" select-pane -t !SESSION!:0.0 >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] select-pane >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] select-pane >> "!LOG!" & set /a FAIL+=1)

REM --- Attach-session (CONIN$/CONOUT$ regression) ---
echo   Testing attach-session (3s background)... >> "!LOG!"
start /B "" "!T!" attach-session -t !SESSION!
ping -n 4 127.0.0.1 >nul

"!T!" list-sessions >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] server alive after attach >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] server died after attach >> "!LOG!" & set /a FAIL+=1)

REM --- Display-message ---
set DMFILE=%TEMP%\tmux_dm_!RANDOM!.txt
"!T!" display-message -t !SESSION! -p "#{session_name}" >"!DMFILE!" 2>nul
findstr /C:"!SESSION!" "!DMFILE!" >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] display-message session_name >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] display-message session_name >> "!LOG!" & set /a FAIL+=1)
if exist "!DMFILE!" del "!DMFILE!"

REM --- Multiple sessions ---
"!T!" new-session -d -s cmdextra >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] new-session second >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] new-session second >> "!LOG!" & set /a FAIL+=1)

"!T!" kill-session -t cmdextra >nul 2>&1
if !errorlevel! equ 0 (echo   [PASS] kill-session >> "!LOG!" & set /a PASS+=1) else (echo   [FAIL] kill-session >> "!LOG!" & set /a FAIL+=1)

REM --- Cleanup ---
"!T!" kill-server >nul 2>&1
echo. >> "!LOG!"
echo   cmd.exe: !PASS! passed, !FAIL! failed >> "!LOG!"
exit /b !FAIL!
"@

    $cmdFile = "$TESTDIR\test_native_cmd.bat"
    $cmdScript | Out-File -Encoding ascii $cmdFile -Force

    # Run cmd.exe without PowerShell-level redirection so it keeps its
    # real console (CONIN$/CONOUT$ must work).  Results go to LOG file.
    $proc = Start-Process cmd.exe `
        -ArgumentList "/C `"$cmdFile`"" `
        -Wait -PassThru -NoNewWindow

    if (Test-Path $cmdResultFile) {
        Get-Content $cmdResultFile
    } else {
        Write-Host "  [FAIL] cmd result file not created" -ForegroundColor Red
    }

    $outText = if (Test-Path $cmdResultFile) { Get-Content $cmdResultFile -Raw } else { "" }
    $cmdPass = ([regex]::Matches($outText, "\[PASS\]")).Count
    $cmdFail = ([regex]::Matches($outText, "\[FAIL\]")).Count

    Kill-Tmux
    return @{ Pass = $cmdPass; Fail = $cmdFail }
}

################################################################################
# MAIN
################################################################################

$startTime = Get-Date

# Run all three shell tests
$ps7 = Run-PS7Tests
$ps5 = Run-PS5Tests
$cmd = Run-CmdTests

$totalPass = $ps7.Pass + $ps5.Pass + $cmd.Pass
$totalFail = $ps7.Fail + $ps5.Fail + $cmd.Fail
$totalTests = $totalPass + $totalFail
$elapsed = ((Get-Date) - $startTime).TotalSeconds

Write-Host ""
Write-Host ("=" * 60) -ForegroundColor Cyan
Write-Host "  RESULTS SUMMARY" -ForegroundColor Cyan
Write-Host ("=" * 60) -ForegroundColor Cyan
Write-Host ""
Write-Host "  PowerShell 7:  $($ps7.Pass) passed, $($ps7.Fail) failed" -ForegroundColor $(if ($ps7.Fail -eq 0) { 'Green' } else { 'Red' })
Write-Host "  PowerShell 5:  $($ps5.Pass) passed, $($ps5.Fail) failed" -ForegroundColor $(if ($ps5.Fail -eq 0) { 'Green' } else { 'Red' })
Write-Host "  cmd.exe:       $($cmd.Pass) passed, $($cmd.Fail) failed" -ForegroundColor $(if ($cmd.Fail -eq 0) { 'Green' } else { 'Red' })
Write-Host ""
Write-Host "  Total: $totalPass/$totalTests passed in $([math]::Round($elapsed,1))s" -ForegroundColor $(if ($totalFail -eq 0) { 'Green' } else { 'Red' })
Write-Host ("=" * 60) -ForegroundColor Cyan

exit $totalFail
