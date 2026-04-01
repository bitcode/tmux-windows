# tmux for Windows

[![Windows Build](https://github.com/bitcode/tmux-windows/actions/workflows/windows-build.yml/badge.svg)](https://github.com/bitcode/tmux-windows/actions/workflows/windows-build.yml)

A native Win32 port of [tmux](https://github.com/tmux/tmux) — the terminal
multiplexer — running on Windows 10 1809+ without WSL, Cygwin, or MSYS2.

> **Status:** Core functionality working. 56/56 integration tests passing.
> See [Feature Support](#feature-support) for details.

## Requirements

- **Windows 10 version 1809** (build 17763) or later — required for ConPTY and AF_UNIX socket support
- **Windows 11** recommended
- A VT-capable terminal: [Windows Terminal](https://aka.ms/terminal) is strongly recommended
- No WSL, no Cygwin, no MSYS2 required or used

## Quick Start

Download `tmux-windows-vX.Y.Z.zip` from the [Releases page](https://github.com/bitcode/tmux-windows/releases).
Extract and place both files somewhere on your `PATH`:

- `tmux.exe`
- `tmux-signal-proxy.exe` — must be in the same directory as `tmux.exe`

Then open Windows Terminal and run `tmux`.

Config file: `%APPDATA%\tmux\tmux.conf`

## Building from Source

See [docs/windows/BUILDING.md](docs/windows/BUILDING.md) for full instructions. Short version:

**1. Build libevent (static):**
```bat
git clone --depth 1 --branch release-2.1.12-stable https://github.com/libevent/libevent.git
cd libevent
cmake -B build -DEVENT__LIBRARY_TYPE=STATIC ^
      -DEVENT__DISABLE_TESTS=ON -DEVENT__DISABLE_SAMPLES=ON ^
      -DCMAKE_INSTALL_PREFIX=%USERPROFILE%\libevent-install
cmake --build build --config Release
cmake --install build --config Release
```

**2. Build tmux:**
```bat
git clone https://github.com/bitcode/tmux-windows.git
cd tmux-windows
cmake -B build -DLIBEVENT_ROOT=%USERPROFILE%\libevent-install
cmake --build build --config Release
```

Outputs: `build\Release\tmux.exe` and `build\Release\tmux-signal-proxy.exe`

## Feature Support

| Feature | Status |
|---------|--------|
| Sessions, windows, panes | Working |
| Mouse support (click, scroll, resize) | Working |
| Copy mode + Windows clipboard | Working |
| `run-shell`, `if-shell` | Working |
| `pipe-pane -o` | Working |
| `pipe-pane -I` | Working |
| `respawn-pane` / `respawn-window` | Working |
| Config file (`%APPDATA%\tmux\tmux.conf`) | Working |
| Custom `default-shell` | Working |
| `#{pane_current_command}`, `#{pane_current_path}` | Working |
| `suspend-pane` / `resume-pane` | Working (via Windows Job Objects) |
| `display-popup` | Untested (needs nested ConPTY) |

## Architecture

All Windows-specific code lives under `compat/`. The tmux core (grid, pane,
key binding, protocol) is unmodified except for narrow `#ifdef PLATFORM_WINDOWS`
guards at OS boundary calls.

A force-include (`/FI"win32.h"` on MSVC, `-include win32.h` on GCC) injects
the compatibility layer into every translation unit so no tmux source file
needs to be explicitly aware of Windows.

| POSIX | Windows | Source |
|-------|---------|--------|
| `fork()` + `forkpty()` | `CreateProcess` + ConPTY | `compat/win32-pty.c` |
| Unix domain sockets | AF_UNIX (Winsock) | `compat/win32-socket.c` |
| `select()` / `poll()` | libevent IOCP | libevent |
| Signals | `RegisterWaitForSingleObject` | `compat/win32-signal.c` |
| Clipboard (OSC 52) | `SetClipboardData` | `compat/win32-clipboard.c` |
| Job control | Windows Job Objects + `NtSuspendProcess` | `compat/win32-jobctl.c` |

## Upstream Tracking

This port tracks [tmux/tmux](https://github.com/tmux/tmux).
Currently based on tmux 3.6a. See [SYNCING](SYNCING) for the rebase workflow.

## Contributing

Bug reports and PRs welcome. Before opening an issue:

1. Run `winver` — confirm you are on Windows 10 1809+
2. Confirm `tmux-signal-proxy.exe` is next to `tmux.exe`
3. Run `tmux -vvvv` and attach the debug log to the issue

## License

ISC. See [COPYING](COPYING).
