module;

#include "Utils/StringUtil.h" // STRTONUM macro (uses __SHORT_FILE__)

export module sr.spec_texs;
import rstd.cppstd;
import sr.types;

#define BASE_GLTEX_NAMES(ext)                                                                      \
    "g_Texture0" #ext, "g_Texture1" #ext, "g_Texture2" #ext, "g_Texture3" #ext, "g_Texture4" #ext, \
        "g_Texture5" #ext, "g_Texture6" #ext, "g_Texture7" #ext, "g_Texture8" #ext,                \
        "g_Texture9" #ext, "g_Texture10" #ext, "g_Texture11" #ext, "g_Texture12" #ext

export namespace sr
{

inline constexpr std::array WE_GLTEX_NAMES { BASE_GLTEX_NAMES() };
inline constexpr std::array WE_GLTEX_RESOLUTION_NAMES { BASE_GLTEX_NAMES(Resolution) };
inline constexpr std::array WE_GLTEX_ROTATION_NAMES { BASE_GLTEX_NAMES(Rotation) };
inline constexpr std::array WE_GLTEX_TRANSLATION_NAMES { BASE_GLTEX_NAMES(Translation) };
inline constexpr std::array WE_GLTEX_MIPMAPINFO_NAMES { BASE_GLTEX_NAMES(MipMapInfo) };
inline constexpr std::array WE_GLTEX_TEXEL_NAMES { BASE_GLTEX_NAMES(Texel) };

inline constexpr std::string_view WE_SPEC_PREFIX { "_rt_" };
// --- Names that originate from Wallpaper Engine content
inline constexpr std::string_view WE_IMAGE_LAYER_COMPOSITE_PREFIX { "_rt_imageLayerComposite_" };
inline constexpr std::string_view WE_FULL_COMPO_BUFFER_PREFIX { "_rt_FullCompoBuffer" };
inline constexpr std::string_view WE_HALF_COMPO_BUFFER_PREFIX { "_rt_HalfCompoBuffer" };
inline constexpr std::string_view WE_QUARTER_COMPO_BUFFER_PREFIX { "_rt_QuarterCompoBuffer" };
inline constexpr std::string_view WE_EIGHT_COMPO_BUFFER_PREFIX { "_rt_EightBuffer" };
inline constexpr std::string_view WE_FULL_FRAME_BUFFER { "_rt_FullFrameBuffer" };
inline constexpr std::string_view WE_MIP_MAPPED_FRAME_BUFFER { "_rt_MipMappedFrameBuffer" };
// Other WE engine RTs seen in the assets.
inline constexpr std::string_view WE_SHADOW_ATLAS_PREFIX { "_rt_shadowAtlas" };
inline constexpr std::string_view WE_REFLECTION_PREFIX { "_rt_Reflection" };
inline constexpr std::string_view WE_VOLUMETRICS_PREFIX { "_rt_volumetrics" };
inline constexpr std::string_view WE_QUARTER_FORCE_RG_PREFIX { "_rt_QuarterForceRG" };
inline constexpr std::string_view WE_BLOOM_PREFIX { "_rt_Bloom" };
inline constexpr std::string_view WE_QUARTER_FRAME_BUFFER_PREFIX { "_rt_QuarterFrameBuffer" };
inline constexpr std::string_view WE_EIGHTH_FRAME_BUFFER_PREFIX { "_rt_EighthFrameBuffer" };

// --- Names coined by sr ---
inline constexpr std::string_view SR_EFFECT_PPONG_PREFIX { "_rt_effect_pingpong_" };
inline constexpr std::string_view SR_EFFECT_PPONG_PREFIX_A { "_rt_effect_pingpong_a_" };
inline constexpr std::string_view SR_EFFECT_PPONG_PREFIX_B { "_rt_effect_pingpong_b_" };
inline constexpr std::string_view SR_BLOOM_MIP_PREFIX { "_rt_bloom_mip" };
inline constexpr std::string_view SpecTex_Default { "_rt_default" };
inline constexpr std::string_view SpecTex_Link { "_rt_link_" };

inline constexpr std::string_view WE_IN_POSITION { "a_Position" };
inline constexpr std::string_view WE_IN_NORMAL { "a_Normal" };
inline constexpr std::string_view WE_IN_TEXCOORD { "a_TexCoord" };
inline constexpr std::string_view WE_IN_TANGENT4 { "a_Tangent4" };
inline constexpr std::string_view WE_IN_BLENDINDICES { "a_BlendIndices" };
inline constexpr std::string_view WE_IN_BLENDWEIGHTS { "a_BlendWeights" };
inline constexpr std::string_view WE_IN_CENTER { "a_Center" };
inline constexpr std::string_view WE_IN_COLOR4U { "a_Color4u" };
inline constexpr std::string_view WE_IN_POSITIONC1 { "a_PositionC1" };

// particle

inline constexpr std::string_view WE_IN_POSITIONVEC4 { "a_PositionVec4" };
inline constexpr std::string_view WE_IN_COLOR { "a_Color" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4 { "a_TexCoordVec4" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C1 { "a_TexCoordVec4C1" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C2 { "a_TexCoordVec4C2" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC4C3 { "a_TexCoordVec4C3" };
inline constexpr std::string_view WE_IN_TEXCOORDVEC3C2 { "a_TexCoordVec3C2" };
inline constexpr std::string_view WE_IN_TEXCOORDC2 { "a_TexCoordC2" };
inline constexpr std::string_view WE_IN_TEXCOORDC3 { "a_TexCoordC3" };
inline constexpr std::string_view WE_IN_TEXCOORDC4 { "a_TexCoordC4" };
inline constexpr std::string_view WE_CB_BLENDMODE { "BLENDMODE" };
inline constexpr std::string_view WE_CB_BONECOUNT { "BONECOUNT" };
inline constexpr std::string_view WE_CB_SPRITESHEET { "SPRITESHEET" };
inline constexpr std::string_view WE_CB_SPRITESHEETBLENDNPOT { "SPRITESHEETBLENDNPOT" };
inline constexpr std::string_view WE_CB_THICK_FORMAT { "THICKFORMAT" };
inline constexpr std::string_view WE_CB_TRAILRENDERER { "TRAILRENDERER" };
inline constexpr std::string_view WE_CB_GS_ENABLED { "GS_ENABLED" };
inline constexpr std::string_view WE_CB_LIGHTING { "LIGHTING" };
inline constexpr std::string_view WE_CB_REFLECTION { "REFLECTION" };
inline constexpr std::string_view WE_CB_NORMALMAP { "NORMALMAP" };
inline constexpr std::string_view WE_CB_MORPHING { "MORPHING" };
inline constexpr std::string_view WE_CB_SKINNING { "SKINNING" };
inline constexpr std::string_view WE_CB_POINTEMITTER { "POINTEMITTER" };
inline constexpr std::string_view WE_CB_LINEEMITTER { "LINEEMITTER" };
inline constexpr std::string_view WE_PRENDER_ROPE { "PRENDER_ROPE" };

// Compile-time (name, type) pair for declarative attribute layouts.
struct VertexAttrSpec {
    std::string_view name;
    VertexType       type;
};

namespace VAttr
{
inline constexpr VertexAttrSpec Position { WE_IN_POSITION, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Normal { WE_IN_NORMAL, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec PositionVec4 { WE_IN_POSITIONVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoord { WE_IN_TEXCOORD, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec Tangent4 { WE_IN_TANGENT4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4 { WE_IN_TEXCOORDVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C1 { WE_IN_TEXCOORDVEC4C1, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C2 { WE_IN_TEXCOORDVEC4C2, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C3 { WE_IN_TEXCOORDVEC4C3, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec3C2 { WE_IN_TEXCOORDVEC3C2, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordC2 { WE_IN_TEXCOORDC2, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec TexCoordC3 { WE_IN_TEXCOORDC3, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordC4 { WE_IN_TEXCOORDC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec Color { WE_IN_COLOR, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec BlendIndices { WE_IN_BLENDINDICES, VertexType::UINT4 };
inline constexpr VertexAttrSpec BlendWeights { WE_IN_BLENDWEIGHTS, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec Center { WE_IN_CENTER, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Color4u { WE_IN_COLOR4U, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec PositionC1 { WE_IN_POSITIONC1, VertexType::FLOAT3 };
} // namespace VAttr

inline constexpr std::string_view G_M { "g_ModelMatrix" };
inline constexpr std::string_view G_VP { "g_ViewProjectionMatrix" };
inline constexpr std::string_view G_MVP { "g_ModelViewProjectionMatrix" };
inline constexpr std::string_view G_AM { "g_AltModelMatrix" };
inline constexpr std::string_view G_ALTVIEWPROJECTIONMATRIX { "g_AltViewProjectionMatrix" };
inline constexpr std::string_view G_MI { "g_ModelMatrixInverse" };
inline constexpr std::string_view G_MVPI { "g_ModelViewProjectionMatrixInverse" };
inline constexpr std::string_view G_EYEPOSITION { "g_EyePosition" };
inline constexpr std::string_view G_EMVP { "g_EffectModelViewProjectionMatrix" };
inline constexpr std::string_view G_ETVP { "g_EffectTextureProjectionMatrix" };
inline constexpr std::string_view G_ETVPI { "g_EffectTextureProjectionMatrixInverse" };
inline constexpr std::string_view G_LP { "g_LightsPosition" };
inline constexpr std::string_view G_LCP { "g_LightsColorPremultiplied" };
inline constexpr std::string_view G_LCR { "g_LightsColorRadius" };
inline constexpr std::string_view G_LIGHTAMBIENTCOLOR { "g_LightAmbientColor" };
inline constexpr std::string_view G_LIGHTSKYLIGHTCOLOR { "g_LightSkylightColor" };

inline constexpr std::string_view G_TIME { "g_Time" };
inline constexpr std::string_view G_FRAMETIME { "g_Frametime" };
inline constexpr std::string_view G_DAYTIME { "g_DayTime" };
inline constexpr std::string_view G_POINTERPOSITION { "g_PointerPosition" };
inline constexpr std::string_view G_POINTERPOSITIONLAST { "g_PointerPositionLast" };
inline constexpr std::string_view G_TEXELSIZE { "g_TexelSize" };
inline constexpr std::string_view G_TEXELSIZEHALF { "g_TexelSizeHalf" };
inline constexpr std::string_view G_TEXTURE0SAMPLERSTATE { "g_Texture0SamplerState" };
inline constexpr std::string_view G_BONES { "g_Bones" };
inline constexpr std::string_view G_BONESALPHA { "g_BonesAlpha" };
inline constexpr std::string_view G_SCREEN { "g_Screen" };
inline constexpr std::string_view G_PARALLAXPOSITION { "g_ParallaxPosition" };
inline constexpr std::string_view G_MORPHWEIGHTS { "g_MorphWeights" };
inline constexpr std::string_view G_MORPHOFFSETS { "g_MorphOffsets" };
inline constexpr std::string_view G_VIEWPORTVIEWPROJECTIONMATRICES {
    "g_ViewportViewProjectionMatrices"
};
inline constexpr std::string_view G_VIEWUP { "g_ViewUp" };
inline constexpr std::string_view G_VIEWRIGHT { "g_ViewRight" };
inline constexpr std::string_view G_VIEWFORWARD { "g_ViewForward" };
inline constexpr std::string_view G_ORIENTATIONUP { "g_OrientationUp" };
inline constexpr std::string_view G_ORIENTATIONRIGHT { "g_OrientationRight" };
inline constexpr std::string_view G_ORIENTATIONFORWARD { "g_OrientationForward" };
inline constexpr std::string_view G_NORMALMODELMATRIX { "g_NormalModelMatrix" };
inline constexpr std::string_view G_COLOR4 { "g_Color4" };
inline constexpr std::string_view G_COLOR { "g_Color" };
inline constexpr std::string_view G_ALPHA { "g_Alpha" };
inline constexpr std::string_view G_USERALPHA { "g_UserAlpha" };
inline constexpr std::string_view G_BRIGHTNESS { "g_Brightness" };
inline constexpr std::string_view G_RENDERVAR0 { "g_RenderVar0" };
inline constexpr std::string_view G_RENDERVAR1 { "g_RenderVar1" };
inline constexpr std::string_view G_RENDERVAR2 { "g_RenderVar2" };
inline constexpr std::string_view G_AUDIOFREQUENCYMIN { "g_AudioFrequencyMin" };
inline constexpr std::string_view G_AUDIOFREQUENCYMAX { "g_AudioFrequencyMax" };

// WE audio-bar shaders read one of three (Left, Right) array pairs depending
// on the chosen Frequency Resolution combo. sr sources from wavsen's 64-bin
// log-spaced spectrum; we downsample to 16/32 by averaging neighboring bins.
inline constexpr std::string_view G_AUDIO_SPEC_16_L { "g_AudioSpectrum16Left" };
inline constexpr std::string_view G_AUDIO_SPEC_16_R { "g_AudioSpectrum16Right" };
inline constexpr std::string_view G_AUDIO_SPEC_32_L { "g_AudioSpectrum32Left" };
inline constexpr std::string_view G_AUDIO_SPEC_32_R { "g_AudioSpectrum32Right" };
inline constexpr std::string_view G_AUDIO_SPEC_64_L { "g_AudioSpectrum64Left" };
inline constexpr std::string_view G_AUDIO_SPEC_64_R { "g_AudioSpectrum64Right" };

inline bool IsSpecTex(const std::string_view name) { return name.starts_with(WE_SPEC_PREFIX); }
inline bool IsSpecLinkTex(const std::string_view name) { return name.starts_with(SpecTex_Link); }
inline std::uint32_t ParseLinkTex(const std::string_view name) {
    std::string sid { name };
    sid = sid.substr(9);
    std::uint32_t result { 0 };
    STRTONUM(sid, result);
    return result;
}
inline std::string GenLinkTex(std::ptrdiff_t id) {
    return std::string(SpecTex_Link) + std::to_string(id);
}

inline bool IsImageLayerComposite(const std::string_view name) {
    return name.starts_with(WE_IMAGE_LAYER_COMPOSITE_PREFIX);
}
// Parse <id> from `_rt_imageLayerComposite_<id>[_a|_b]`; nullopt when it isn't a
// composite ref or no id digits follow the prefix.
inline std::optional<std::uint32_t> ParseImageLayerCompositeId(const std::string_view name) {
    if (! IsImageLayerComposite(name)) return std::nullopt;
    const std::string_view rest = name.substr(WE_IMAGE_LAYER_COMPOSITE_PREFIX.size());
    std::size_t            i    = 0;
    std::uint32_t          id   = 0;
    for (; i < rest.size() && rest[i] >= '0' && rest[i] <= '9'; ++i)
        id = id * 10u + std::uint32_t(rest[i] - '0');
    if (i == 0) return std::nullopt;
    return id;
}

} // namespace sr

#undef BASE_GLTEX_NAMES
