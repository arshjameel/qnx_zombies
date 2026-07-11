#include "net.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>

#if defined(__QNXNTO__)
#  include <sys/time.h>    /* struct timeval */
#  include <sys/types.h>   /* close() on QNX */
#  include <unistd.h>      /* close() backup */
#else
#  include <unistd.h>      /* close() */
#  include <fcntl.h>       /* fcntl, O_NONBLOCK */
#endif

int net_udp_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

#if defined(__QNXNTO__)
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 1000;   /* 1 ms */
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sock);
        return -1;
    }
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(sock);
        return -1;
    }
#endif

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    return sock;
}

int net_bind(int sock, u16 port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    return 0;
}
