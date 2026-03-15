/*
 * shm_audio.h — Lock-free shared memory ring buffer for audio
 *
 * Used between jack-fifo-bridge (producer, in chroot) and the DSP plugin
 * (consumer, on host).  Both sides mmap the same file on /tmp (which is
 * bind-mounted into the chroot).
 *
 * Zero-copy: the producer writes interleaved S16LE stereo samples directly
 * into the ring; the consumer reads them straight out.  No syscalls, no
 * kernel pipe buffers, no intermediate copies in the audio hot path.
 *
 * The ring is sized for ~93 ms at 44100 Hz (4096 stereo frames).
 * write_pos / read_pos are frame indices (not byte offsets).
 */

#ifndef SHM_AUDIO_H
#define SHM_AUDIO_H

#include <stdint.h>
#include <string.h>

#define SHM_AUDIO_MAGIC     0x4E524E53   /* "NRNS" */
#define SHM_AUDIO_VERSION   1
#define SHM_AUDIO_CHANNELS  2
#define SHM_RING_FRAMES     16384        /* stereo frames in ring (~371ms at 44100) */
#define SHM_RING_MASK       (SHM_RING_FRAMES - 1)  /* power-of-2 wrap */

/* Laid out at the start of the mmap'd file. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t ring_frames;       /* always SHM_RING_FRAMES */
    volatile uint32_t write_pos; /* frame index, wraps via mask — producer */
    volatile uint32_t read_pos;  /* frame index, wraps via mask — consumer */
    uint32_t _pad[9];           /* pad header to 64 bytes (cache line) */
    int16_t  data[];            /* ring_frames * channels samples */
} shm_audio_t;

#define SHM_AUDIO_DATA_BYTES  (SHM_RING_FRAMES * SHM_AUDIO_CHANNELS * sizeof(int16_t))
#define SHM_AUDIO_FILE_SIZE   (sizeof(shm_audio_t) + SHM_AUDIO_DATA_BYTES)

/* Path pattern — /dev/shm is a real tmpfs (RAM), not the SD card.
 * Move's own audio SHM lives here too (move-shadow-pub-audio). */
#define SHM_AUDIO_PATH_FMT    "/dev/shm/norns-audio-%d"

/* --- helpers (header-only, used by both sides) --- */

static inline uint32_t shm_frames_available(const shm_audio_t *shm) {
    uint32_t w = __atomic_load_n(&shm->write_pos, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&shm->read_pos,  __ATOMIC_ACQUIRE);
    return (w - r) & (SHM_RING_FRAMES * 2 - 1);  /* handles wrap */
}

static inline uint32_t shm_frames_free(const shm_audio_t *shm) {
    return SHM_RING_FRAMES - shm_frames_available(shm);
}

/* Producer: write n stereo frames.  Returns frames actually written. */
static inline uint32_t shm_write(shm_audio_t *shm,
                                  const int16_t *src, uint32_t n) {
    uint32_t free = shm_frames_free(shm);
    if (n > free) n = free;
    if (n == 0) return 0;

    uint32_t pos = __atomic_load_n(&shm->write_pos, __ATOMIC_RELAXED)
                   & SHM_RING_MASK;
    uint32_t first = SHM_RING_FRAMES - pos;
    if (first > n) first = n;

    memcpy(&shm->data[pos * SHM_AUDIO_CHANNELS], src,
           first * SHM_AUDIO_CHANNELS * sizeof(int16_t));
    if (n > first) {
        memcpy(&shm->data[0], src + first * SHM_AUDIO_CHANNELS,
               (n - first) * SHM_AUDIO_CHANNELS * sizeof(int16_t));
    }

    __atomic_add_fetch(&shm->write_pos, n, __ATOMIC_RELEASE);
    return n;
}

/* Consumer: read n stereo frames.  Returns frames actually read. */
static inline uint32_t shm_read(shm_audio_t *shm,
                                 int16_t *dst, uint32_t n) {
    uint32_t avail = shm_frames_available(shm);
    if (n > avail) n = avail;
    if (n == 0) return 0;

    uint32_t pos = __atomic_load_n(&shm->read_pos, __ATOMIC_RELAXED)
                   & SHM_RING_MASK;
    uint32_t first = SHM_RING_FRAMES - pos;
    if (first > n) first = n;

    memcpy(dst, &shm->data[pos * SHM_AUDIO_CHANNELS],
           first * SHM_AUDIO_CHANNELS * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first * SHM_AUDIO_CHANNELS, &shm->data[0],
               (n - first) * SHM_AUDIO_CHANNELS * sizeof(int16_t));
    }

    __atomic_add_fetch(&shm->read_pos, n, __ATOMIC_RELEASE);
    return n;
}

/* Initialize a freshly mmap'd region as producer. */
static inline void shm_audio_init(shm_audio_t *shm, uint32_t sample_rate) {
    memset(shm, 0, sizeof(shm_audio_t));
    shm->magic       = SHM_AUDIO_MAGIC;
    shm->version     = SHM_AUDIO_VERSION;
    shm->sample_rate = sample_rate;
    shm->channels    = SHM_AUDIO_CHANNELS;
    shm->ring_frames = SHM_RING_FRAMES;
    memset(shm->data, 0, SHM_AUDIO_DATA_BYTES);
    /* Pre-fill with ~50ms of silence so consumer never starves at startup */
    __atomic_store_n(&shm->write_pos, 2205, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->read_pos,  0,    __ATOMIC_RELEASE);
}

#endif /* SHM_AUDIO_H */
