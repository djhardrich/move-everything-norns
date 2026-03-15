/*
 * jack-fifo-bridge — Bidirectional audio bridge between JACK and shared memory.
 *
 * Audio output (crone → Move):
 *   crone:output_1/2 → JACK → this bridge → SHM ring → DSP plugin render_block
 *
 * Audio input (Move → crone):
 *   DSP plugin render_block → SHM ring → this bridge → JACK → crone:input_1/2
 *   Only active when the DSP plugin writes to the input SHM (audio_in param).
 *
 * Primary path: lock-free SHM ring buffers (zero-copy, no syscalls).
 * Fallback: writes raw S16LE stereo to a FIFO if output SHM isn't set up.
 *
 * Usage: jack-fifo-bridge <fifo_path> [slot]
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

/* Audio output: JACK input ports (from crone) → SHM → DSP plugin */
static jack_port_t *port_in_l = NULL;
static jack_port_t *port_in_r = NULL;
static int fifo_fd = -1;
static shm_audio_t *shm_out = NULL;

/* Audio input: DSP plugin → SHM → JACK output ports (to crone) */
static jack_port_t *port_out_l = NULL;
static jack_port_t *port_out_r = NULL;
static shm_audio_t *shm_in = NULL;

/* Pre-allocated conversion buffers — zero allocations in audio callback */
#define MAX_BRIDGE_FRAMES 8192
static int16_t g_conv_buf[MAX_BRIDGE_FRAMES * 2];
static int16_t g_in_buf[MAX_BRIDGE_FRAMES * 2];

static void handle_signal(int sig) { (void)sig; running = 0; }

static int process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    jack_nframes_t n = nframes;
    if (n > MAX_BRIDGE_FRAMES) n = MAX_BRIDGE_FRAMES;

    /* ── Audio OUTPUT: crone → SHM/FIFO → Move ── */
    float *in_l = (float *)jack_port_get_buffer(port_in_l, nframes);
    float *in_r = (float *)jack_port_get_buffer(port_in_r, nframes);

    if (in_l && in_r) {
        int16_t *buf = g_conv_buf;
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

        if (shm_out) {
            shm_write(shm_out, buf, n);
        } else if (fifo_fd >= 0) {
            write(fifo_fd, buf, n * 4);
        }
    }

    /* ── Audio INPUT: Move → SHM → crone ── */
    float *out_l = (float *)jack_port_get_buffer(port_out_l, nframes);
    float *out_r = (float *)jack_port_get_buffer(port_out_r, nframes);

    if (out_l && out_r) {
        uint32_t got = 0;
        if (shm_in) {
            got = shm_read(shm_in, g_in_buf, n);
        }

        if (got > 0) {
            /* Convert S16LE interleaved → F32 separate channels */
            for (jack_nframes_t i = 0; i < got; i++) {
                out_l[i] = (float)g_in_buf[i * 2]     / 32768.0f;
                out_r[i] = (float)g_in_buf[i * 2 + 1] / 32768.0f;
            }
            /* Zero-fill remainder if SHM had fewer frames */
            for (jack_nframes_t i = got; i < n; i++) {
                out_l[i] = 0.0f;
                out_r[i] = 0.0f;
            }
        } else {
            /* No input data — output silence (no feedback risk) */
            memset(out_l, 0, n * sizeof(float));
            memset(out_r, 0, n * sizeof(float));
        }
    }

    return 0;
}

static shm_audio_t *open_shm_path(const char *path, const char *label) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "jack-fifo-bridge: %s SHM %s not found\n", label, path);
        return NULL;
    }

    void *p = mmap(NULL, SHM_AUDIO_FILE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "jack-fifo-bridge: %s SHM mmap failed\n", label);
        return NULL;
    }

    shm_audio_t *s = (shm_audio_t *)p;
    if (s->magic != SHM_AUDIO_MAGIC) {
        fprintf(stderr, "jack-fifo-bridge: %s SHM bad magic\n", label);
        munmap(p, SHM_AUDIO_FILE_SIZE);
        return NULL;
    }

    fprintf(stderr, "jack-fifo-bridge: %s SHM at %s (zero-copy)\n", label, path);
    return s;
}

int main(int argc, char *argv[]) {
    const char *fifo_path = argc > 1 ? argv[1] : "/tmp/pw-to-move-1";
    int slot = argc > 2 ? atoi(argv[2]) : 1;

    if (slot <= 0) {
        const char *p = strrchr(fifo_path, '-');
        if (p) slot = atoi(p + 1);
        if (slot <= 0) slot = 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Open SHM rings (created by DSP plugin) */
    char path[64];
    snprintf(path, sizeof(path), SHM_AUDIO_PATH_FMT, slot);
    shm_out = open_shm_path(path, "output");

    snprintf(path, sizeof(path), SHM_AUDIO_IN_PATH_FMT, slot);
    shm_in = open_shm_path(path, "input");

    /* Open FIFO as output fallback */
    if (!shm_out) {
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

    /* Audio output capture ports (from crone) */
    port_in_l = jack_port_register(client, "in_L",
                                   JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    port_in_r = jack_port_register(client, "in_R",
                                   JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

    /* Audio input playback ports (to crone) */
    port_out_l = jack_port_register(client, "out_L",
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    port_out_r = jack_port_register(client, "out_R",
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (!port_in_l || !port_in_r || !port_out_l || !port_out_r) {
        fprintf(stderr, "jack-fifo-bridge: can't register ports\n");
        jack_client_close(client);
        return 1;
    }

    jack_set_process_callback(client, process, NULL);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "jack-fifo-bridge: mlockall failed (not fatal)\n");

    if (jack_activate(client)) {
        fprintf(stderr, "jack-fifo-bridge: can't activate\n");
        jack_client_close(client);
        return 1;
    }

    /* Connect output path: crone → bridge */
    jack_connect(client, "crone:output_1", "move-bridge:in_L");
    jack_connect(client, "crone:output_2", "move-bridge:in_R");

    /* Connect input path: bridge → crone
     * Outputs silence when audio_in is disabled (SHM empty) — no feedback. */
    jack_connect(client, "move-bridge:out_L", "crone:input_1");
    jack_connect(client, "move-bridge:out_R", "crone:input_2");

    fprintf(stderr, "jack-fifo-bridge: running (out=%s, in=%s)\n",
            shm_out ? "SHM" : "FIFO",
            shm_in  ? "SHM" : "none");

    while (running)
        usleep(100000);

    jack_deactivate(client);
    jack_client_close(client);
    if (shm_out) munmap(shm_out, SHM_AUDIO_FILE_SIZE);
    if (shm_in)  munmap(shm_in,  SHM_AUDIO_FILE_SIZE);
    if (fifo_fd >= 0) close(fifo_fd);
    return 0;
}
