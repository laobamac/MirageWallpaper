module;

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

export module pipewire;

export {

using ::pw_main_loop;
using ::pw_thread_loop;
using ::pw_loop;
using ::pw_stream;
using ::pw_properties;
using ::pw_buffer;
using ::pw_time;
using ::pw_stream_events;
using ::pw_stream_state;
using ::pw_direction;

using ::spa_hook;
using ::spa_buffer;
using ::spa_data;
using ::spa_chunk;
using ::spa_pod;
using ::spa_pod_builder;
using ::spa_audio_info_raw;
using ::spa_audio_format;
using ::spa_audio_channel;

using ::pw_init;
using ::pw_deinit;

using ::pw_thread_loop_new;
using ::pw_thread_loop_destroy;
using ::pw_thread_loop_start;
using ::pw_thread_loop_stop;
using ::pw_thread_loop_lock;
using ::pw_thread_loop_unlock;
using ::pw_thread_loop_get_loop;

using ::pw_properties_new;
using ::pw_properties_set;
using ::pw_properties_setf;

using ::pw_stream_new_simple;
using ::pw_stream_destroy;
using ::pw_stream_connect;
using ::pw_stream_disconnect;
using ::pw_stream_set_active;
using ::pw_stream_get_time_n;
using ::pw_stream_dequeue_buffer;
using ::pw_stream_queue_buffer;
using ::pw_stream_state_as_string;

using ::PW_DIRECTION_OUTPUT;
using ::PW_DIRECTION_INPUT;

using ::SPA_AUDIO_FORMAT_F32_LE;

} // export
