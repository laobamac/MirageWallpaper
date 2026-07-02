// Primary module interface. Re-exports the partitions so consumers
// just `import wavsen.video;` and get Producer / VideoDecoder /
// YuvToRgba / Presenter without naming each piece individually.

export module wavsen.video;

export import :vk_device;
export import :video_decoder;
export import :yuv_to_rgba;
export import :presenter;
