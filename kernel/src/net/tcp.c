#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/mm.h>
#include <vexa/random.h>
#include <vexa/string.h>

#include "inet.h"

/*
 * TCP (RFC 9293), kept simple:
 *
 * - Each connection has a 64 KiB send buffer (data not yet acknowledged) and a
 *   64 KiB receive buffer; the window we advertise is the free part of it.
 * - Segments that arrive out of order are dropped and the sender is reminded
 *   what we expect (a duplicate ACK); it sends them again.
 * - Retransmission is go-back-N from the oldest unacknowledged byte, with a
 *   timeout that doubles on each try. No congestion control, no window
 *   scaling, no SACK, no Nagle (every write goes out at once).
 * - A connection closed by its program lingers (orphaned) until its FIN is
 *   acknowledged, then TIME-WAIT keeps its port for a couple of seconds.
 */

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

#define TCP_HEADER 20
#define BUFFER_SIZE (64 * 1024)
#define DEFAULT_MSS 536
#define OUR_MSS (NET_MTU - IP_HEADER - TCP_HEADER)
#define RTO_INITIAL_MS 1000
#define RTO_MAX_MS 16000
#define MAX_RETRIES 8
#define SYN_RETRIES 5
#define TIME_WAIT_MS 2000
#define FIN_WAIT_2_MS 60000 /* An orphan whose peer never closes. */

struct __attribute__((packed)) tcp_header {
    uint16_t source_port, destination_port;
    uint32_t seq, ack;
    uint8_t offset; /* Header length in 32-bit words, in the high 4 bits. */
    uint8_t flags;
    uint16_t window, checksum, urgent;
};

struct tcp_conn {
    struct tcp_conn *next; /* In `connections`. */
    struct socket *socket; /* NULL when orphaned, or not accepted yet. */
    enum tcp_state state;
    ipv4_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    bool bound, listed;

    /* Sending. The send buffer holds the bytes from sequence number
     * `buffer_seq` on: sent but unacknowledged, then not yet sent. */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, buffer_seq;
    uint16_t mss;
    uint8_t *send_buffer;
    size_t send_start, send_used;
    bool fin_queued, fin_sent;
    uint32_t fin_seq;

    /* Receiving. */
    uint32_t rcv_nxt;
    uint8_t *receive_buffer;
    size_t receive_start, receive_used;
    bool fin_received;
    uint32_t advertised; /* The window in our last segment. */
    bool ack_now;

    /* Timers (timer_ms() times; 0 = off). */
    uint64_t retransmit_at, close_at;
    uint32_t rto;
    int retries;

    int error;        /* For the program: VX_ECONNRESET, VX_ECONNREFUSED... */
    bool shut_read;

    /* Listening sockets: connections waiting for accept(). A connection
     * still in the handshake points to its listener. */
    struct tcp_conn *listener;
    struct tcp_conn *accept_next;
    struct tcp_conn *accept_head, *accept_tail;
    int backlog, pending; /* pending: in the handshake or waiting for accept. */

    struct wait_queue wait;
};

static struct tcp_conn *connections;

static struct tcp_conn *conn_of(struct socket *socket) {
    return socket->data;
}

/* Sequence number comparisons, modulo 2^32. */
static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

static void wake(struct tcp_conn *conn) {
    wait_queue_wake_all(&conn->wait);
}

bool tcp_port_in_use(ipv4_t ip, uint16_t port, bool reuse) {
    for (struct tcp_conn *c = connections; c; c = c->next) {
        if (!c->bound || c->local_port != port ||
            (ip && c->local_ip && c->local_ip != ip)) {
            continue;
        }
        /* SO_REUSEADDR: connections on their way out don't count. */
        if (reuse && c->state != TCP_LISTEN &&
            (c->state == TCP_TIME_WAIT || !c->socket)) {
            continue;
        }
        return true;
    }
    return false;
}

static struct tcp_conn *conn_new(void) {
    struct tcp_conn *conn = kzalloc(sizeof(*conn));
    if (!conn) {
        return NULL;
    }
    conn->send_buffer = kmalloc(BUFFER_SIZE);
    conn->receive_buffer = kmalloc(BUFFER_SIZE);
    if (!conn->send_buffer || !conn->receive_buffer) {
        kfree(conn->send_buffer);
        kfree(conn->receive_buffer);
        kfree(conn);
        return NULL;
    }
    conn->mss = DEFAULT_MSS;
    conn->rto = RTO_INITIAL_MS;
    return conn;
}

static void list_add(struct tcp_conn *conn) {
    if (!conn->listed) {
        conn->next = connections;
        connections = conn;
        conn->listed = true;
    }
}

static void list_remove(struct tcp_conn *conn) {
    if (!conn->listed) {
        return;
    }
    struct tcp_conn **p = &connections;
    while (*p && *p != conn) {
        p = &(*p)->next;
    }
    if (*p) {
        *p = conn->next;
    }
    conn->listed = false;
}

static void conn_free(struct tcp_conn *conn) {
    list_remove(conn);
    kfree(conn->send_buffer);
    kfree(conn->receive_buffer);
    kfree(conn);
}

static uint32_t receive_window(struct tcp_conn *conn) {
    size_t free = BUFFER_SIZE - conn->receive_used;
    return free > 65535 ? 65535 : (uint32_t)free;
}

/* Builds and sends one segment. `data_offset` is where its data starts in
 * the send buffer (from send_start). */
static void send_segment(struct tcp_conn *conn, uint32_t seq, uint8_t flags, size_t data_offset,
                         size_t length) {
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    struct tcp_header *tcp = (struct tcp_header *)(frame + TRANSPORT_OFFSET);
    size_t header = TCP_HEADER;
    if (flags & FLAG_SYN) {
        /* Option: maximum segment size. */
        uint8_t *option = (uint8_t *)(tcp + 1);
        option[0] = 2;
        option[1] = 4;
        option[2] = OUR_MSS >> 8;
        option[3] = OUR_MSS & 0xff;
        header += 4;
    }
    uint8_t *data = (uint8_t *)tcp + header;
    for (size_t i = 0; i < length; i++) {
        data[i] = conn->send_buffer[(conn->send_start + data_offset + i) % BUFFER_SIZE];
    }
    tcp->source_port = conn->local_port;
    tcp->destination_port = conn->remote_port;
    tcp->seq = net32(seq);
    tcp->ack = (flags & FLAG_ACK) ? net32(conn->rcv_nxt) : 0;
    tcp->offset = (uint8_t)(header / 4) << 4;
    tcp->flags = flags;
    conn->advertised = receive_window(conn);
    tcp->window = net16((uint16_t)conn->advertised);
    tcp->checksum = 0;
    tcp->urgent = 0;
    tcp->checksum = ip_transport_checksum(conn->local_ip, conn->remote_ip, IP_PROTOCOL_TCP, tcp,
                                          header + length);
    ip_send(frame, conn->local_ip, conn->remote_ip, IP_PROTOCOL_TCP, header + length);
    if (flags & FLAG_ACK) {
        conn->ack_now = false;
    }
}

/* A reset in answer to a segment nobody wants. */
static void send_reset(const struct ip_packet *packet, const struct tcp_header *in,
                       size_t segment_length) {
    if (in->flags & FLAG_RST) {
        return;
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    struct tcp_header *tcp = (struct tcp_header *)(frame + TRANSPORT_OFFSET);
    memset(tcp, 0, TCP_HEADER);
    tcp->source_port = in->destination_port;
    tcp->destination_port = in->source_port;
    if (in->flags & FLAG_ACK) {
        tcp->seq = in->ack;
        tcp->flags = FLAG_RST;
    } else {
        tcp->ack = net32(net32(in->seq) + (uint32_t)segment_length);
        tcp->flags = FLAG_RST | FLAG_ACK;
    }
    tcp->offset = (TCP_HEADER / 4) << 4;
    tcp->checksum = ip_transport_checksum(packet->destination, packet->source, IP_PROTOCOL_TCP,
                                          tcp, TCP_HEADER);
    ip_send(frame, packet->destination, packet->source, IP_PROTOCOL_TCP, TCP_HEADER);
}

static void start_timer(struct tcp_conn *conn) {
    if (!conn->retransmit_at) {
        conn->retransmit_at = timer_ms() + conn->rto;
    }
}

/* Ends the connection. Frees it if its program is done with it. */
static void conn_close(struct tcp_conn *conn, int error) {
    if (error && !conn->error) {
        conn->error = error;
    }
    conn->state = TCP_CLOSED;
    conn->retransmit_at = 0;
    conn->close_at = 0;
    list_remove(conn);
    conn->bound = false;
    struct tcp_conn *listener = conn->listener;
    if (listener) {
        /* Never accepted: the listener forgets it. */
        struct tcp_conn **p = &listener->accept_head, *previous = NULL;
        while (*p && *p != conn) {
            previous = *p;
            p = &(*p)->accept_next;
        }
        if (*p) {
            *p = conn->accept_next;
            if (listener->accept_tail == conn) {
                listener->accept_tail = previous;
            }
        }
        listener->pending--;
        conn->listener = NULL;
    }
    wake(conn);
    if (!conn->socket) {
        conn_free(conn);
    }
}

static void enter_time_wait(struct tcp_conn *conn) {
    conn->state = TCP_TIME_WAIT;
    conn->retransmit_at = 0;
    conn->close_at = timer_ms() + TIME_WAIT_MS;
    wake(conn);
}

static bool can_send_data(struct tcp_conn *conn) {
    return conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT;
}

/* Sends what the window allows, then a FIN if one is due, then an ACK if
 * one is owed and nothing else carried it. `probe`: send at least one byte
 * even into a closed window. */
static void output(struct tcp_conn *conn, bool probe) {
    if (can_send_data(conn) || conn->state == TCP_FIN_WAIT_1 || conn->state == TCP_LAST_ACK ||
        conn->state == TCP_CLOSING) {
        for (;;) {
            size_t sent = conn->snd_nxt - conn->buffer_seq;
            if (conn->fin_sent && sent > 0) {
                sent--; /* The FIN takes a sequence number but no buffer space. */
            }
            size_t unsent = conn->send_used > sent ? conn->send_used - sent : 0;
            uint32_t window = conn->snd_wnd ? conn->snd_wnd : (probe ? 1 : 0);
            uint32_t in_flight = conn->snd_nxt - conn->snd_una;
            size_t room = window > in_flight ? window - in_flight : 0;
            size_t n = unsent < room ? unsent : room;
            if (n > conn->mss) {
                n = conn->mss;
            }
            if (n == 0 || conn->fin_sent) {
                if (unsent && !room) {
                    start_timer(conn); /* Zero window: probe later. */
                }
                break;
            }
            send_segment(conn, conn->snd_nxt, FLAG_ACK | FLAG_PSH, sent, n);
            conn->snd_nxt += (uint32_t)n;
            probe = false;
            start_timer(conn);
        }
        size_t sent = conn->snd_nxt - conn->buffer_seq - (conn->fin_sent ? 1 : 0);
        if (conn->fin_queued && !conn->fin_sent && sent == conn->send_used) {
            conn->fin_seq = conn->snd_nxt;
            send_segment(conn, conn->snd_nxt, FLAG_FIN | FLAG_ACK, 0, 0);
            conn->snd_nxt++;
            conn->fin_sent = true;
            if (conn->state == TCP_ESTABLISHED) {
                conn->state = TCP_FIN_WAIT_1;
            } else if (conn->state == TCP_CLOSE_WAIT) {
                conn->state = TCP_LAST_ACK;
            }
            start_timer(conn);
        }
    }
    if (conn->ack_now && conn->state != TCP_CLOSED && conn->state != TCP_LISTEN &&
        conn->state != TCP_SYN_SENT) {
        send_segment(conn, conn->snd_nxt, FLAG_ACK, 0, 0);
    }
}

static void parse_options(struct tcp_conn *conn, const struct tcp_header *tcp,
                          size_t header_length) {
    const uint8_t *o = (const uint8_t *)(tcp + 1), *end = (const uint8_t *)tcp + header_length;
    while (o < end && *o != 0) {
        if (*o == 1) { /* No-op. */
            o++;
            continue;
        }
        if (o + 2 > end || o[1] < 2 || o + o[1] > end) {
            break;
        }
        if (o[0] == 2 && o[1] == 4) {
            uint16_t mss = (uint16_t)(o[2] << 8 | o[3]);
            conn->mss = mss < OUR_MSS ? (mss ? mss : DEFAULT_MSS) : OUR_MSS;
        }
        o += o[1];
    }
}

/* Takes acknowledged bytes out of the send buffer. */
static void acknowledge(struct tcp_conn *conn, uint32_t ack) {
    if (!seq_lt(conn->snd_una, ack) || seq_lt(conn->snd_nxt, ack)) {
        return;
    }
    uint32_t data_acked = ack - conn->buffer_seq;
    if (conn->fin_sent && seq_lt(conn->fin_seq, ack)) {
        data_acked--; /* Our FIN. */
    }
    if (data_acked > conn->send_used) {
        data_acked = (uint32_t)conn->send_used;
    }
    conn->send_start = (conn->send_start + data_acked) % BUFFER_SIZE;
    conn->send_used -= data_acked;
    conn->buffer_seq += data_acked;
    conn->snd_una = ack;
    conn->retries = 0;
    conn->rto = RTO_INITIAL_MS;
    conn->retransmit_at = conn->snd_una == conn->snd_nxt ? 0 : timer_ms() + conn->rto;
    wake(conn); /* Room to write. */
}

static bool fin_acked(struct tcp_conn *conn) {
    return conn->fin_sent && seq_lt(conn->fin_seq, conn->snd_una);
}

/* A new connection for a listener, from a SYN. */
static void accept_syn(struct tcp_conn *listener, const struct ip_packet *packet,
                       const struct tcp_header *tcp, size_t header_length) {
    if (listener->pending >= listener->backlog) {
        return; /* Full: the peer will try again. */
    }
    struct tcp_conn *conn = conn_new();
    if (!conn) {
        return;
    }
    conn->state = TCP_SYN_RECEIVED;
    conn->local_ip = packet->destination;
    conn->local_port = tcp->destination_port;
    conn->remote_ip = packet->source;
    conn->remote_port = tcp->source_port;
    conn->bound = true;
    conn->rcv_nxt = net32(tcp->seq) + 1;
    random_bytes(&conn->iss, sizeof(conn->iss));
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1;
    conn->buffer_seq = conn->iss + 1;
    conn->snd_wnd = net16(tcp->window);
    parse_options(conn, tcp, header_length);
    conn->listener = listener;
    listener->pending++;
    list_add(conn);
    send_segment(conn, conn->iss, FLAG_SYN | FLAG_ACK, 0, 0);
    start_timer(conn);
}

static struct tcp_conn *find(const struct ip_packet *packet, const struct tcp_header *tcp) {
    struct tcp_conn *listener = NULL;
    for (struct tcp_conn *c = connections; c; c = c->next) {
        if (c->local_port != tcp->destination_port) {
            continue;
        }
        if (c->state == TCP_LISTEN) {
            if (!c->local_ip || c->local_ip == packet->destination) {
                listener = c;
            }
        } else if (c->state != TCP_CLOSED && c->remote_port == tcp->source_port &&
                   c->remote_ip == packet->source && c->local_ip == packet->destination) {
            return c;
        }
    }
    return listener;
}

void tcp_input(const struct ip_packet *packet) {
    if (packet->length < TCP_HEADER || packet->broadcast) {
        return;
    }
    const struct tcp_header *tcp = (const struct tcp_header *)packet->data;
    size_t header_length = (size_t)(tcp->offset >> 4) * 4;
    if (header_length < TCP_HEADER || header_length > packet->length ||
        ip_transport_checksum(packet->source, packet->destination, IP_PROTOCOL_TCP, tcp,
                              packet->length) != 0) {
        return;
    }
    const uint8_t *data = packet->data + header_length;
    size_t length = packet->length - header_length;
    uint8_t flags = tcp->flags;
    uint32_t seq = net32(tcp->seq), ack = net32(tcp->ack);
    size_t segment_length = length + !!(flags & FLAG_SYN) + !!(flags & FLAG_FIN);

    struct tcp_conn *conn = find(packet, tcp);
    if (!conn) {
        send_reset(packet, tcp, segment_length);
        return;
    }

    if (conn->state == TCP_LISTEN) {
        if (flags & FLAG_RST) {
            return;
        }
        if (flags & FLAG_ACK) {
            send_reset(packet, tcp, segment_length);
        } else if (flags & FLAG_SYN) {
            accept_syn(conn, packet, tcp, header_length);
        }
        return;
    }

    if (conn->state == TCP_SYN_SENT) {
        if ((flags & FLAG_ACK) && (seq_le(ack, conn->iss) || seq_lt(conn->snd_nxt, ack))) {
            send_reset(packet, tcp, segment_length);
            return;
        }
        if (flags & FLAG_RST) {
            if (flags & FLAG_ACK) {
                conn_close(conn, -VX_ECONNREFUSED);
            }
            return;
        }
        if ((flags & FLAG_SYN) && (flags & FLAG_ACK)) {
            conn->rcv_nxt = seq + 1;
            conn->snd_una = ack;
            conn->snd_wnd = net16(tcp->window);
            parse_options(conn, tcp, header_length);
            conn->state = TCP_ESTABLISHED;
            conn->retransmit_at = 0;
            conn->retries = 0;
            conn->rto = RTO_INITIAL_MS;
            conn->ack_now = true;
            output(conn, false);
            wake(conn);
        }
        return;
    }

    /* Synchronized states. First: is the segment in our receive window? */
    uint32_t window = receive_window(conn);
    bool acceptable;
    if (segment_length == 0) {
        acceptable = window == 0 ? seq == conn->rcv_nxt
                                 : seq_le(conn->rcv_nxt, seq) &&
                                       seq_lt(seq, conn->rcv_nxt + window);
    } else {
        uint32_t last = seq + (uint32_t)segment_length - 1;
        acceptable = window != 0 && ((seq_le(conn->rcv_nxt, seq) &&
                                      seq_lt(seq, conn->rcv_nxt + window)) ||
                                     (seq_le(conn->rcv_nxt, last) &&
                                      seq_lt(last, conn->rcv_nxt + window)));
    }
    if (!acceptable) {
        if (!(flags & FLAG_RST)) {
            conn->ack_now = true; /* Tell the peer what we expect. */
            output(conn, false);
        }
        return;
    }
    if (flags & FLAG_RST) {
        if (conn->state == TCP_SYN_RECEIVED && conn->listener) {
            conn_close(conn, 0);
        } else {
            conn_close(conn, conn->state == TCP_CLOSE_WAIT ? -VX_EPIPE : -VX_ECONNRESET);
        }
        return;
    }
    if (flags & FLAG_SYN) {
        /* A SYN inside the window: the peer lost track (RFC 5961: answer
         * with an ACK and let it reset). */
        conn->ack_now = true;
        output(conn, false);
        return;
    }
    if (!(flags & FLAG_ACK)) {
        return;
    }

    /* The acknowledgment. */
    if (conn->state == TCP_SYN_RECEIVED) {
        if (seq_lt(conn->snd_una, ack) && seq_le(ack, conn->snd_nxt)) {
            conn->state = TCP_ESTABLISHED;
            conn->snd_una = ack;
            conn->retransmit_at = 0;
            conn->retries = 0;
            struct tcp_conn *listener = conn->listener;
            if (listener) {
                /* Ready for accept(). */
                conn->accept_next = NULL;
                if (listener->accept_tail) {
                    listener->accept_tail->accept_next = conn;
                } else {
                    listener->accept_head = conn;
                }
                listener->accept_tail = conn;
                wake(listener);
            }
        } else {
            send_reset(packet, tcp, segment_length);
            return;
        }
    }
    if (seq_lt(conn->snd_nxt, ack)) {
        conn->ack_now = true; /* Acknowledges something we haven't sent. */
        output(conn, false);
        return;
    }
    if (seq_le(conn->snd_una, ack)) {
        conn->snd_wnd = net16(tcp->window);
        acknowledge(conn, ack);
    }
    switch (conn->state) {
    case TCP_FIN_WAIT_1:
        if (fin_acked(conn)) {
            conn->state = TCP_FIN_WAIT_2;
            if (!conn->socket) {
                conn->close_at = timer_ms() + FIN_WAIT_2_MS;
            }
        }
        break;
    case TCP_CLOSING:
        if (fin_acked(conn)) {
            enter_time_wait(conn);
        }
        break;
    case TCP_LAST_ACK:
        if (fin_acked(conn)) {
            conn_close(conn, 0);
            return;
        }
        break;
    default:
        break;
    }

    /* The data. */
    if (length && (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
                   conn->state == TCP_FIN_WAIT_2)) {
        size_t skip = 0;
        if (seq_lt(seq, conn->rcv_nxt)) {
            skip = conn->rcv_nxt - seq; /* Already have the start. */
        }
        if (seq == conn->rcv_nxt || skip) {
            if (skip < length && !conn->shut_read) {
                size_t n = length - skip;
                size_t room = BUFFER_SIZE - conn->receive_used;
                if (n > room) {
                    n = room;
                }
                for (size_t i = 0; i < n; i++) {
                    conn->receive_buffer[(conn->receive_start + conn->receive_used + i) %
                                         BUFFER_SIZE] = data[skip + i];
                }
                conn->receive_used += n;
                conn->rcv_nxt += (uint32_t)n;
                if (n < length - skip) {
                    flags &= ~FLAG_FIN; /* The FIN comes after data we couldn't take. */
                }
                wake(conn);
            } else if (conn->shut_read && skip < length) {
                conn->rcv_nxt += (uint32_t)(length - skip); /* Discarded. */
            }
        } else {
            flags &= ~FLAG_FIN; /* Out of order: wait for the gap to be filled. */
        }
        conn->ack_now = true;
    }

    /* The FIN. */
    if ((flags & FLAG_FIN) && seq + (uint32_t)length == conn->rcv_nxt && !conn->fin_received) {
        conn->fin_received = true;
        conn->rcv_nxt++;
        conn->ack_now = true;
        switch (conn->state) {
        case TCP_ESTABLISHED:
            conn->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            if (fin_acked(conn)) {
                enter_time_wait(conn);
            } else {
                conn->state = TCP_CLOSING;
            }
            break;
        case TCP_FIN_WAIT_2:
            enter_time_wait(conn);
            break;
        default:
            break;
        }
        wake(conn);
    } else if ((flags & FLAG_FIN) && conn->state == TCP_TIME_WAIT) {
        conn->ack_now = true; /* Our last ACK was lost. */
    }
    output(conn, false);
}

static void retransmit(struct tcp_conn *conn, uint64_t now) {
    bool outstanding = conn->snd_una != conn->snd_nxt;
    if (outstanding) {
        int limit = conn->state == TCP_SYN_SENT || conn->state == TCP_SYN_RECEIVED
                        ? SYN_RETRIES
                        : MAX_RETRIES;
        if (++conn->retries > limit) {
            conn_close(conn, -VX_ETIMEDOUT);
            return;
        }
        conn->rto = conn->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : conn->rto * 2;
    }
    conn->retransmit_at = now + conn->rto;
    switch (conn->state) {
    case TCP_SYN_SENT:
        send_segment(conn, conn->iss, FLAG_SYN, 0, 0);
        return;
    case TCP_SYN_RECEIVED:
        send_segment(conn, conn->iss, FLAG_SYN | FLAG_ACK, 0, 0);
        return;
    default:
        break;
    }
    /* Go back to the oldest unacknowledged byte and send from there. */
    conn->snd_nxt = conn->snd_una;
    if (conn->fin_sent && !fin_acked(conn)) {
        conn->fin_sent = false;
    }
    output(conn, !outstanding);
    if (conn->snd_una == conn->snd_nxt && conn->send_used == 0) {
        conn->retransmit_at = 0;
    }
}

void tcp_tick(uint64_t now) {
    struct tcp_conn *next;
    for (struct tcp_conn *conn = connections; conn; conn = next) {
        next = conn->next;
        if (conn->close_at && now >= conn->close_at) {
            conn_close(conn, 0);
        } else if (conn->retransmit_at && now >= conn->retransmit_at) {
            conn->retransmit_at = 0;
            retransmit(conn, now);
        }
    }
}

/* ---- The socket side ---- */

static int tcp_bind(struct socket *socket, const struct vx_socket_address *address,
                    size_t length) {
    ipv4_t ip;
    uint16_t port;
    int error = inet_parse_address(address, length, &ip, &port);
    if (error) {
        return error;
    }
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    if (conn->bound || conn->state != TCP_CLOSED) {
        error = -VX_EINVAL;
    } else if (ip && !ip_is_local(ip)) {
        error = -VX_EADDRNOTAVAIL;
    } else if (port && tcp_port_in_use(ip, port, socket->reuse_address)) {
        error = -VX_EADDRINUSE;
    } else {
        if (!port) {
            port = inet_ephemeral_port(tcp_port_in_use);
        }
        if (!port) {
            error = -VX_EADDRINUSE;
        } else {
            conn->local_ip = ip;
            conn->local_port = port;
            conn->bound = true;
            list_add(conn);
        }
    }
    mutex_unlock(&net_lock);
    return error;
}

static int tcp_listen(struct socket *socket, int backlog) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    int error = 0;
    if (conn->state == TCP_LISTEN) {
        conn->backlog = backlog;
    } else if (conn->state != TCP_CLOSED || conn->error) {
        error = -VX_EINVAL;
    } else {
        if (!conn->bound) {
            conn->local_port = inet_ephemeral_port(tcp_port_in_use);
            conn->bound = conn->local_port != 0;
        }
        if (!conn->bound) {
            error = -VX_EADDRINUSE;
        } else {
            conn->state = TCP_LISTEN;
            conn->backlog = backlog;
            list_add(conn);
        }
    }
    mutex_unlock(&net_lock);
    return error;
}

static bool accept_ready(void *arg) {
    struct tcp_conn *conn = arg;
    return conn->accept_head || conn->state != TCP_LISTEN;
}

static const struct socket_ops tcp_ops;

static int tcp_accept(struct socket *socket, struct socket **out) {
    struct socket *new_socket = kzalloc(sizeof(*new_socket));
    if (!new_socket) {
        return -VX_ENOMEM;
    }
    mutex_lock(&net_lock);
    struct tcp_conn *listener = conn_of(socket);
    int error = listener->state == TCP_LISTEN ? 0 : -VX_EINVAL;
    if (!error) {
        error = socket_wait(socket, &net_lock, &listener->wait, accept_ready, listener, 0, false);
    }
    if (!error && listener->state != TCP_LISTEN) {
        error = -VX_EINVAL;
    }
    if (!error) {
        struct tcp_conn *conn = listener->accept_head;
        listener->accept_head = conn->accept_next;
        if (!listener->accept_head) {
            listener->accept_tail = NULL;
        }
        listener->pending--;
        conn->listener = NULL;
        conn->accept_next = NULL;
        object_init(&new_socket->object, &socket_object_type);
        new_socket->family = VX_AF_INET;
        new_socket->type = VX_SOCK_STREAM;
        new_socket->protocol = VX_IPPROTO_TCP;
        new_socket->linger_seconds = -1;
        new_socket->ops = &tcp_ops;
        new_socket->data = conn;
        conn->socket = new_socket;
        *out = new_socket;
    }
    mutex_unlock(&net_lock);
    if (error) {
        kfree(new_socket);
    }
    return error;
}

static bool connect_done(void *arg) {
    struct tcp_conn *conn = arg;
    return conn->state != TCP_SYN_SENT;
}

static int tcp_connect(struct socket *socket, const struct vx_socket_address *address,
                       size_t length) {
    ipv4_t ip;
    uint16_t port;
    int error = inet_parse_address(address, length, &ip, &port);
    if (error) {
        return error;
    }
    if (!port || !ip) {
        return -VX_ECONNREFUSED;
    }
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    switch (conn->state) {
    case TCP_CLOSED:
        break;
    case TCP_SYN_SENT:
        error = -VX_EALREADY;
        goto out;
    case TCP_LISTEN:
        error = -VX_EINVAL;
        goto out;
    default:
        error = -VX_EISCONN;
        goto out;
    }
    if (conn->error) {
        error = conn->error;
        conn->error = 0;
        goto out;
    }
    ipv4_t source = ip_source_for(ip);
    if (!source) {
        error = -VX_ENETUNREACH;
        goto out;
    }
    if (!conn->bound) {
        conn->local_port = inet_ephemeral_port(tcp_port_in_use);
        if (!conn->local_port) {
            error = -VX_EADDRNOTAVAIL;
            goto out;
        }
        conn->bound = true;
    }
    if (!conn->local_ip) {
        conn->local_ip = source;
    }
    conn->remote_ip = ip;
    conn->remote_port = port;
    random_bytes(&conn->iss, sizeof(conn->iss));
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1;
    conn->buffer_seq = conn->iss + 1;
    conn->state = TCP_SYN_SENT;
    list_add(conn);
    send_segment(conn, conn->iss, FLAG_SYN, 0, 0);
    start_timer(conn);
    error = socket_wait(socket, &net_lock, &conn->wait, connect_done, conn, 0, true);
    if (error == -VX_EAGAIN) {
        error = -VX_EINPROGRESS;
    } else if (!error && conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT) {
        error = conn->error ? conn->error : -VX_ECONNREFUSED;
        conn->error = 0;
    }
out:
    mutex_unlock(&net_lock);
    return error;
}

static bool writable(void *arg) {
    struct tcp_conn *conn = arg;
    return conn->send_used < BUFFER_SIZE || !can_send_data(conn) || conn->fin_queued;
}

static int64_t tcp_send(struct socket *socket, struct socket_message *message) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    size_t done = 0;
    int64_t error = 0;
    while (done < message->size || message->size == 0) {
        if (conn->error) {
            error = conn->error == -VX_ECONNRESET ? -VX_ECONNRESET : -VX_EPIPE;
            if (error == -VX_ECONNRESET) {
                conn->error = 0; /* Reported once; then EPIPE. */
                conn->fin_queued = true;
            }
            break;
        }
        if (conn->state == TCP_SYN_SENT) {
            error = socket_wait(socket, &net_lock, &conn->wait, connect_done, conn,
                                message->flags, true);
            if (error) {
                break;
            }
            continue;
        }
        if (!can_send_data(conn) || conn->fin_queued) {
            /* Never connected, or can't send any more. */
            error = conn->remote_port == 0 ? -VX_ENOTCONN : -VX_EPIPE;
            break;
        }
        if (message->size == 0) {
            break;
        }
        error = socket_wait(socket, &net_lock, &conn->wait, writable, conn, message->flags, true);
        if (error) {
            break;
        }
        if (!can_send_data(conn) || conn->fin_queued || conn->error) {
            continue; /* Report what happened. */
        }
        size_t room = BUFFER_SIZE - conn->send_used;
        size_t n = message->size - done < room ? message->size - done : room;
        for (size_t i = 0; i < n; i++) {
            conn->send_buffer[(conn->send_start + conn->send_used + i) % BUFFER_SIZE] =
                ((const uint8_t *)message->data)[done + i];
        }
        conn->send_used += n;
        done += n;
        output(conn, false);
    }
    mutex_unlock(&net_lock);
    return done ? (int64_t)done : error;
}

static bool readable(void *arg) {
    struct tcp_conn *conn = arg;
    return conn->receive_used || conn->fin_received || conn->error || conn->shut_read ||
           (conn->state != TCP_ESTABLISHED && conn->state != TCP_FIN_WAIT_1 &&
            conn->state != TCP_FIN_WAIT_2 && conn->state != TCP_SYN_SENT &&
            conn->state != TCP_SYN_RECEIVED);
}

static int64_t tcp_receive(struct socket *socket, struct socket_message *message) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    int64_t result = 0;
    size_t done = 0;
    if (conn->state == TCP_LISTEN || (conn->state == TCP_CLOSED && !conn->remote_port)) {
        result = -VX_ENOTCONN;
        goto out;
    }
    while (done < message->size) {
        int error = socket_wait(socket, &net_lock, &conn->wait, readable, conn,
                                done ? message->flags | VX_MSG_DONTWAIT : message->flags,
                                false);
        if (error) {
            result = done ? 0 : error;
            break;
        }
        if (!conn->receive_used) {
            if (conn->error && !done && !conn->fin_received) {
                result = conn->error;
                conn->error = 0;
            }
            break; /* The end of the data. */
        }
        size_t n = conn->receive_used < message->size - done ? conn->receive_used
                                                               : message->size - done;
        for (size_t i = 0; i < n; i++) {
            ((uint8_t *)message->data)[done + i] =
                conn->receive_buffer[(conn->receive_start + i) % BUFFER_SIZE];
        }
        done += n;
        if (message->flags & VX_MSG_PEEK) {
            break;
        }
        conn->receive_start = (conn->receive_start + n) % BUFFER_SIZE;
        conn->receive_used -= n;
        /* Tell the peer when the window opens up again noticeably. */
        uint32_t window = receive_window(conn);
        if (window > conn->advertised &&
            (window - conn->advertised >= conn->mss || conn->advertised == 0)) {
            conn->ack_now = true;
            output(conn, false);
        }
        if (!(message->flags & VX_MSG_WAITALL)) {
            break;
        }
    }
    if (message->address && done) {
        inet_make_address(message->address, &message->address_length, conn->remote_ip,
                          conn->remote_port);
    }
out:
    mutex_unlock(&net_lock);
    return done ? (int64_t)done : result;
}

static int tcp_shutdown(struct socket *socket, int how) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    int error = 0;
    if (conn->state == TCP_CLOSED || conn->state == TCP_LISTEN || conn->state == TCP_SYN_SENT) {
        error = conn->state == TCP_LISTEN ? 0 : -VX_ENOTCONN;
        if (conn->state == TCP_LISTEN && how != VX_SHUT_WRITE) {
            conn->shut_read = true;
            wake(conn);
        }
    } else {
        if (how != VX_SHUT_WRITE) {
            conn->shut_read = true;
            conn->receive_used = 0;
        }
        if (how != VX_SHUT_READ && !conn->fin_queued) {
            conn->fin_queued = true;
            output(conn, false);
        }
        wake(conn);
    }
    mutex_unlock(&net_lock);
    return error;
}

static int tcp_address(struct socket *socket, bool peer, struct vx_socket_address *address,
                       size_t *length) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    int error = 0;
    if (peer) {
        if (conn->state == TCP_CLOSED || conn->state == TCP_LISTEN ||
            conn->state == TCP_SYN_SENT) {
            error = -VX_ENOTCONN;
        } else {
            inet_make_address(address, length, conn->remote_ip, conn->remote_port);
        }
    } else {
        inet_make_address(address, length, conn->local_ip, conn->local_port);
    }
    mutex_unlock(&net_lock);
    return error;
}

static uint32_t tcp_poll(struct socket *socket) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    uint32_t ready = 0;
    switch (conn->state) {
    case TCP_LISTEN:
        ready = conn->accept_head ? OBJECT_READABLE : 0;
        break;
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        break;
    case TCP_CLOSED:
        ready = OBJECT_WRITABLE | OBJECT_HANGUP | (conn->remote_port ? OBJECT_READABLE : 0);
        break;
    default:
        if (conn->receive_used || conn->fin_received || conn->shut_read) {
            ready |= OBJECT_READABLE;
        }
        if (can_send_data(conn) && !conn->fin_queued && conn->send_used < BUFFER_SIZE) {
            ready |= OBJECT_WRITABLE;
        }
        if (conn->fin_received && conn->fin_queued) {
            ready |= OBJECT_HANGUP;
        }
    }
    if (conn->error) {
        ready |= OBJECT_ERROR | OBJECT_READABLE | OBJECT_WRITABLE;
    }
    mutex_unlock(&net_lock);
    return ready;
}

static int64_t tcp_pending(struct socket *socket) {
    mutex_lock(&net_lock);
    int64_t n = (int64_t)conn_of(socket)->receive_used;
    mutex_unlock(&net_lock);
    return n;
}

static int tcp_take_error(struct socket *socket) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    int error = conn->error;
    conn->error = 0;
    mutex_unlock(&net_lock);
    return error;
}

static void tcp_release(struct socket *socket) {
    mutex_lock(&net_lock);
    struct tcp_conn *conn = conn_of(socket);
    conn->socket = NULL;
    switch (conn->state) {
    case TCP_LISTEN: {
        /* Reset the connections nobody accepted. */
        struct tcp_conn *next;
        for (struct tcp_conn *c = connections; c; c = next) {
            next = c->next;
            if (c->listener == conn) {
                send_segment(c, c->snd_nxt, FLAG_RST | FLAG_ACK, 0, 0);
                c->listener = NULL;
                c->socket = NULL;
                conn_close(c, 0);
            }
        }
        conn_free(conn);
        break;
    }
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        conn_free(conn);
        break;
    case TCP_TIME_WAIT:
        break; /* Its timer frees it. */
    default:
        if (conn->receive_used || socket->linger_seconds == 0) {
            /* Unread data (or SO_LINGER 0): abort, as Linux does. */
            send_segment(conn, conn->snd_nxt, FLAG_RST | FLAG_ACK, 0, 0);
            conn_close(conn, 0);
            break;
        }
        conn->fin_queued = true;
        output(conn, false);
        if (conn->state == TCP_FIN_WAIT_2) {
            conn->close_at = timer_ms() + FIN_WAIT_2_MS;
        }
        break;
    }
    mutex_unlock(&net_lock);
}

static const struct socket_ops tcp_ops = {
    .bind = tcp_bind,
    .connect = tcp_connect,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .send = tcp_send,
    .receive = tcp_receive,
    .shutdown = tcp_shutdown,
    .address = tcp_address,
    .poll = tcp_poll,
    .pending = tcp_pending,
    .take_error = tcp_take_error,
    .release = tcp_release,
};

int tcp_create(struct socket *socket) {
    struct tcp_conn *conn = conn_new();
    if (!conn) {
        return -VX_ENOMEM;
    }
    conn->socket = socket;
    socket->data = conn;
    socket->ops = &tcp_ops;
    return 0;
}
