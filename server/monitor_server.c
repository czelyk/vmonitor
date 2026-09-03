#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_PORT 5000
#define BACKLOG 16

int main(void)
{
    int server_fd;
    int opt = 1;
    int flags;

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
     * 4. Bind the socket to the configured address and port.
     */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 5. Start listening for incoming TCP connections.
     */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 6. Get the current file status flags.
     */
    flags = fcntl(server_fd, F_GETFL, 0);

    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 7. Enable non-blocking mode on the listening socket.
     */
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d\n", SERVER_PORT);
    printf("Listening socket is non-blocking\n");

    /*
     * 8. Try to accept incoming clients.
     */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd;

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);

        if (client_fd < 0) {
            /*
             * Because the listening socket is non-blocking,
             * accept() returns immediately when no client
             * is waiting.
             *
             * EAGAIN / EWOULDBLOCK is therefore expected.
             */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            }

            /*
             * accept() may be interrupted by a signal.
             * In that case, simply try again.
             */
            if (errno == EINTR) {
                continue;
            }

            /*
             * Any other accept() error is unexpected.
             */
            perror("accept");
            break;
        }

        printf("Client connected. fd = %d\n", client_fd);

        /*
         * Client communication is outside the scope
         * of the current task.
         *
         * We accept the connection and close it for now.
         */
        close(client_fd);
    }

    close(server_fd);

    return EXIT_SUCCESS;
}