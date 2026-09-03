#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    int sockfd;
    int port;
    struct sockaddr_in server_addr;

    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];

    ssize_t sent;
    ssize_t received;

    /*
     * Usage:
     * ./monitor_cli <server_ip> <port>
     */
    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <server_ip> <port>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * Convert port argument to integer.
     */
    port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    /*
     * Create an IPv4 TCP socket.
     */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Configure server address.
     */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    /*
     * Convert the server IP address from text
     * to binary representation.
     */
    if (inet_pton(AF_INET,
                  argv[1],
                  &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * Connect to monitor_server.
     */
    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n", argv[1], port);
    printf("Enter commands. Ctrl+D to exit.\n");

    /*
     * Read commands from stdin.
     */
    while (fgets(send_buffer,
                 sizeof(send_buffer),
                 stdin) != NULL) {

        size_t total_sent = 0;
        size_t message_len = strlen(send_buffer);

        /*
         * send() is not guaranteed to send the
         * complete buffer in one call.
         */
        while (total_sent < message_len) {

            sent = send(sockfd,
                        send_buffer + total_sent,
                        message_len - total_sent,
                        0);

            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }

                perror("send");
                close(sockfd);
                return EXIT_FAILURE;
            }

            if (sent == 0) {
                fprintf(stderr,
                        "Connection closed while sending\n");
                close(sockfd);
                return EXIT_FAILURE;
            }

            total_sent += (size_t)sent;
        }

        /*
         * Wait for the server response.
         *
         * Advanced TCP stream buffering will be
         * implemented in a later task.
         */
        do {
            received = recv(sockfd,
                            recv_buffer,
                            sizeof(recv_buffer) - 1,
                            0);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            perror("recv");
            close(sockfd);
            return EXIT_FAILURE;
        }

        /*
         * recv() == 0 means the peer performed
         * an orderly shutdown.
         */
        if (received == 0) {
            printf("Server disconnected\n");
            break;
        }

        recv_buffer[received] = '\0';

        printf("%s", recv_buffer);

        /*
         * Add a newline only if the server response
         * did not already contain one.
         */
        if (recv_buffer[received - 1] != '\n') {
            printf("\n");
        }
    }

    printf("Closing connection\n");

    close(sockfd);

    return EXIT_SUCCESS;
}