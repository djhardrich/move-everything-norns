/*
 * jack-fifo-bridge — Captures JACK audio and writes raw S16LE stereo to a FIFO.
 *
 * Replaces PipeWire's broken pipe-tunnel module for JACK→FIFO audio bridging.
 * Connects to crone:output_1/2 and writes interleaved S16LE to the FIFO.
 *
 * Usage: jack-fifo-bridge <fifo_path>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <jack/jack.h>

static volatile int running = 1;
static jack_client_t *client = NULL;
static jack_port_t *port_l = NULL;
static jack_port_t *port_r = NULL;
static int fifo_fd = -1;

static void handle_signal(int sig) { (void)sig; running = 0; }

static int process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    if (fifo_fd < 0) return 0;

    float *in_l = (float *)jack_port_get_buffer(port_l, nframes);
    float *in_r = (float *)jack_port_get_buffer(port_r, nframes);
    if (!in_l || !in_r) return 0;

    /* Convert F32 → S16LE interleaved and write to FIFO.
     * PipeWire may use up to 8192 frames per callback (clock.quantum-limit).
     * Heap-allocate for large blocks to avoid stack overflow. */
    int16_t stack_buf[1024 * 2];
    int16_t *buf = stack_buf;
    jack_nframes_t n = nframes;
    if (n > 1024) {
        buf = (int16_t *)malloc(n * 2 * sizeof(int16_t));
        if (!buf) { buf = stack_buf; n = 1024; }
    }
    jack_nframes_t i;

    for (i = 0; i < n; i++) {
        float sl = in_l[i] * 32767.0f;
        float sr = in_r[i] * 32767.0f;
        if (sl > 32767.0f) sl = 32767.0f;
        if (sl < -32768.0f) sl = -32768.0f;
        if (sr > 32767.0f) sr = 32767.0f;
        if (sr < -32768.0f) sr = -32768.0f;
        buf[i * 2] = (int16_t)sl;
        buf[i * 2 + 1] = (int16_t)sr;
    }

    /* Non-blocking write — drop frames if FIFO full */
    write(fifo_fd, buf, n * 4);
    if (buf != stack_buf) free(buf);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *fifo_path = argc > 1 ? argv[1] : "/tmp/pw-to-move-1";

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        /* FIFO might not have a reader yet — open RDWR to avoid blocking */
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

    if (jack_activate(client)) {
        fprintf(stderr, "jack-fifo-bridge: can't activate\n");
        jack_client_close(client);
        return 1;
    }

    /* Connect to crone outputs */
    jack_connect(client, "crone:output_1", "move-bridge:in_L");
    jack_connect(client, "crone:output_2", "move-bridge:in_R");

    fprintf(stderr, "jack-fifo-bridge: running, writing to %s\n", fifo_path);

    while (running) {
        usleep(100000);
    }

    jack_deactivate(client);
    jack_client_close(client);
    close(fifo_fd);
    return 0;
}
