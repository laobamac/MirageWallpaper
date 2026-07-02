#pragma once

// VVK_CHECK macros. Stay as classic preprocessor macros — module purview
// can't host macros, and the macros use control-flow (`return`) that can't
// be wrapped in an inline function.
//
// Module impl units that need these macros must GMF-include this header
// AND `import rstd;` + `import sr.vulkan;` so rstd::log::* and
// vvk::ToString resolve at the call site.

#include <rstd/macro.hpp>

#define VVK_CHECK(f)         VVK_CHECK_ACT(, f)
#define VVK_CHECK_BOOL_RE(f) VVK_CHECK_ACT(return false, f)
#define VVK_CHECK_VOID_RE(f) VVK_CHECK_ACT(return, f)
#define VVK_CHECK_RE(f)      VVK_CHECK_ACT(return _res, f)
#define VVK_CHECK_ACT(act, f)                                      \
    {                                                              \
        VkResult _res = (f);                                       \
        if (_res != VK_SUCCESS && _res != VK_SUBOPTIMAL_KHR) {     \
            rstd_error("VkResult is \"{}\"", vvk::ToString(_res)); \
            rstd_assert(_res == VK_SUCCESS);                       \
            {                                                      \
                act;                                               \
            };                                                     \
        }                                                          \
    }
