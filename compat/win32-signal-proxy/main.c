/*
 * tmux-signal-proxy.exe
 *
 * Ephemeral helper that sends CTRL_BREAK_EVENT to a target process group
 * across the ConPTY isolation boundary.
 *
 * Problem: GenerateConsoleCtrlEvent(CTRL_C_EVENT, ...) ignores the
 * dwProcessGroupId parameter and broadcasts to ALL processes on the
 * caller's console — this would kill the tmux server daemon itself.
 * CTRL_BREAK_EVENT respects the process group, but the tmux server runs
 * inside a ConPTY and is therefore console-isolated from the child.
 *
 * Solution: spawn this tiny helper as a detached process. It:
 *   1. FreeConsole()           — detach from any inherited console
 *   2. AttachConsole(target)   — attach to the target process's console
 *   3. GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0)  — send to its group
 *   4. exits
 *
 * Usage:
 *   tmux-signal-proxy.exe <pid> [SIGINT|SIGTERM]
 *
 * The signal type argument is accepted for forward-compatibility but both
 * SIGINT and SIGTERM map to CTRL_BREAK_EVENT — CTRL_C_EVENT is disqualified
 * as noted above.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char *argv[])
{
    DWORD target_pid;

    if (argc < 2) {
        fprintf(stderr, "usage: tmux-signal-proxy <pid> [SIGINT|SIGTERM]\n");
        return 1;
    }

    target_pid = (DWORD)strtoul(argv[1], NULL, 10);
    if (target_pid == 0) {
        fprintf(stderr, "tmux-signal-proxy: invalid pid '%s'\n", argv[1]);
        return 1;
    }

    /*
     * Detach from any inherited console first.  Without this,
     * AttachConsole may fail if we already have a console attached.
     */
    FreeConsole();

    /*
     * Attach to the target process's console.  This is what crosses the
     * ConPTY isolation boundary: the target's ConPTY presents a virtual
     * console, and AttachConsole connects us to it so that
     * GenerateConsoleCtrlEvent reaches the right process group.
     */
    if (!AttachConsole(target_pid)) {
        /* Target may have already exited — not an error */
        return 0;
    }

    /*
     * CTRL_BREAK_EVENT with dwProcessGroupId=0 sends to all processes
     * in this console's process group — which is the target's group,
     * not tmux's, because we attached to its console.
     */
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);

    FreeConsole();
    return 0;
}
