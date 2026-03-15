/*
 * jack-fifo-bridge — Captures JACK audio and writes to shared memory ring.
 *
 * Primary path: lock-free SHM ring buffer (zero-copy, no syscalls).
 * Fallback: writes raw S16LE stereo to a FIFO if SHM isn't set up.
 *
 * Connects to crone:output_1/2 and writes interleaved S16LE.
 *
 * Usage: jack-fifo-bridge <fifo_path> [slot]
 *        fifo_path is still required (used as fallback + for slot detection)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sched.h>
#include <jack/jack.h>

#include "shm_audio.h"

static volatile int running = 1;
static jack_client_t *client = NULL;
static jack_port_t *port_l = NULL;
static jack_port_t *port_r = NULL;
static int fifo_fd = -1;
static shm_audio_t *shm = NULL;

/* Pre-allocated conversion buffer — no malloc in audio callback.
 * 8192 frames covers any plausible PipeWire quantum. */
#define MAX_BRIDGE_FRAMES 8192
static int16_t g_conv_buf[MAX_BRIDGE_FRAMES * 2];

static void handle_signal(int sig) { (void)sig; running = 0; }

static int process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    float *in_l = (float *)jack_port_get_buffer(port_l, nframes);
    float *in_r = (float *)jack_port_get_buffer(port_r, nframes);
    if (!in_l || !in_r) return 0;

    /* Convert F32 → S16LE interleaved — zero allocations in this callback */
    int16_t *buf = g_conv_buf;
    jack_nframes_t n = nframes;
    if (n > MAX_BRIDGE_FRAMES) n = MAX_BRIDGE_FRAMES;

    for (jack_nframes_t i = 0; i < n; i++) {
        float sl = in_l[i] * 32767.0f;
        float sr = in_r[i] * 32767.0f;
        if (sl >  32767.0f) sl =  32767.0f;
        if (sl < -32768.0f) sl = -32768.0f;
        if (sr >  32767.0f) sr =  32767.0f;
        if (sr < -32768.0f) sr = -32768.0f;
        buf[i * 2]     = (int16_t)sl;
        buf[i * 2 + 1] = (int16_t)sr;
    }

    /* Write to SHM ring (zero-copy, no syscalls) */
    if (shm) {
        shm_write(shm, buf, n);
    } else if (fifo_fd >= 0) {
        /* Fallback: FIFO (non-blocking, drop if full) */
        write(fifo_fd, buf, n * 4);
    }

    return 0;
}

static shm_audio_t *open_shm(int slot) {
    char path[64];
    snprintf(path, sizeof(path), SHM_AUDIO_PATH_FMT, slot);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "jack-fifo-bridge: SHM %s not found, using FIFO fallback\n", path);
        return NULL;
    }

    void *p = mmap(NULL, SHM_AUDIO_FILE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "jack-fifo-bridge: mmap failed, using FIFO fallback\n");
        return NULL;
    }

    shm_audio_t *s = (shm_audio_t *)p;
    if (s->magic != SHM_AUDIO_MAGIC) {
        fprintf(stderr, "jack-fifo-bridge: SHM bad magic, using FIFO fallback\n");
        munmap(p, SHM_AUDIO_FILE_SIZE);
        return NULL;
    }

    fprintf(stderr, "jack-fifo-bridge: using SHM ring at %s (zero-copy)\n", path);
    return s;
}

int main(int argc, char *argv[]) {
    const char *fifo_path = argc > 1 ? argv[1] : "/tmp/pw-to-move-1";
    int slot = argc > 2 ? atoi(argv[2]) : 1;

    /* Detect slot from fifo path if not explicit */
    if (slot <= 0) {
        const char *p = strrchr(fifo_path, '-');
        if (p) slot = atoi(p + 1);
        if (slot <= 0) slot = 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Try SHM first (created by DSP plugin) */
    shm = open_shm(slot);

    /* Open FIFO as fallback */
    if (!shm) {
        fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0)
            fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (fifo_fd < 0) {
            perror("open fifo");
            return 1;
        }
    }

    jack_status_t status;
    client = jack_client_open("move-bridge", JackNoStartServer, &status);
    if (!client) {
        fprintf(stderr, "jack-fifo-bridge: can't open JACK client (status=%d)\n", status);
        return 1;
    }

    port_l = jack_port_register(client, "in_L",
                                JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    port_r = jack_port_register(client, "in_R",
                                JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!port_l || !port_r) {
        fprintf(stderr, "jack-fifo-bridge: can't register ports\n");
        jack_client_close(client);
        return 1;
    }

    jack_set_process_callback(client, process, NULL);

    /* Lock all pages in RAM — prevents page faults from hitting SD card
     * during the JACK process callback (the main source of glitches). */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "jack-fifo-bridge: mlockall failed (not fatal)\n");

    if (jack_activate(client)) {
        fprintf(stderr, "jack-fifo-bridge: can't activate\n");
        jack_client_close(client);
        return 1;
    }

    /* Connect to crone outputs */
    jack_connect(client, "crone:output_1", "move-bridge:in_L");
    jack_connect(client, "crone:output_2", "move-bridge:in_R");

    fprintf(stderr, "jack-fifo-bridge: running (%s)\n",
            shm ? "SHM zero-copy" : "FIFO fallback");

    while (running)
        usleep(100000);

    jack_deactivate(client);
    jack_client_close(client);
    if (shm) munmap(shm, SHM_AUDIO_FILE_SIZE);
    if (fifo_fd >= 0) close(fifo_fd);
    return 0;
}
