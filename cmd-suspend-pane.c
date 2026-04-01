/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include "tmux.h"

/*
 * suspend-pane / resume-pane — Windows job control (PERM-02).
 *
 * These are Windows-only commands.  On non-Windows builds they are present
 * in the command table but return an error immediately, so key bindings that
 * reference them compile without #ifdefs in the config parser.
 *
 * suspend-pane [-t target-pane]
 *   Suspend the process tree of the target pane via NtSuspendProcess.
 *   Equivalent to Ctrl+Z / SIGTSTP.  Press prefix + F (or run resume-pane)
 *   to resume.
 *
 * resume-pane [-t target-pane]
 *   Resume a previously suspended pane and synthesize a resize event to
 *   redraw the shell prompt.  Equivalent to fg / SIGCONT.
 */

static enum cmd_retval	cmd_suspend_pane_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_suspend_pane_entry = {
	.name = "suspend-pane",
	.alias = "suspp",

	.args = { "t:", 0, 0, NULL },
	.usage = "[" CMD_TARGET_PANE_USAGE "]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_suspend_pane_exec
};

const struct cmd_entry cmd_resume_pane_entry = {
	.name = "resume-pane",
	.alias = "respp",

	.args = { "t:", 0, 0, NULL },
	.usage = "[" CMD_TARGET_PANE_USAGE "]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_suspend_pane_exec
};

static enum cmd_retval
cmd_suspend_pane_exec(struct cmd *self, struct cmdq_item *item)
{
#ifndef PLATFORM_WINDOWS
	(void)self;
	(void)item;
	cmdq_error(item, "suspend-pane/resume-pane: Windows only");
	return (CMD_RETURN_ERROR);
#else
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct client		*tc = cmdq_get_target_client(item);
	win32_pty_t		*pty;
	HANDLE			 hJob;
	DWORD			 cpid;
	int			 is_resume = (cmd_get_entry(self) == &cmd_resume_pane_entry);

	if (wp == NULL || wp->fd == -1) {
		cmdq_error(item, "pane has no process");
		return (CMD_RETURN_ERROR);
	}

	pty  = win32_pty_lookup(wp->fd);
	hJob = win32_pty_get_job(pty);
	cpid = win32_pty_get_child_pid(wp->fd);

	if (cpid == 0) {
		cmdq_error(item, "could not determine pane process ID");
		return (CMD_RETURN_ERROR);
	}

	if (is_resume) {
		HPCON  hPC   = win32_pty_get_console(pty);
		COORD  size  = win32_pty_get_coord(pty);
		if (!win32_jobctl_resume(hJob, cpid, hPC, size)) {
			cmdq_error(item, "resume-pane: failed");
			return (CMD_RETURN_ERROR);
		}
		if (tc != NULL)
			status_message_set(tc, 1500, 1, 0, 0, "Pane resumed");
	} else {
		if (!win32_jobctl_suspend(hJob, cpid)) {
			cmdq_error(item, "suspend-pane: failed");
			return (CMD_RETURN_ERROR);
		}
		if (tc != NULL)
			status_message_set(tc, 2000, 1, 0, 0,
			    "Pane suspended (run resume-pane or press Ctrl+F to resume)");
	}

	return (CMD_RETURN_NORMAL);
#endif
}
