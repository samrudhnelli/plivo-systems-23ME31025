/*
 * SENDER — Portable Lock-Free Dual FEC + Ring Buffer NACKs
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdatomic.h>

#define PAYLOAD_BYTES 160
#define PAYLOAD_U64   (PAYLOAD_BYTES / 8) /* 20 blocks of 64-bit integers */
#define HARNESS_HDR   4
#define WIRE_HDR      3        /* 1 type + 2 seq */
#define WIRE_PKT      (WIRE_HDR + PAYLOAD_BYTES)  /* 163 */

#define TYPE_DATA       0x01
#define TYPE_FEC        0x02
#define TYPE_RETX       0x03
#define TYPE_NACK       0x04
#define TYPE_FEC_BRIDGE 0x05

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define RING_SIZE   4096
#define RING_MASK   (RING_SIZE - 1)

/* Ring buffer for retransmission (aligned to prevent false sharing, lock-free SPSC) */
static uint64_t ring_payload[RING_SIZE][PAYLOAD_U64] __attribute__((aligned(64)));
static _Atomic int ring_valid[RING_SIZE] __attribute__((aligned(64)));

/* Shared socket for sending to relay */
static int out_fd;
static struct sockaddr_in relay_addr;

/* FEC state: hold previous frame for pair generation (64-bit aligned) */
static uint64_t prev_payload[PAYLOAD_U64];
static int      prev_seq = -1;

static void send_wire_packet(uint8_t type, uint16_t seq, const uint8_t *payload) {
    uint8_t pkt[WIRE_PKT];
    pkt[0] = type;
    pkt[1] = (uint8_t)(seq >> 8);
    pkt[2] = (uint8_t)(seq & 0xFF);
    memcpy(pkt + WIRE_HDR, payload, PAYLOAD_BYTES);
    sendto(out_fd, pkt, WIRE_PKT, 0,
           (struct sockaddr *)&relay_addr, sizeof(relay_addr));
}

/* Feedback listener thread: receives NACKs, retransmits from ring buffer (lock-free read) */
static void *feedback_thread(void *arg) {
    int fb_fd = *(int *)arg;
    uint8_t buf[64];

    for (;;) {
        ssize_t n = recvfrom(fb_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (UNLIKELY(n < WIRE_HDR)) continue;
        if (UNLIKELY(buf[0] != TYPE_NACK)) continue;

        uint16_t seq = ((uint16_t)buf[1] << 8) | buf[2];
        int idx = seq & RING_MASK;

        if (atomic_load_explicit(&ring_valid[idx], memory_order_acquire)) {
            uint64_t payload_copy[PAYLOAD_U64];
            memcpy(payload_copy, ring_payload[idx], PAYLOAD_BYTES);
            send_wire_packet(TYPE_RETX, seq, (const uint8_t *)payload_copy);
        }
    }
    return NULL;
}

int main(void) {
    /* Bind to receive harness frames */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47010");
        return 1;
    }

    /* Output socket to relay */
    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_addr, 0, sizeof(relay_addr));
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(47001);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Feedback socket */
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr;
    memset(&fb_addr, 0, sizeof(fb_addr));
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof(fb_addr)) < 0) {
        perror("bind 47004");
        return 1;
    }

    /* Initialize ring buffer */
    for (int i = 0; i < RING_SIZE; i++) {
        atomic_store_explicit(&ring_valid[i], 0, memory_order_relaxed);
    }

    /* Start feedback listener thread */
    pthread_t fb_tid;
    pthread_create(&fb_tid, NULL, feedback_thread, &fb_fd);
    pthread_detach(fb_tid);

    /* Main loop: receive from harness, send DATA + dual FEC */
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (UNLIKELY(n < HARNESS_HDR + PAYLOAD_BYTES)) continue;

        /* Parse harness frame: 4-byte big-endian seq + 160-byte payload */
        uint32_t seq32 = ((uint32_t)buf[0] << 24) |
                         ((uint32_t)buf[1] << 16) |
                         ((uint32_t)buf[2] << 8)  |
                         ((uint32_t)buf[3]);
        uint16_t seq = (uint16_t)(seq32 & 0xFFFF);
        uint8_t *payload = buf + HARNESS_HDR;

        /* Store in ring buffer (lock-free write) */
        int idx = seq & RING_MASK;
        memcpy(ring_payload[idx], payload, PAYLOAD_BYTES);
        atomic_store_explicit(&ring_valid[idx], 1, memory_order_release);

        /* Send DATA packet */
        send_wire_packet(TYPE_DATA, seq, payload);

        /* Generate XOR FEC payload with previous frame */
        if (LIKELY(prev_seq >= 0 && seq == (uint16_t)(prev_seq + 1))) {
            uint64_t fec_pl[PAYLOAD_U64];
            uint64_t *curr_pl = ring_payload[idx];
            
            #pragma GCC unroll 4
            for (int j = 0; j < PAYLOAD_U64; j++) {
                fec_pl[j] = prev_payload[j] ^ curr_pl[j];
            }

            if ((seq & 1) == 1) {
                /* Odd seq: non-overlapping pair FEC for (seq-1, seq) */
                send_wire_packet(TYPE_FEC, (uint16_t)prev_seq, (const uint8_t *)fec_pl);
            } else if ((prev_seq & 3) == 1) {
                /* Even seq (>=2): bridge FEC at 50% density (covers 1,2; 5,6; 9,10...) */
                send_wire_packet(TYPE_FEC_BRIDGE, (uint16_t)prev_seq, (const uint8_t *)fec_pl);
            }
        }

        /* Save current as "previous" for next FEC pair */
        memcpy(prev_payload, ring_payload[idx], PAYLOAD_BYTES);
        prev_seq = (int)seq;
    }
    return 0;
}
