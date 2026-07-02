#pragma once

// __SHORT_FILE__ — compile-time-trimmed __FILE__ that retains only the
// segment after the last slash. Kept as a header (no module attachment)
// because String.h's STRTONUM macro relies on it. Logging itself migrated
// to rstd's rstd_info / rstd_error / etc. — `#include <rstd/macro.hpp>`
// for those.
//
// Pulls <string>/<string_view> into the GMF so consumers' std::less<>
// instantiations on std::string keys still resolve `operator<` cleanly
// even with the modular cppstd in scope.
#include <string>
#include <string_view>

#define __SHORT_FILE__ past_last_slash(__FILE__)

constexpr const char* past_last_slash(const char* const path, const int pos = 0,
                                      const int last_slash = 0) {
    if (path[pos] == '\0') return &path[last_slash];
    if (path[pos] == '/')
        return past_last_slash(path, pos + 1, pos + 1);
    else
        return past_last_slash(path, pos + 1, last_slash);
}
