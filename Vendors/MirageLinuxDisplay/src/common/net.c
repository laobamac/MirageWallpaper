#define _GNU_SOURCE

#include "net.h"

#include "mirage_display.h"

#include <stddef.h>
#include <string.h>
#include <sys/un.h>

int md_fill_unix_address(const char* path, struct sockaddr_un* address,
                         socklen_t* address_length) {
    if (path == NULL || address == NULL || address_length == NULL) return MD_ERR_INVALID;
    size_t length = strlen(path);
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path[0] == '@') {
        if (length <= 1u || length >= sizeof(address->sun_path)) return MD_ERR_INVALID;
        memcpy(address->sun_path + 1, path + 1, length - 1u);
        *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length);
    } else {
        if (length == 0 || length >= sizeof(address->sun_path)) return MD_ERR_INVALID;
        memcpy(address->sun_path, path, length + 1u);
        *address_length = (socklen_t)sizeof(*address);
    }
    return MD_OK;
}
