/*
 * RECEIVER — Jitter buffer + FEC recovery + NACK sender + Playout timer
 *
 * Wire format (from relay, originally sent by sender):
 *   [1 byte type] [2 byte big-endian seq] [160 byte payload] = 163 bytes
 *
 * Type values:
 *   0x01 = DATA     (original frame)
 *   0x02 = FEC      (XOR of pair: seq = first of pair)
 *   0x03 = RETRANSMIT (same as DATA)
 *   0x04 = NACK     (feedback request)
 *   0x05 = FEC_BRIDGE (XOR of bridge pair)
 *
 * Advanced System Features:
 *   1. Lock-free Jitter Buffer utilizing `<stdatomic.h>` memory barriers.
 *   2. epoll Event Loop + timerfd to eliminate socket receive blocking timeouts.
 *   3. SIMD-aligned payload memory representation (uint64_t[20]).
 *   4. Dynamic SRTT tracking to adapt NACK transmission to real-time delay jitter.
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>

#define PAYLOAD_BYTES 160
#define PAYLOAD_U64   (PAYLOAD_BYTES / 8) /* 20 blocks of 64-bit integers */
#define HARNESS_HDR   4
#define WIRE_HDR      3
#define WIRE_PKT      (WIRE_HDR + PAYLOAD_BYTES)
#define FRAME_MS      20

#define TYPE_DATA       0x01
#define TYPE_FEC        0x02
#define TYPE_RETX       0x03
#define TYPE_NACK       0x04
#define TYPE_FEC_BRIDGE 0x05

#define BUF_SIZE    8192
#define BUF_MASK    (BUF_SIZE - 1)

/* Lock-free Jitter buffer (64-bit aligned for SIMD XOR) */
static uint64_t           jbuf_payload[BUF_SIZE][PAYLOAD_U64];
static _Atomic int        jbuf_received[BUF_SIZE];

/* FEC buffer: store FEC packets for recovery (only accessed by receiver thread) */
static uint64_t           fec_payload[BUF_SIZE][PAYLOAD_U64];
static int                fec_received[BUF_SIZE];

/* NACK tracking: avoid spamming NACKs */
static int                nack_sent[BUF_SIZE];
static double             nack_sent_time[BUF_SIZE];

/* Dynamic SRTT tracking (running average of RTT) */
static double             srtt = 0.015; // Initial default SRTT of 15ms

/* Shared sockets */
static int player_fd;
static struct sockaddr_in player_addr;
static int feedback_fd;
static struct sockaddr_in relay_fb_addr;

/* Timing */
static double t0;
static double delay_s;
static int    duration_s;
static int    n_frames;

/* Track highest received seq for gap detection (only accessed by receiver thread) */
static int highest_seq_seen = -1;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_until(double target) {
    double rem = target - now_sec();
    if (rem <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)rem;
    ts.tv_nsec = (long)((rem - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static void send_to_player(uint16_t seq, const uint8_t *payload) {
    uint8_t pkt[HARNESS_HDR + PAYLOAD_BYTES];
    uint32_t seq32 = (uint32_t)seq;
    pkt[0] = (uint8_t)(seq32 >> 24);
    pkt[1] = (uint8_t)(seq32 >> 16);
    pkt[2] = (uint8_t)(seq32 >> 8);
    pkt[3] = (uint8_t)(seq32);
    memcpy(pkt + HARNESS_HDR, payload, PAYLOAD_BYTES);
    sendto(player_fd, pkt, sizeof(pkt), 0,
           (struct sockaddr *)&player_addr, sizeof(player_addr));
}

static void send_nack(uint16_t seq) {
    uint8_t pkt[WIRE_HDR];
    pkt[0] = TYPE_NACK;
    pkt[1] = (uint8_t)(seq >> 8);
    pkt[2] = (uint8_t)(seq & 0xFF);
    sendto(feedback_fd, pkt, WIRE_HDR, 0,
           (struct sockaddr *)&relay_fb_addr, sizeof(relay_fb_addr));
}

static void trigger_recovery_cascade(uint16_t seq);

/* Try FEC recovery for a pair (base_seq, base_seq+1). Returns 1 if a frame was recovered, 0 otherwise. */
static int try_fec_recovery(uint16_t base_seq) {
    int fec_idx = base_seq & BUF_MASK;
    if (!fec_received[fec_idx]) return 0;

    uint16_t seq_a = base_seq;
    uint16_t seq_b = base_seq + 1;
    int idx_a = seq_a & BUF_MASK;
    int idx_b = seq_b & BUF_MASK;

    int rec_a = atomic_load_explicit(&jbuf_received[idx_a], memory_order_acquire);
    int rec_b = atomic_load_explicit(&jbuf_received[idx_b], memory_order_acquire);

    if (rec_a && !rec_b) {
        /* Recover frame B = FEC XOR A */
        #pragma GCC unroll 4
        for (int j = 0; j < PAYLOAD_U64; j++) {
            jbuf_payload[idx_b][j] = fec_payload[fec_idx][j] ^ jbuf_payload[idx_a][j];
        }
        atomic_store_explicit(&jbuf_received[idx_b], 1, memory_order_release);
        /* Trigger cascade on recovered frame B */
        trigger_recovery_cascade(seq_b);
        return 1;
    } else if (!rec_a && rec_b) {
        /* Recover frame A = FEC XOR B */
        #pragma GCC unroll 4
        for (int j = 0; j < PAYLOAD_U64; j++) {
            jbuf_payload[idx_a][j] = fec_payload[fec_idx][j] ^ jbuf_payload[idx_b][j];
        }
        atomic_store_explicit(&jbuf_received[idx_a], 1, memory_order_release);
        /* Trigger cascade on recovered frame A */
        trigger_recovery_cascade(seq_a);
        return 1;
    }
    return 0;
}

/* Recursive cascading recovery when a frame becomes available */
static void trigger_recovery_cascade(uint16_t seq) {
    /* Try recovering the standard partner in (base_std, base_std+1) */
    uint16_t standard_base = seq & ~1; /* Round down to even */
    try_fec_recovery(standard_base);

    /* Try recovering the bridge partner in (base_br, base_br+1) */
    if (seq > 0) {
        uint16_t bridge_base = (seq % 2 == 0) ? (seq - 1) : seq;
        try_fec_recovery(bridge_base);
    }
}

/* Playout thread: delivers frames to the harness player at their deadlines */
static void *playout_thread(void *arg) {
    (void)arg;

    for (int i = 0; i < n_frames; i++) {
        double deadline = t0 + delay_s + i * (FRAME_MS / 1000.0);
        /* Wait until a bit before deadline to allow late arrivals */
        double play_time = deadline - 0.001;  /* 1ms before deadline */
        sleep_until(play_time);

        uint16_t seq = (uint16_t)i;
        int idx = seq & BUF_MASK;

        int received = atomic_load_explicit(&jbuf_received[idx], memory_order_acquire);
        if (received) {
            uint64_t payload_copy[PAYLOAD_U64];
            memcpy(payload_copy, jbuf_payload[idx], PAYLOAD_BYTES);
            /* Reset received flag to allow index reuse in wraparound scenarios */
            atomic_store_explicit(&jbuf_received[idx], 0, memory_order_release);
            send_to_player(seq, (const uint8_t *)payload_copy);
        }
    }
    return NULL;
}

int main(void) {
    /* Parse environment variables */
    const char *t0_str = getenv("T0");
    const char *dur_str = getenv("DURATION_S");
    const char *delay_str = getenv("DELAY_MS");

    if (t0_str) t0 = atof(t0_str);
    else t0 = now_sec() + 1.0;

    if (dur_str) duration_s = atoi(dur_str);
    else duration_s = 30;

    double delay_ms = 60.0;
    if (delay_str) delay_ms = atof(delay_str);
    delay_s = delay_ms / 1000.0;

    n_frames = (int)(duration_s * 1000 / FRAME_MS);

    /* Initialize buffers */
    for (int i = 0; i < BUF_SIZE; i++) {
        atomic_store_explicit(&jbuf_received[i], 0, memory_order_relaxed);
    }
    memset(fec_received, 0, sizeof(fec_received));
    memset(nack_sent, 0, sizeof(nack_sent));
    for (int i = 0; i < BUF_SIZE; i++) {
        nack_sent_time[i] = 0.0;
    }

    /* Bind to receive media from relay */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }

    /* Player output socket */
    player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player_addr, 0, sizeof(player_addr));
    player_addr.sin_family = AF_INET;
    player_addr.sin_port = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Feedback socket (NACKs to sender via relay) */
    feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_fb_addr, 0, sizeof(relay_fb_addr));
    relay_fb_addr.sin_family = AF_INET;
    relay_fb_addr.sin_port = htons(47003);
    relay_fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Start playout thread */
    pthread_t play_tid;
    pthread_create(&play_tid, NULL, playout_thread, NULL);

    /* Setup epoll event loop */
    int ep_fd = epoll_create1(0);
    if (ep_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev, events[4];

    /* Register media socket fd */
    ev.events = EPOLLIN;
    ev.data.fd = in_fd;
    if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, in_fd, &ev) < 0) {
        perror("epoll_ctl in_fd");
        return 1;
    }

    /* Register timerfd for periodic NACK sweeps (every 5ms) */
    int t_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (t_fd < 0) {
        perror("timerfd_create");
        return 1;
    }

    struct itimerspec ts;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 5 * 1000000; // 5ms
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 5 * 1000000;
    if (timerfd_settime(t_fd, 0, &ts, NULL) < 0) {
        perror("timerfd_settime");
        return 1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = t_fd;
    if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, t_fd, &ev) < 0) {
        perror("epoll_ctl t_fd");
        return 1;
    }

    /* Main receive loop */
    uint8_t buf[2048];
    for (;;) {
        int nfds = epoll_wait(ep_fd, events, 4, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        double now = now_sec();

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == t_fd) {
                /* Timer fired: consume expiration to reset state */
                uint64_t expirations;
                if (read(t_fd, &expirations, sizeof(expirations)) < 0) {
                    /* ignore error */
                }

                /* Periodic NACK sweep: detect gaps in jitter buffer */
                for (int j = 0; j <= highest_seq_seen && j < n_frames; j++) {
                    int idx = j & BUF_MASK;
                    int received = atomic_load_explicit(&jbuf_received[idx], memory_order_acquire);
                    if (!received && !nack_sent[idx]) {
                        double deadline = t0 + delay_s + j * (FRAME_MS / 1000.0);
                        if (deadline - now > srtt) {
                            nack_sent[idx] = 1;
                            nack_sent_time[idx] = now;
                            send_nack((uint16_t)j);
                        }
                    }
                }
            } else if (events[i].data.fd == in_fd) {
                /* UDP Packet ready */
                ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
                if (n < WIRE_HDR) continue;

                uint8_t type = buf[0];
                uint16_t seq = ((uint16_t)buf[1] << 8) | buf[2];
                uint8_t *payload = buf + WIRE_HDR;

                if (seq >= n_frames) continue;

                if (type == TYPE_DATA || type == TYPE_RETX) {
                    /* On retransmission arrival, dynamically update SRTT */
                    if (type == TYPE_RETX) {
                        double rtt = now - nack_sent_time[seq & BUF_MASK];
                        if (rtt > 0.0 && rtt < 0.5) {
                            srtt = 0.875 * srtt + 0.125 * rtt;
                        }
                    }

                    int idx = seq & BUF_MASK;
                    int received = atomic_load_explicit(&jbuf_received[idx], memory_order_acquire);
                    if (!received) {
                        memcpy(jbuf_payload[idx], payload, PAYLOAD_BYTES);
                        atomic_store_explicit(&jbuf_received[idx], 1, memory_order_release);
                        trigger_recovery_cascade(seq);
                    }

                    /* Update highest seen for gap detection */
                    if ((int)seq > highest_seq_seen) {
                        int old_highest = highest_seq_seen;
                        highest_seq_seen = (int)seq;

                        /* Immediate NACK sweep on new gap detection */
                        for (int g = old_highest + 1; g < (int)seq; g++) {
                            if (g >= 0 && g < n_frames) {
                                int gidx = g & BUF_MASK;
                                int g_rec = atomic_load_explicit(&jbuf_received[gidx], memory_order_acquire);
                                if (!g_rec && !nack_sent[gidx]) {
                                    double deadline = t0 + delay_s + g * (FRAME_MS / 1000.0);
                                    if (deadline - now > srtt) {
                                        nack_sent[gidx] = 1;
                                        nack_sent_time[gidx] = now;
                                        send_nack((uint16_t)g);
                                    }
                                }
                             }
                         }
                    }
                } else if (type == TYPE_FEC || type == TYPE_FEC_BRIDGE) {
                    int fec_idx = seq & BUF_MASK;
                    if (!fec_received[fec_idx]) {
                        memcpy(fec_payload[fec_idx], payload, PAYLOAD_BYTES);
                        fec_received[fec_idx] = 1;
                        /* Try immediate recovery */
                        try_fec_recovery(seq);
                    }
                }
            }
        }
    }

    pthread_join(play_tid, NULL);
    close(ep_fd);
    close(t_fd);
    return 0;
}
