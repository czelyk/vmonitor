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
#define UDP_BUFFER_SIZE 2048

/*
 * Parse and validate a TCP/UDP port.
 */
static int parse_port(const char *text)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value <= 0 ||
        value > 65535) {

        return -1;
    }

    return (int)value;
}

/*
 * Send the complete TCP buffer.
 */
static int send_all(int sockfd,
                    const char *buffer,
                    size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent;

        sent = send(sockfd,
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
            fprintf(stderr,
                    "Connection closed while sending\n");
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

/*
 * Print all complete newline-terminated messages
 * currently available in the TCP RX buffer.
 *
 * Any incomplete message remains in the buffer.
 */
static void process_rx_buffer(char *buffer,
                              size_t *buffer_len)
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

    if (start > 0) {
        size_t remaining = *buffer_len - start;

        memmove(buffer,
                buffer + start,
                remaining);

        *buffer_len = remaining;
    }
}

/*
 * Validate UDP telemetry format:
 *
 * STAT <udp_seq> <last_seq> <last_value_mC>
 *      <last_alarm> <dropped_total> <running>
 */
static int validate_stat_message(const char *message)
{
    unsigned long long udp_seq;
    unsigned long long last_seq;
    int last_value_mC;
    unsigned int last_alarm;
    unsigned long long dropped_total;
    unsigned int running;
    char extra[32];

    int matched;

    matched = sscanf(message,
                     "STAT %llu %llu %d %u %llu %u %31s",
                     &udp_seq,
                     &last_seq,
                     &last_value_mC,
                     &last_alarm,
                     &dropped_total,
                     &running,
                     extra);

    /*
     * Exactly 6 values must follow STAT.
     *
     * If a seventh token exists, sscanf returns 7.
     */
    if (matched != 6) {
        return -1;
    }

    if (last_alarm > 1 || running > 1) {
        return -1;
    }

    return 0;
}

/*
 * UDP telemetry listening mode.
 */
static int run_udp_mode(int port)
{
    int sockfd;
    struct sockaddr_in local_addr;

    sockfd = socket(AF_INET,
                    SOCK_DGRAM,
                    0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&local_addr,
           0,
           sizeof(local_addr));

    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons((unsigned short)port);

    if (bind(sockfd,
             (struct sockaddr *)&local_addr,
             sizeof(local_addr)) < 0) {

        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Listening for UDP telemetry on port %d\n",
           port);

    printf("Press Ctrl+C to stop.\n");

    for (;;) {
        char buffer[UDP_BUFFER_SIZE];

        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t received;

        received = recvfrom(sockfd,
                            buffer,
                            sizeof(buffer) - 1,
                            MSG_TRUNC,
                            (struct sockaddr *)&sender_addr,
                            &sender_len);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("recvfrom");
            close(sockfd);
            return EXIT_FAILURE;
        }

        /*
         * MSG_TRUNC allows us to detect a datagram
         * larger than our receive buffer.
         */
        if ((size_t)received >= sizeof(buffer)) {
            fprintf(stderr,
                    "Dropped oversized UDP datagram\n");
            continue;
        }

        buffer[received] = '\0';

        /*
         * Remove a trailing newline if one exists.
         */
        if (received > 0 &&
            buffer[received - 1] == '\n') {

            buffer[received - 1] = '\0';
        }

        if (validate_stat_message(buffer) < 0) {
            fprintf(stderr,
                    "Malformed telemetry packet: %s\n",
                    buffer);

            continue;
        }

        printf("%s\n", buffer);
    }
}

/*
 * Existing TCP + WATCH mode.
 */
static int run_tcp_mode(const char *server_ip,
                        int port)
{
    int sockfd;

    struct sockaddr_in server_addr;
    struct pollfd fds[2];

    char input_buffer[INPUT_BUFFER_SIZE];
    char rx_buffer[RX_BUFFER_SIZE];

    size_t rx_len = 0;

    sockfd = socket(AF_INET,
                    SOCK_STREAM,
                    0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port =
        htons((unsigned short)port);

    if (inet_pton(AF_INET,
                  server_ip,
                  &server_addr.sin_addr) != 1) {

        fprintf(stderr,
                "Invalid IPv4 address: %s\n",
                server_ip);

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
           server_ip,
           port);

    printf("Enter commands. Ctrl+D to exit.\n");

    /*
     * fd 0 -> stdin
     * fd 1 -> TCP socket
     */
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    fds[1].fd = sockfd;
    fds[1].events = POLLIN;

    for (;;) {
        int ready;

        ready = poll(fds,
                     2,
                     -1);

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
         * stdin closed/error.
         */
        if (fds[0].revents &
            (POLLERR | POLLHUP | POLLNVAL)) {

            printf("Standard input closed\n");
            break;
        }

        /*
         * Server sent TCP data.
         */
        if (fds[1].revents & POLLIN) {

            for (;;) {
                ssize_t received;

                if (rx_len >= sizeof(rx_buffer)) {
                    fprintf(stderr,
                            "TCP receive buffer overflow\n");

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
         * TCP connection closed/error.
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

int main(int argc, char *argv[])
{
    int port;

    /*
     * UDP mode:
     *
     * ./monitor_cli --udp 5001
     */
    if (argc == 3 &&
        strcmp(argv[1], "--udp") == 0) {

        port = parse_port(argv[2]);

        if (port < 0) {
            fprintf(stderr,
                    "Invalid UDP port: %s\n",
                    argv[2]);

            return EXIT_FAILURE;
        }

        return run_udp_mode(port);
    }

    /*
     * TCP mode:
     *
     * ./monitor_cli 127.0.0.1 5000
     */
    if (argc == 3) {

        port = parse_port(argv[2]);

        if (port < 0) {
            fprintf(stderr,
                    "Invalid TCP port: %s\n",
                    argv[2]);

            return EXIT_FAILURE;
        }

        return run_tcp_mode(argv[1],
                            port);
    }

    fprintf(stderr,
            "Usage:\n"
            "  %s <server_ip> <tcp_port>\n"
            "  %s --udp <udp_port>\n",
            argv[0],
            argv[0]);

    return EXIT_FAILURE;
}