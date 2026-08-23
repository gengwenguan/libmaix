#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*camera_bytes_callback)(void* opaque, const uint8_t* data, size_t len);
typedef void (*camera_float_callback)(void* opaque, float value);
typedef void (*camera_video_callback)(void* opaque, const uint8_t* data,
                                      size_t len, int64_t pts_us,
                                      uint8_t is_key);

typedef struct camera_native_callbacks {
    void* opaque;
    camera_bytes_callback on_init_segment;
    camera_bytes_callback on_fragment;
    camera_video_callback on_audio_adts;
    camera_bytes_callback on_log_line;
    camera_bytes_callback on_nv21_frame;
    camera_float_callback on_person_detected;
    camera_video_callback on_h264_access_unit;
    camera_video_callback on_opus_frame;
} camera_native_callbacks;

typedef struct camera_native_runtime_config {
    uint8_t ai_enabled;
    float ai_threshold;
    int32_t ai_min_interval_s;
    int32_t ai_infer_fps;
    int32_t album_max_photos;
    int32_t photo_jpeg_qual;
    int32_t mic_filter_mode;
    uint8_t osd_show_ip;
    uint8_t osd_show_time;
    uint8_t osd_show_ai_box;
} camera_native_runtime_config;

typedef struct camera_native camera_native;
typedef struct camera_tls_context camera_tls_context;
typedef struct camera_tls_connection camera_tls_connection;

camera_native* camera_native_create(const char* runtime_dir,
                                    const camera_native_callbacks* callbacks);
int camera_native_start(camera_native* camera);
void camera_native_stop(camera_native* camera);
void camera_native_destroy(camera_native* camera);

void camera_native_force_iframe(camera_native* camera);
void camera_native_webrtc_set_active(camera_native* camera, int active);
void camera_native_remote_video_set_active(camera_native* camera, int active);
int camera_native_config_set(camera_native* camera,
                             const camera_native_runtime_config* config);
int camera_native_encode_jpeg(camera_native* camera,
                              const uint8_t* nv21, size_t len, int quality,
                              uint8_t** output, size_t* output_len);

int camera_native_talk_open(camera_native* camera);
void camera_native_talk_close(camera_native* camera);
int camera_native_talk_feed(camera_native* camera, const uint8_t* data, size_t len);
int camera_native_play_prompt_pcm(camera_native* camera,
                                  const int16_t* samples, size_t frames);
int camera_native_mic_loudness(camera_native* camera);

void camera_native_free(void* ptr);
const char* camera_native_last_error(camera_native* camera);

camera_tls_context* camera_tls_context_create(const char* cert_path,
                                              const char* key_path);
void camera_tls_context_destroy(camera_tls_context* context);
camera_tls_connection* camera_tls_accept(camera_tls_context* context, int fd);
int camera_tls_read(camera_tls_connection* connection, void* data, int len,
                    int* want_more);
int camera_tls_write(camera_tls_connection* connection, const void* data, int len,
                     int* want_more);
void camera_tls_connection_destroy(camera_tls_connection* connection);

#ifdef __cplusplus
}
#endif
