#ifndef CLIENT_H
#define CLIENT_H

#define CLIENT_RX_BUFFER_SIZE 4096
#define CLIENT_TX_BUFFER_SIZE (64 * 1024)

struct client {
    int fd;

    char rx_buffer[CLIENT_RX_BUFFER_SIZE];
    size_t rx_len;

    char tx_buffer[CLIENT_TX_BUFFER_SIZE];
    size_t tx_len;
    size_t tx_sent;

    int watching;
};

int client_add(int fd);
void client_remove(int fd);
int client_handle_read(struct client *client);
int client_handle_write(struct client *client);

#endif