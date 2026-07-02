export module wavsen.audio;

// Primary module interface. Re-exports the partitions so consumers
// just `import wavsen.audio;` and get everything.
export import :byte_stream;  // IByteStream
export import :core;         // DeviceDesc, IPullChannel, AudioDevice
export import :mixer;        // SoundStream, SoundManager
export import :file;         // StreamDecoder, make_stream
export import :av_sync;      // AvPlayer, AvPlayerError
export import :capture;      // AudioSpectrum, AudioCapture
