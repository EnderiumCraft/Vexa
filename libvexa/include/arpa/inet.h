#ifndef LIBVEXA_ARPA_INET_H
#define LIBVEXA_ARPA_INET_H

#include <netinet/in.h>

in_addr_t inet_addr(const char *text);
int inet_aton(const char *text, struct in_addr *address);
char *inet_ntoa(struct in_addr address);
int inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *text, socklen_t size);

#endif
