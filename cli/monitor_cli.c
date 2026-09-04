#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define INPUT_BUFFER_SIZE 4096
#define RX_BUFFER_SIZE 8192

static int send_all(int sockfd, const char *buffer, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(sockfd,
                            buffer + total_sent,
                            length - total_sent,
                            0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("send");
            return -1;
        }

        if (sent == 0) {
            fprintf(stderr, "Connection closed while sending\n");
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int process_rx_buffer(char *buffer, size_t *buffer_len)
{
    size_t start = 0;
    size_t i;

    for (i = 0; i < *buffer_len; i++) {
        if (buffer[i] == '\n') {
            size_t line_len = i - start;

            printf("%.*s\n",
                   (int)line_len,
                   buffer + start);

            start = i + 1;
        }
    }

    /*
     * Move incomplete trailing data to the beginning
     * of the buffer.
     */
    if (start > 0) {
        size_t remaining = *buffer_len - start;

        memmove(buffer,
                buffer + start,
                remaining);

        *buffer_len = remaining;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int sockfd;
    int port;

    struct sockaddr_in server_addr;

    struct pollfd fds[2];

    char input_buffer[INPUT_BUFFER_SIZE];
    char rx_buffer[RX_BUFFER_SIZE];

    size_t rx_len = 0;

    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <server_ip> <port>\n",
                argv[0]);

        return EXIT_FAILURE;
    }

    port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr,
                "Invalid port: %s\n",
                argv[2]);

        return EXIT_FAILURE;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET,
                  argv[1],
                  &server_addr.sin_addr) != 1) {

        fprintf(stderr,
                "Invalid IPv4 address: %s\n",
                argv[1]);

        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {

        perror("connect");

        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n",
           argv[1],
           port);

    printf("Enter commands. Ctrl+D to exit.\n");

    /*
     * poll fd #0 -> stdin
     * poll fd #1 -> TCP socket
     */
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    fds[1].fd = sockfd;
    fds[1].events = POLLIN;

    for (;;) {
        int ready;

        ready = poll(fds, 2, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            break;
        }

        /*
         * User entered a command.
         */
        if (fds[0].revents & POLLIN) {

            if (fgets(input_buffer,
                      sizeof(input_buffer),
                      stdin) == NULL) {

                printf("Input closed\n");
                break;
            }

            if (send_all(sockfd,
                         input_buffer,
                         strlen(input_buffer)) < 0) {

                break;
            }
        }

        /*
         * Handle stdin close/error.
         */
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("Standard input closed\n");
            break;
        }

        /*
         * Server sent data.
         */
        if (fds[1].revents & POLLIN) {

            for (;;) {
                ssize_t received;

                if (rx_len >= sizeof(rx_buffer)) {
                    fprintf(stderr,
                            "Receive buffer overflow\n");
                    close(sockfd);
                    return EXIT_FAILURE;
                }

                received = recv(sockfd,
                                rx_buffer + rx_len,
                                sizeof(rx_buffer) - rx_len,
                                MSG_DONTWAIT);

                if (received > 0) {

                    rx_len += (size_t)received;

                    process_rx_buffer(rx_buffer,
                                      &rx_len);

                    continue;
                }

                if (received == 0) {
                    printf("Server disconnected\n");

                    close(sockfd);
                    return EXIT_SUCCESS;
                }

                if (errno == EINTR) {
                    continue;
                }

                if (errno == EAGAIN ||
                    errno == EWOULDBLOCK) {

                    break;
                }

                perror("recv");

                close(sockfd);
                return EXIT_FAILURE;
            }
        }

        /*
         * Server connection error/hangup.
         */
        if (fds[1].revents &
            (POLLERR | POLLHUP | POLLNVAL)) {

            printf("Server connection closed\n");
            break;
        }
    }

    close(sockfd);

    return EXIT_SUCCESS;
}