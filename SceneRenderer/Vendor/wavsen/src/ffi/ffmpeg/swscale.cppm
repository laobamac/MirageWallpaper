module;

extern "C" {
#include <libswscale/swscale.h>
}

export module swscale;

#if LIBSWSCALE_VERSION_MAJOR < 9
constexpr auto M_SWS_FAST_BILINEAR = SWS_FAST_BILINEAR;
constexpr auto M_SWS_BILINEAR = SWS_BILINEAR;
constexpr auto M_SWS_BICUBIC = SWS_BICUBIC;
constexpr auto M_SWS_LANCZOS = SWS_LANCZOS;

#undef SWS_FAST_BILINEAR
#undef SWS_BILINEAR
#undef SWS_BICUBIC
#undef SWS_LANCZOS

#endif

export {

  using ::SwsContext;

  using ::sws_freeContext;
  using ::sws_getContext;
  using ::sws_scale;

#if LIBSWSCALE_VERSION_MAJOR < 9
  constexpr auto SWS_FAST_BILINEAR = M_SWS_FAST_BILINEAR;
  constexpr auto SWS_BILINEAR = M_SWS_BILINEAR;
  constexpr auto SWS_BICUBIC = M_SWS_BICUBIC;
  constexpr auto SWS_LANCZOS = M_SWS_LANCZOS;
#else
  using ::SWS_BICUBIC;
  using ::SWS_BILINEAR;
  using ::SWS_FAST_BILINEAR;
  using ::SWS_LANCZOS;
#endif
}
