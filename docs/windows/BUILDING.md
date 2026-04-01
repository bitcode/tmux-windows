# Building tmux for Windows

## Prerequisites

- **Windows 10 version 1809** (build 17763) or later — required for ConPTY and AF_UNIX
- **CMake 3.10+** — [cmake.org/download](https://cmake.org/download/)
- **MSVC** (Visual Studio 2019 or later, Desktop C++ workload) **or** MinGW-w64
- **Git** — [git-scm.com](https://git-scm.com/)

## Step 1 — Build libevent (static)

tmux requires libevent 2.1+. Build it as a static library:

```bat
git clone https://github.com/libevent/libevent.git
cd libevent
cmake -B build ^
      -DEVENT__LIBRARY_TYPE=STATIC ^
      -DEVENT__DISABLE_TESTS=ON ^
      -DEVENT__DISABLE_SAMPLES=ON ^
      -DCMAKE_INSTALL_PREFIX=%USERPROFILE%\libevent-install
cmake --build build --config Release
cmake --install build --config Release
```

This installs libevent headers and `.lib` files under `%USERPROFILE%\libevent-install`.

## Step 2 — Build tmux

```bat
git clone https://github.com/bitcode/tmux-windows.git
cd tmux-windows
cmake -B build -DLIBEVENT_ROOT=%USERPROFILE%\libevent-install
cmake --build build --config Release
```

Output binaries will be in `build\Release\`:
- `tmux.exe`
- `tmux-signal-proxy.exe`

Both files must be kept in the same directory.

## Step 3 — Install

Copy both EXEs somewhere on your `PATH`, for example `%USERPROFILE%\bin\`:

```bat
copy build\Release\tmux.exe %USERPROFILE%\bin\
copy build\Release\tmux-signal-proxy.exe %USERPROFILE%\bin\
```

## Running tmux

Open **Windows Terminal** and run:

```
tmux
```

Config file location: `%APPDATA%\tmux\tmux.conf`

## Troubleshooting

**CMake can't find libevent**

Pass the install prefix explicitly:
```bat
cmake -B build -DLIBEVENT_ROOT=C:\path\to\libevent-install
```

**`tmux-signal-proxy.exe` not found**

Both EXEs must be in the same directory. tmux looks for `tmux-signal-proxy.exe`
next to itself at startup.

**`ConPtyOpen` fails / ConPTY not available**

Run `winver` and confirm you are on Windows 10 build 17763 or later.
ConPTY is not available on older Windows versions.

**AF_UNIX socket errors**

AF_UNIX support requires Windows 10 build 17763+. Ensure the
`%LOCALAPPDATA%\tmux\` directory is writable.

**Debug logging**

Run `tmux -vvvv` to enable verbose logging. Attach the output to bug reports.

## MinGW build

MinGW is supported but less tested than MSVC. Use the same CMake steps with
the MinGW generator:

```bat
cmake -B build -G "MinGW Makefiles" -DLIBEVENT_ROOT=%USERPROFILE%\libevent-install
cmake --build build
```
