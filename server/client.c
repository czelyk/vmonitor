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
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        size_t available;
        ssize_t received;

        /*
         * A completely full buffer with no complete
         * command means the client exceeded the
         * maximum command size.
         */
        if (client->rx_len >= CLIENT_RX_BUFFER_SIZE) {
            errno = EMSGSIZE;
            return -1;
        }

        available =
            CLIENT_RX_BUFFER_SIZE - client->rx_len;

        /*
         * Append incoming bytes after any partial
         * command already stored in the RX buffer.
         */
        received = recv(client->fd,
                        client->rx_buffer + client->rx_len,
                        available,
                        0);

        if (received > 0) {
            client->rx_len += (size_t)received;

            /*
             * Process every complete newline-terminated
             * command currently stored in the buffer.
             */
            for (;;) {
                char *newline;
                char line[CLIENT_RX_BUFFER_SIZE];

                size_t line_len;
                size_t remaining;

                newline = memchr(client->rx_buffer,
                                 '\n',
                                 client->rx_len);

                /*
                 * No full line yet.
                 * Preserve partial command for the next recv().
                 */
                if (newline == NULL) {
                    break;
                }

                line_len =
                    (size_t)(newline - client->rx_buffer);

                memcpy(line,
                       client->rx_buffer,
                       line_len);

                line[line_len] = '\0';

                /*
                 * Pass complete command to protocol parser.
                 *
                 * Actual protocol command execution /
                 * response generation is handled separately.
                 */
                {
                    parsed_cmd_t cmd;

                    (void)parse_command_line(line, &cmd);
                }

                /*
                 * Remove processed command from RX buffer.
                 */
                remaining =
                    client->rx_len - (line_len + 1);

                memmove(client->rx_buffer,
                        newline + 1,
                        remaining);

                client->rx_len = remaining;
            }

            /*
             * Buffer became full but still contains no
             * complete newline-terminated command.
             */
            if (client->rx_len == CLIENT_RX_BUFFER_SIZE) {
                errno = EMSGSIZE;
                return -1;
            }

            /*
             * Continue recv() until the non-blocking
             * socket reports EAGAIN/EWOULDBLOCK.
             */
            continue;
        }

        /*
         * Peer performed an orderly shutdown.
         */
        if (received == 0) {
            return 1;
        }

        /*
         * Interrupted system call: retry.
         */
        if (errno == EINTR) {
            continue;
        }

        /*
         * No more data available for now.
         */
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK) {

            return 0;
        }

        /*
         * Fatal recv() error.
         */
        return -1;
    }
}

int client_has_pending_tx(const struct client *client)
{
    if (client == NULL) {
        return 0;
    }

    return client->tx_sent < client->tx_len;
}

int client_queue_tx(struct client *client,
                    const void *data,
                    size_t length)
{
    size_t pending;

    if (client == NULL ||
        (data == NULL && length != 0)) {

        errno = EINVAL;
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    /*
     * Validate TX state before using it.
     */
    if (client->tx_sent > client->tx_len ||
        client->tx_len > CLIENT_TX_BUFFER_SIZE) {

        errno = EINVAL;
        return -1;
    }

    pending =
        client->tx_len - client->tx_sent;

    /*
     * Slow-client policy:
     *
     * Never allow more than 64 KiB of pending output.
     */
    if (length >
        CLIENT_TX_BUFFER_SIZE - pending) {

        errno = ENOBUFS;
        return -1;
    }

    /*
     * Reclaim space occupied by bytes that have
     * already been sent.
     */
    if (client->tx_sent > 0) {

        memmove(client->tx_buffer,
                client->tx_buffer + client->tx_sent,
                pending);

        client->tx_len = pending;
        client->tx_sent = 0;
    }

    /*
     * Append new data to the pending TX queue.
     */
    memcpy(client->tx_buffer + client->tx_len,
           data,
           length);

    client->tx_len += length;

    return 0;
}

int client_handle_write(struct client *client)
{
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (client->tx_sent > client->tx_len ||
        client->tx_len > CLIENT_TX_BUFFER_SIZE) {

        errno = EINVAL;
        return -1;
    }

    /*
     * Try to flush as much pending data as possible.
     */
    while (client->tx_sent < client->tx_len) {
        ssize_t sent;

        sent = send(client->fd,
                    client->tx_buffer + client->tx_sent,
                    client->tx_len - client->tx_sent,
                    MSG_NOSIGNAL);

        if (sent > 0) {
            client->tx_sent += (size_t)sent;
            continue;
        }

        if (sent == 0) {
            errno = EPIPE;
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        /*
         * Healthy non-blocking socket, but its send
         * buffer is currently full.
         *
         * Keep unsent bytes queued and wait for
         * another EPOLLOUT notification.
         */
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK) {

            return 0;
        }

        /*
         * Fatal send() error.
         */
        return -1;
    }

    /*
     * All queued bytes have been transmitted.
     */
    client->tx_len = 0;
    client->tx_sent = 0;

    return 0;
}