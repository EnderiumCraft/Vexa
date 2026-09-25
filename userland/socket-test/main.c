/* socket-test: checks sockets without leaving the machine: local socket
 * pairs, TCP and UDP over the loopback interface, refused connections and
 * non-blocking mode. */
#include <stdio.h>
#include <string.h>
#include <vexa/net.h>
#include <vexa/thread.h>

#define TCP_BYTES (300 * 1024)

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("socket-test: FAILED: %s\n", what);
        failures++;
    }
}

static unsigned char pattern(long i) {
    return (unsigned char)(i * 7 + i / 251);
}

static void test_pair(void) {
    int pair[2];
    check(vx_socket_pair(VX_AF_UNIX, VX_SOCK_STREAM, pair) == 0, "socket pair");
    check(vx_write(pair[0], "ping", 4) == 4, "write to a pair");
    char buffer[8] = {0};
    check(vx_read(pair[1], buffer, sizeof(buffer)) == 4 && memcmp(buffer, "ping", 4) == 0,
          "read from a pair");
    vx_close(pair[0]);
    check(vx_read(pair[1], buffer, sizeof(buffer)) == 0, "end of data once the other end closes");
    vx_close(pair[1]);
}

struct server {
    int listener;
    long received;
    int ok;
};

static void *serve(void *arg) {
    struct server *s = arg;
    struct vx_socket_address peer;
    int connection = vx_accept(s->listener, &peer, 0);
    if (connection < 0) {
        return NULL;
    }
    static unsigned char buffer[8192];
    s->ok = 1;
    for (;;) {
        long n = vx_read(connection, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            if (buffer[i] != pattern(s->received + i)) {
                s->ok = 0;
            }
        }
        s->received += n;
    }
    vx_write(connection, "thanks", 6);
    vx_close(connection);
    return NULL;
}

static void test_tcp(void) {
    struct server s = {0};
    s.listener = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    struct vx_socket_address local = vx_inet_address(0x0100007f, 0); /* 127.0.0.1, any port */
    check(vx_bind(s.listener, &local, sizeof(local.inet)) == 0, "bind");
    check(vx_listen(s.listener, 4) == 0, "listen");
    struct vx_socket_address bound;
    vx_socket_address(s.listener, 0, &bound);
    check(bound.inet.port != 0, "a port was chosen");
    struct vx_thread *thread = vx_thread_create(serve, &s);

    int client = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    long error = vx_connect(client, &bound, sizeof(bound.inet));
    check(error == 0, "connect over loopback");
    static unsigned char data[TCP_BYTES];
    for (long i = 0; i < TCP_BYTES; i++) {
        data[i] = pattern(i);
    }
    long sent = 0;
    while (sent < TCP_BYTES) {
        long n = vx_write(client, data + sent, TCP_BYTES - sent);
        if (n <= 0) {
            break;
        }
        sent += n;
    }
    check(sent == TCP_BYTES, "send everything");
    vx_shutdown(client, VX_SHUT_WRITE);
    char reply[16] = {0};
    long n = vx_read(client, reply, sizeof(reply));
    check(n == 6 && memcmp(reply, "thanks", 6) == 0, "the server's reply");
    vx_close(client);
    vx_thread_join(thread);
    check(s.received == TCP_BYTES && s.ok, "the server got every byte, in order");
    vx_close(s.listener);

    /* Nobody listens there any more. */
    client = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    check(vx_connect(client, &bound, sizeof(bound.inet)) == -VX_ECONNREFUSED,
          "connection refused");
    vx_close(client);
}

static void test_udp(void) {
    int a = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    int b = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    struct vx_socket_address any = vx_inet_address(0, 0);
    check(vx_bind(b, &any, sizeof(any.inet)) == 0, "bind UDP");
    struct vx_socket_address where;
    vx_socket_address(b, 0, &where);
    where.inet.address = 0x0100007f;
    struct vx_message out = {"datagram", 8, 0, sizeof(where.inet), &where};
    check(vx_send(a, &out) == 8, "send a datagram");
    char buffer[32];
    struct vx_socket_address from;
    struct vx_message in = {buffer, sizeof(buffer), 0, 0, &from};
    check(vx_receive(b, &in) == 8 && memcmp(buffer, "datagram", 8) == 0, "receive it");
    check(from.inet.address == 0x0100007f && from.inet.port != 0, "the sender's address");
    vx_close(a);
    vx_close(b);
}

static void test_nonblocking(void) {
    int listener = vx_socket(VX_AF_INET, VX_SOCK_STREAM | VX_SOCK_NONBLOCK, 0);
    struct vx_socket_address local = vx_inet_address(0x0100007f, 0);
    vx_bind(listener, &local, sizeof(local.inet));
    vx_listen(listener, 1);
    check(vx_accept(listener, NULL, 0) == -VX_EAGAIN, "non-blocking accept");
    struct vx_poll poll = {listener, VX_POLL_READ, 0};
    check(vx_poll(&poll, 1, 50) == 0, "poll times out");
    vx_close(listener);
}

int main(void) {
    test_pair();
    test_tcp();
    test_udp();
    test_nonblocking();
    if (failures) {
        printf("socket-test: %d checks failed\n", failures);
        return 1;
    }
    printf("socket-test: passed\n");
    return 0;
}
