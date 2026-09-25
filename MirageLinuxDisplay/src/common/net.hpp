#ifndef MIRAGE_DISPLAY_COMMON_NET_HPP
#define MIRAGE_DISPLAY_COMMON_NET_HPP

#include "mirage_display.h"

#include <sys/socket.h>
#include <sys/un.h>

/*
 * AF_UNIX address encoding for pathname and @-prefixed abstract sockets.
 */

/*
 * Encodes an explicitly NUL-terminated filesystem path or an @-prefixed
 * abstract AF_UNIX name into caller-owned sockaddr_un storage.
 */
md_result_t md_fill_unix_address(const char* path, sockaddr_un* address,
                                 socklen_t* address_length);

#endif
