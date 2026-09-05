#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include "client.h"

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
    char buffer[1024];

    for (;;) {
        ssize_t received;

        received = recv(client->fd,
                        buffer,
                        sizeof(buffer),
                        0);

        if (received > 0) {
            /*
             * Client data is intentionally discarded
             * in this task.
             *
             * RX buffering and command parsing will
             * be implemented in a later task.
             */
            continue;
        }

        if (received == 0) {
            /*
             * recv() == 0 means the peer closed
             * the TCP connection.
             */
            return 1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        return -1;
    }
}