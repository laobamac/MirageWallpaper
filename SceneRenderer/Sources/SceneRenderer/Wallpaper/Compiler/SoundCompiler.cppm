module;

export module sr.pkg.parse:wp_sound_parser;
import rstd.cppstd;
import wavsen.audio;
import sr.fs;
import sr.scene;
import sr.pkg.scene_obj;

export namespace sr
{

class SoundAssetCompiler {
public:
    static std::shared_ptr<SceneSoundControl> Parse(const wpscene::SoundObject&, fs::VFS&,
                                                    wavsen::audio::SoundManager&, Scene*);
};

} // namespace sr
