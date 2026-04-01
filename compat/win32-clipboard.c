/*
 * win32-clipboard.c - Windows clipboard integration for tmux GAP-03.
 *
 * Provides two functions:
 *
 *   win32_clipboard_set(buf, len)
 *     Write UTF-8 text to the Windows clipboard (CF_UNICODETEXT).
 *     Called from tty_set_selection() instead of emitting OSC 52.
 *
 *   win32_clipboard_get(out_buf, out_len)
 *     Read text from the Windows clipboard into a newly-allocated UTF-8
 *     buffer.  Caller owns the buffer and must free() it.
 *     Returns NULL if clipboard is empty or unavailable.
 *     Called from tty_clipboard_query() to seed paste_add().
 *
 * Both functions are no-ops when the clipboard is unavailable (locked by
 * another application) rather than hard-failing — clipboard contention is
 * common on Windows and should not crash or block tmux.
 *
 * OpenClipboard() requires a window handle on console applications.
 * Passing NULL associates the clipboard with the current thread but requires
 * a message queue, which the tmux server event loop thread does not have.
 * We use GetConsoleWindow() instead — the console HWND is always valid for
 * a console-subsystem process and does not require a message pump.
 */

#include "tmux.h"

/*
 * clipboard_hwnd() - return a stable HWND for OpenClipboard calls.
 * GetConsoleWindow() returns NULL in processes without a console (e.g. when
 * the server was started detached); in that case we create a hidden
 * message-only window once and cache it.
 */
static HWND
clipboard_hwnd(void)
{
    static HWND s_hwnd = (HWND)-1; /* sentinel: not yet initialised */

    if (s_hwnd != (HWND)-1)
        return s_hwnd;

    s_hwnd = GetConsoleWindow();
    if (s_hwnd != NULL)
        return s_hwnd;

    /* No console window — create a hidden message-only window */
    s_hwnd = CreateWindowExW(0, L"STATIC", L"tmux-clipboard",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    if (s_hwnd == NULL) {
        win32_log("clipboard_hwnd: CreateWindowEx failed, error=%lu\n",
            GetLastError());
        s_hwnd = NULL; /* will retry next call */
    }
    return s_hwnd;
}

/*
 * win32_clipboard_set - write UTF-8 text to the Windows clipboard.
 */
void
win32_clipboard_set(const char *buf, size_t len)
{
    HGLOBAL hMem;
    wchar_t *wbuf;
    int wlen;

    if (buf == NULL || len == 0)
        return;

    /* Convert UTF-8 → UTF-16LE */
    wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)len, NULL, 0);
    if (wlen <= 0) {
        win32_log("win32_clipboard_set: MultiByteToWideChar failed, error=%lu\n",
            GetLastError());
        return;
    }

    /* Allocate global memory: (wlen + 1) wide chars, NUL-terminated */
    hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
    if (hMem == NULL) {
        win32_log("win32_clipboard_set: GlobalAlloc failed, error=%lu\n",
            GetLastError());
        return;
    }

    wbuf = (wchar_t *)GlobalLock(hMem);
    if (wbuf == NULL) {
        GlobalFree(hMem);
        win32_log("win32_clipboard_set: GlobalLock failed, error=%lu\n",
            GetLastError());
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, buf, (int)len, wbuf, wlen);
    wbuf[wlen] = L'\0';
    GlobalUnlock(hMem);

    if (!OpenClipboard(clipboard_hwnd())) {
        GlobalFree(hMem);
        win32_log("win32_clipboard_set: OpenClipboard failed, error=%lu\n",
            GetLastError());
        return;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, hMem) == NULL) {
        /* SetClipboardData failed — hMem is still ours, free it */
        win32_log("win32_clipboard_set: SetClipboardData failed, error=%lu\n",
            GetLastError());
        GlobalFree(hMem);
    }
    /* On success, the clipboard owns hMem — do NOT free it */

    CloseClipboard();
    win32_log("win32_clipboard_set: wrote %zu bytes to clipboard\n", len);
}

/*
 * win32_clipboard_get - read text from the Windows clipboard.
 *
 * Returns a newly-allocated UTF-8 buffer and sets *out_len to its size
 * (not including NUL terminator).  Returns NULL on failure.
 */
char *
win32_clipboard_get(size_t *out_len)
{
    HANDLE hData;
    wchar_t *wbuf;
    char *utf8buf;
    int utf8len;

    if (out_len == NULL)
        return NULL;
    *out_len = 0;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return NULL;

    if (!OpenClipboard(clipboard_hwnd())) {
        win32_log("win32_clipboard_get: OpenClipboard failed, error=%lu\n",
            GetLastError());
        return NULL;
    }

    hData = GetClipboardData(CF_UNICODETEXT);
    if (hData == NULL) {
        CloseClipboard();
        win32_log("win32_clipboard_get: GetClipboardData failed, error=%lu\n",
            GetLastError());
        return NULL;
    }

    wbuf = (wchar_t *)GlobalLock(hData);
    if (wbuf == NULL) {
        CloseClipboard();
        win32_log("win32_clipboard_get: GlobalLock failed, error=%lu\n",
            GetLastError());
        return NULL;
    }

    /* Convert UTF-16LE → UTF-8 */
    utf8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
    if (utf8len <= 0) {
        GlobalUnlock(hData);
        CloseClipboard();
        win32_log("win32_clipboard_get: WideCharToMultiByte size failed, error=%lu\n",
            GetLastError());
        return NULL;
    }

    utf8buf = xmalloc(utf8len); /* includes NUL */
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8buf, utf8len, NULL, NULL);

    GlobalUnlock(hData);
    CloseClipboard();

    /* utf8len includes the NUL terminator — report length without it */
    *out_len = (size_t)(utf8len - 1);
    win32_log("win32_clipboard_get: read %zu bytes from clipboard\n", *out_len);
    return utf8buf;
}
