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
static size_t active_client_count = 0;

struct client *client_find(int fd)
{
    struct client_node *node = client_list;

    while (node != NULL) {
        if (node->client.fd == fd)
            return &node->client;

        node = node->next;
    }

    return NULL;
}

size_t client_count(void)
{
    return active_client_count;
}

int client_limit_reached(void)
{
    return active_client_count >= CLIENT_MAX_COUNT;
}

int client_add(int fd)
{
    struct client_node *node;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (client_find(fd) != NULL) {
        errno = EEXIST;
        return -1;
    }

    if (client_limit_reached()) {
        errno = ENOSPC;
        return -1;
    }

    node = calloc(1, sizeof(*node));

    if (node == NULL)
        return -1;

    node->client.fd = fd;

    node->next = client_list;
    client_list = node;

    active_client_count++;

    return 0;
}

int client_remove(int fd)
{
    struct client_node *node = client_list;
    struct client_node *previous = NULL;

    while (node != NULL) {
        if (node->client.fd == fd) {
            if (previous == NULL)
                client_list = node->next;
            else
                previous->next = node->next;

            /*
             * Unlink the node before closing/freeing it.
             * This prevents a stale entry from remaining
             * visible in the client list.
             */
            if (active_client_count > 0)
                active_client_count--;

            close(node->client.fd);
            free(node);

            return 0;
        }

        previous = node;
        node = node->next;
    }

    /*
     * The client is already gone.
     *
     * Do not close(fd) here: the descriptor number may
     * already have been reused for another resource.
     */
    errno = ENOENT;
    return -1;
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

        /*
         * Retry recv() if it was interrupted by a signal.
         */
        if (errno == EINTR) {
            continue;

        /*
         * No more data is currently available on the
         * non-blocking socket.
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;

        return -1;
    }
}

int client_has_pending_tx(const struct client *client)
{
    if (client == NULL)
        return 0;

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

    if (length == 0)
        return 0;

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
     * Never allow more than CLIENT_TX_BUFFER_SIZE
     * bytes of pending output.
     */
    if (length >
        CLIENT_TX_BUFFER_SIZE - pending) {

        errno = ENOBUFS;
        return -1;
    }

    /*
     * Reclaim already-sent space before appending.
     */
    if (client->tx_sent > 0) {
        memmove(client->tx_buffer,
                client->tx_buffer + client->tx_sent,
                pending);

        client->tx_len = pending;
        client->tx_sent = 0;
    }

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

        /*
         * Any other recv() error is treated as a
         * client read failure.
         */
        return -1;
    }
}
