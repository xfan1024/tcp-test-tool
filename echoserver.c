#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BINDADDR    "127.0.0.1"
#define BINDPORT    9000
#define MAX_EVENTS  16

static void check_capi_result(long res, const char *fmt, ...)
{
    if (res < 0) {
        va_list arg;
        va_start(arg, fmt);
        vfprintf(stderr, fmt, arg);
        va_end(arg);
        fprintf(stderr, ": %s\n", strerror(errno));
        exit(1);
    }
}

static void setnonblock(int fd)
{
    int res;
    res = fcntl(fd, F_GETFL, 0);
    check_capi_result(res, "fcntl() F_GETFL");
    res = fcntl(fd, F_SETFL, res | O_NONBLOCK);
    check_capi_result(res, "fnctl() F_SETFL");
}

int main()
{
    int serverfd, clientfd, epollfd, nfds, res, i;
    struct sockaddr_in saddr, caddr;
    struct epoll_event ev, events[MAX_EVENTS];
    socklen_t socklen;
    const char * address;
    uint16_t port;
    uint8_t databuf[1024];
    ssize_t size;

    memset(&saddr,  0, sizeof(saddr));
    memset(&ev,     0, sizeof(ev));
    memset(events,  0, sizeof(events));

    saddr.sin_addr.s_addr = inet_addr(BINDADDR);
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(BINDPORT);
    
    serverfd = socket(AF_INET, SOCK_STREAM, 0);
    setnonblock(serverfd);
    check_capi_result(serverfd, "socket");
    res = bind(serverfd, (struct sockaddr *)&saddr, sizeof(saddr));
    check_capi_result(res, "bind");
    res = listen(serverfd, 10);
    check_capi_result(res, "listen");

    epollfd = epoll_create(1024);
    check_capi_result(epollfd, "epoll_create");

    ev.events = EPOLLIN;
    ev.data.fd = serverfd;
    res = epoll_ctl(epollfd, EPOLL_CTL_ADD, serverfd, &ev);
    check_capi_result(epollfd, "epollctl add serverfd");

    while (1) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        check_capi_result(nfds, "epoll_wait");
        // printf("epoll_wait return %d\n", nfds);
        for (i = 0; i < nfds; i++) {
            if (events[i].data.fd == serverfd) {
                socklen = sizeof(caddr);
                clientfd = accept(serverfd, (struct sockaddr *)&caddr, &socklen);
                check_capi_result(clientfd, "accept");
                address = inet_ntoa(caddr.sin_addr);
                port = ntohs(caddr.sin_port);
                printf("accept %s:%d\n", address, (int)port);
                ev.data.fd = clientfd;
                ev.events = EPOLLIN | EPOLLRDHUP;
                res = epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &ev);
                check_capi_result(epollfd, "epollctl add clientfd");
            } else {
                socklen = sizeof(caddr);
                res = getpeername(events[i].data.fd, (struct sockaddr *)&caddr, &socklen);
                check_capi_result(res, "getpeername");
                address = inet_ntoa(caddr.sin_addr);
                port = ntohs(caddr.sin_port);
                if (events[i].events & EPOLLRDHUP) {
                    printf("%s:%d connection disconnect\n", address, port);
                    close(events[i].data.fd);
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    size = read(events[i].data.fd, databuf, sizeof(databuf));
                    check_capi_result((long)size, "read from client");
                    printf("%s:%d data comming, size=%ld\n", address, (int)port, (long)size);
                    size = write(events[i].data.fd, databuf, size);
                    check_capi_result((long)size, "write to client");
                    printf("%s:%d data writing, size=%ld\n", address, (int)port, (long)size);
                }
            }
        }
    }   

    
    return 0;
}
