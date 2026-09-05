#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "event_loop.h"

#define SERVER_PORT 5000
#define BACKLOG 16

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

int main(void)
{
    int server_fd;
    int opt = 1;

    struct sockaddr_in server_addr;

    /*
     * 1. Create an IPv4 TCP socket.
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * 2. Allow the address/port to be reused.
     */
    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) < 0) {

        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 3. Configure the server address.
     */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    /*
     * 4. Bind the socket to the configured address.
     */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {

        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 5. Start listening for TCP connections.
     */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 6. The listening socket must be non-blocking.
     */
    if (set_nonblocking(server_fd) < 0) {
        perror("fcntl(server_fd)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d\n", SERVER_PORT);
    printf("Listening socket is non-blocking\n");

    /*
     * 7. Enter the epoll-based event loop.
     */
    if (event_loop_run(server_fd) < 0) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    close(server_fd);

    return EXIT_SUCCESS;
}