/*
 * PLink - a command-line (stdin/stdout) variant of PuTTY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#ifndef HAVE_NO_SYS_SELECT_H
#include <sys/select.h>
#endif

#define PUTTY_DO_GLOBALS	       /* actually _define_ globals */
#include "putty.h"
#include "storage.h"
#include "tree234.h"

#define MAX_STDIN_BACKLOG 4096

void *logctx;

void fatalbox(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (logctx) {
        log_free(logctx);
        logctx = NULL;
    }
    cleanup_exit(1);
}
void modalfatalbox(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (logctx) {
        log_free(logctx);
        logctx = NULL;
    }
    cleanup_exit(1);
}
void connection_fatal(void *frontend, char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (logctx) {
        log_free(logctx);
        logctx = NULL;
    }
    cleanup_exit(1);
}
void cmdline_error(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "plink: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int local_tty = 0; /* do we have a local tty? */
static struct termios orig_termios;

static Backend *back;
static void *backhandle;
static Config cfg;

/*
 * Default settings that are specific to pterm.
 */
char *platform_default_s(const char *name)
{
    if (!strcmp(name, "TermType"))
	return dupstr(getenv("TERM"));
    if (!strcmp(name, "UserName"))
	return get_username();
    if (!strcmp(name, "SerialLine"))
	return dupstr("/dev/ttyS0");
    return NULL;
}

int platform_default_i(const char *name, int def)
{
    if (!strcmp(name, "TermWidth") ||
	!strcmp(name, "TermHeight")) {
	struct winsize size;
	if (ioctl(0, TIOCGWINSZ, (void *)&size) >= 0)
	    return (!strcmp(name, "TermWidth") ? size.ws_col : size.ws_row);
    }
    return def;
}

FontSpec platform_default_fontspec(const char *name)
{
    FontSpec ret;
    *ret.name = '\0';
    return ret;
}

Filename platform_default_filename(const char *name)
{
    Filename ret;
    if (!strcmp(name, "LogFileName"))
	strcpy(ret.path, "putty.log");
    else
	*ret.path = '\0';
    return ret;
}

char *x_get_default(const char *key)
{
    return NULL;		       /* this is a stub */
}
int term_ldisc(Terminal *term, int mode)
{
    return FALSE;
}
void ldisc_update(void *frontend, int echo, int edit)
{
    /* Update stdin read mode to reflect changes in line discipline. */
    struct termios mode;

    if (!local_tty) return;

    mode = orig_termios;

    if (echo)
	mode.c_lflag |= ECHO;
    else
	mode.c_lflag &= ~ECHO;

    if (edit) {
	mode.c_iflag |= ICRNL;
	mode.c_lflag |= ISIG | ICANON;
	mode.c_oflag |= OPOST;
    } else {
	mode.c_iflag &= ~ICRNL;
	mode.c_lflag &= ~(ISIG | ICANON);
	mode.c_oflag &= ~OPOST;
	/* Solaris sets these to unhelpful values */
	mode.c_cc[VMIN] = 1;
	mode.c_cc[VTIME] = 0;
	/* FIXME: perhaps what we do with IXON/IXOFF should be an
	 * argument to ldisc_update(), to allow implementation of SSH-2
	 * "xon-xoff" and Rlogin's equivalent? */
	mode.c_iflag &= ~IXON;
	mode.c_iflag &= ~IXOFF;
    }
    /* 
     * Mark parity errors and (more important) BREAK on input.  This
     * is more complex than it need be because POSIX-2001 suggests
     * that escaping of valid 0xff in the input stream is dependent on
     * IGNPAR being clear even though marking of BREAK isn't.  NetBSD
     * 2.0 goes one worse and makes it dependent on INPCK too.  We
     * deal with this by forcing these flags into a useful state and
     * then faking the state in which we found them in from_tty() if
     * we get passed a parity or framing error.
     */
    mode.c_iflag = (mode.c_iflag | INPCK | PARMRK) & ~IGNPAR;

    tcsetattr(0, TCSANOW, &mode);
}

/* Helper function to extract a special character from a termios. */
static char *get_ttychar(struct termios *t, int index)
{
    cc_t c = t->c_cc[index];
#if defined(_POSIX_VDISABLE)
    if (c == _POSIX_VDISABLE)
	return dupprintf("");
#endif
    return dupprintf("^<%d>", c);
}

char *get_ttymode(void *frontend, const char *mode)
{
    /*
     * Propagate appropriate terminal modes from the local terminal,
     * if any.
     */
    if (!local_tty) return NULL;

#define GET_CHAR(ourname, uxname) \
    do { \
	if (strcmp(mode, ourname) == 0) \
	    return get_ttychar(&orig_termios, uxname); \
    } while(0)
#define GET_BOOL(ourname, uxname, uxmemb, transform) \
    do { \
	if (strcmp(mode, ourname) == 0) { \
	    int b = (orig_termios.uxmemb & uxname) != 0; \
	    transform; \
	    return dupprintf("%d", b); \
	} \
    } while (0)

    /*
     * Modes that want to be the same on all terminal devices involved.
     */
    /* All the special characters supported by SSH */
#if defined(VINTR)
    GET_CHAR("INTR", VINTR);
#endif
#if defined(VQUIT)
    GET_CHAR("QUIT", VQUIT);
#endif
#if defined(VERASE)
    GET_CHAR("ERASE", VERASE);
#endif
#if defined(VKILL)
    GET_CHAR("KILL", VKILL);
#endif
#if defined(VEOF)
    GET_CHAR("EOF", VEOF);
#endif
#if defined(VEOL)
    GET_CHAR("EOL", VEOL);
#endif
#if defined(VEOL2)
    GET_CHAR("EOL2", VEOL2);
#endif
#if defined(VSTART)
    GET_CHAR("START", VSTART);
#endif
#if defined(VSTOP)
    GET_CHAR("STOP", VSTOP);
#endif
#if defined(VSUSP)
    GET_CHAR("SUSP", VSUSP);
#endif
#if defined(VDSUSP)
    GET_CHAR("DSUSP", VDSUSP);
#endif
#if defined(VREPRINT)
    GET_CHAR("REPRINT", VREPRINT);
#endif
#if defined(VWERASE)
    GET_CHAR("WERASE", VWERASE);
#endif
#if defined(VLNEXT)
    GET_CHAR("LNEXT", VLNEXT);
#endif
#if defined(VFLUSH)
    GET_CHAR("FLUSH", VFLUSH);
#endif
#if defined(VSWTCH)
    GET_CHAR("SWTCH", VSWTCH);
#endif
#if defined(VSTATUS)
    GET_CHAR("STATUS", VSTATUS);
#endif
#if defined(VDISCARD)
    GET_CHAR("DISCARD", VDISCARD);
#endif
    /* Modes that "configure" other major modes. These should probably be
     * considered as user preferences. */
    /* Configuration of ICANON */
#if defined(ECHOK)
    GET_BOOL("ECHOK", ECHOK, c_lflag, );
#endif
#if defined(ECHOKE)
    GET_BOOL("ECHOKE", ECHOKE, c_lflag, );
#endif
#if defined(ECHOE)
    GET_BOOL("ECHOE", ECHOE, c_lflag, );
#endif
#if defined(ECHONL)
    GET_BOOL("ECHONL", ECHONL, c_lflag, );
#endif
#if defined(XCASE)
    GET_BOOL("XCASE", XCASE, c_lflag, );
#endif
    /* Configuration of ECHO */
#if defined(ECHOCTL)
    GET_BOOL("ECHOCTL", ECHOCTL, c_lflag, );
#endif
    /* Configuration of IXON/IXOFF */
#if defined(IXANY)
    GET_BOOL("IXANY", IXANY, c_iflag, );
#endif
    /* Configuration of OPOST */
#if defined(OLCUC)
    GET_BOOL("OLCUC", OLCUC, c_oflag, );
#endif
#if defined(ONLCR)
    GET_BOOL("ONLCR", ONLCR, c_oflag, );
#endif
#if defined(OCRNL)
    GET_BOOL("OCRNL", OCRNL, c_oflag, );
#endif
#if defined(ONOCR)
    GET_BOOL("ONOCR", ONOCR, c_oflag, );
#endif
#if defined(ONLRET)
    GET_BOOL("ONLRET", ONLRET, c_oflag, );
#endif

    /*
     * Modes that want to be set in only one place, and that we have
     * squashed locally.
     */
#if defined(ISIG)
    GET_BOOL("ISIG", ISIG, c_lflag, );
#endif
#if defined(ICANON)
    GET_BOOL("ICANON", ICANON, c_lflag, );
#endif
#if defined(ECHO)
    GET_BOOL("ECHO", ECHO, c_lflag, );
#endif
#if defined(IXON)
    GET_BOOL("IXON", IXON, c_iflag, );
#endif
#if defined(IXOFF)
    GET_BOOL("IXOFF", IXOFF, c_iflag, );
#endif
#if defined(OPOST)
    GET_BOOL("OPOST", OPOST, c_oflag, );
#endif

    /*
     * We do not propagate the following modes:
     *  - Parity/serial settings, which are a local affair and don't
     *    make sense propagated over SSH's 8-bit byte-stream.
     *      IGNPAR PARMRK INPCK CS7 CS8 PARENB PARODD
     *  - Things that want to be enabled in one place that we don't
     *    squash locally.
     *      IUCLC
     *  - Status bits.
     *      PENDIN
     *  - Things I don't know what to do with. (FIXME)
     *      ISTRIP IMAXBEL NOFLSH TOSTOP IEXTEN
     *      INLCR IGNCR ICRNL
     */

#undef GET_CHAR
#undef GET_BOOL

    /* Fall through to here for unrecognised names, or ones that are
     * unsupported on this platform */
    return NULL;
}

void cleanup_termios(void)
{
    if (local_tty)
	tcsetattr(0, TCSANOW, &orig_termios);
}

bufchain stdout_data, stderr_data;

void try_output(int is_stderr)
{
    bufchain *chain = (is_stderr ? &stderr_data : &stdout_data);
    int fd = (is_stderr ? 2 : 1);
    void *senddata;
    int sendlen, ret;

    if (bufchain_size(chain) == 0)
        return;

    bufchain_prefix(chain, &senddata, &sendlen);
    ret = write(fd, senddata, sendlen);
    if (ret > 0)
	bufchain_consume(chain, ret);
    else if (ret < 0) {
	perror(is_stderr ? "stderr: write" : "stdout: write");
	exit(1);
    }
}

int from_backend(void *frontend_handle, int is_stderr,
		 const char *data, int len)
{
    int osize, esize;

    if (is_stderr) {
	bufchain_add(&stderr_data, data, len);
	try_output(1);
    } else {
	bufchain_add(&stdout_data, data, len);
	try_output(0);
    }

    osize = bufchain_size(&stdout_data);
    esize = bufchain_size(&stderr_data);

    return osize + esize;
}

int from_backend_untrusted(void *frontend_handle, const char *data, int len)
{
    /*
     * No "untrusted" output should get here (the way the code is
     * currently, it's all diverted by FLAG_STDERR).
     */
    assert(!"Unexpected call to from_backend_untrusted()");
    return 0; /* not reached */
}

int get_userpass_input(prompts_t *p, unsigned char *in, int inlen)
{
    int ret;
    ret = cmdline_get_passwd_input(p, in, inlen);
    if (ret == -1)
	ret = console_get_userpass_input(p, in, inlen);
    return ret;
}

/*
 * Handle data from a local tty in PARMRK format.
 */
static void from_tty(void *vbuf, unsigned len)
{
    char *p, *q, *end, *buf = vbuf;
    static enum {NORMAL, FF, FF00} state = NORMAL;

    p = buf; end = buf + len;
    while (p < end) {
	switch (state) {
	    case NORMAL:
		if (*p == '\xff') {
		    p++;
		    state = FF;
		} else {
		    q = memchr(p, '\xff', end - p);
		    if (q == NULL) q = end;
		    back->send(backhandle, p, q - p);
		    p = q;
		}
		break;
	    case FF:
		if (*p == '\xff') {
		    back->send(backhandle, p, 1);
		    p++;
		    state = NORMAL;
		} else if (*p == '\0') {
		    p++;
		    state = FF00;
		} else abort();
		break;
	    case FF00:
		if (*p == '\0') {
		    back->special(backhandle, TS_BRK);
		} else {
		    /* 
		     * Pretend that PARMRK wasn't set.  This involves
		     * faking what INPCK and IGNPAR would have done if
		     * we hadn't overridden them.  Unfortunately, we
		     * can't do this entirely correctly because INPCK
		     * distinguishes between framing and parity
		     * errors, but PARMRK format represents both in
		     * the same way.  We assume that parity errors are
		     * more common than framing errors, and hence
		     * treat all input errors as being subject to
		     * INPCK.
		     */
		    if (orig_termios.c_iflag & INPCK) {
			/* If IGNPAR is set, we throw away the character. */
			if (!(orig_termios.c_iflag & IGNPAR)) {
			    /* PE/FE get passed on as NUL. */
			    *p = 0;
			    back->send(backhandle, p, 1);
			}
		    } else {
			/* INPCK not set.  Assume we got a parity error. */
			back->send(backhandle, p, 1);
		    }
		}
		p++;
		state = NORMAL;
	}
    }
}

int signalpipe[2];

void sigwinch(int signum)
{
    write(signalpipe[1], "x", 1);
}

/*
 * In Plink our selects are synchronous, so these functions are
 * empty stubs.
 */
int uxsel_input_add(int fd, int rwx) { return 0; }
void uxsel_input_remove(int id) { }

/*
 * Short description of parameters.
 */
static void usage(void)
{
    printf("PuTTY Link: command-line connection utility\n");
    printf("%s\n", ver);
    printf("Usage: plink [options] [user@]host [command]\n");
    printf("       (\"host\" can also be a PuTTY saved session name)\n");
    printf("Options:\n");
    printf("  -V        print version information and exit\n");
    printf("  -pgpfp    print PGP key fingerprints and exit\n");
    printf("  -v        show verbose messages\n");
    printf("  -load sessname  Load settings from saved session\n");
    printf("  -ssh -telnet -rlogin -raw\n");
    printf("            force use of a particular protocol\n");
    printf("  -P port   connect to specified port\n");
    printf("  -l user   connect with specified username\n");
    printf("  -batch    disable all interactive prompts\n");
    printf("The following options only apply to SSH connections:\n");
    printf("  -pw passw login with specified password\n");
    printf("  -D [listen-IP:]listen-port\n");
    printf("            Dynamic SOCKS-based port forwarding\n");
    printf("  -L [listen-IP:]listen-port:host:port\n");
    printf("            Forward local port to remote address\n");
    printf("  -R [listen-IP:]listen-port:host:port\n");
    printf("            Forward remote port to local address\n");
    printf("  -X -x     enable / disable X11 forwarding\n");
    printf("  -A -a     enable / disable agent forwarding\n");
    printf("  -t -T     enable / disable pty allocation\n");
    printf("  -1 -2     force use of particular protocol version\n");
    printf("  -4 -6     force use of IPv4 or IPv6\n");
    printf("  -C        enable compression\n");
    printf("  -i key    private key file for authentication\n");
    printf("  -noagent  disable use of Pageant\n");
    printf("  -agent    enable use of Pageant\n");
    printf("  -m file   read remote command(s) from file\n");
    printf("  -s        remote command is an SSH subsystem (SSH-2 only)\n");
    printf("  -N        don't start a shell/command (SSH-2 only)\n");
    printf("  -nc host:port\n");
    printf("            open tunnel in place of session (SSH-2 only)\n");
    exit(1);
}

static void version(void)
{
    printf("plink: %s\n", ver);
    exit(1);
}

int main(int argc, char **argv)
{
    int sending;
    int portnumber = -1;
    int *fdlist;
    int fd;
    int i, fdcount, fdsize, fdstate;
    int connopen;
    int exitcode;
    int errors;
    int use_subsystem = 0;
    void *ldisc;
    long now;

    fdlist = NULL;
    fdcount = fdsize = 0;
    /*
     * Initialise port and protocol to sensible defaults. (These
     * will be overridden by more or less anything.)
     */
    default_protocol = PROT_SSH;
    default_port = 22;

    flags = FLAG_STDERR;
    /*
     * Process the command line.
     */
    do_defaults(NULL, &cfg);
    loaded_session = FALSE;
    default_protocol = cfg.protocol;
    default_port = cfg.port;
    errors = 0;
    {
	/*
	 * Override the default protocol if PLINK_PROTOCOL is set.
	 */
	char *p = getenv("PLINK_PROTOCOL");
	if (p) {
	    const Backend *b = backend_from_name(p);
	    if (b) {
		default_protocol = cfg.protocol = b->protocol;
		default_port = cfg.port = b->default_port;
	    }
	}
    }
    while (--argc) {
	char *p = *++argv;
	if (*p == '-') {
	    int ret = cmdline_process_param(p, (argc > 1 ? argv[1] : NULL),
					    1, &cfg);
	    if (ret == -2) {
		fprintf(stderr,
			"plink: option \"%s\" requires an argument\n", p);
		errors = 1;
	    } else if (ret == 2) {
		--argc, ++argv;
	    } else if (ret == 1) {
		continue;
	    } else if (!strcmp(p, "-batch")) {
		console_batch_mode = 1;
	    } else if (!strcmp(p, "-s")) {
                /* Save status to write to cfg later. */
		use_subsystem = 1;
	    } else if (!strcmp(p, "-V")) {
                version();
            } else if (!strcmp(p, "-pgpfp")) {
                pgp_fingerprints();
                exit(1);
	    } else if (!strcmp(p, "-o")) {
                if (argc <= 1) {
                    fprintf(stderr,
                            "plink: option \"-o\" requires an argument\n");
		    errors = 1;
		} else {
                    --argc;
		    provide_xrm_string(*++argv);
		}
	    } else {
		fprintf(stderr, "plink: unknown option \"%s\"\n", p);
		errors = 1;
	    }
	} else if (*p) {
	    if (!cfg_launchable(&cfg)) {
		char *q = p;

		/*
		 * If the hostname starts with "telnet:", set the
		 * protocol to Telnet and process the string as a
		 * Telnet URL.
		 */
		if (!strncmp(q, "telnet:", 7)) {
		    char c;

		    q += 7;
		    if (q[0] == '/' && q[1] == '/')
			q += 2;
		    cfg.protocol = PROT_TELNET;
		    p = q;
		    while (*p && *p != ':' && *p != '/')
			p++;
		    c = *p;
		    if (*p)
			*p++ = '\0';
		    if (c == ':')
			cfg.port = atoi(p);
		    else
			cfg.port = -1;
		    strncpy(cfg.host, q, sizeof(cfg.host) - 1);
		    cfg.host[sizeof(cfg.host) - 1] = '\0';
		} else {
		    char *r, *user, *host;
		    /*
		     * Before we process the [user@]host string, we
		     * first check for the presence of a protocol
		     * prefix (a protocol name followed by ",").
		     */
		    r = strchr(p, ',');
		    if (r) {
			const Backend *b;
			*r = '\0';
			b = backend_from_name(p);
			if (b) {
			    default_protocol = cfg.protocol = b->protocol;
			    portnumber = b->default_port;
			}
			p = r + 1;
		    }

		    /*
		     * A nonzero length string followed by an @ is treated
		     * as a username. (We discount an _initial_ @.) The
		     * rest of the string (or the whole string if no @)
		     * is treated as a session name and/or hostname.
		     */
		    r = strrchr(p, '@');
		    if (r == p)
			p++, r = NULL; /* discount initial @ */
		    if (r) {
			*r++ = '\0';
			user = p, host = r;
		    } else {
			user = NULL, host = p;
		    }

		    /*
		     * Now attempt to load a saved session with the
		     * same name as the hostname.
		     */
		    {
			Config cfg2;
			do_defaults(host, &cfg2);
			if (loaded_session || !cfg_launchable(&cfg2)) {
			    /* No settings for this host; use defaults */
			    /* (or session was already loaded with -load) */
			    strncpy(cfg.host, host, sizeof(cfg.host) - 1);
			    cfg.host[sizeof(cfg.host) - 1] = '\0';
			    cfg.port = default_port;
			} else {
			    cfg = cfg2;
			}
		    }

		    if (user) {
			/* Patch in specified username. */
			strncpy(cfg.username, user,
				sizeof(cfg.username) - 1);
			cfg.username[sizeof(cfg.username) - 1] = '\0';
		    }

		}
	    } else {
		char *command;
		int cmdlen, cmdsize;
		cmdlen = cmdsize = 0;
		command = NULL;

		while (argc) {
		    while (*p) {
			if (cmdlen >= cmdsize) {
			    cmdsize = cmdlen + 512;
			    command = sresize(command, cmdsize, char);
			}
			command[cmdlen++]=*p++;
		    }
		    if (cmdlen >= cmdsize) {
			cmdsize = cmdlen + 512;
			command = sresize(command, cmdsize, char);
		    }
		    command[cmdlen++]=' '; /* always add trailing space */
		    if (--argc) p = *++argv;
		}
		if (cmdlen) command[--cmdlen]='\0';
				       /* change trailing blank to NUL */
		cfg.remote_cmd_ptr = command;
		cfg.remote_cmd_ptr2 = NULL;
		cfg.nopty = TRUE;      /* command => no terminal */

		break;		       /* done with cmdline */
	    }
	}
    }

    if (errors)
	return 1;

    if (!cfg_launchable(&cfg)) {
	usage();
    }

    /*
     * Trim leading whitespace off the hostname if it's there.
     */
    {
	int space = strspn(cfg.host, " \t");
	memmove(cfg.host, cfg.host+space, 1+strlen(cfg.host)-space);
    }

    /* See if host is of the form user@host */
    if (cfg.host[0] != '\0') {
	char *atsign = strrchr(cfg.host, '@');
	/* Make sure we're not overflowing the user field */
	if (atsign) {
	    if (atsign - cfg.host < sizeof cfg.username) {
		strncpy(cfg.username, cfg.host, atsign - cfg.host);
		cfg.username[atsign - cfg.host] = '\0';
	    }
	    memmove(cfg.host, atsign + 1, 1 + strlen(atsign + 1));
	}
    }

    /*
     * Perform command-line overrides on session configuration.
     */
    cmdline_run_saved(&cfg);

    /*
     * Apply subsystem status.
     */
    if (use_subsystem)
        cfg.ssh_subsys = TRUE;

    /*
     * Trim a colon suffix off the hostname if it's there.
     */
    cfg.host[strcspn(cfg.host, ":")] = '\0';

    /*
     * Remove any remaining whitespace from the hostname.
     */
    {
	int p1 = 0, p2 = 0;
	while (cfg.host[p2] != '\0') {
	    if (cfg.host[p2] != ' ' && cfg.host[p2] != '\t') {
		cfg.host[p1] = cfg.host[p2];
		p1++;
	    }
	    p2++;
	}
	cfg.host[p1] = '\0';
    }

    if (!cfg.remote_cmd_ptr && !*cfg.remote_cmd && !*cfg.ssh_nc_host)
	flags |= FLAG_INTERACTIVE;

    /*
     * Select protocol. This is farmed out into a table in a
     * separate file to enable an ssh-free variant.
     */
    back = backend_from_proto(cfg.protocol);
    if (back == NULL) {
	fprintf(stderr,
		"Internal fault: Unsupported protocol found\n");
	return 1;
    }

    /*
     * Select port.
     */
    if (portnumber != -1)
	cfg.port = portnumber;

    /*
     * Set up the pipe we'll use to tell us about SIGWINCH.
     */
    if (pipe(signalpipe) < 0) {
	perror("pipe");
	exit(1);
    }
    putty_signal(SIGWINCH, sigwinch);

    sk_init();
    uxsel_init();

    /*
     * Start up the connection.
     */
    logctx = log_init(NULL, &cfg);
    console_provide_logctx(logctx);
    {
	const char *error;
	char *realhost;
	/* nodelay is only useful if stdin is a terminal device */
	int nodelay = cfg.tcp_nodelay && isatty(0);

	error = back->init(NULL, &backhandle, &cfg, cfg.host, cfg.port,
			   &realhost, nodelay, cfg.tcp_keepalives);
	if (error) {
	    fprintf(stderr, "Unable to open connection:\n%s\n", error);
	    return 1;
	}
	back->provide_logctx(backhandle, logctx);
	ldisc = ldisc_create(&cfg, NULL, back, backhandle, NULL);
	sfree(realhost);
    }
    connopen = 1;

    /*
     * Set up the initial console mode. We don't care if this call
     * fails, because we know we aren't necessarily running in a
     * console.
     */
    local_tty = (tcgetattr(0, &orig_termios) == 0);
    atexit(cleanup_termios);
    ldisc_update(NULL, 1, 1);
    sending = FALSE;
    now = GETTICKCOUNT();

    while (1) {
	fd_set rset, wset, xset;
	int maxfd;
	int rwx;
	int ret;

	FD_ZERO(&rset);
	FD_ZERO(&wset);
	FD_ZERO(&xset);
	maxfd = 0;

	FD_SET_MAX(signalpipe[0], maxfd, rset);

	if (connopen && !sending &&
	    back->connected(backhandle) &&
	    back->sendok(backhandle) &&
	    back->sendbuffer(backhandle) < MAX_STDIN_BACKLOG) {
	    /* If we're OK to send, then try to read from stdin. */
	    FD_SET_MAX(0, maxfd, rset);
	}

	if (bufchain_size(&stdout_data) > 0) {
	    /* If we have data for stdout, try to write to stdout. */
	    FD_SET_MAX(1, maxfd, wset);
	}

	if (bufchain_size(&stderr_data) > 0) {
	    /* If we have data for stderr, try to write to stderr. */
	    FD_SET_MAX(2, maxfd, wset);
	}

	/* Count the currently active fds. */
	i = 0;
	for (fd = first_fd(&fdstate, &rwx); fd >= 0;
	     fd = next_fd(&fdstate, &rwx)) i++;

	/* Expand the fdlist buffer if necessary. */
	if (i > fdsize) {
	    fdsize = i + 16;
	    fdlist = sresize(fdlist, fdsize, int);
	}

	/*
	 * Add all currently open fds to the select sets, and store
	 * them in fdlist as well.
	 */
	fdcount = 0;
	for (fd = first_fd(&fdstate, &rwx); fd >= 0;
	     fd = next_fd(&fdstate, &rwx)) {
	    fdlist[fdcount++] = fd;
	    if (rwx & 1)
		FD_SET_MAX(fd, maxfd, rset);
	    if (rwx & 2)
		FD_SET_MAX(fd, maxfd, wset);
	    if (rwx & 4)
		FD_SET_MAX(fd, maxfd, xset);
	}

	do {
	    long next, ticks;
	    struct timeval tv, *ptv;

	    if (run_timers(now, &next)) {
		ticks = next - GETTICKCOUNT();
		if (ticks < 0) ticks = 0;   /* just in case */
		tv.tv_sec = ticks / 1000;
		tv.tv_usec = ticks % 1000 * 1000;
		ptv = &tv;
	    } else {
		ptv = NULL;
	    }
	    ret = select(maxfd, &rset, &wset, &xset, ptv);
	    if (ret == 0)
		now = next;
	    else {
		long newnow = GETTICKCOUNT();
		/*
		 * Check to see whether the system clock has
		 * changed massively during the select.
		 */
		if (newnow - now < 0 || newnow - now > next - now) {
		    /*
		     * If so, look at the elapsed time in the
		     * select and use it to compute a new
		     * tickcount_offset.
		     */
		    long othernow = now + tv.tv_sec * 1000 + tv.tv_usec / 1000;
		    /* So we'd like GETTICKCOUNT to have returned othernow,
		     * but instead it return newnow. Hence ... */
		    tickcount_offset += othernow - newnow;
		    now = othernow;
		} else {
		    now = newnow;
		}
	    }
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
	    perror("select");
	    exit(1);
	}

	for (i = 0; i < fdcount; i++) {
	    fd = fdlist[i];
            /*
             * We must process exceptional notifications before
             * ordinary readability ones, or we may go straight
             * past the urgent marker.
             */
	    if (FD_ISSET(fd, &xset))
		select_result(fd, 4);
	    if (FD_ISSET(fd, &rset))
		select_result(fd, 1);
	    if (FD_ISSET(fd, &wset))
		select_result(fd, 2);
	}

	if (FD_ISSET(signalpipe[0], &rset)) {
	    char c[1];
	    struct winsize size;
	    read(signalpipe[0], c, 1); /* ignore its value; it'll be `x' */
	    if (ioctl(0, TIOCGWINSZ, (void *)&size) >= 0)
		back->size(backhandle, size.ws_col, size.ws_row);
	}

	if (FD_ISSET(0, &rset)) {
	    char buf[4096];
	    int ret;

	    if (connopen && back->connected(backhandle)) {
		ret = read(0, buf, sizeof(buf));
		if (ret < 0) {
		    perror("stdin: read");
		    exit(1);
		} else if (ret == 0) {
		    back->special(backhandle, TS_EOF);
		    sending = FALSE;   /* send nothing further after this */
		} else {
		    if (local_tty)
			from_tty(buf, ret);
		    else
			back->send(backhandle, buf, ret);
		}
	    }
	}

	if (FD_ISSET(1, &wset)) {
	    try_output(0);
	}

	if (FD_ISSET(2, &wset)) {
	    try_output(1);
	}

	if ((!connopen || !back->connected(backhandle)) &&
	    bufchain_size(&stdout_data) == 0 &&
	    bufchain_size(&stderr_data) == 0)
	    break;		       /* we closed the connection */
    }
    exitcode = back->exitcode(backhandle);
    if (exitcode < 0) {
	fprintf(stderr, "Remote process exit code unavailable\n");
	exitcode = 1;		       /* this is an error condition */
    }
    cleanup_exit(exitcode);
    return exitcode;		       /* shouldn't happen, but placates gcc */
}
