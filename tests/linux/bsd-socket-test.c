/* bsd-socket-test: BSD sockets on the Linux subsystem. Local sockets (pairs,
 * named ones, passing a descriptor with SCM_RIGHTS), TCP and UDP over
 * loopback with poll and non-blocking connect, name lookup, and the
 * interface ioctls. Prints "bsd-socket-test: passed" if everything works. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, what)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            printf("bsd-socket-test: FAILED: %s (errno %d: %s)\n", what, errno,      \
                   strerror(errno));                                             \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void test_pair_and_rights(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    /* Send one end of a pipe along with a byte of data. */
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0, "pipe");
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {0};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = &control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &pipe_fds[1], sizeof(int));
    CHECK(sendmsg(pair[0], &message, 0) == 1, "sendmsg with SCM_RIGHTS");
    close(pipe_fds[1]);

    char got = 0;
    iov.iov_base = &got;
    memset(&control, 0, sizeof(control));
    message.msg_controllen = sizeof(control);
    CHECK(recvmsg(pair[1], &message, 0) == 1 && got == 'x', "recvmsg");
    c = CMSG_FIRSTHDR(&message);
    int passed = -1;
    if (c && c->cmsg_type == SCM_RIGHTS) {
        memcpy(&passed, CMSG_DATA(c), sizeof(int));
    }
    CHECK(passed >= 0, "a descriptor arrived");
    if (passed >= 0) {
        CHECK(write(passed, "through", 7) == 7, "write to the passed pipe");
        close(passed);
        char buffer[16] = {0};
        CHECK(read(pipe_fds[0], buffer, sizeof(buffer)) == 7 && !strcmp(buffer, "through"),
              "read what was written through it");
    }
    close(pipe_fds[0]);
    close(pair[0]);
    CHECK(read(pair[1], &got, 1) == 0, "end of data after close");
    close(pair[1]);
}

static void test_named(void) {
    const char *path = "/tmp/socket-test.sock";
    unlink(path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    CHECK(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0, "bind a path");
    CHECK(listen(server, 2) == 0, "listen on a path");
    pid_t child = fork();
    if (child == 0) {
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0) {
            _exit(1);
        }
        write(client, "from the child", 14);
        _exit(0);
    }
    int connection = accept(server, NULL, NULL);
    CHECK(connection >= 0, "accept a local connection");
    char buffer[32] = {0};
    CHECK(read(connection, buffer, sizeof(buffer)) == 14 && !strcmp(buffer, "from the child"),
          "read from the child");
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child connected");
    close(connection);
    close(server);
    unlink(path);
}

static void test_tcp(void) {
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    CHECK(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "SO_REUSEADDR");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0, "bind TCP");
    CHECK(listen(server, 4) == 0, "listen TCP");
    socklen_t length = sizeof(address);
    CHECK(getsockname(server, (struct sockaddr *)&address, &length) == 0 && address.sin_port,
          "getsockname");

    /* A non-blocking connect, finished with poll and SO_ERROR. */
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int result = connect(client, (struct sockaddr *)&address, sizeof(address));
    CHECK(result == 0 || errno == EINPROGRESS, "non-blocking connect");
    struct pollfd p = {client, POLLOUT, 0};
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLOUT), "poll for the connection");
    int error = -1;
    length = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0,
          "SO_ERROR says connected");
    CHECK(setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0, "TCP_NODELAY");

    struct sockaddr_in peer;
    length = sizeof(peer);
    int connection = accept4(server, (struct sockaddr *)&peer, &length, SOCK_CLOEXEC);
    CHECK(connection >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "accept4");

    /* Fill the pipe until a non-blocking send says EAGAIN, then drain it. */
    static char block[65536];
    long sent = 0;
    for (;;) {
        ssize_t n = send(client, block, sizeof(block), MSG_NOSIGNAL);
        if (n < 0) {
            CHECK(errno == EAGAIN, "EAGAIN when the buffers are full");
            break;
        }
        sent += n;
    }
    fcntl(client, F_SETFL, fcntl(client, F_GETFL) & ~O_NONBLOCK);
    shutdown(client, SHUT_WR);
    long received = 0;
    for (;;) {
        ssize_t n = recv(connection, block, sizeof(block), 0);
        if (n <= 0) {
            break;
        }
        received += n;
    }
    CHECK(sent > 0 && received == sent, "everything sent was received");
    CHECK(send(connection, "bye", 3, 0) == 3, "reply");
    char reply[4] = {0};
    CHECK(recv(client, reply, 3, MSG_WAITALL) == 3 && !strcmp(reply, "bye"), "read the reply");
    close(connection);
    close(client);
    close(server);

    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == -1 &&
              errno == ECONNREFUSED,
          "ECONNREFUSED once nobody listens");
    close(client);
}

static void test_udp(void) {
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(b, (struct sockaddr *)&address, sizeof(address)) == 0, "bind UDP");
    socklen_t length = sizeof(address);
    getsockname(b, (struct sockaddr *)&address, &length);
    CHECK(sendto(a, "one", 3, 0, (struct sockaddr *)&address, sizeof(address)) == 3, "sendto");
    CHECK(connect(a, (struct sockaddr *)&address, sizeof(address)) == 0, "connect UDP");
    CHECK(send(a, "two!", 4, 0) == 4, "send on a connected UDP socket");
    char buffer[8];
    struct sockaddr_in from;
    length = sizeof(from);
    CHECK(recvfrom(b, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &length) == 3 &&
              from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "recvfrom");
    /* The second datagram may still be on its way (loopback goes through the
     * network thread): wait for it, then ask its size. */
    struct pollfd p = {b, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1, "poll for the second datagram");
    int pending = 0;
    CHECK(ioctl(b, FIONREAD, &pending) == 0 && pending == 4, "FIONREAD");
    CHECK(recv(b, buffer, 2, 0) == 2, "a datagram cut short");
    close(a);
    close(b);
}

static void test_lookup_and_interfaces(void) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *list;
    CHECK(getaddrinfo("localhost", "80", &hints, &list) == 0, "getaddrinfo localhost");
    if (failures == 0) {
        struct sockaddr_in *a = (struct sockaddr_in *)list->ai_addr;
        CHECK(a->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(a->sin_port) == 80,
              "localhost is 127.0.0.1");
        freeaddrinfo(list);
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    strcpy(request.ifr_name, "lo");
    CHECK(ioctl(s, SIOCGIFFLAGS, &request) == 0 && (request.ifr_flags & IFF_LOOPBACK),
          "SIOCGIFFLAGS lo");
    CHECK(if_nametoindex("lo") == 1, "if_nametoindex");
    close(s);
}

int main(void) {
    test_pair_and_rights();
    test_named();
    test_tcp();
    test_udp();
    test_lookup_and_interfaces();
    if (failures) {
        printf("bsd-socket-test: %d checks failed\n", failures);
        return 1;
    }
    printf("bsd-socket-test: passed\n");
    return 0;
}
