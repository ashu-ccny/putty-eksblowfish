#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "putty.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#define RLOGIN_MAX_BACKLOG 4096

typedef struct rlogin_tag {
    const struct plug_function_table *fn;
    /* the above field _must_ be first in the structure */

    Socket s;
    int bufsize;
    int term_width, term_height;
    void *frontend;
} *Rlogin;

static void rlogin_size(void *handle, int width, int height);

static void c_write(Rlogin rlogin, char *buf, int len)
{
    int backlog = from_backend(rlogin->frontend, 0, buf, len);
    sk_set_frozen(rlogin->s, backlog > RLOGIN_MAX_BACKLOG);
}

static int rlogin_closing(Plug plug, char *error_msg, int error_code,
			  int calling_back)
{
    Rlogin rlogin = (Rlogin) plug;
    if (rlogin->s) {
        sk_close(rlogin->s);
        rlogin->s = NULL;
    }
    if (error_msg) {
	/* A socket error has occurred. */
	logevent(error_msg);
	connection_fatal("%s", error_msg);
    }				       /* Otherwise, the remote side closed the connection normally. */
    return 0;
}

static int rlogin_receive(Plug plug, int urgent, char *data, int len)
{
    Rlogin rlogin = (Rlogin) plug;
    if (urgent == 2) {
	char c;

	c = *data++;
	len--;
	if (c == '\x80')
	    rlogin_size(rlogin, rlogin->term_width, rlogin->term_height);
	/*
	 * We should flush everything (aka Telnet SYNCH) if we see
	 * 0x02, and we should turn off and on _local_ flow control
	 * on 0x10 and 0x20 respectively. I'm not convinced it's
	 * worth it...
	 */
    } else {
	/*
	 * Main rlogin protocol. This is really simple: the first
	 * byte is expected to be NULL and is ignored, and the rest
	 * is printed.
	 */
	static int firstbyte = 1;
	if (firstbyte) {
	    if (data[0] == '\0') {
		data++;
		len--;
	    }
	    firstbyte = 0;
	}
	if (len > 0)
            c_write(rlogin, data, len);
    }
    return 1;
}

static void rlogin_sent(Plug plug, int bufsize)
{
    Rlogin rlogin = (Rlogin) plug;
    rlogin->bufsize = bufsize;
}

/*
 * Called to set up the rlogin connection.
 * 
 * Returns an error message, or NULL on success.
 *
 * Also places the canonical host name into `realhost'. It must be
 * freed by the caller.
 */
static char *rlogin_init(void *frontend_handle, void **backend_handle,
			 char *host, int port, char **realhost, int nodelay)
{
    static const struct plug_function_table fn_table = {
	rlogin_closing,
	rlogin_receive,
	rlogin_sent
    };
    SockAddr addr;
    char *err;
    Rlogin rlogin;

    rlogin = smalloc(sizeof(*rlogin));
    rlogin->fn = &fn_table;
    rlogin->s = NULL;
    rlogin->frontend = frontend_handle;
    rlogin->term_width = cfg.width;
    rlogin->term_height = cfg.height;
    *backend_handle = rlogin;

    /*
     * Try to find host.
     */
    {
	char buf[200];
	sprintf(buf, "Looking up host \"%.170s\"", host);
	logevent(buf);
    }
    addr = sk_namelookup(host, realhost);
    if ((err = sk_addr_error(addr)))
	return err;

    if (port < 0)
	port = 513;		       /* default rlogin port */

    /*
     * Open socket.
     */
    {
	char buf[200], addrbuf[100];
	sk_getaddr(addr, addrbuf, 100);
	sprintf(buf, "Connecting to %.100s port %d", addrbuf, port);
	logevent(buf);
    }
    rlogin->s = new_connection(addr, *realhost, port, 1, 0,
			       nodelay, (Plug) rlogin);
    if ((err = sk_socket_error(rlogin->s)))
	return err;

    sk_addr_free(addr);

    /*
     * Send local username, remote username, terminal/speed
     */

    {
	char z = 0;
	char *p;
	sk_write(rlogin->s, &z, 1);
	sk_write(rlogin->s, cfg.localusername, strlen(cfg.localusername));
	sk_write(rlogin->s, &z, 1);
	sk_write(rlogin->s, cfg.username, strlen(cfg.username));
	sk_write(rlogin->s, &z, 1);
	sk_write(rlogin->s, cfg.termtype, strlen(cfg.termtype));
	sk_write(rlogin->s, "/", 1);
	for (p = cfg.termspeed; isdigit(*p); p++);
	sk_write(rlogin->s, cfg.termspeed, p - cfg.termspeed);
	rlogin->bufsize = sk_write(rlogin->s, &z, 1);
    }

    return NULL;
}

/*
 * Called to send data down the rlogin connection.
 */
static int rlogin_send(void *handle, char *buf, int len)
{
    Rlogin rlogin = (Rlogin) handle;

    if (rlogin->s == NULL)
	return 0;

    rlogin->bufsize = sk_write(rlogin->s, buf, len);

    return rlogin->bufsize;
}

/*
 * Called to query the current socket sendability status.
 */
static int rlogin_sendbuffer(void *handle)
{
    Rlogin rlogin = (Rlogin) handle;
    return rlogin->bufsize;
}

/*
 * Called to set the size of the window
 */
static void rlogin_size(void *handle, int width, int height)
{
    Rlogin rlogin = (Rlogin) handle;
    char b[12] = { '\xFF', '\xFF', 0x73, 0x73, 0, 0, 0, 0, 0, 0, 0, 0 };

    rlogin->term_width = width;
    rlogin->term_height = height;

    if (rlogin->s == NULL)
	return;

    b[6] = rlogin->term_width >> 8;
    b[7] = rlogin->term_width & 0xFF;
    b[4] = rlogin->term_height >> 8;
    b[5] = rlogin->term_height & 0xFF;
    rlogin->bufsize = sk_write(rlogin->s, b, 12);
    return;
}

/*
 * Send rlogin special codes.
 */
static void rlogin_special(void *handle, Telnet_Special code)
{
    /* Do nothing! */
    return;
}

static Socket rlogin_socket(void *handle)
{
    Rlogin rlogin = (Rlogin) handle;
    return rlogin->s;
}

static int rlogin_sendok(void *handle)
{
    Rlogin rlogin = (Rlogin) handle;
    return 1;
}

static void rlogin_unthrottle(void *handle, int backlog)
{
    Rlogin rlogin = (Rlogin) handle;
    sk_set_frozen(rlogin->s, backlog > RLOGIN_MAX_BACKLOG);
}

static int rlogin_ldisc(void *handle, int option)
{
    Rlogin rlogin = (Rlogin) handle;
    return 0;
}

static void rlogin_provide_ldisc(void *handle, void *ldisc)
{
    /* This is a stub. */
}

static int rlogin_exitcode(void *handle)
{
    Rlogin rlogin = (Rlogin) handle;
    /* If we ever implement RSH, we'll probably need to do this properly */
    return 0;
}

Backend rlogin_backend = {
    rlogin_init,
    rlogin_send,
    rlogin_sendbuffer,
    rlogin_size,
    rlogin_special,
    rlogin_socket,
    rlogin_exitcode,
    rlogin_sendok,
    rlogin_ldisc,
    rlogin_provide_ldisc,
    rlogin_unthrottle,
    1
};
