#include "net.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

/*
 * Implementation of md_fill_unix_address.  Pathname addresses are
 * NUL-terminated inside sockaddr_un; abstract names occupy the first byte of
 * sun_path with a leading NUL and use a shorter address length.
 */

md_result_t md_fill_unix_address(const char* const path, sockaddr_un* const address,
                                 socklen_t* const address_length) {
    if (path == nullptr || address == nullptr || address_length == nullptr) {
        return MD_ERR_INVALID;
    }

    /* The C API documents path as NUL-terminated, so its byte extent is known here. */
    const std::string_view path_view(path);
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (!path_view.empty() && path_view.front() == '@') {
        if (path_view.size() <= 1U || path_view.size() >= sizeof(address->sun_path)) {
            return MD_ERR_INVALID;
        }
        std::copy_n(path_view.data() + 1U, path_view.size() - 1U, address->sun_path + 1U);
        *address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_view.size());
        return MD_OK;
    }

    if (path_view.empty() || path_view.size() >= sizeof(address->sun_path)) {
        return MD_ERR_INVALID;
    }
    std::copy_n(path_view.data(), path_view.size() + 1U, address->sun_path);
    *address_length = static_cast<socklen_t>(sizeof(*address));
    return MD_OK;
}
