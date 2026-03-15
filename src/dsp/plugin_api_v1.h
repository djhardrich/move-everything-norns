#ifndef MOVE_PLUGIN_API_V1_H
#define MOVE_PLUGIN_API_V1_H

#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION   1
#define MOVE_PLUGIN_API_VERSION_2 2

#define MOVE_AUDIO_SAMPLE_RATE 44100
#define MOVE_AUDIO_BLOCK_FRAMES 128

#define MIDI_SOURCE_INTERNAL 0
#define MIDI_SOURCE_EXTERNAL 1
#define MIDI_SOURCE_HOST     2

typedef struct host_api_v1 {
    uint32_t api_version;
    uint32_t sample_rate;
    uint32_t block_frames;
    void *mmap_base;
    int16_t *audio_in;
    void (*log)(const char *fmt, ...);
    void (*send_midi_internal)(const uint8_t *msg, int len);
    void (*send_midi_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void  (*destroy_instance)(void *instance);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int   (*get_error)(void *instance, char *buf, int buf_len);
    void  (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);

#endif
