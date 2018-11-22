#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <malloc.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */ 
#include <netdb.h>
#include "timespec.h"

#define MSGLEN_MAX 1024

struct program_option
{
    char *      remote_name;
    uint16_t    remote_port;
    void *      send_data;
    size_t      send_size;
    int         packet_count;
    int         repeat_times;
    int         packet_delay_ms;
    int         repeat_delay_ms;
    int         timeout_ms;
    bool        ipv4;
    bool        ipv6;
} g_option =
{
    .packet_count = 1,
    .repeat_times = 1,
    .packet_delay_ms = 1000,
    .repeat_delay_ms = 1000,
    .timeout_ms   = 2000,
};

union sockaddr_inx
{
    struct sockaddr     saddr;
    struct sockaddr_in  sin;
    struct sockaddr_in6 sin6;
};

static void show_usage(int exitcode)
{
    FILE * f = exitcode ? stderr : stdout;
    fprintf(f, "no usage available\n");
    exit(exitcode);
}

static bool parse_int(const char * s, int * result_p)
{
    return sscanf(s, "%d", result_p) == 1;
}

static void parse_args(int argc, char * const argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "hm:c:r:t:d:D:46")) != -1) {
        switch (opt) {
            case 'm':
                g_option.send_size = strlen(optarg);
                if (g_option.send_size == 0) {
                    fprintf(stderr, "message length mustn't equal to 0\n");
                    exit(1);
                }
                free(g_option.send_data);
                g_option.send_data = (void*)strdup(optarg);
                break;
            case 'c':
                if (!parse_int(optarg, &g_option.packet_count)) {
                    fprintf(stderr, "packet count format error\n");
                    exit(1);
                }
                break;
            case 'r':
                if (!parse_int(optarg, &g_option.repeat_times)) {
                    fprintf(stderr, "repeat times format error\n");
                    exit(1);
                }
                break;
            case 't':
                if (!parse_int(optarg, &g_option.timeout_ms)) {
                    fprintf(stderr, "timeout format error\n");
                    exit(1);
                }
                break;
            case 'd':
                if (!parse_int(optarg, &g_option.packet_delay_ms)) {
                    fprintf(stderr, "delay in every test packet format error\n");
                    exit(1);
                }
                break;
            case 'D':
                if (!parse_int(optarg, &g_option.repeat_delay_ms)) {
                    fprintf(stderr, "delay in every connection format error\n");
                    exit(1);
                }
                break;
            case 'h':
                show_usage(0);
                break;
            case '4':
                g_option.ipv4 = true;
                break;
            case '6':
                g_option.ipv6 = true;
                break;
            default:
                show_usage(1);
        }
    }
    argc -= optind;
    argv += optind;
    if (argc == 2) {
        int port;
        g_option.remote_name = strdup(argv[0]);
        if (parse_int(argv[1], &port)) {
            if (0 < port && port <= 65535) {
                g_option.remote_port = (uint16_t)port;
            } else {
                fprintf(stderr, "remote port range should in [1, 65535]\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "remote port format error\n");
            exit(1);
        }
    } else {
        if (argc < 2) {
            fprintf(stderr, "require remote name and remote port\n");
        } else {
            fprintf(stderr, "too many argument\n");
        }
        exit(1);
    }
    if (!g_option.send_data) {
        g_option.send_data = (void*)strdup("hello world");
        g_option.send_size = strlen(g_option.send_data);
    }
    if (g_option.ipv4 && g_option.ipv6) {
        fprintf(stderr, "force-ipv4 and force-ipv6 can not be set at the same time\n");
        exit(1);
    }
}

static int sockaddr_inx_from_option(union sockaddr_inx * xaddr)
{
    int res;
    struct addrinfo hints;
    struct addrinfo *result, *rp;

    memset(&hints, 0, sizeof(struct addrinfo));
    if (g_option.ipv4) {
        hints.ai_family = AF_INET;
    } else if (g_option.ipv6) {
        hints.ai_family = AF_INET6;
    } else {
        hints.ai_family = AF_UNSPEC;
    }
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    res = getaddrinfo(g_option.remote_name, NULL, &hints, &result);
    if (res != 0) {
        fprintf(stderr, "getaddrinfo %s: %s\n", g_option.remote_name, gai_strerror(res));
        return res;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET || rp->ai_family == AF_INET6) {
            memcpy(xaddr, rp->ai_addr, rp->ai_addrlen);
            break;
        }
    }
    freeaddrinfo(result);
    if (!rp) {
        fprintf(stderr, "fff getaddrinfo %s: %s\n", g_option.remote_name, gai_strerror(EAI_FAMILY));
        return EAI_FAMILY;
    }
    if (xaddr->saddr.sa_family == AF_INET) {
        xaddr->sin.sin_port = htons(g_option.remote_port);
    } else {
        xaddr->sin6.sin6_port = htons(g_option.remote_port);
    }
    return 0;
}

static int prepare_socket(int counter)
{
    enum {
        RESOLV,
        CONNECT,
        ACTION_MAX,
    } action;
    const char * action_strtable[] =
    {
        "resolv",
        "connect",
    };

    int sock = -1, res;
    struct timespec start;
    long elapsed_list[ACTION_MAX];
    union sockaddr_inx xaddr;
    struct timeval timeout;
    fd_set wrfds;

    timeout = timeval_from_ms(g_option.timeout_ms);
    start = timespec_monotonic();
    res = sockaddr_inx_from_option(&xaddr);
    action = RESOLV;
    elapsed_list[action] = timeval_to_ms(timespec_sub_timespec(timespec_monotonic(), start));
    if (res) {
        goto leave;
    }
    sock = socket(xaddr.saddr.sa_family, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        goto leave;
    }
    res = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, res | O_NONBLOCK);
    connect(sock, &xaddr.saddr, xaddr.saddr.sa_family == AF_INET ? sizeof(xaddr.sin) : sizeof(xaddr.sin6));
    FD_ZERO(&wrfds);
    FD_SET(sock, &wrfds);
    res = select(sock + 1, NULL, &wrfds, NULL, g_option.timeout_ms > 0 ? &timeout : NULL);
    assert(res >= 0);
    action = CONNECT;
    elapsed_list[action] = timeval_to_ms(timespec_sub_timespec(timespec_monotonic(), start));
    if (res == 0) {
        fprintf(stderr, "connect: timeout\n");
        close(sock);
        sock = -1;
        goto leave;
    } else {
        socklen_t len = sizeof(res);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &res, &len);
    }
    if (res) {
        fprintf(stderr, "connect: %s\n", strerror(res));
        goto leave;
    }
leave:
    for (unsigned i = 0; i <= (unsigned)action; i++) {
        if (i) {
            printf(", ");
        }
        printf("%d %s %ldms", counter, action_strtable[i], elapsed_list[i]);
    }
    putchar('\n');
    return sock;
}

static void log_hex(FILE * f, const char * text, const void* data, size_t len)
{
    uint8_t * u = (uint8_t*)data;
    fprintf(f, "%s: ", text);
    while (len--) {
        fprintf(f, "%02x ", *u++);
    }
    fprintf(f, "\n");
}

ssize_t write_timeout(int fd, const void * data, size_t size, struct timeval * to)
{
    struct timespec stop;
    struct timeval to_calc;
    ssize_t res;
    size_t total = size;
    fd_set wrfds;
    if (to) {
        stop = timespec_add_timeval(timespec_monotonic(), *to);
        to = &to_calc;
    }
    while (size > 0) {
        res = write(fd, data, size);
        if (res < 0) {
            if (errno == EAGAIN) {
                if (to) {
                    to_calc = timespec_sub_timespec(stop, timespec_monotonic());
                }
                FD_ZERO(&wrfds);
                FD_SET(fd, &wrfds);
                res = select(fd + 1, NULL, &wrfds, NULL, to);
                assert(res >= 0);
                if (res == 0) {
                    errno = ETIME;
                    return -1;
                }
            } else {
                return res;
            }
        } else {
            data = (void*)((char*)data + res);
            size -= res;            
        }

    }
    return (ssize_t)total;
}

ssize_t read_timeout(int fd, void * data, size_t size, struct timeval * to)
{
    struct timespec stop;
    struct timeval to_calc;
    ssize_t res;
    size_t total = size;
    fd_set rdfds;
    if (to) {
        stop = timespec_add_timeval(timespec_monotonic(), *to);
        to = &to_calc;
    }
    while (size > 0) {
        res = read(fd, data, size);
        if (res < 0) {
            if (errno == EAGAIN) {
                if (to) {
                    to_calc = timespec_sub_timespec(stop, timespec_monotonic());
                }
                FD_ZERO(&rdfds);
                FD_SET(fd, &rdfds);
                res = select(fd + 1, &rdfds, NULL, NULL, to);
                assert(res >= 0);
                if (res == 0) {
                    errno = ETIME;
                    return -1;
                }
            } else {
                return res;
            }
        } else {
            data = (void*)((char*)data + res);
            size -= res;
        }

    }
    return (ssize_t)total;
}

void tcp_test_once(int counter)
{
    uint8_t recvbuf[MSGLEN_MAX];
    struct timeval timeout, *timeout_p;
    ssize_t res, wres, rres;
    
    int sock = prepare_socket(counter);
    if (sock < 0) {
        return;
    }
    if (g_option.timeout_ms > 0) {
        timeout = timeval_from_ms(g_option.timeout_ms);
        timeout_p = &timeout;
    } else {
        timeout_p = NULL;
    }
    for (int i = 0; i < g_option.packet_count || g_option.packet_count < 0; i++) {
        struct timespec start;
        ssize_t size;
        if (i) {
            usleep((useconds_t)g_option.packet_delay_ms * 1000);
        }
        start = timespec_monotonic();
        wres = write_timeout(sock, g_option.send_data, g_option.send_size, timeout_p);
        if (wres < 0) {
            perror("write data");
            break;
        }
        rres = read_timeout(sock, recvbuf, g_option.send_size, timeout_p);
        if (rres < 0) {
            perror("read data");
            break;
        }
        if (memcmp(recvbuf, g_option.send_data, g_option.send_size) != 0) {
            fprintf(stderr, "data not matched\n");
            log_hex(stderr, "TX", g_option.send_data, g_option.send_size);
            log_hex(stderr, "RX", recvbuf, g_option.send_size);
            break;
        }
        printf("  %d.%d echo test ok, rtt=%ldms\n", counter, i+1, timeval_to_ms(timespec_sub_timespec(timespec_monotonic(), start)));
    }

    close(sock);
}

int main(int argc, char *argv[])
{
    parse_args(argc, argv);
    bool first = true;
    for (int i = 0; i < g_option.repeat_times || g_option.repeat_times <= 0; i++)
    {
        if (i) {
            usleep((useconds_t)g_option.repeat_delay_ms * 1000);
        }
        tcp_test_once(i+1);
    }
    return 0;
}
