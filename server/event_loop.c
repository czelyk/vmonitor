#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include "event_loop.h"
#include "client.h"

#define MAX_EVENTS 64

/*
 * Set a file descriptor to non-blocking mode.
 */
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

/*
 * Remove a client from epoll and destroy
 * its per-client state.
 */
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

/*
 * Update the events monitored for a client.
 *
 * EPOLLIN and EPOLLRDHUP are always enabled.
 *
 * EPOLLOUT is enabled only while the client
 * has unsent data in its TX buffer.
 */
static int update_client_events(int epoll_fd,
                                struct client *client)
{
    struct epoll_event event;

    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&event, 0, sizeof(event));

    event.events = EPOLLIN | EPOLLRDHUP;

    if (client_has_pending_tx(client)) {
        event.events |= EPOLLOUT;
    }

    event.data.fd = client->fd;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_MOD,
                  client->fd,
                  &event) < 0) {

        return -1;
    }

    return 0;
}

/*
 * Accept every currently pending TCP connection.
 *
 * The listening socket is non-blocking, so accept()
 * eventually returns EAGAIN/EWOULDBLOCK when the
 * accept queue has been drained.
 */
static int accept_clients(int epoll_fd, int server_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len;
        struct epoll_event event;

        int client_fd;

        client_addr_len = sizeof(client_addr);

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);

        if (client_fd < 0) {

            /*
             * No more pending clients.
             */
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK) {

                return 0;
            }

            /*
             * Retry interrupted accept().
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
         * Create the per-client state.
         */
        if (client_add(client_fd) < 0) {
            perror("client_add");
            close(client_fd);
            continue;
        }

        memset(&event, 0, sizeof(event));

        /*
         * Initially there is no pending TX data,
         * therefore EPOLLOUT is not enabled.
         */
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;

        if (epoll_ctl(epoll_fd,
                      EPOLL_CTL_ADD,
                      client_fd,
                      &event) < 0) {

            perror("epoll_ctl(ADD client)");
            client_remove(client_fd);
            continue;
        }

        printf("Client connected. fd = %d\n",
               client_fd);
    }
}

/*
 * Main server event loop.
 */
int event_loop_run(int server_fd)
{
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    int epoll_fd;

    /*
     * Create epoll instance.
     */
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /*
     * Register listening socket.
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
            int fd;
            uint32_t event_flags;

            fd = events[i].data.fd;
            event_flags = events[i].events;

            /*
             * Listening socket event.
             */
            if (fd == server_fd) {

                if (event_flags &
                    (EPOLLERR | EPOLLHUP)) {

                    fprintf(stderr,
                            "Listening socket error or hangup\n");

                    close(epoll_fd);
                    return -1;
                }

                if (event_flags & EPOLLIN) {

                    /*
                     * Accept all pending connections.
                     */
                    if (accept_clients(epoll_fd,
                                       server_fd) < 0) {

                        /*
                         * Keep server alive even if one
                         * accept cycle fails.
                         */
                        continue;
                    }
                }

                continue;
            }

            /*
             * From here onward this event belongs
             * to a connected client.
             */

            /*
             * Fatal socket error.
             */
            if (event_flags & EPOLLERR) {
                printf("Client error. fd = %d\n",
                       fd);

                remove_client(epoll_fd, fd);
                continue;
            }

            /*
             * Read incoming TCP data first.
             *
             * This must happen before handling
             * EPOLLRDHUP / EPOLLHUP because readable
             * data and peer shutdown can be reported
             * in the same epoll event.
             */
            if (event_flags & EPOLLIN) {
                struct client *client;
                int result;

                client = client_find(fd);

                if (client == NULL) {
                    fprintf(stderr,
                            "Unknown client fd = %d\n",
                            fd);

                    remove_client(epoll_fd, fd);
                    continue;
                }

                result = client_handle_read(client);

                /*
                 * recv() returned 0.
                 */
                if (result > 0) {
                    printf("Client disconnected. fd = %d\n",
                           fd);

                    remove_client(epoll_fd, fd);
                    continue;
                }

                /*
                 * Fatal receive error or oversized
                 * incoming command.
                 */
                if (result < 0) {
                    perror("recv");

                    remove_client(epoll_fd, fd);
                    continue;
                }

                /*
                 * Reading/parsing the command may have
                 * queued response data.
                 *
                 * Enable EPOLLOUT if TX data is pending.
                 */
                if (update_client_events(epoll_fd,
                                         client) < 0) {

                    perror("epoll_ctl(MOD client)");

                    remove_client(epoll_fd, fd);
                    continue;
                }
            }

            /*
             * Socket is currently writable.
             *
             * Flush as much pending TX data as possible.
             */
            if (event_flags & EPOLLOUT) {
                struct client *client;
                int result;

                client = client_find(fd);

                if (client == NULL) {
                    fprintf(stderr,
                            "Unknown client fd = %d\n",
                            fd);

                    remove_client(epoll_fd, fd);
                    continue;
                }

                result = client_handle_write(client);

                /*
                 * Fatal send error.
                 */
                if (result < 0) {
                    perror("send");

                    remove_client(epoll_fd, fd);
                    continue;
                }

                /*
                 * If all bytes were sent,
                 * client_has_pending_tx() becomes false
                 * and EPOLLOUT is removed.
                 *
                 * If send() reached EAGAIN, pending
                 * bytes remain and EPOLLOUT stays active.
                 */
                if (update_client_events(epoll_fd,
                                         client) < 0) {

                    perror("epoll_ctl(MOD client)");

                    remove_client(epoll_fd, fd);
                    continue;
                }
            }

            /*
             * Handle peer shutdown only after pending
             * readable/writable work has been processed.
             */
            if (event_flags &
                (EPOLLHUP | EPOLLRDHUP)) {

                printf("Client disconnected. fd = %d\n",
                       fd);

                remove_client(epoll_fd, fd);
                continue;
            }
        }
    }
}