#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <string.h>

#include "client.h"
#include "protocol.h"

struct client_node {
    struct client client;
    struct client_node *next;
};

static struct client_node *client_list = NULL;

struct client *client_find(int fd)
{
    struct client_node *node = client_list;

    while (node != NULL) {
        if (node->client.fd == fd) {
            return &node->client;
        }

        node = node->next;
    }

    return NULL;
}

int client_add(int fd)
{
    struct client_node *node;

    if (client_find(fd) != NULL) {
        errno = EEXIST;
        return -1;
    }

    node = calloc(1, sizeof(*node));

    if (node == NULL) {
        return -1;
    }

    node->client.fd = fd;

    node->next = client_list;
    client_list = node;

    return 0;
}

void client_remove(int fd)
{
    struct client_node *node = client_list;
    struct client_node *previous = NULL;

    while (node != NULL) {

        if (node->client.fd == fd) {

            if (previous == NULL) {
                client_list = node->next;
            } else {
                previous->next = node->next;
            }

            close(node->client.fd);
            free(node);

            return;
        }

        previous = node;
        node = node->next;
    }
}

int client_handle_read(struct client *client)
{
    for (;;) {
        size_t available;
        ssize_t received;

        /*
         * Calculate the remaining space in the
         * client's receive buffer.
         */
        available = CLIENT_RX_BUFFER_SIZE - client->rx_len;

        /*
         * Append newly received bytes after the data
         * already stored in the RX buffer.
         */
        received = recv(client->fd,
                        client->rx_buffer + client->rx_len,
                        available,
                        0);

        if (received > 0) {
            client->rx_len += (size_t)received;

            /*
             * Process every complete newline-terminated
             * command currently stored in the RX buffer.
             */
            for (;;) {
                char *newline;
                char line[CLIENT_RX_BUFFER_SIZE];
                size_t line_len;
                size_t remaining;

                /*
                 * Search for the first complete command.
                 */
                newline = memchr(client->rx_buffer,
                                 '\n',
                                 client->rx_len);

                /*
                 * No complete command is available yet.
                 * Preserve the partial data for the next recv().
                 */
                if (newline == NULL) {
                    break;
                }

                /*
                 * Calculate the command length without
                 * including the newline character.
                 */
                line_len =
                    (size_t)(newline - client->rx_buffer);

                /*
                 * Copy the complete command into a
                 * null-terminated temporary string.
                 */
                memcpy(line,
                    client->rx_buffer,
                    line_len);

                line[line_len] = '\0';

                /*
                * Pass the complete command line to the
                * protocol layer for parsing.
                */
                {
                    parsed_cmd_t cmd;

                    (void)parse_command_line(line, &cmd);
                }

                remaining =
                    client->rx_len - (line_len + 1);

                memmove(client->rx_buffer,
                        newline + 1,
                        remaining);

                client->rx_len = remaining;
            }

            /*
             * If the RX buffer is completely full and
             * no newline was found, the client exceeded
             * the maximum allowed command size.
             */
            if (client->rx_len == CLIENT_RX_BUFFER_SIZE) {
                errno = EMSGSIZE;
                return -1;
            }

            continue;
        }

        /*
         * recv() returning zero means that the peer
         * closed the TCP connection.
         */
        if (received == 0) {
            return 1;
        }

        /*
         * Retry recv() if it was interrupted by a signal.
         */
        if (errno == EINTR) {
            continue;
        }

        /*
         * No more data is currently available on the
         * non-blocking socket.
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        /*
         * Any other recv() error is treated as a
         * client read failure.
         */
        return -1;
    }
}
