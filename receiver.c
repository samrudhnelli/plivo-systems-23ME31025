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
 *
 * This receiver:
 *   1. Receives DATA/FEC/RETX packets, stores in jitter buffer
 *   2. On FEC arrival, attempts to recover missing pair member
 *   3. Sends NACKs for detected gaps
 *   4. Playout thread delivers frames at T0 + DELAY_MS + i*20ms
 *
 * Ports:
 *   bind 47002 ← media from relay
 *   send 47020 → harness player (4-byte BE seq + 160-byte payload)
 *   send 47003 → feedback to sender via relay
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_BYTES 160
#define HARNESS_HDR   4
#define WIRE_HDR      3
#define WIRE_PKT      (WIRE_HDR + PAYLOAD_BYTES)
#define FRAME_MS      20

#define TYPE_DATA   0x01
#define TYPE_FEC    0x02
#define TYPE_RETX   0x03
#define TYPE_NACK   0x04
#define TYPE_FEC_BRIDGE 0x05

#define BUF_SIZE    8192
#define BUF_MASK    (BUF_SIZE - 1)

/* Jitter buffer */
static uint8_t  jbuf_payload[BUF_SIZE][PAYLOAD_BYTES];
static int      jbuf_received[BUF_SIZE];
static pthread_mutex_t jbuf_lock = PTHREAD_MUTEX_INITIALIZER;

/* FEC buffer: store FEC packets for recovery */
static uint8_t  fec_payload[BUF_SIZE][PAYLOAD_BYTES];
static int      fec_received[BUF_SIZE];

/* NACK tracking: avoid spamming NACKs */
static int      nack_sent[BUF_SIZE];

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

/* Track highest received seq for gap detection */
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

    if (jbuf_received[idx_a] && !jbuf_received[idx_b]) {
        /* Recover frame B = FEC XOR A */
        for (int j = 0; j < PAYLOAD_BYTES; j++) {
            jbuf_payload[idx_b][j] = fec_payload[fec_idx][j] ^ jbuf_payload[idx_a][j];
        }
        jbuf_received[idx_b] = 1;
        /* Trigger cascade on recovered frame B */
        trigger_recovery_cascade(seq_b);
        return 1;
    } else if (!jbuf_received[idx_a] && jbuf_received[idx_b]) {
        /* Recover frame A = FEC XOR B */
        for (int j = 0; j < PAYLOAD_BYTES; j++) {
            jbuf_payload[idx_a][j] = fec_payload[fec_idx][j] ^ jbuf_payload[idx_b][j];
        }
        jbuf_received[idx_a] = 1;
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

        pthread_mutex_lock(&jbuf_lock);
        if (jbuf_received[idx]) {
            uint8_t payload_copy[PAYLOAD_BYTES];
            memcpy(payload_copy, jbuf_payload[idx], PAYLOAD_BYTES);
            pthread_mutex_unlock(&jbuf_lock);
            send_to_player(seq, payload_copy);
        } else {
            pthread_mutex_unlock(&jbuf_lock);
            /* Frame missed — nothing to deliver */
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
    memset(jbuf_received, 0, sizeof(jbuf_received));
    memset(fec_received, 0, sizeof(fec_received));
    memset(nack_sent, 0, sizeof(nack_sent));

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

    /* Set receive timeout so we can do periodic NACK checks */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 5000;  /* 5ms timeout */
    setsockopt(in_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

    /* Main receive loop */
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < WIRE_HDR) {
            /* Timeout or short packet — use this time for NACK checks */
            /* Periodic NACK: check for gaps in received frames */
            double now = now_sec();
            pthread_mutex_lock(&jbuf_lock);
            for (int i = 0; i <= highest_seq_seen && i < n_frames; i++) {
                int idx = i & BUF_MASK;
                if (!jbuf_received[idx] && !nack_sent[idx]) {
                    /* Check if we still have time to recover this frame */
                    double deadline = t0 + delay_s + i * (FRAME_MS / 1000.0);
                    if (deadline - now > 0.015) {  /* Need at least 15ms RTT */
                        nack_sent[idx] = 1;
                        pthread_mutex_unlock(&jbuf_lock);
                        send_nack((uint16_t)i);
                        pthread_mutex_lock(&jbuf_lock);
                    }
                }
            }
            pthread_mutex_unlock(&jbuf_lock);
            continue;
        }

        uint8_t type = buf[0];
        uint16_t seq = ((uint16_t)buf[1] << 8) | buf[2];
        uint8_t *payload = buf + WIRE_HDR;

        if (seq >= n_frames) continue;  /* Out of range */

        pthread_mutex_lock(&jbuf_lock);

        if (type == TYPE_DATA || type == TYPE_RETX) {
            int idx = seq & BUF_MASK;
            if (!jbuf_received[idx]) {
                memcpy(jbuf_payload[idx], payload, PAYLOAD_BYTES);
                jbuf_received[idx] = 1;

                /* Trigger cascading FEC recovery for neighbors */
                trigger_recovery_cascade(seq);
            }

            /* Update highest seen for gap detection */
            if ((int)seq > highest_seq_seen) {
                int old_highest = highest_seq_seen;
                highest_seq_seen = (int)seq;

                /* Send NACKs for newly detected gaps */
                for (int g = old_highest + 1; g < (int)seq; g++) {
                    if (g >= 0 && g < n_frames) {
                        int gidx = g & BUF_MASK;
                        if (!jbuf_received[gidx] && !nack_sent[gidx]) {
                            double deadline = t0 + delay_s + g * (FRAME_MS / 1000.0);
                            if (deadline - now_sec() > 0.015) {
                                nack_sent[gidx] = 1;
                                pthread_mutex_unlock(&jbuf_lock);
                                send_nack((uint16_t)g);
                                pthread_mutex_lock(&jbuf_lock);
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
                /* Try immediate recovery; if it succeeds, it cascades recursively */
                try_fec_recovery(seq);
            }
        }

        pthread_mutex_unlock(&jbuf_lock);
    }

    pthread_join(play_tid, NULL);
    return 0;
}
