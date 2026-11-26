/* Sockets for netplay.c and online.c: Winsock on Windows, and the BSD sockets elsewhere under Winsock's
 * names. */

#ifndef PORT_NET_COMPAT_H
#define PORT_NET_COMPAT_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define ioctlsocket ioctl
typedef struct WSAData {
    int unused;
} WSADATA;
#define MAKEWORD(low, high) ((low) | (high) << 8)
#define WSAStartup(version, data) ((void)(version), (void)(data), 0)
#endif

#endif
