module;

#include <pulse/pulseaudio.h>

export module pulse;

export {

using ::pa_threaded_mainloop;
using ::pa_mainloop_api;
using ::pa_context;
using ::pa_stream;
using ::pa_sample_spec;
using ::pa_sample_format_t;
using ::pa_channel_map;
using ::pa_buffer_attr;
using ::pa_server_info;
using ::pa_timing_info;
using ::pa_context_state_t;
using ::pa_stream_state_t;
using ::pa_stream_flags_t;
using ::pa_context_flags_t;
using ::pa_seek_mode_t;
using ::pa_usec_t;

using ::pa_threaded_mainloop_new;
using ::pa_threaded_mainloop_free;
using ::pa_threaded_mainloop_start;
using ::pa_threaded_mainloop_stop;
using ::pa_threaded_mainloop_lock;
using ::pa_threaded_mainloop_unlock;
using ::pa_threaded_mainloop_wait;
using ::pa_threaded_mainloop_signal;
using ::pa_threaded_mainloop_get_api;
using ::pa_threaded_mainloop_in_thread;

using ::pa_context_new;
using ::pa_context_connect;
using ::pa_context_disconnect;
using ::pa_context_unref;
using ::pa_context_set_state_callback;
using ::pa_context_get_state;
using ::pa_context_get_server_info;
using ::pa_context_errno;

using ::pa_stream_new;
using ::pa_stream_unref;
using ::pa_stream_connect_playback;
using ::pa_stream_connect_record;
using ::pa_stream_disconnect;
using ::pa_stream_set_state_callback;
using ::pa_stream_set_write_callback;
using ::pa_stream_set_read_callback;
using ::pa_stream_get_state;
using ::pa_stream_writable_size;
using ::pa_stream_readable_size;
using ::pa_stream_begin_write;
using ::pa_stream_cancel_write;
using ::pa_stream_write;
using ::pa_stream_peek;
using ::pa_stream_drop;
using ::pa_stream_get_time;
using ::pa_stream_get_latency;
using ::pa_stream_update_timing_info;
using ::pa_stream_get_timing_info;
using ::pa_stream_cork;
using ::pa_stream_set_buffer_attr;

using ::pa_channel_map_init_stereo;

using ::pa_strerror;

} // export
