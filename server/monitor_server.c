#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "event_loop.h"

#define DEFAULT_PORT 5000
#define LISTEN_BACKLOG 64

#define VMONITOR_DEVICE_PATH "/dev/vmonitor"

static int parse_port(const char *text,
                      unsigned short *port_out)
{
    char *end;
    long value;

    if (text == NULL ||
        port_out == NULL) {

        errno = EINVAL;

        return -1;
    }

    errno = 0;

    value =
        strtol(text,
               &end,
               10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535) {

        errno = EINVAL;

        return -1;
    }

    *port_out =
        (unsigned short)value;

    return 0;
}

static int create_listen_socket(
    unsigned short port)
{
    struct sockaddr_in addr;

    int fd;
    int one;
    int flags;

    one = 1;

    fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (fd < 0)
        return -1;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &one,
                   sizeof(one)) < 0) {

        close(fd);

        return -1;
    }

    /*
     * Non-blocking listening socket.
     */
    flags =
        fcntl(fd,
              F_GETFL,
              0);

    if (flags < 0 ||
        fcntl(fd,
              F_SETFL,
              flags | O_NONBLOCK) < 0) {

        close(fd);

        return -1;
    }

    /*
     * Do not leak the socket across exec().
     */
    flags =
        fcntl(fd,
              F_GETFD,
              0);

    if (flags < 0 ||
        fcntl(fd,
              F_SETFD,
              flags | FD_CLOEXEC) < 0) {

        close(fd);

        return -1;
    }

    memset(&addr,
           0,
           sizeof(addr));

    addr.sin_family =
        AF_INET;

    addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    addr.sin_port =
        htons(port);

    if (bind(fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0) {

        close(fd);

        return -1;
    }

    if (listen(fd,
               LISTEN_BACKLOG) < 0) {

        close(fd);

        return -1;
    }

    return fd;
}

static int open_driver(void)
{
    /*
     * The server is the only userspace process
     * that should open /dev/vmonitor.
     */
    return open(
        VMONITOR_DEVICE_PATH,
        O_RDWR |
        O_NONBLOCK |
        O_CLOEXEC
    );
}

int main(int argc,
         char **argv)
{
    unsigned short port;

    int server_fd;
    int driver_fd;
    int rc;

    port = DEFAULT_PORT;

    if (argc > 2) {

        fprintf(stderr,
                "Usage: %s [port]\n",
                argv[0]);

        return EXIT_FAILURE;
    }

    if (argc == 2) {

        if (parse_port(argv[1],
                       &port) < 0) {

            fprintf(stderr,
                    "Invalid TCP port: %s\n",
                    argv[1]);

            return EXIT_FAILURE;
        }
    }

    /*
     * Open the kernel driver once.
     */
    driver_fd =
        open_driver();

    if (driver_fd < 0) {

        perror("open(/dev/vmonitor)");

        return EXIT_FAILURE;
    }

    server_fd =
        create_listen_socket(port);

    if (server_fd < 0) {

        perror("create_listen_socket");

        close(driver_fd);

        return EXIT_FAILURE;
    }

    printf(
        "vmonitor server listening "
        "on 0.0.0.0:%u\n",
        port
    );

    printf(
        "vmonitor driver opened: "
        "%s fd=%d\n",
        VMONITOR_DEVICE_PATH,
        driver_fd
    );

    rc =
        event_loop_run(server_fd,
                       driver_fd);

    close(server_fd);
    close(driver_fd);

    return
        rc == 0
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
}
