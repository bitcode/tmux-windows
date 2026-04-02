# tmux-windows — Claude Code Context

## Project Overview

Native Win32 port of [tmux](https://github.com/tmux/tmux) 3.6a. Runs on **Windows 10 1809+** (build 17763+) using ConPTY and AF_UNIX APIs. No WSL, Cygwin, or MSYS2.

- **Repo:** `bitcode/tmux-windows`
- **License:** ISC
- **Status:** Functionally complete (56/56 integration tests, 64/64 native shell tests). Entering security and performance hardening phase.

## Architecture

All Windows-specific code lives in `compat/`. The tmux core is unmodified except for narrow `#ifdef PLATFORM_WINDOWS` guards at OS boundary calls.

A force-include (`/FI"win32.h"` on MSVC, `-include win32.h` on GCC) injects the compatibility layer into every translation unit via CMakeLists.txt — no tmux source file needs explicit Windows awareness.

### POSIX → Windows Translation

| POSIX | Windows | Source |
|-------|---------|--------|
| `fork()` / `forkpty()` | `CreateProcess` + ConPTY | `compat/win32-pty.c` |
| PTY (`/dev/pts/*`) | `CreatePseudoConsole` | `compat/win32-pty.c` |
| Unix domain sockets | AF_UNIX (Winsock) | `compat/win32-socket.c` |
| `select()` / `poll()` | libevent (Winsock backend) | libevent dependency |
| POSIX signals | Event callbacks + `RegisterWaitForSingleObject` | `compat/win32-signal.c` |
| `waitpid()` | `WaitForSingleObject` + process table | `compat/win32-process.c` |
| `/bin/sh` | `cmd.exe` (via COMSPEC) | `compat/win32/paths.h` |
| `/tmp/` | `C:\Windows\Temp\` | `compat/win32/paths.h` |
| `/dev/null` | `NUL` | `compat/win32/paths.h` |
| `gettimeofday()` | `GetSystemTimeAsFileTime` | `compat/win32.h` |
| Clipboard (OSC 52) | `SetClipboardData` / `GetClipboardData` | `compat/win32-clipboard.c` |
| Job control (`tcsetpgrp`) | Job Objects + `NtSuspendProcess` | `compat/win32-jobctl.c` |
| `SIGWINCH` | 250ms polling thread + `MSG_RESIZE` | `compat/win32-signal.c` |

### Execution Modes

Three paths for running external commands:

1. **Fast Path** — No pipes, exit code only. `CreateProcess` + `RegisterWaitForSingleObject` + self-pipe. Used by `if-shell`.
2. **Full I/O Bridge Path** — `CreatePipe` + bridge thread + TCP loopback socketpair + `bufferevent_new`. Used by `run-shell`, `pipe-pane`.
3. **PTY Path** — ConPTY + dual bridge threads (ingress: socket→hPipeIn, egress: hPipeOut→socket). Used by pane spawning, `display-popup`.

### Signal Proxy

`tmux-signal-proxy.exe` relays Ctrl+Break across the ConPTY boundary. Must be in the same directory as `tmux.exe` at startup. Built separately without libevent or force-include.

## Repository Layout

```
tmux-windows/
├── compat/                     # Windows compatibility layer (~6700 LOC)
│   ├── win32.h                 # Master compat header (force-included in all TUs)
│   ├── win32-pty.c             # ConPTY wrapper — CRITICAL (bridge thread, deadlock rules)
│   ├── win32-job.c             # Job execution (Fast Path + I/O Bridge)
│   ├── win32-jobctl.c          # Job control (Job Objects + NtSuspendProcess)
│   ├── win32-socket.c          # AF_UNIX + fake fd mapping (WINSOCK_FD_OFFSET=2000)
│   ├── win32-signal.c          # Signal emulation via Windows events
│   ├── win32-clipboard.c       # Win32 clipboard integration
│   ├── win32-term.c            # Terminal capability functions
│   ├── win32-process.c         # Child process table, waitpid emulation
│   ├── win32-io.c              # I/O wrappers (read/write/fd management)
│   ├── win32-misc.c            # Miscellaneous POSIX helpers, socket path init
│   ├── win32-session-log.c     # JSON session ledger (utmp replacement)
│   ├── osdep-win32.c           # OS queries (cwd, process name via PSAPI/PEB walk)
│   ├── win32-signal-proxy/     # Signal proxy helper (standalone executable)
│   └── win32/                  # POSIX header surrogates (intercept #include paths)
│       ├── paths.h             # _PATH_BSHELL=cmd.exe, _PATH_TMP, _PATH_DEVNULL
│       ├── termios.h, term.h   # Terminal stubs
│       ├── sys/socket.h, sys/wait.h, sys/time.h  # System header surrogates
│       └── arpa/, netinet/     # Network header surrogates
├── docs/windows/BUILDING.md    # Full build instructions
├── installer/                  # Inno Setup installer script
├── tests/                      # test_native_shells.ps1 (64/64 pass)
├── regress/                    # Upstream regression tests
├── cmake/                      # CMake modules
├── .github/workflows/          # CI (windows-build.yml)
├── CMakeLists.txt              # Build configuration
├── build_static.bat            # Local Windows build script
├── SYNCING                     # Upstream sync workflow
├── tmux.h                      # Main tmux header
├── tmux.c                      # Entry point
└── [70+ tmux core .c files]    # Unmodified upstream source
```

## Build System

- **CMake 3.10+**, MSVC (VS 2019+) or MinGW
- **libevent 2.1+** — built as static library
- **Static CRT** — `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` for self-contained binaries
- **Targets:** `tmux` (main) and `tmux-signal-proxy` (helper)
- **Windows libs:** ws2_32, advapi32, user32, shell32, kernel32

Build commands:
```bat
cmake -B build -DLIBEVENT_ROOT=%USERPROFILE%\libevent-install
cmake --build build --config Release
```

Or use `build_static.bat` for a one-shot build. See `docs/windows/BUILDING.md` for full instructions.

## Critical Implementation Details

### ConPTY Bridge Thread (MANDATORY)

The ConPTY output pipe **must** be drained by a separate thread. Reading it synchronously on the main event loop thread causes deadlock. See `compat/win32-pty.c:win32_pty_open()`.

### Socket FD Mapping (WINSOCK_FD_OFFSET = 2000)

Winsock `SOCKET` handles (64-bit) can't fit in `int` fd. Fake POSIX fds start at offset 2000, above the CRT fd space. `win32_socket_map[]` (1024 entries) maps fake fd ↔ SOCKET. All socket operations route through wrappers in `win32-socket.c`. Any code comparing raw fd values or using them as array indices must account for this.

### File Paths

- **AF_UNIX socket:** `%LOCALAPPDATA%\tmux\tmux-0\default`
- **Config file:** `%APPDATA%\tmux\tmux.conf`
- **Session log:** `%LOCALAPPDATA%\tmux\sessions.json`

### No fork()

Every place tmux calls `fork()` routes through `CreateProcess` wrappers. Never add a raw `fork()` call.

### Signal Emulation

`SIGWINCH`, `SIGCHLD`, etc. are synthesized events, not real signals. Signal handlers via `signal()` may not fire — prefer libevent signal event API. Console control handler maps Ctrl+C/Ctrl+Break → SIGINT/SIGTERM. `RegisterWaitForSingleObject` watches process handles for exit → SIGCHLD emulation.

### Shell Resolution

`_PATH_BSHELL` = `"cmd.exe"` in `compat/win32/paths.h`. Fallback: `COMSPEC` env var. Do not change to a Unix path.

## Permanent Constraints

| ID | Constraint | Mitigation | Status |
|----|-----------|------------|--------|
| PERM-01 | No `SCM_RIGHTS` (FD passing) | `WSADuplicateSocket` + `WSAPROTOCOL_INFO` + bridge threads | Functional |
| PERM-02 | No process groups / `tcsetpgrp` | Job Objects + `NtSuspendProcess`/`NtResumeProcess`; Ctrl+Z → suspend, Ctrl+F → resume | Partial |
| PERM-03 | No privilege separation (`setuid`) | `SIO_AF_UNIX_GETPEERPID` + SID comparison in `server_accept()` (single-user) | Mitigated |
| PERM-04 | No utmp/wtmp session logging | JSON ledger at `%LOCALAPPDATA%\tmux\sessions.json` with named mutex | Mitigated |

## Known Limitations

- **`run-shell -p`** — Output display to view-mode pane disabled. libevent 2.x holds internal evbuffer lock during bufferevent error callback, causing deadlock on `evbuffer_readln`. Command execution and exit-code-based completion work fine.
- **`display-popup`** — Nested ConPTY architecture is complete but untested interactively.
- **`display-message`** — Some format strings (`#{window_name}`, `#{pane_width}`) may return empty in automated tests. Suspected test race condition — likely works interactively.

## Testing

- **Integration tests:** `tests/test_native_shells.ps1` — 64/64 (PowerShell 7, PowerShell 5, cmd.exe)
- **CI:** `.github/workflows/windows-build.yml` (Windows Server 2022, caches libevent)
- **Debug logging:** `tmux -vvvv` for verbose output

## Security Hardening (Next Phase)

Areas requiring audit and hardening:

- **Peer identity:** `SIO_AF_UNIX_GETPEERPID` + SID comparison already in `server_accept()`. Verify coverage for all connection paths.
- **Socket directory permissions:** Ensure `%LOCALAPPDATA%\tmux\` has restrictive ACLs.
- **Handle lifetime:** Audit process handle, pipe handle, and HPCON lifetime in `win32-pty.c` and `win32-process.c`. Ensure no leaks or dangling handles after pane close/respawn.
- **Pipe handle inheritance:** Verify `STARTUPINFOEX` attribute lists properly restrict handle inheritance to intended pipes only.
- **Bridge thread cleanup:** Confirm all bridge threads terminate cleanly on pane close, respawn, and server shutdown. Check for thread pool callback races in `pty_process_exited`.
- **Session log integrity:** Named mutex on `sessions.json` — verify no TOCTOU races.
- **Input validation:** Audit VT input parsing (`client.c`) for malformed escape sequences.

## Performance Hardening (Next Phase)

Optimization opportunities:

- **IOCP migration:** libevent currently uses Winsock select backend. Migrating to IOCP would improve scalability. See libevent docs for `event_config_set_flag(EVENT_BASE_FLAG_USE_IOCP)` (note: requires careful testing of bufferevent integration).
- **SIGWINCH polling:** 250ms polling thread for terminal resize. Could be replaced with `ReadConsoleInputW` resize events if input thread architecture allows.
- **Bridge thread overhead:** Each ConPTY pane has 2 bridge threads. For many-pane sessions, context switching may become measurable. Consider thread pool or IOCP-based I/O completion.
- **ConPTY output drain:** Measure pipe buffer utilization under high-throughput scenarios (e.g., `cat` large file).

## Upstream Sync

Based on tmux 3.6a. Upstream: [tmux/tmux](https://github.com/tmux/tmux). See `SYNCING` for the rebase/merge workflow. Windows-specific changes are isolated in `compat/` to minimize merge conflicts.

## Development Workflow

### Dual-Repo Situation

Development originated in `C:\Users\mdrozrosario\port` (submodule-based, tmux as git submodule). The public repo `bitcode/tmux-windows` flattened the structure — tmux source files are at root level alongside `compat/`.

**Recommendation:** Use `tmux-windows` as the single source of truth going forward. The `port/` repo contains valuable research documents in `port/research/` — reference them when making architectural decisions, but do all code changes in `tmux-windows`.

### When to Consult port/research/

| Document | Consult When |
|----------|-------------|
| `Native Tmux Port To Windows Analysis.md` | Architecture decisions, process model changes |
| `Technical_Debt_Analysis_2026_01_10.md` | Threading/deadlock concerns, AF_UNIX limitations |
| `Debugging_Analysis_tmux_ls_hang.md` | Shell spawning or libevent hang debugging |
| `Architectural Analysis Cross-Process File Descriptor Passing...` | Modifying PERM-01 (FD passing) |
| `POSIX Job Control Emulation...` | Modifying PERM-02 (job control) |
| `Native Win32 Privilege Separation...` | Modifying PERM-03 (security) |
| `Native Windows Session Accounting...` | Modifying PERM-04 (session logging) |

## Coding Conventions

- **Style:** BSD (tabs, K&R braces) — match the surrounding file
- **Windows code:** Goes in `compat/` only. No `#ifdef _WIN32` in core tmux sources unless absolutely unavoidable (and then make it `#ifdef PLATFORM_WINDOWS`)
- **Debug output:** `win32_log()` (goes to `win32_debug.log` or stderr)
- **Error reporting:** `win32_err()` / `win32_warn()`
- **New POSIX functions:** Stub in `compat/win32-stubs.c` or full impl in appropriate `compat/win32-*.c`
- **New path constants:** `compat/win32/paths.h`
- **New type definitions:** `compat/win32.h`
- **Thread safety:** Child process table and socket map have separate locks (`CRITICAL_SECTION`). Do not mix them.
