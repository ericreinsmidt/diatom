/* The launcher protocol - ADR-0009.
 *
 * One Unix domain socket, line-based, tab-separated `key=value`. Tabs separate
 * fields so that paths may contain spaces, which ROM filenames routinely do.
 *
 * The rule this file exists to uphold is about the DISPLAY, not about error
 * reporting:
 *
 *     ERROR means the game never started. EXIT means it ran and stopped.
 *
 * A launcher that hears RUNNING stops drawing. If a missing BIOS were reported
 * as RUNNING and then immediately EXIT, the launcher would hand over the screen
 * for a game that never existed, and the player would see a black frame before
 * the shelf came back.
 *
 * Unknown verbs and unknown keys are ignored rather than refused, so a newer
 * launcher can talk to an older Diatom without a version negotiation.
 *
 * One connection at a time. A second concurrent connect is accepted and closed
 * immediately rather than queued: the launcher restart case is handled by the
 * dead connection reaching EOF, which happens even on SIGKILL, so there is
 * never a live connection to displace.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "diatom.h"

static int  g_listen = -1;
static int  g_conn   = -1;
static char g_path[256];
static char g_in[4096];      /* accumulates until a newline arrives */
static size_t g_used;

static void log_(diatom_log_level lvl, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	diatom_port_log(lvl, buf);
}

bool diatom_proto_listen(const char *path)
{
	struct sockaddr_un addr;

	if (!path || !*path) return false;
	if (strlen(path) >= sizeof addr.sun_path) {
		log_(DIATOM_LOG_ERROR, "proto: socket path too long: %s", path);
		return false;
	}

	/* A stale socket file outbids a live one, so remove it. Safe because a
	 * second Diatom is not a supported configuration - the display cannot be
	 * shared (see the handoff spike). */
	unlink(path);

	g_listen = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_listen < 0) {
		log_(DIATOM_LOG_ERROR, "proto: socket: %s", strerror(errno));
		return false;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

	if (bind(g_listen, (struct sockaddr *)&addr, sizeof addr) != 0 ||
	    listen(g_listen, 1) != 0) {
		log_(DIATOM_LOG_ERROR, "proto: bind/listen %s: %s", path, strerror(errno));
		close(g_listen);
		g_listen = -1;
		return false;
	}
	fcntl(g_listen, F_SETFL, O_NONBLOCK);
	snprintf(g_path, sizeof g_path, "%s", path);
	log_(DIATOM_LOG_INFO, "proto: listening on %s", path);
	return true;
}

void diatom_proto_close(void)
{
	if (g_conn   >= 0) { close(g_conn);   g_conn   = -1; }
	if (g_listen >= 0) { close(g_listen); g_listen = -1; }
	if (g_path[0]) { unlink(g_path); g_path[0] = '\0'; }
	g_used = 0;
}

bool diatom_proto_active(void)     { return g_listen >= 0; }
bool diatom_proto_connected(void)  { return g_conn   >= 0; }

void diatom_proto_send(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	if (g_conn < 0) return;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	buf[n++] = '\n';
	/* MSG_NOSIGNAL: a launcher that died mid-write must not take Diatom with
	 * it. The write fails, the next poll reports the hangup, and the game
	 * keeps running - which is the whole point of ADR-0008. */
	if (send(g_conn, buf, (size_t)n, MSG_NOSIGNAL) < 0 && errno != EAGAIN)
		log_(DIATOM_LOG_WARN, "proto: send: %s", strerror(errno));
}

/* The crash path. Async-signal-safe, which rules out the send above: vsnprintf
 * is not on POSIX's list, and neither is re-entering the buffer it formats
 * into. So `line` must arrive complete, newline included, as a constant.
 *
 * send() rather than write() for two reasons, both required. send() is on the
 * async-signal-safe list. MSG_NOSIGNAL stops a launcher that died first from
 * turning a reportable crash into an unreportable one - without it the SIGPIPE
 * default disposition would kill us here, before we could re-raise the signal
 * that actually caused the crash.
 *
 * One pass, and no retry on EAGAIN. A launcher that has stopped reading would
 * otherwise turn a crash into a hang, and an unreported crash is much the
 * lesser fault - the socket closing tells the launcher something died anyway.
 */
void diatom_proto_emit_fatal(const char *line)
{
	size_t n = 0, off = 0;

	if (g_conn < 0 || !line) return;
	while (line[n]) n++;          /* strlen is not on the safe list either */

	while (off < n) {
		ssize_t w = send(g_conn, line + off, n - off, MSG_NOSIGNAL);
		if (w <= 0) return;
		off += (size_t)w;
	}
}

/* Fill `out` from one complete line. Unknown keys are skipped silently. */
static void parse_line(char *line, diatom_msg *out)
{
	char *save = NULL, *field;

	memset(out, 0, sizeof *out);
	field = strtok_r(line, "\t", &save);
	if (!field) { out->kind = DIATOM_MSG_NONE; return; }

	if      (!strcmp(field, "RUN"))     out->kind = DIATOM_MSG_RUN;
	else if (!strcmp(field, "STOP"))    out->kind = DIATOM_MSG_STOP;
	else if (!strcmp(field, "QUIT"))    out->kind = DIATOM_MSG_QUIT;
	else if (!strcmp(field, "RESUME"))  out->kind = DIATOM_MSG_RESUME;
	else if (!strcmp(field, "RESET"))   out->kind = DIATOM_MSG_RESET;
	else if (!strcmp(field, "SAVE"))    out->kind = DIATOM_MSG_SAVE;
	else if (!strcmp(field, "LOAD"))    out->kind = DIATOM_MSG_LOAD;
	else if (!strcmp(field, "OPTIONS")) out->kind = DIATOM_MSG_OPTIONS;
	else if (!strcmp(field, "SETOPT"))  out->kind = DIATOM_MSG_SETOPT;
	else if (!strcmp(field, "INPUTS"))   out->kind = DIATOM_MSG_INPUTS;
	else if (!strcmp(field, "MAP"))      out->kind = DIATOM_MSG_MAP;
	else if (!strcmp(field, "SETMAP"))   out->kind = DIATOM_MSG_SETMAP;
	else if (!strcmp(field, "LEVELS"))   out->kind = DIATOM_MSG_LEVELS;
	else if (!strcmp(field, "SETLEVEL")) out->kind = DIATOM_MSG_SETLEVEL;
	else if (!strcmp(field, "DISPLAY"))  out->kind = DIATOM_MSG_DISPLAY;
	else if (!strcmp(field, "SETDISPLAY")) out->kind = DIATOM_MSG_SETDISPLAY;
	else if (!strcmp(field, "OVERLAY"))    out->kind = DIATOM_MSG_OVERLAY;
	else if (!strcmp(field, "CHEEVOS"))    out->kind = DIATOM_MSG_CHEEVOS;
	else if (!strcmp(field, "SETCHEEVOS")) out->kind = DIATOM_MSG_SETCHEEVOS;
	else {
		log_(DIATOM_LOG_WARN, "proto: ignoring unknown verb '%s'", field);
		out->kind = DIATOM_MSG_NONE;
		return;
	}

	while ((field = strtok_r(NULL, "\t", &save)) != NULL) {
		char *eq = strchr(field, '=');
		const char *v;
		if (!eq) continue;
		*eq = '\0';
		v = eq + 1;
		if      (!strcmp(field, "core")) snprintf(out->core, sizeof out->core, "%s", v);
		else if (!strcmp(field, "rom"))  snprintf(out->rom,  sizeof out->rom,  "%s", v);
		/* ADR-0017. A key ADR-0009 did not define, which costs nothing to add
		 * because unknown keys are ignored: an older Diatom drops it and fails
		 * the way it always did, a newer one checks before loading. */
		else if (!strcmp(field, "firmware"))
			snprintf(out->firmware, sizeof out->firmware, "%s", v);
		else if (!strcmp(field, "tag"))  snprintf(out->tag,  sizeof out->tag,  "%s", v);
		else if (!strcmp(field, "slot")) snprintf(out->slot,  sizeof out->slot,  "%s", v);
		else if (!strcmp(field, "resume"))
			snprintf(out->resume, sizeof out->resume, "%s", v);
		else if (!strcmp(field, "exit_state"))
			snprintf(out->exit_state, sizeof out->exit_state, "%s", v);
		else if (!strcmp(field, "preview"))
			snprintf(out->preview, sizeof out->preview, "%s", v);
		else if (!strcmp(field, "path")) snprintf(out->path,  sizeof out->path,  "%s", v);
		/* ADR-0026. `cheevos=` is a path on RUN rather than the set itself:
		 * a condition string runs to kilobytes and would not survive a line
		 * protocol, and ADR-0016 already has the launcher passing paths. */
		else if (!strcmp(field, "cheevos"))
			snprintf(out->cheevos, sizeof out->cheevos, "%s", v);
		else if (!strcmp(field, "console")) out->console = (int)strtol(v, NULL, 10);
		else if (!strcmp(field, "key"))  snprintf(out->key,   sizeof out->key,   "%s", v);
		else if (!strcmp(field, "value"))snprintf(out->value, sizeof out->value, "%s", v);
		else if (!strcmp(field, "map"))  snprintf(out->map,   sizeof out->map,   "%s", v);
		else if (!strcmp(field, "kind")) snprintf(out->lkind, sizeof out->lkind, "%s", v);
		else if (!strcmp(field, "index")) out->index = (int)strtol(v, NULL, 10);
		else if (!strcmp(field, "count")) out->count = (int)strtol(v, NULL, 10);
		/* OVERLAY's duration, into the same int. A second integer field whose
		 * only difference is which verb reads it would be two names for one
		 * slot, which is how the launcher ends up setting the wrong one. */
		else if (!strcmp(field, "ms"))    out->count = (int)strtol(v, NULL, 10);
		else if (!strcmp(field, "mode"))  snprintf(out->dmode,  sizeof out->dmode,  "%s", v);
		else if (!strcmp(field, "filter"))snprintf(out->dfilter,sizeof out->dfilter,"%s", v);
		/* anything else: forward compatibility, ignore */
	}
}

/* Take one buffered line if there is one. */
static bool take_line(diatom_msg *out)
{
	char *nl = memchr(g_in, '\n', g_used);
	size_t len;

	if (!nl) return false;
	*nl = '\0';
	len = (size_t)(nl - g_in) + 1;
	parse_line(g_in, out);
	memmove(g_in, g_in + len, g_used - len);
	g_used -= len;
	return true;
}

static void accept_or_refuse(void)
{
	int fd = accept(g_listen, NULL, NULL);
	if (fd < 0) return;
	if (g_conn >= 0) {
		/* One at a time. Closing immediately is the refusal. */
		log_(DIATOM_LOG_WARN, "proto: refusing a second connection");
		close(fd);
		return;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	g_conn = fd;
	g_used = 0;
	log_(DIATOM_LOG_INFO, "proto: launcher connected");
}

diatom_msg_kind diatom_proto_poll(diatom_msg *out, int timeout_ms, bool running)
{
	struct pollfd p[2];
	int n = 0, li = -1, ci = -1;
	ssize_t got;

	memset(out, 0, sizeof *out);
	if (g_listen < 0) return DIATOM_MSG_NONE;

	/* A line may already be buffered from a previous read. */
	if (g_conn >= 0 && take_line(out)) return out->kind;

	if (g_conn < 0) { p[n].fd = g_listen; p[n].events = POLLIN; li = n++; }
	else            { p[n].fd = g_conn;   p[n].events = POLLIN; ci = n++; }

	if (poll(p, (nfds_t)n, timeout_ms) <= 0) return DIATOM_MSG_NONE;

	if (li >= 0 && (p[li].revents & POLLIN)) {
		accept_or_refuse();
		/* Tell a fresh launcher whether the screen is already spoken for.
		 * Without this, a launcher restarted by launch.sh while a game runs
		 * would draw its shelf over live output. */
		/* proto=2 adds ADR-0020's state plane. ADR-0009 already promises
		 * unknown verbs are ignored, so an old launcher is unaffected; this is
		 * how a NEW launcher discovers the plane is absent rather than
		 * inferring it from silence. */
		diatom_proto_send("READY\tproto=2\tstate=%s", running ? "running" : "idle");
		return DIATOM_MSG_NONE;
	}

	if (ci >= 0 && (p[ci].revents & (POLLIN | POLLHUP | POLLERR))) {
		got = read(g_conn, g_in + g_used, sizeof g_in - g_used - 1);
		if (got > 0) {
			g_used += (size_t)got;
			if (take_line(out)) return out->kind;
			if (g_used >= sizeof g_in - 1) {
				log_(DIATOM_LOG_WARN, "proto: oversized line, dropping");
				g_used = 0;
			}
			return DIATOM_MSG_NONE;
		}
		/* EOF, including the launcher being SIGKILLed. The game keeps
		 * running; ADR-0008 exists so a dead launcher cannot end it. */
		log_(DIATOM_LOG_INFO, "proto: launcher disconnected");
		close(g_conn);
		g_conn = -1;
		g_used = 0;
		return DIATOM_MSG_HANGUP;
	}
	return DIATOM_MSG_NONE;
}
