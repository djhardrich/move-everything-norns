/*
 * norns-input-bridge — Translates MIDI from Move into Norns encoder/key events
 *
 * Reads: /tmp/midi-to-chroot-<slot>  (2-byte LE length + raw MIDI)
 * Writes: /tmp/norns-input-<slot>    (4-byte frames: [type][id][val_lo][val_hi])
 *
 * Usage: norns-input-bridge <midi_fifo> <input_fifo>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

static volatile int running = 1;
static void handle_signal(int sig) { (void)sig; running = 0; }

/* (prev_cc tracking removed — Move knobs are relative encoders) */

/* Write a 4-byte input event to the norns input FIFO */
static void send_input(int fd, uint8_t type, uint8_t id, int16_t value) {
    uint8_t frame[4];
    frame[0] = type;
    frame[1] = id;
    frame[2] = (uint8_t)(value & 0xFF);
    frame[3] = (uint8_t)((value >> 8) & 0xFF);
    write(fd, frame, 4);  /* non-blocking, drop if full */
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <midi_fifo> <input_fifo>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int midi_fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (midi_fd < 0) { perror("open midi fifo"); return 1; }

    int input_fd = open(argv[2], O_RDWR | O_NONBLOCK);
    if (input_fd < 0) { perror("open input fifo"); return 1; }

    fprintf(stderr, "norns-input-bridge: MIDI=%s INPUT=%s\n", argv[1], argv[2]);

    uint8_t buf[4096];
    size_t buf_len = 0;

    while (running) {
        /* Read from MIDI FIFO */
        uint8_t tmp[512];
        ssize_t n = read(midi_fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (buf_len + n <= sizeof(buf)) {
                memcpy(buf + buf_len, tmp, n);
                buf_len += n;
            }
        }

        /* Parse complete MIDI frames (2-byte LE length prefix) */
        size_t pos = 0;
        while (pos + 2 <= buf_len) {
            uint16_t msg_len = buf[pos] | (buf[pos + 1] << 8);
            if (msg_len == 0) { pos += 2; continue; }
            if (pos + 2 + msg_len > buf_len) break;

            uint8_t *msg = buf + pos + 2;

            /* CC 71-73 → encoder delta E1-E3.
             * Move knobs are RELATIVE encoders:
             *   1-63  = clockwise  (1=slow, 63=fast)
             *   65-127 = counter-clockwise (127=slow, 65=fast) */
            if (msg_len >= 3 && (msg[0] & 0xF0) == 0xB0) {
                uint8_t cc = msg[1];
                uint8_t val = msg[2];
                if (cc >= 71 && cc <= 73) {
                    uint8_t enc_id = cc - 71;  /* 0=E1, 1=E2, 2=E3 */
                    int delta = 0;
                    if (val >= 1 && val <= 63) {
                        delta = (int)val;       /* CW: positive */
                    } else if (val >= 65) {
                        delta = (int)val - 128;  /* CCW: negative (-63 to -1) */
                    }
                    /* Scale down for norns — norns expects small deltas (±1 to ±3) */
                    if (delta > 3) delta = 3;
                    if (delta < -3) delta = -3;
                    if (delta != 0) {
                        send_input(input_fd, 0, enc_id, (int16_t)delta);
                    }
                }
            }

            /* MIDI note on/off → type 2 frame for Lua MIDI injection.
             * Frame: [2, status, note, velocity] */
            if (msg_len >= 3 && ((msg[0] & 0xF0) == 0x90 || (msg[0] & 0xF0) == 0x80)) {
                uint8_t frame[4];
                frame[0] = 2;       /* type 2 = MIDI */
                frame[1] = msg[0];  /* status byte */
                frame[2] = msg[1];  /* note */
                frame[3] = msg[2];  /* velocity */
                write(input_fd, frame, 4);
            }

            /* Grid key marker (0xF9 x y state) → type 3 frame for grid events.
             * DSP plugin sends this when ui.js is in grid mode. */
            if (msg_len >= 4 && msg[0] == 0xF9) {
                uint8_t frame[4];
                frame[0] = 3;       /* type 3 = grid key */
                frame[1] = msg[1];  /* x */
                frame[2] = msg[2];  /* y */
                frame[3] = msg[3];  /* state (1=press, 0=release) */
                write(input_fd, frame, 4);
            }

            /* Track buttons (CC 43/42/41) → key K1/K2/K3
             * CC 43=Track1→K1, CC 42=Track2→K2, CC 41=Track3→K3
             * value > 0 = pressed, value == 0 = released */
            if (msg_len >= 3 && (msg[0] & 0xF0) == 0xB0) {
                uint8_t cc = msg[1];
                uint8_t val = msg[2];
                if (cc >= 41 && cc <= 43) {
                    uint8_t key_id = 43 - cc;  /* CC43→0(K1), CC42→1(K2), CC41→2(K3) */
                    if (val > 0) {
                        send_input(input_fd, 1, key_id, 1);  /* key down */
                    } else {
                        send_input(input_fd, 1, key_id, 0);  /* key up */
                    }
                }
            }

            pos += 2 + msg_len;
        }

        /* Shift unconsumed bytes */
        if (pos > 0 && pos < buf_len) {
            memmove(buf, buf + pos, buf_len - pos);
            buf_len -= pos;
        } else if (pos >= buf_len) {
            buf_len = 0;
        }

        usleep(1000);  /* 1ms */
    }

    close(midi_fd);
    close(input_fd);
    return 0;
}
