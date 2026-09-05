#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include "event_loop.h"
#include "client.h"

#define MAX_EVENTS 64

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
}

static void remove_client(int epoll_fd, int client_fd)
{
    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_DEL,
                  client_fd,
                  NULL) < 0) {

        if (errno != ENOENT) {
            perror("epoll_ctl(DEL client)");
        }
    }

    client_remove(client_fd);
}

static int accept_clients(int epoll_fd, int server_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        struct epoll_event event;

        int client_fd;

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);

        if (client_fd < 0) {

            /*
             * All pending clients have been accepted.
             */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            /*
             * accept() was interrupted by a signal.
             * Retry it.
             */
            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            return -1;
        }

        /*
         * Every accepted client socket must also
         * operate in non-blocking mode.
         */
        if (set_nonblocking(client_fd) < 0) {
            perror("fcntl(client_fd)");
            close(client_fd);
            continue;
        }

        /*
         * Create client state.
         */
        if (client_add(client_fd) < 0) {
            perror("client_add");
            close(client_fd);
            continue;
        }

        memset(&event, 0, sizeof(event));

        event.events = EPOLLIN;
        event.data.fd = client_fd;

        /*
         * Register the accepted client with epoll.
         */
        if (epoll_ctl(epoll_fd,
                      EPOLL_CTL_ADD,
                      client_fd,
                      &event) < 0) {

            perror("epoll_ctl(ADD client)");
            client_remove(client_fd);
            continue;
        }

        printf("Client connected. fd = %d\n", client_fd);
    }
}

int event_loop_run(int server_fd)
{
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    int epoll_fd;

    /*
     * Create the epoll instance.
     */
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /*
     * Register the listening socket with epoll.
     */
    memset(&event, 0, sizeof(event));

    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_ADD,
                  server_fd,
                  &event) < 0) {

        perror("epoll_ctl(ADD server)");
        close(epoll_fd);
        return -1;
    }

    printf("Epoll event loop started\n");

    for (;;) {
        int ready;
        int i;

        /*
         * Sleep here until one or more registered
         * file descriptors have an event.
         */
        ready = epoll_wait(epoll_fd,
                           events,
                           MAX_EVENTS,
                           -1);

        if (ready < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            close(epoll_fd);
            return -1;
        }

        for (i = 0; i < ready; i++) {
            int fd = events[i].data.fd;
            uint32_t event_flags = events[i].events;

            /*
             * Event belongs to the listening socket.
             */
            if (fd == server_fd) {

                if (event_flags & (EPOLLERR | EPOLLHUP)) {
                    fprintf(stderr,
                            "Listening socket error or hangup\n");

                    close(epoll_fd);
                    return -1;
                }

                if (event_flags & EPOLLIN) {

                    /*
                     * Accept every connection currently
                     * waiting in the accept queue.
                     */
                    if (accept_clients(epoll_fd,
                                       server_fd) < 0) {

                        /*
                         * Keep the server alive even if
                         * one accept cycle fails.
                         */
                        continue;
                    }
                }

                continue;
            }

            /*
             * From this point onward, the event belongs
             * to an accepted client socket.
             */

            if (event_flags & (EPOLLERR | EPOLLHUP)) {

                printf("Client error/hangup. fd = %d\n", fd);

                remove_client(epoll_fd, fd);
                continue;
            }

            if (event_flags & EPOLLIN) {
                struct client *client;
                int result;

                client = client_find(fd);

                if (client == NULL) {
                    fprintf(stderr,
                            "Unknown client fd = %d\n",
                            fd);

                    epoll_ctl(epoll_fd,
                              EPOLL_CTL_DEL,
                              fd,
                              NULL);

                    close(fd);
                    continue;
                }

                result = client_handle_read(client);

                if (result > 0) {

                    /*
                     * recv() returned 0:
                     * peer disconnected normally.
                     */
                    printf("Client disconnected. fd = %d\n",
                           fd);

                    remove_client(epoll_fd, fd);
                    continue;
                }

                if (result < 0) {
                    perror("recv");

                    remove_client(epoll_fd, fd);
                    continue;
                }
            }
        }
    }
}