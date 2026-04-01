/* $OpenBSD$ */

/*
 * Copyright (c) 2019 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PLATFORM_WINDOWS
#include <io.h>        /* for _dup, _open_osfhandle */
#include <windows.h>   /* for GetStdHandle */
#endif

#include "tmux.h"


/*
 * IPC file handling. Both client and server use the same data structures
 * (client_file and client_files) to store list of active files. Most functions
 * are for use either in client or server but not both.
 */

static int	file_next_stream = 3;

RB_GENERATE(client_files, client_file, entry, file_cmp);

/* Get path for file, either as given or from working directory. */
static char *
file_get_path(struct client *c, const char *file)
{
	const char	*home;
	char		*path, *full_path;

	if (strncmp(file, "~/", 2) != 0)
		path = xstrdup(file);
	else {
		home = find_home();
		if (home == NULL)
			home = "";
		xasprintf(&path, "%s%s", home, file + 1);
	}
	if (*path == '/')
		return (path);
#ifdef PLATFORM_WINDOWS
	/* Accept Windows absolute paths: C:\..., C:/..., \\UNC */
	if ((isalpha((unsigned char)*path) && path[1] == ':') || *path == '\\')
		return (path);
#endif
	xasprintf(&full_path, "%s/%s", server_client_get_cwd(c, NULL), path);
	return (full_path);
}

/* Tree comparison function. */
int
file_cmp(struct client_file *cf1, struct client_file *cf2)
{
	if (cf1->stream < cf2->stream)
		return (-1);
	if (cf1->stream > cf2->stream)
		return (1);
	return (0);
}

/*
 * Create a file object in the client process - the peer is the server to send
 * messages to. Check callback is fired when the file is finished with so the
 * process can decide if it needs to exit (if it is waiting for files to
 * flush).
 */
struct client_file *
file_create_with_peer(struct tmuxpeer *peer, struct client_files *files,
    int stream, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;

	cf = xcalloc(1, sizeof *cf);
	cf->c = NULL;
	cf->references = 1;
	cf->stream = stream;

	cf->buffer = evbuffer_new();
	if (cf->buffer == NULL)
		fatalx("out of memory");

	cf->cb = cb;
	cf->data = cbdata;

	cf->peer = peer;
	cf->tree = files;
	RB_INSERT(client_files, files, cf);

	return (cf);
}

/* Create a file object in the server, communicating with the given client. */
struct client_file *
file_create_with_client(struct client *c, int stream, client_file_cb cb,
    void *cbdata)
{
	struct client_file	*cf;

#ifndef PLATFORM_WINDOWS
	if (c != NULL && (c->flags & CLIENT_ATTACHED))
		c = NULL;
#endif

	cf = xcalloc(1, sizeof *cf);
	cf->c = c;
	cf->references = 1;
	cf->stream = stream;

	cf->buffer = evbuffer_new();
	if (cf->buffer == NULL)
		fatalx("out of memory");

	cf->cb = cb;
	cf->data = cbdata;

	if (cf->c != NULL) {
		cf->peer = cf->c->peer;
		cf->tree = &cf->c->files;
		RB_INSERT(client_files, &cf->c->files, cf);
		cf->c->references++;
	}

	return (cf);
}

/* Free a file. */
void
file_free(struct client_file *cf)
{
	if (--cf->references != 0)
		return;

	evbuffer_free(cf->buffer);
	free(cf->path);

	if (cf->tree != NULL)
		RB_REMOVE(client_files, cf->tree, cf);
	if (cf->c != NULL)
		server_client_unref(cf->c);

	free(cf);
}

/* Event to fire the done callback. */
static void
file_fire_done_cb(__unused int fd, __unused short events, void *arg)
{
	struct client_file	*cf = arg;
	struct client		*c = cf->c;

	if (cf->cb != NULL &&
	    (cf->closed || c == NULL || (~c->flags & CLIENT_DEAD)))
		cf->cb(c, cf->path, cf->error, 1, cf->buffer, cf->data);
	file_free(cf);
}

/* Add an event to fire the done callback (used by the server). */
void
file_fire_done(struct client_file *cf)
{
	event_once(-1, EV_TIMEOUT, file_fire_done_cb, cf, NULL);
}

/* Fire the read callback. */
void
file_fire_read(struct client_file *cf)
{
	if (cf->cb != NULL)
		cf->cb(cf->c, cf->path, cf->error, 0, cf->buffer, cf->data);
}

/* Can this file be printed to? */
int
file_can_print(struct client *c)
{
	if (c == NULL ||
	    (c->flags & CLIENT_CONTROL))
		return (0);
#ifndef PLATFORM_WINDOWS
	/* On POSIX, attached clients have a valid tty fd - use direct write */
	if (c->flags & CLIENT_ATTACHED)
		return (0);
#endif
	/* On Windows, attached clients use IPC since we can't pass console handles */
	return (1);
}


/* Print a message to a file. */
void
file_print(struct client *c, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	file_vprint(c, fmt, ap);
	va_end(ap);
}

/* Print a message to a file. */
void
file_vprint(struct client *c, const char *fmt, va_list ap)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;

	if (!file_can_print(c))
		return;

	find.stream = 1;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 1, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add_vprintf(cf->buffer, fmt, ap);

		msg.stream = 1;
		msg.fd = STDOUT_FILENO;
		msg.flags = 0;
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add_vprintf(cf->buffer, fmt, ap);
		file_push(cf);
	}
}

/* Print a buffer to a file. */
void
file_print_buffer(struct client *c, void *data, size_t size)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;

#ifdef PLATFORM_WINDOWS
	win32_log("file_print_buffer: c=%p size=%zu\n", (void*)c, size);
#endif

	if (!file_can_print(c))
		return;

	find.stream = 1;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 1, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add(cf->buffer, data, size);

		msg.stream = 1;
		msg.fd = STDOUT_FILENO;
		msg.flags = 0;
#ifdef PLATFORM_WINDOWS
		win32_log("file_print_buffer: sending MSG_WRITE_OPEN stream=%d fd=%d\n",
			msg.stream, msg.fd);
#endif
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add(cf->buffer, data, size);
		file_push(cf);
	}
}


#ifdef PLATFORM_WINDOWS
/* 
 * Windows-specific: Send TTY output buffer to client via IPC.
 * Uses a dedicated stream (TTY_STREAM = 100) for attached client TTY output.
 * Unlike file_print_buffer, this works for attached clients.
 */
#define WIN32_TTY_STREAM 100

void
win32_tty_write(struct client *c, void *data, size_t size)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;

	if (c == NULL || c->peer == NULL)
		return;

	win32_log("win32_tty_write: c=%p size=%zu\n", (void*)c, size);

	/* Look for existing TTY stream file, or create one */
	find.stream = WIN32_TTY_STREAM;
	cf = RB_FIND(client_files, &c->files, &find);
	if (cf == NULL) {
		/* First time: create file and send MSG_WRITE_OPEN */
		cf = file_create_with_client(c, WIN32_TTY_STREAM, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add(cf->buffer, data, size);

		msg.stream = WIN32_TTY_STREAM;
		msg.fd = STDOUT_FILENO;  /* Tell client to write to stdout */
		msg.flags = 0;
		
		win32_log("win32_tty_write: sending MSG_WRITE_OPEN stream=%d fd=%d\n",
			msg.stream, msg.fd);
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else if (cf->closed) {
		/* Stream was closed? Should not happen for TTY but handle it */
		win32_log("win32_tty_write: ERROR stream already closed\n");
	} else {
		/* Stream already open: add data and push */
		evbuffer_add(cf->buffer, data, size);
		file_push(cf);
	}
}
#endif


/* Report an error to a file. */

void
file_error(struct client *c, const char *fmt, ...)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;
	va_list			 ap;

	if (!file_can_print(c))
		return;

	va_start(ap, fmt);

	find.stream = 2;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 2, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add_vprintf(cf->buffer, fmt, ap);

		msg.stream = 2;
		msg.fd = STDERR_FILENO;
		msg.flags = 0;
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add_vprintf(cf->buffer, fmt, ap);
		file_push(cf);
	}

	va_end(ap);
}

/* Write data to a file. */
void
file_write(struct client *c, const char *path, int flags, const void *bdata,
    size_t bsize, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;
	struct msg_write_open	*msg;
	size_t			 msglen;
	int			 fd = -1;
	u_int			 stream = file_next_stream++;
	FILE			*f;
	const char		*mode;

	if (strcmp(path, "-") == 0) {
		cf = file_create_with_client(c, stream, cb, cbdata);
		cf->path = xstrdup("-");

		fd = STDOUT_FILENO;
		if (c == NULL ||
		    (c->flags & CLIENT_ATTACHED) ||
		    (c->flags & CLIENT_CONTROL)) {
			cf->error = EBADF;
			goto done;
		}
		goto skip;
	}

	cf = file_create_with_client(c, stream, cb, cbdata);
	cf->path = file_get_path(c, path);

	if (c == NULL || c->flags & CLIENT_ATTACHED) {
		if (flags & O_APPEND)
			mode = "ab";
		else
			mode = "wb";
		f = fopen(cf->path, mode);
		if (f == NULL) {
			cf->error = errno;
			goto done;
		}
		if (fwrite(bdata, 1, bsize, f) != bsize) {
			fclose(f);
			cf->error = EIO;
			goto done;
		}
		fclose(f);
		goto done;
	}

skip:
	evbuffer_add(cf->buffer, bdata, bsize);

	msglen = strlen(cf->path) + 1 + sizeof *msg;
	if (msglen > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		cf->error = E2BIG;
		goto done;
	}
	msg = xmalloc(msglen);
	msg->stream = cf->stream;
	msg->fd = fd;
	msg->flags = flags;
	memcpy(msg + 1, cf->path, msglen - sizeof *msg);
	if (proc_send(cf->peer, MSG_WRITE_OPEN, -1, msg, msglen) != 0) {
		free(msg);
		cf->error = EINVAL;
		goto done;
	}
	free(msg);
	return;

done:
	file_fire_done(cf);
}

/* Read a file. */
struct client_file *
file_read(struct client *c, const char *path, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;
	struct msg_read_open	*msg;
	size_t			 msglen;
	int			 fd = -1;
	u_int			 stream = file_next_stream++;
	FILE			*f = NULL;
	size_t			 size;
	char			 buffer[BUFSIZ];

	if (strcmp(path, "-") == 0) {
		cf = file_create_with_client(c, stream, cb, cbdata);
		cf->path = xstrdup("-");

		fd = STDIN_FILENO;
		if (c == NULL ||
		    (c->flags & CLIENT_ATTACHED) ||
		    (c->flags & CLIENT_CONTROL)) {
			cf->error = EBADF;
			goto done;
		}
		goto skip;
	}

	cf = file_create_with_client(c, stream, cb, cbdata);
	cf->path = file_get_path(c, path);

	if (c == NULL || c->flags & CLIENT_ATTACHED) {
		f = fopen(cf->path, "rb");
		if (f == NULL) {
			cf->error = errno;
			goto done;
		}
		for (;;) {
			size = fread(buffer, 1, sizeof buffer, f);
			if (evbuffer_add(cf->buffer, buffer, size) != 0) {
				cf->error = ENOMEM;
				goto done;
			}
			if (size != sizeof buffer)
				break;
		}
		if (ferror(f)) {
			cf->error = EIO;
			goto done;
		}
		goto done;
	}

skip:
	msglen = strlen(cf->path) + 1 + sizeof *msg;
	if (msglen > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		cf->error = E2BIG;
		goto done;
	}
	msg = xmalloc(msglen);
	msg->stream = cf->stream;
	msg->fd = fd;
	memcpy(msg + 1, cf->path, msglen - sizeof *msg);
	if (proc_send(cf->peer, MSG_READ_OPEN, -1, msg, msglen) != 0) {
		free(msg);
		cf->error = EINVAL;
		goto done;
	}
	free(msg);
	return cf;

done:
	if (f != NULL)
		fclose(f);
	file_fire_done(cf);
	return NULL;
}

/* Cancel a file read. */
void
file_cancel(struct client_file *cf)
{
	struct msg_read_cancel	 msg;

	log_debug("read cancel file %d", cf->stream);

	if (cf->closed)
		return;
	cf->closed = 1;

	msg.stream = cf->stream;
	proc_send(cf->peer, MSG_READ_CANCEL, -1, &msg, sizeof msg);
}

/* Push event, fired if there is more writing to be done. */
static void
file_push_cb(__unused int fd, __unused short events, void *arg)
{
	struct client_file	*cf = arg;

	if (cf->c == NULL || ~cf->c->flags & CLIENT_DEAD)
		file_push(cf);
	file_free(cf);
}

/* Push uwritten data to the client for a file, if it will accept it. */
void
file_push(struct client_file *cf)
{
	struct msg_write_data	*msg;
	size_t			 msglen, sent, left;
	struct msg_write_close	 close;

	msg = xmalloc(sizeof *msg);
	left = EVBUFFER_LENGTH(cf->buffer);
	while (left != 0) {
		sent = left;
		if (sent > MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg)
			sent = MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg;

		msglen = (sizeof *msg) + sent;
		msg = xrealloc(msg, msglen);
		msg->stream = cf->stream;
		memcpy(msg + 1, EVBUFFER_DATA(cf->buffer), sent);
		if (proc_send(cf->peer, MSG_WRITE, -1, msg, msglen) != 0)
			break;
		evbuffer_drain(cf->buffer, sent);

		left = EVBUFFER_LENGTH(cf->buffer);
		log_debug("file %d sent %zu, left %zu", cf->stream, sent, left);
	}
	if (left != 0) {
		cf->references++;
		event_once(-1, EV_TIMEOUT, file_push_cb, cf, NULL);
	} else if (cf->stream > 2 && cf->stream != WIN32_TTY_STREAM) {
		close.stream = cf->stream;
		proc_send(cf->peer, MSG_WRITE_CLOSE, -1, &close, sizeof close);
		file_fire_done(cf);
	}
	free(msg);
}

/* Check if any files have data left to write. */
int
file_write_left(struct client_files *files)
{
	struct client_file	*cf;
	size_t			 left;
	int			 waiting = 0;

	RB_FOREACH(cf, client_files, files) {
		if (cf->event == NULL)
			continue;
		left = EVBUFFER_LENGTH(cf->event->output);
		if (left != 0) {
			waiting++;
			log_debug("file %u %zu bytes left", cf->stream, left);
		}
	}
	return (waiting != 0);
}

/* Client file write error callback. */
static void
file_write_error_callback(__unused struct bufferevent *bev, __unused short what,
    void *arg)
{
	struct client_file	*cf = arg;

	log_debug("write error file %d", cf->stream);

	bufferevent_free(cf->event);
	cf->event = NULL;

	close(cf->fd);
	cf->fd = -1;

	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);
}

/* Client file write callback. */
static void
file_write_callback(__unused struct bufferevent *bev, void *arg)
{
	struct client_file	*cf = arg;

	log_debug("write check file %d", cf->stream);

	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);

	if (cf->closed && EVBUFFER_LENGTH(cf->event->output) == 0) {
		bufferevent_free(cf->event);
		close(cf->fd);
		RB_REMOVE(client_files, cf->tree, cf);
		file_free(cf);
	}
}

/* Handle a file write open message (client). */
void
file_write_open(struct client_files *files, struct tmuxpeer *peer,
    struct imsg *imsg, int allow_streams, int close_received,
    client_file_cb cb, void *cbdata)
{
	struct msg_write_open	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	const char		*path;
	struct msg_write_ready	 reply;
	struct client_file	 find, *cf;
	const int		 flags = O_NONBLOCK|O_WRONLY|O_CREAT;
	int			 error = 0;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_WRITE_OPEN size");
	if (msglen == sizeof *msg)
		path = "-";
	else
		path = (const char *)(msg + 1);
	log_debug("open write file %d %s", msg->stream, path);

	find.stream = msg->stream;
	if (RB_FIND(client_files, files, &find) != NULL) {
		error = EBADF;
		goto reply;
	}
	cf = file_create_with_peer(peer, files, msg->stream, cb, cbdata);
	if (cf->closed) {
		error = EBADF;
		goto reply;
	}

#ifdef PLATFORM_WINDOWS
	win32_log("file_write_open: stream=%d path=%s msg->fd=%d allow_streams=%d\n",
		msg->stream, path, msg->fd, allow_streams);
#endif

	cf->fd = -1;
	if (msg->fd == -1)
		cf->fd = open(path, msg->flags|flags, 0644);
	else if (allow_streams) {
		if (msg->fd != STDOUT_FILENO && msg->fd != STDERR_FILENO)
			errno = EBADF;
		else {
#ifdef PLATFORM_WINDOWS
			/*
			 * Windows: _dup() asserts if the fd is not open in the
			 * MSVCRT table (e.g. in the detached server process which
			 * has no console). Validate via _get_osfhandle first, then
			 * fall back to GetStdHandle + _open_osfhandle.
			 */
			{
				DWORD std_handle_id = (msg->fd == STDOUT_FILENO)
					? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
				intptr_t oh = _get_osfhandle(msg->fd);
				if (oh != -1 && (HANDLE)oh != INVALID_HANDLE_VALUE) {
					cf->fd = _dup(msg->fd);
				} else {
					HANDLE h = GetStdHandle(std_handle_id);
					if (h != INVALID_HANDLE_VALUE && h != NULL)
						cf->fd = _open_osfhandle((intptr_t)h, _O_TEXT);
					else
						cf->fd = -1;
				}
			}
			win32_log("file_write_open: Windows dup result cf->fd=%d errno=%d\n",
				cf->fd, (cf->fd == -1 ? errno : 0));
#else
			cf->fd = dup(msg->fd);
			if (close_received)
				close(msg->fd); /* can only be used once */
#endif
		}
	} else
	      errno = EBADF;
	if (cf->fd == -1) {
		error = errno;
#ifdef PLATFORM_WINDOWS
		win32_log("file_write_open: FAILED cf->fd=-1 error=%d\n", error);
#endif
		goto reply;
	}


#ifdef PLATFORM_WINDOWS
	/* Windows: STDOUT/STDERR handles are not selectable and cause spin in libevent. */
	if (msg->fd == STDOUT_FILENO || msg->fd == STDERR_FILENO) {
		cf->event = NULL;
		win32_log("file_write_open: skipping bufferevent for stdout/stderr (fd=%d)\n", cf->fd);
		goto reply;
	}
#endif

	cf->event = bufferevent_new(cf->fd, NULL, file_write_callback,
	    file_write_error_callback, cf);
	if (cf->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(cf->event, EV_WRITE);
	goto reply;

reply:
	reply.stream = msg->stream;
	reply.error = error;
	proc_send(peer, MSG_WRITE_READY, -1, &reply, sizeof reply);
}

/* Handle a file write data message (client). */
void
file_write_data(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_data	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;
	size_t			 size = msglen - sizeof *msg;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_WRITE size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("write %zu to file %d", size, cf->stream);

	if (cf->event != NULL)
		bufferevent_write(cf->event, msg + 1, size);
#ifdef PLATFORM_WINDOWS
	else if (cf->fd != -1 && (cf->stream == 1 || cf->stream == 2 || cf->stream == 100)) {
		/* Write directly to console handle to ensure VT sequences are processed */
		DWORD std_handle = (cf->stream == 2) ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
		HANDLE hConsole = GetStdHandle(std_handle);
		if (hConsole != INVALID_HANDLE_VALUE && hConsole != NULL) {
			DWORD mode = 0, written = 0;
			GetConsoleMode(hConsole, &mode);
			win32_log("file_write_data: stream=%d hConsole=%p mode=0x%lx size=%zu\n", 
				cf->stream, (void*)hConsole, mode, size);
			
			/* Hex dump first 64 bytes to verify VT sequences */
			if (size > 0) {
				char hexbuf[200];
				size_t dumplen = (size > 64) ? 64 : size;
				const unsigned char *data = (const unsigned char*)(msg + 1);
				char *p = hexbuf;
				for (size_t i = 0; i < dumplen && (p - hexbuf) < 190; i++) {
					if (data[i] == 0x1b) {
						p += sprintf(p, "[ESC]");
					} else if (data[i] < 32) {
						p += sprintf(p, "[%02X]", data[i]);
					} else {
						*p++ = data[i];
					}
				}
				*p = '\0';
				win32_log("file_write_data: first bytes: %s\n", hexbuf);
			}
			
			/* Capture raw VT data to file for debugging */
			{
				static FILE *cap = NULL;
				if (cap == NULL) cap = fopen("tty_capture.bin", "wb");
				if (cap) { fwrite(msg + 1, 1, size, cap); fflush(cap); }
			}
			if (!WriteFile(hConsole, msg + 1, (DWORD)size, &written, NULL)) {
				win32_log("file_write_data: WriteFile failed, error=%lu\n", GetLastError());
			} else {
				win32_log("file_write_data: WriteFile ok, requested=%zu written=%lu\n", size, written);
			}
		} else {
			/* Fallback to win32_write if console handle unavailable */
			win32_log("file_write_data: no console handle, using win32_write\n");
			win32_write(cf->fd, msg + 1, size);
		}
	}
#endif
}

/* Handle a file write close message (client). */
void
file_write_close(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_close	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_WRITE_CLOSE size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("close file %d", cf->stream);

	if (cf->event == NULL || EVBUFFER_LENGTH(cf->event->output) == 0) {
		if (cf->event != NULL)
			bufferevent_free(cf->event);
		if (cf->fd != -1)
			close(cf->fd);
		RB_REMOVE(client_files, files, cf);
		file_free(cf);
	}
}

/* Client file read error callback. */
static void
file_read_error_callback(__unused struct bufferevent *bev, __unused short what,
    void *arg)
{
	struct client_file	*cf = arg;
	struct msg_read_done	 msg;

	log_debug("read error file %d", cf->stream);

	msg.stream = cf->stream;
	msg.error = 0;
	proc_send(cf->peer, MSG_READ_DONE, -1, &msg, sizeof msg);

	bufferevent_free(cf->event);
	close(cf->fd);
	RB_REMOVE(client_files, cf->tree, cf);
	file_free(cf);
}

/* Client file read callback. */
static void
file_read_callback(__unused struct bufferevent *bev, void *arg)
{
	struct client_file	*cf = arg;
	void			*bdata;
	size_t			 bsize;
	struct msg_read_data	*msg;
	size_t			 msglen;

	msg = xmalloc(sizeof *msg);
	for (;;) {
		bdata = EVBUFFER_DATA(cf->event->input);
		bsize = EVBUFFER_LENGTH(cf->event->input);

		if (bsize == 0)
			break;
		if (bsize > MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg)
			bsize = MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg;
		log_debug("read %zu from file %d", bsize, cf->stream);

		msglen = (sizeof *msg) + bsize;
		msg = xrealloc(msg, msglen);
		msg->stream = cf->stream;
		memcpy(msg + 1, bdata, bsize);
		proc_send(cf->peer, MSG_READ, -1, msg, msglen);

		evbuffer_drain(cf->event->input, bsize);
	}
	free(msg);
}

/* Handle a file read open message (client). */
void
file_read_open(struct client_files *files, struct tmuxpeer *peer,
    struct imsg *imsg, int allow_streams, int close_received, client_file_cb cb,
    void *cbdata)
{
	struct msg_read_open	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	const char		*path;
	struct msg_read_done	 reply;
	struct client_file	 find, *cf;
	const int		 flags = O_NONBLOCK|O_RDONLY;
	int			 error;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_READ_OPEN size");
	if (msglen == sizeof *msg)
		path = "-";
	else
		path = (const char *)(msg + 1);
	log_debug("open read file %d %s", msg->stream, path);

	find.stream = msg->stream;
	if (RB_FIND(client_files, files, &find) != NULL) {
		error = EBADF;
		goto reply;
	}
	cf = file_create_with_peer(peer, files, msg->stream, cb, cbdata);
	if (cf->closed) {
		error = EBADF;
		goto reply;
	}

	cf->fd = -1;
	if (msg->fd == -1)
		cf->fd = open(path, flags);
	else if (allow_streams) {
		if (msg->fd != STDIN_FILENO)
			errno = EBADF;
		else {
			cf->fd = dup(msg->fd);
			if (close_received)
				close(msg->fd); /* can only be used once */
		}
	} else
		errno = EBADF;
	if (cf->fd == -1) {
		error = errno;
		goto reply;
	}


#ifdef PLATFORM_WINDOWS
	/*
	 * On Windows, regular file fds are not compatible with libevent's
	 * event loop (which uses WaitForMultipleObjects/select on sockets).
	 * Read the file synchronously and send MSG_READ + MSG_READ_DONE
	 * directly instead of using bufferevent.
	 */
	{
		char			 rbuf[BUFSIZ];
		ssize_t			 rn;
		struct msg_read_data	*rmsg;
		struct msg_read_done	 rdone;
		size_t			 rmsglen;

		rmsg = xmalloc(sizeof *rmsg + sizeof rbuf);
		for (;;) {
			rn = read(cf->fd, rbuf, sizeof rbuf);
			if (rn <= 0)
				break;
			rmsglen = sizeof *rmsg + (size_t)rn;
			rmsg = xrealloc(rmsg, rmsglen);
			rmsg->stream = cf->stream;
			memcpy(rmsg + 1, rbuf, (size_t)rn);
			proc_send(cf->peer, MSG_READ, -1, rmsg, rmsglen);
		}
		free(rmsg);
		close(cf->fd);
		cf->fd = -1;
		rdone.stream = cf->stream;
		rdone.error = (rn < 0) ? errno : 0;
		proc_send(cf->peer, MSG_READ_DONE, -1, &rdone, sizeof rdone);
		file_free(cf);
		return;
	}
#endif

	cf->event = bufferevent_new(cf->fd, file_read_callback, NULL,
	    file_read_error_callback, cf);
	if (cf->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(cf->event, EV_READ);
	return;

reply:
	reply.stream = msg->stream;
	reply.error = error;
	proc_send(peer, MSG_READ_DONE, -1, &reply, sizeof reply);
}

/* Handle a read cancel message (client). */
void
file_read_cancel(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_cancel	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_READ_CANCEL size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("cancel file %d", cf->stream);

	file_read_error_callback(NULL, 0, cf);
}

/* Handle a write ready message (server). */
void
file_write_ready(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_ready	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_WRITE_READY size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return;
	if (msg->error != 0) {
		cf->error = msg->error;
		file_fire_done(cf);
	} else
		file_push(cf);
}

/* Handle read data message (server). */
void
file_read_data(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_data	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;
	void			*bdata = msg + 1;
	size_t			 bsize = msglen - sizeof *msg;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_READ_DATA size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return;

	log_debug("file %d read %zu bytes", cf->stream, bsize);
	if (cf->error == 0 && !cf->closed) {
		if (evbuffer_add(cf->buffer, bdata, bsize) != 0) {
			cf->error = ENOMEM;
			file_fire_done(cf);
		} else
			file_fire_read(cf);
	}
}

/* Handle a read done message (server). */
void
file_read_done(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_done	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_READ_DONE size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return;

	log_debug("file %d read done", cf->stream);
	cf->error = msg->error;
	file_fire_done(cf);
}
