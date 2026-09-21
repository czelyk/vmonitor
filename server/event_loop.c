#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include "event_loop.h"
#include "client.h"

#define MAX_EVENTS 128

_Static_assert(sizeof(struct vmonitor_sample) == 24,
               "vmonitor_sample must be exactly 24 bytes");

static struct vmonitor_sample latest_sample;
static int latest_sample_valid;

/*
 * Put an fd into non-blocking mode.
 */
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
 * Remove a client exactly once.
 *
 * epoll registration is removed before the client
 * object closes and frees the fd.
 */
static void remove_client(int epoll_fd, int client_fd)
{
    if (client_find(client_fd) == NULL)
        return;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_DEL,
                  client_fd,
                  NULL) < 0) {

        if (errno != ENOENT &&
            errno != EBADF) {

            perror("epoll_ctl(DEL client)");
        }
    }

    if (client_remove(client_fd) < 0 &&
        errno != ENOENT) {

        perror("client_remove");
    }
}

/*
 * EPOLLOUT must only be active while the client
 * actually has unsent bytes.
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

    event.events =
        EPOLLIN |
        EPOLLRDHUP;

    if (client_has_pending_tx(client))
        event.events |= EPOLLOUT;

    event.data.fd = client->fd;

    return epoll_ctl(epoll_fd,
                     EPOLL_CTL_MOD,
                     client->fd,
                     &event);
}

/*
 * Accept every currently pending connection.
 */
static int accept_clients(int epoll_fd,
                          int server_fd)
{
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len;
        struct epoll_event event;

        int client_fd;

        client_addr_len =
            sizeof(client_addr);

        client_fd =
            accept(server_fd,
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
         * Reject excess clients immediately.
         */
        if (client_limit_reached()) {

            fprintf(stderr,
                    "Client limit reached (%d), "
                    "rejecting fd=%d\n",
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

        /*
         * client_add() takes ownership only if it
         * succeeds.
         */
        if (client_add(client_fd) < 0) {

            perror("client_add");

            close(client_fd);

            continue;
        }

        memset(&event, 0, sizeof(event));

        event.events =
            EPOLLIN |
            EPOLLRDHUP;

        event.data.fd =
            client_fd;

        if (epoll_ctl(epoll_fd,
                      EPOLL_CTL_ADD,
                      client_fd,
                      &event) < 0) {

            perror("epoll_ctl(ADD client)");

            /*
             * client_remove() owns and closes the
             * descriptor after successful client_add().
             */
            (void)client_remove(client_fd);

            continue;
        }

        printf("Client connected. fd=%d active=%zu\n",
               client_fd,
               client_count());
    }
}

/*
 * Register an fd with epoll.
 */
static int register_fd(int epoll_fd,
                       int fd,
                       uint32_t events)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));

    event.events = events;
    event.data.fd = fd;

    return epoll_ctl(epoll_fd,
                     EPOLL_CTL_ADD,
                     fd,
                     &event);
}

/*
 * Drain every currently available vmonitor sample.
 *
 * /dev/vmonitor is opened O_NONBLOCK, therefore once
 * the driver queue becomes empty read() returns
 * EAGAIN/EWOULDBLOCK.
 */
static int drain_driver(int driver_fd)
{
    for (;;) {
        struct vmonitor_sample sample;
        ssize_t n;

        n = read(driver_fd,
                 &sample,
                 sizeof(sample));

        if (n == (ssize_t)sizeof(sample)) {

            latest_sample = sample;
            latest_sample_valid = 1;

            printf(
                "Driver sample: "
                "seq=%" PRIu64 " "
                "ts=%" PRIu64 " "
                "value_mC=%" PRId32 " "
                "alarm=%" PRIu32 "\n",
                (uint64_t)sample.seq,
                (uint64_t)sample.timestamp_ns,
                (int32_t)sample.value_mC,
                (uint32_t)sample.alarm
            );

            /*
             * Continue until the driver's queue
             * has been drained.
             */
            continue;
        }

        if (n < 0) {

            if (errno == EINTR)
                continue;

            if (errno == EAGAIN ||
                errno == EWOULDBLOCK) {

                return 0;
            }

            return -1;
        }

        /*
         * The vmonitor UAPI specifies an exact
         * 24-byte read. EOF is therefore unexpected.
         */
        if (n == 0) {

            errno = EIO;

            return -1;
        }

        /*
         * A compliant vmonitor driver never returns
         * a positive short sample.
         *
         * Do not combine it with the next read because
         * that could accidentally combine two different
         * samples.
         */
        fprintf(stderr,
                "Short read from /dev/vmonitor: "
                "%zd of %zu bytes\n",
                n,
                sizeof(sample));

        errno = EPROTO;

        return -1;
    }
}

int event_loop_get_latest_sample(
    struct vmonitor_sample *out)
{
    if (out == NULL) {

        errno = EINVAL;

        return -1;
    }

    if (!latest_sample_valid) {

        errno = ENODATA;

        return -1;
    }

    *out = latest_sample;

    return 0;
}

/*
 * Main single-threaded epoll loop.
 */
int event_loop_run(int server_fd,
                   int driver_fd)
{
    struct epoll_event events[MAX_EVENTS];

    int epoll_fd;
    int driver_registered;

    if (server_fd < 0 ||
        driver_fd < 0) {

        errno = EINVAL;

        return -1;
    }

    epoll_fd =
        epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd < 0) {

        perror("epoll_create1");

        return -1;
    }

    /*
     * TCP listening socket.
     */
    if (register_fd(epoll_fd,
                    server_fd,
                    EPOLLIN) < 0) {

        perror("epoll_ctl(ADD server)");

        close(epoll_fd);

        return -1;
    }

    /*
     * Kernel driver.
     *
     * The driver's poll() implementation wakes us
     * whenever at least one sample is queued.
     */
    if (register_fd(epoll_fd,
                    driver_fd,
                    EPOLLIN) < 0) {

        perror("epoll_ctl(ADD driver)");

        close(epoll_fd);

        return -1;
    }

    driver_registered = 1;

    printf("Epoll event loop started. "
           "driver_fd=%d\n",
           driver_fd);

    for (;;) {
        int ready;
        int i;

        ready =
            epoll_wait(epoll_fd,
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

        for (i = 0; i < ready; ++i) {

            int fd;
            uint32_t event_flags;

            fd =
                events[i].data.fd;

            event_flags =
                events[i].events;

            /*
             * -------------------------------------------------
             * Listening socket
             * -------------------------------------------------
             */
            if (fd == server_fd) {

                if (event_flags &
                    (EPOLLERR |
                     EPOLLHUP)) {

                    fprintf(stderr,
                            "Listening socket "
                            "error or hangup\n");

                    close(epoll_fd);

                    return -1;
                }

                if (event_flags & EPOLLIN) {

                    /*
                     * Failure to accept one connection
                     * must not kill existing clients.
                     */
                    if (accept_clients(epoll_fd,
                                       server_fd) < 0) {

                        continue;
                    }
                }

                continue;
            }

            /*
             * -------------------------------------------------
             * vmonitor driver
             * -------------------------------------------------
             */
            if (driver_registered &&
                fd == driver_fd) {

                /*
                 * Process readable samples before
                 * handling a possible hangup/error.
                 */
                if (event_flags & EPOLLIN) {

                    if (drain_driver(driver_fd) < 0) {

                        perror(
                            "read(/dev/vmonitor)"
                        );

                        if (epoll_ctl(
                                epoll_fd,
                                EPOLL_CTL_DEL,
                                driver_fd,
                                NULL) < 0 &&
                            errno != ENOENT &&
                            errno != EBADF) {

                            perror(
                                "epoll_ctl(DEL driver)"
                            );
                        }

                        driver_registered = 0;

                        /*
                         * A driver failure does not
                         * destroy existing TCP clients.
                         */
                        fprintf(
                            stderr,
                            "Driver polling disabled; "
                            "TCP server remains alive\n"
                        );

                        continue;
                    }
                }

                if (event_flags &
                    (EPOLLERR |
                     EPOLLHUP)) {

                    fprintf(
                        stderr,
                        "Driver fd error/hangup; "
                        "disabling driver polling\n"
                    );

                    if (epoll_ctl(
                            epoll_fd,
                            EPOLL_CTL_DEL,
                            driver_fd,
                            NULL) < 0 &&
                        errno != ENOENT &&
                        errno != EBADF) {

                        perror(
                            "epoll_ctl(DEL driver)"
                        );
                    }

                    driver_registered = 0;
                }

                continue;
            }

            /*
             * -------------------------------------------------
             * TCP client
             * -------------------------------------------------
             */

            /*
             * Protect against stale epoll events.
             */
            if (client_find(fd) == NULL)
                continue;

            /*
             * Fatal socket error only destroys this
             * client.
             */
            if (event_flags & EPOLLERR) {

                printf("Client error. fd=%d\n",
                       fd);

                remove_client(epoll_fd,
                              fd);

                continue;
            }

            /*
             * Read first.
             *
             * EPOLLIN and EPOLLRDHUP can arrive in
             * the same event, so consume remaining
             * input before closing.
             */
            if (event_flags & EPOLLIN) {

                struct client *client;
                int result;

                client =
                    client_find(fd);

                if (client == NULL)
                    continue;

                result =
                    client_handle_read(client);

                /*
                 * Existing client API:
                 *
                 * > 0 : peer closed
                 *   0 : success / EAGAIN
                 * < 0 : fatal RX/parser/buffer error
                 */
                if (result > 0) {

                    printf(
                        "Client disconnected. "
                        "fd=%d\n",
                        fd
                    );

                    remove_client(epoll_fd,
                                  fd);

                    continue;
                }

                if (result < 0) {

                    perror("recv");

                    remove_client(epoll_fd,
                                  fd);

                    continue;
                }

                /*
                 * Parsing may have queued a response.
                 */
                if (update_client_events(
                        epoll_fd,
                        client) < 0) {

                    perror(
                        "epoll_ctl(MOD client)"
                    );

                    remove_client(epoll_fd,
                                  fd);

                    continue;
                }
            }

            /*
             * Client may already have been deleted.
             */
            if (client_find(fd) == NULL)
                continue;

            /*
             * Flush pending output.
             */
            if (event_flags & EPOLLOUT) {

                struct client *client;

                client =
                    client_find(fd);

                if (client == NULL)
                    continue;

                if (client_handle_write(client) < 0) {

                    perror("send");

                    remove_client(epoll_fd,
                                  fd);

                    continue;
                }

                /*
                 * Removes EPOLLOUT automatically once
                 * the TX buffer becomes empty.
                 */
                if (update_client_events(
                        epoll_fd,
                        client) < 0) {

                    perror(
                        "epoll_ctl(MOD client)"
                    );

                    remove_client(epoll_fd,
                                  fd);

                    continue;
                }
            }

            if (client_find(fd) == NULL)
                continue;

            /*
             * Handle shutdown only after readable and
             * writable work.
             */
            if (event_flags &
                (EPOLLHUP |
                 EPOLLRDHUP)) {

                printf(
                    "Client disconnected. fd=%d\n",
                    fd
                );

                remove_client(epoll_fd,
                              fd);
            }
        }
    }
}
