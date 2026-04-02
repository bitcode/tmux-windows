# Startup Performance Analysis

> Observed: 2-3 second startup time from `tmux.exe` to first shell prompt.

## Startup Timeline (Order of Operations)

```
CLIENT PROCESS                          SERVER PROCESS (spawned)
──────────────────────────────────────  ──────────────────────────────────────
1. main() → win32_socket_init()
   WSAStartup (Winsock 2.2)
   [~1ms]

2. Locale setup, env copy, config path
   [~5ms]

3. Socket path: mkdir %LOCALAPPDATA%\tmux
   [~5ms]

4. event_init() → libevent base
   [<1ms]

5. client_connect() → first connect()
   → ECONNREFUSED (no server yet)

6. client_get_lock() → flock()
   [~1ms]

7. Retry with lock held → connect()
   → ECONNREFUSED again

8. server_start() → CreateProcessA()     ┐
   CREATE_NO_WINDOW, no handle inherit   │
   [50-200ms]                            │
                                         ├→ 9. main() → __win32_server path
9. *** Sleep(300) *** ←── BOTTLENECK     │   [~1ms]
   [300ms fixed delay]                   │
                                         │  10. signal mask, proc_start("server")
                                         │      [<1ms]
                                         │
                                         │  11. input_key_build()
                                         │      utf8_update_width_cache()
                                         │      key_bindings_init()
                                         │      [~5ms]
                                         │
                                         │  12. server_create_socket()
                                         │      socket() + bind() + listen()
                                         │      [~5ms]
                                         │
                                         │  13. server_add_accept(0)
                                         │      50ms polling timer for accept
                                         │      (Winsock can't EV_READ AF_UNIX)
                                         │
                                         │  14. win32_job_init()
                                         │      win32_session_log_init()
                                         │      [~5ms]
                                         │
                                         │  15. proc_loop() → event_loop()
                                         │      Server is now listening
                                         │
10. Retry connect() ──────────────────→  │
    May need 1-2 retries at 200ms each   │
    *** Sleep(200) per retry ***          │
    [0-400ms]                            │
                                         │
11. connect() succeeds                   │
    setblocking(fd, 0)                   │
                                         │
12. proc_add_peer() → imsgbuf_init()     │
    [<1ms]                               │
                                         │
13. client_send_identify()               16. server_accept() fires
    MSG_IDENTIFY + capabilities              (on 50ms poll timer)
    [<1ms]                                   Create client struct
                                             [<1ms]
14. Send MSG_COMMAND (new-session)
    proc_flush_peer()                    17. Process MSG_IDENTIFY
    [<1ms]                                   [<1ms]

15. Enter client event loop              18. cmdq_next() → new-session
    proc_loop()                              window_create()
                                             [~2ms]

                                         19. spawn_pane() →
                                             win32_spawn_process()
                                             ├─ win32_pty_open()
                                             │  CreatePipe × 2
                                             │  CreatePseudoConsole()
                                             │  [5-20ms]
                                             ├─ win32_pty_spawn()
                                             │  CreateProcessW(cmd.exe)
                                             │  [50-200ms]
                                             ├─ RegisterWaitForSingleObject
                                             │  [<1ms]
                                             └─ Bridge threads start
                                                [<1ms]

                                         20. cmd.exe loads, prints prompt
                                             Bridge thread → server → client
                                             [5-50ms]
```

## Identified Bottlenecks

### BOTTLENECK-01: ~~Sleep(300) after server_start~~ — FIXED (named event)

**File:** `server.c` (server_start) + `server.c` (server_child_main)
**Fix:** Replaced `Sleep(300)` + `Sleep(200)` retry loop with a named Windows event (`tmux-ready-{pid}`). The client creates the event before spawning the server, passes the name on the command line. The server signals it after `server_add_accept(0)` (bind+listen complete). The client waits with `WaitForSingleObject(event, 5000)`.
**Impact:** Eliminates ~500ms+ of fixed sleep. Server typically signals within 10-20ms of spawn.

### BOTTLENECK-02: ~~Sleep(200) retry loop~~ — REDUCED (safety fallback only)

**File:** `client.c:856`
**Fix:** Reduced from 20 retries × 200ms to 5 retries × 50ms. This is now a safety net for the extremely unlikely case where the ready event fires but `connect()` still fails (e.g., kernel backlog not fully set up). In practice this path should almost never be hit.
**Impact:** Worst case dropped from 4000ms to 250ms.

### BOTTLENECK-03: 50ms server accept polling timer

**File:** `server.c:685`
**Impact:** Up to 50ms latency before server notices client connection
**Why it exists:** Winsock `select()` does not support AF_UNIX sockets. `EV_READ` on the listening socket never fires. A timer polls `accept()` every 50ms as a workaround.
**Problem:** Client may connect but wait up to 50ms before server calls `accept()`.

### BOTTLENECK-04: CreateProcessA for server — 50-200ms

**File:** `server.c:275`
**Impact:** 50-200ms (OS-dependent, AV scanning adds more)
**Why it exists:** Windows process creation is inherently slower than `fork()`.
**Factors:** Antivirus scanning, shell startup, registry reads.

### BOTTLENECK-05: CreateProcessW for shell — 50-200ms

**File:** `win32-pty.c` (win32_pty_spawn)
**Impact:** 50-200ms for cmd.exe, 500ms-2s for PowerShell
**Why it exists:** Same as above — Windows process creation cost.

### BOTTLENECK-06: 250ms resize polling thread

**File:** `client.c:153`
**Impact:** No startup impact, but 250ms resize latency at runtime.
**Why it exists:** No console resize callback on Windows.

## Worst-Case vs Best-Case

| Scenario | Time |
|----------|------|
| **Best case** (server binds fast, cmd.exe shell) | ~500ms |
| **Typical case** (1-2 retries, cmd.exe) | ~700-900ms |
| **Observed case** (2-3 retries, some AV delay) | ~1500-2500ms |
| **Worst case** (20 retries, PowerShell shell) | ~6000ms+ |

## Proposed Solutions

### SOL-01: Replace Sleep(300) + retry loop with event-driven readiness

Replace the fixed sleep and polling retry with a **named event** that the server signals after `listen()` completes.

```
Client                              Server
──────                              ──────
CreateEvent("tmux-ready-{pid}")
CreateProcess(server)
WaitForSingleObject(event, 5000)    ... init ...
                                    server_create_socket()
                                    bind() + listen()
                                    OpenEvent("tmux-ready-{pid}")
                                    SetEvent(event)
connect() ←─────────────────────    accept()
```

**Expected savings:** Eliminates 300ms sleep + 200ms×N retries. Server init takes ~20ms, so total wait drops from ~500ms to ~20ms.
**Complexity:** Medium. Requires passing event name to server via command line.

### SOL-02: Replace 50ms accept poll with non-blocking accept loop on self-pipe

Instead of polling on a timer, the server could use a self-pipe or `WSAEventSelect` to get notified when the listening socket has a pending connection.

**Option A:** `WSAEventSelect(server_fd, hEvent, FD_ACCEPT)` — register an event object for accept readiness, then use `WaitForSingleObject` on a thread that writes to a self-pipe when the event fires.

**Option B:** Reduce poll interval from 50ms to 5ms (quick fix, low risk).

**Expected savings:** Up to 50ms per connection (affects attach too, not just startup).

### SOL-03: Audit and reduce ConPTY creation overhead

Profile `CreatePseudoConsole()` timing. If consistently >10ms, investigate whether the ConPTY can be pre-created or cached.

### SOL-04: Reduce verbose win32_log calls in startup path

Every `win32_log()` call in the startup path does a file write. With `-vvvv` or debug builds, this adds I/O overhead. Consider deferring log writes or batching them.

### SOL-05: Parallel server init

Some server initialization steps are independent and could be parallelized:
- `input_key_build()` and `utf8_update_width_cache()` don't depend on socket
- `win32_session_log_init()` doesn't depend on key tables
- Socket bind could happen first, then signal readiness, then do remaining init

## All Timing-Sensitive Code Locations

Reference for systematic audit:

| File | Line | Code | Value | Category |
|------|------|------|-------|----------|
| `client.c` | 153 | `Sleep(250)` | 250ms | Resize polling |
| `client.c` | 834 | ~~Sleep(300)~~ | ~~300ms~~ | Replaced by named event |
| `client.c` | 856 | `Sleep(50)` | 50ms | Connect retry (5 max, safety net) |
| `server.c` | 685 | `poll_tv {0, 50000}` | 50ms | Accept polling |
| `win32-job.c` | 520 | `Sleep(1)` | 1ms | Exit flag spin-wait |
| `win32-session-log.c` | 61 | `WaitForSingleObject(..., 5000)` | 5000ms | Mutex timeout |
| `win32-misc.c` | 186 | `Sleep(ms)` | dynamic | nanosleep wrapper |
| `win32-misc.c` | 205 | `Sleep(ms)` | dynamic | usleep wrapper |
| `input.c` | 75 | `INPUT_REQUEST_TIMEOUT` | 500ms | Terminal query timeout |
| `input.c` | 3277 | `tv {0, 100000}` | 100ms | Request timer tick |
| `tty.c` | 87 | `TTY_BLOCK_INTERVAL` | 100ms | TTY write blocking |
| `tty.c` | 91 | `TTY_QUERY_TIMEOUT` | 5s | TTY query timeout |
| `control.c` | 1050 | `tv {1, 0}` | 1000ms | Subscription check |
