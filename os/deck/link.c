// TCP link to the Mac. The deck listens on 127.0.0.1:<port>; the Mac reaches it through
// `adb forward`. One client at a time; a new connection replaces the old one.
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "deck.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS preview build
#endif
#define MAX_LINE 512
#define LINK_TIMEOUT 5.0

static int client = -1;
static char rx[MAX_LINE * 2];
static size_t rxlen;
static double last_rx;

int link_fd(void) { return client; }

void link_send(const char *fmt, ...) {
    if (client < 0) return;
    char out[MAX_LINE];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(out, sizeof out - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof out - 2) n = sizeof out - 2;
    out[n++] = '\n';
    if (send(client, out, n, MSG_NOSIGNAL) != n) logmsg("send failed: %s", strerror(errno));
}

static void drop(const char *why) {
    if (client < 0) return;
    logmsg("client dropped: %s", why);
    close(client); client = -1; rxlen = 0;
    connected = 0; dirty = 1;
    toast("Mac disconnected", 4);
}

int link_listen(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    while (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { logmsg("bind: %s", strerror(errno)); sleep(1); }
    listen(srv, 2);
    logmsg("listening on 127.0.0.1:%d", port);
    return srv;
}

void link_accept(int srv) {
    int c = accept(srv, NULL, NULL), one = 1;
    if (c < 0) return;
    if (client >= 0) drop("replaced by new connection");
    client = c;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    connected = 1; last_rx = now(); rxlen = 0; dirty = 1;
    prompt_clear();  // a new host session starts with no prompts
    logmsg("client connected");
    toast("Mac connected", 4);
    link_send("HELLO|macdeck/1|%s|%dx%d", BUILD, W, H);
    link_send("SCREEN|%s", screen_name(screen));
}

void link_read(void) {
    char tmp[1024];
    ssize_t r = recv(client, tmp, sizeof tmp, 0);
    if (r <= 0) { drop(r == 0 ? "closed by peer" : strerror(errno)); return; }
    last_rx = now();
    for (ssize_t i = 0; i < r; i++) {
        if (tmp[i] == '\n') {
            rx[rxlen] = 0;
            if (rxlen && rx[rxlen - 1] == '\r') rx[rxlen - 1] = 0;
            if (rxlen <= MAX_LINE) model_handle(rx);
            rxlen = 0;
        } else if (rxlen < sizeof rx - 1) rx[rxlen++] = tmp[i];
        else rxlen = sizeof rx;  // overlong line: discard until the next newline
    }
}

void link_check(double t) { if (client >= 0 && t - last_rx > LINK_TIMEOUT) drop("heartbeat timeout"); }
