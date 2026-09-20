#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>

#define CLIENT_RX_BUFFER_SIZE 4096
#define CLIENT_TX_BUFFER_SIZE (64 * 1024)

/*
 * Maximum number of simultaneously connected TCP clients.
 */
#define CLIENT_MAX_COUNT 64

struct client {
    int fd;

    char rx_buffer[CLIENT_RX_BUFFER_SIZE];
    size_t rx_len;

    char tx_buffer[CLIENT_TX_BUFFER_SIZE];
    size_t tx_len;
    size_t tx_sent;

    int watching;
};

/*
 * Client lifecycle.
 */
int client_add(int fd);
int client_remove(int fd);
struct client *client_find(int fd);

size_t client_count(void);
int client_limit_reached(void);

/*
 * RX handling.
 *
 * Returns:
 *   0  -> success / EAGAIN
 *   1  -> peer closed connection
 *  -1  -> fatal recv error / oversized command
 */
int client_handle_read(struct client *client);

/*
 * Queue data for non-blocking transmission.
 *
 * Returns:
 *   0  -> success
 *  -1  -> error / TX buffer overflow
 */
int client_queue_tx(struct client *client,
                    const void *data,
                    size_t length);

/*
 * Try to flush pending TX data.
 *
 * Returns:
 *   0  -> success or EAGAIN/EWOULDBLOCK
 *  -1  -> fatal send error
 */
int client_handle_write(struct client *client);

int client_has_pending_tx(const struct client *client);

#endif
