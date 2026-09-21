#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>

#define CLIENT_RX_BUFFER_SIZE 4096
#define CLIENT_TX_BUFFER_SIZE (64 * 1024)

/*
 * Maximum number of simultaneously connected
 * TCP clients.
 */
#define CLIENT_MAX_COUNT 64

struct client {
    int fd;

    /*
     * Per-client receive buffer.
     *
     * Partial TCP command data is preserved between
     * recv() calls until a complete newline-terminated
     * command is available.
     */
    char rx_buffer[CLIENT_RX_BUFFER_SIZE];
    size_t rx_len;

    /*
     * Per-client transmit buffer.
     *
     * tx_len:
     *     total number of valid bytes currently stored
     *
     * tx_sent:
     *     number of bytes already successfully sent
     *
     * Unsent range:
     *
     *     [tx_sent, tx_len)
     */
    char tx_buffer[CLIENT_TX_BUFFER_SIZE];
    size_t tx_len;
    size_t tx_sent;

    /*
     * WATCH state.
     *
     * The field lives in the per-client state, but
     * WATCH command behaviour is implemented by the
     * protocol/integration layer.
     */
    int watching;
};

/*
 * Client lifecycle.
 */
int client_add(int fd);
int client_remove(int fd);

struct client *client_find(int fd);

/*
 * Number of currently active clients.
 */
size_t client_count(void);

/*
 * Returns non-zero when CLIENT_MAX_COUNT has
 * already been reached.
 */
int client_limit_reached(void);

/*
 * Receive and process currently available TCP data.
 *
 * Return convention:
 *
 *   0  -> success / EAGAIN
 *   1  -> peer closed connection
 *  -1  -> fatal error
 */
int client_handle_read(struct client *client);

/*
 * Flush as much pending TX data as possible.
 *
 * Returns:
 *
 *   0  -> success, including EAGAIN
 *  -1  -> fatal send error
 */
int client_handle_write(struct client *client);

/*
 * Queue bytes into the client's TX buffer.
 *
 * Returns:
 *
 *   0  -> queued successfully
 *  -1  -> invalid argument or insufficient buffer
 */
int client_queue_tx(struct client *client,
                    const void *data,
                    size_t length);

/*
 * Returns non-zero while unsent bytes remain.
 */
int client_has_pending_tx(const struct client *client);

#endif
