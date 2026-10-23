#include "net_bringup.h"

#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

int net_listen_tcp(uint16_t port, int backlog)
{
    int sock = -1;
#if CONFIG_LWIP_IPV6
    sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
    if (sock >= 0) {
        int v6only = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in6 dest = {0};
        dest.sin6_family = AF_INET6;
        dest.sin6_port = htons(port);
        if (bind(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0 && listen(sock, backlog) == 0) {
            return sock;
        }
        close(sock);
        sock = -1;
    }
#endif
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0 || listen(sock, backlog) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

bool net_send_all(int sock, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    size_t off = 0;
    while (off < len) {
        int n = send(sock, bytes + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

void net_tune_accepted(int sock, int timeout_ms)
{
    if (timeout_ms < 1000) {
        timeout_ms = 1000;
    }
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    int idle = 5;
    int intvl = 3;
    int cnt = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}
