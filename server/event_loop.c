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

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    return 0;
}

/*
 * Single cleanup path for registered clients.
 *
 * Remove from epoll first, then remove client state.
 * client_remove() owns closing the client socket.
 */
static void remove_client(int epoll_fd, int client_fd)
{
    /*
     * First verify that this descriptor still belongs
     * to a tracked client.
     *
     * This makes repeated cleanup attempts harmless.
     */
    if (client_find(client_fd) == NULL)
        return;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_DEL,
                  client_fd,
                  NULL) < 0) {

        /*
         * ENOENT means it is already absent from epoll.
         * Cleanup of our own client state must still occur.
         */
        if (errno != ENOENT &&
            errno != EBADF) {

            perror("epoll_ctl(DEL client)");
        }
    }

    if (client_remove(client_fd) < 0) {
        if (errno != ENOENT)
            perror("client_remove");
    }
}

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

    if (client_has_pending_tx(client))
        event.events |= EPOLLOUT;

    event.data.fd = client->fd;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_MOD,
                  client->fd,
                  &event) < 0) {

        return -1;
    }

    return 0;
}

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
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK) {

                return 0;
            }

            if (errno == EINTR)
                continue;

            perror("accept");
            return -1;
        }

        /*
         * Enforce the connection limit immediately after
         * accept(). The accepted socket is not inserted
         * into either the client list or epoll.
         */
        if (client_limit_reached()) {
            fprintf(stderr,
                    "Client limit reached (%d). "
                    "Rejecting fd = %d\n",
                    CLIENT_MAX_COUNT,
                    client_fd);

            close(client_fd);
            continue;
        }

        if (set_nonblocking(client_fd) < 0) {
            perror("fcntl(client_fd)");
            close(client_fd);
            continue;
        }

        if (client_add(client_fd) < 0) {
            perror("client_add");

            /*
             * client_add() has not taken ownership when
             * it returns an error.
             */
            close(client_fd);
            continue;
        }

        memset(&event, 0, sizeof(event));

        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;

        if (epoll_ctl(epoll_fd,
                      EPOLL_CTL_ADD,
                      client_fd,
                      &event) < 0) {

            perror("epoll_ctl(ADD client)");

            /*
             * client_add() succeeded, so client_remove()
             * now owns closing the socket.
             *
             * No EPOLL_CTL_DEL is needed because ADD failed.
             */
            (void)client_remove(client_fd);
            continue;
        }

        printf("Client connected. fd = %d "
               "active = %zu/%d\n",
               client_fd,
               client_count(),
               CLIENT_MAX_COUNT);
    }
}

int event_loop_run(int server_fd)
{
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    int epoll_fd;

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

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

    printf("Epoll event loop started. "
           "Maximum clients = %d\n",
           CLIENT_MAX_COUNT);

    for (;;) {
        int ready;
        int i;

        ready = epoll_wait(epoll_fd,
                           events,
                           MAX_EVENTS,
                           -1);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

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
             * Listening socket.
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
                    if (accept_clients(epoll_fd,
                                       server_fd) < 0) {

                        /*
                         * Keep the server alive after an
                         * isolated accept failure.
                         */
                        continue;
                    }
                }

                continue;
            }

            /*
             * Ignore stale epoll events for a client that
             * was already removed earlier in this batch.
             */
            if (client_find(fd) == NULL)
                continue;

            /*
             * EPOLLERR represents a fatal socket error.
             */
            if (event_flags & EPOLLERR) {
                fprintf(stderr,
                        "Client socket error. fd = %d\n",
                        fd);

                remove_client(epoll_fd, fd);
                continue;
            }

            /*
             * Process readable bytes before RDHUP/HUP.
             *
             * epoll may report readable data and peer
             * shutdown in the same event.
             */
            if (event_flags & EPOLLIN) {
                struct client *client;
                int result;

                client = client_find(fd);

                if (client == NULL)
                    continue;

                result = client_handle_read(client);

                if (result > 0) {
                    printf("Client closed connection. "
                           "fd = %d\n",
                           fd);

                    remove_client(epoll_fd, fd);
                    continue;
                }

                if (result < 0) {
                    perror("client recv");

                    remove_client(epoll_fd, fd);
                    continue;
                }

                /*
                 * Protocol handling may eventually queue
                 * TX data while processing input.
                 */
                if (update_client_events(epoll_fd,
                                         client) < 0) {

                    perror("epoll_ctl(MOD client)");

                    remove_client(epoll_fd, fd);
                    continue;
                }
            }

            /*
             * The client might have been removed while
             * handling EPOLLIN.
             */
            if (client_find(fd) == NULL)
                continue;

            if (event_flags & EPOLLOUT) {
                struct client *client;
                int result;

                client = client_find(fd);

                if (client == NULL)
                    continue;

                result = client_handle_write(client);

                if (result < 0) {
                    perror("client send");

                    remove_client(epoll_fd, fd);
                    continue;
                }

                if (update_client_events(epoll_fd,
                                         client) < 0) {

                    perror("epoll_ctl(MOD client)");

                    remove_client(epoll_fd, fd);
                    continue;
                }
            }

            /*
             * EPOLLIN/EPOLLOUT may have removed the client.
             */
            if (client_find(fd) == NULL)
                continue;

            /*
             * Handle peer shutdown after processing readable
             * data that arrived with the same event.
             */
            if (event_flags &
                (EPOLLRDHUP | EPOLLHUP)) {

                printf("Client hangup. fd = %d\n",
                       fd);

                remove_client(epoll_fd, fd);
                continue;
            }
        }
    }
}
