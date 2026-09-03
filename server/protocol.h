#ifndef PROTOCOL_H
#define PROTOCOL_H

struct client;

int handle_command(struct client *client, const char *line);

#endif