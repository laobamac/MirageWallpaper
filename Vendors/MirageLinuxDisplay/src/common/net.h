#ifndef MIRAGE_DISPLAY_COMMON_NET_H
#define MIRAGE_DISPLAY_COMMON_NET_H

#include <sys/socket.h>

/* Fills a sockaddr_un for a filesystem path or an '@'-prefixed abstract
 * socket name. Returns MD_OK or MD_ERR_INVALID. */
int md_fill_unix_address(const char* path, struct sockaddr_un* address,
                         socklen_t* address_length);

#endif
