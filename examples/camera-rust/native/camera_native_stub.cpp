#include "camera_native.h"

#include <cstdlib>
#include <string>

struct camera_native {
    std::string error = "V831 media engine is only available on armv7 musl";
};
struct camera_tls_context {};
struct camera_tls_connection {};

extern "C" camera_native* camera_native_create(
    const char*, const camera_native_callbacks*)
{
    return new camera_native();
}

extern "C" int camera_native_start(camera_native*) { return -1; }
extern "C" void camera_native_stop(camera_native*) {}
extern "C" void camera_native_destroy(camera_native* camera) { delete camera; }
extern "C" void camera_native_force_iframe(camera_native*) {}
extern "C" void camera_native_webrtc_set_active(camera_native*, int) {}
extern "C" void camera_native_remote_video_set_active(camera_native*, int) {}
extern "C" int camera_native_config_set(
    camera_native*, const camera_native_runtime_config*) { return 0; }
extern "C" int camera_native_encode_jpeg(
    camera_native*, const uint8_t*, size_t, int, uint8_t**, size_t*)
{
    return -1;
}
extern "C" int camera_native_talk_open(camera_native*) { return -1; }
extern "C" void camera_native_talk_close(camera_native*) {}
extern "C" int camera_native_talk_feed(camera_native*, const uint8_t*, size_t)
{
    return -1;
}
extern "C" int camera_native_play_prompt_pcm(
    camera_native*, const int16_t*, size_t) { return -1; }
extern "C" int camera_native_mic_loudness(camera_native*) { return 0; }
extern "C" void camera_native_free(void* ptr) { std::free(ptr); }
extern "C" const char* camera_native_last_error(camera_native* camera)
{
    return camera ? camera->error.c_str() : "camera_native is null";
}
extern "C" camera_tls_context* camera_tls_context_create(const char*, const char*)
{
    return nullptr;
}
extern "C" void camera_tls_context_destroy(camera_tls_context*) {}
extern "C" camera_tls_connection* camera_tls_accept(camera_tls_context*, int)
{
    return nullptr;
}
extern "C" int camera_tls_read(camera_tls_connection*, void*, int, int*) { return -1; }
extern "C" int camera_tls_write(camera_tls_connection*, const void*, int, int*) { return -1; }
extern "C" void camera_tls_connection_destroy(camera_tls_connection*) {}
