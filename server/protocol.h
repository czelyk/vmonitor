#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/vmonitor_uapi.h"

// Command enumeration for protocol commands
typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GET,
    CMD_START,
    CMD_STOP,
    CMD_PERIOD,
    CMD_THRESHOLD,
    CMD_INJECT
} cmd_type_t;

// Structure to hold parsed command parameters
typedef struct {
    uint32_t id;        // Request sequence ID preserved across RSP messages
    cmd_type_t type;    // Parsed command enum
    int32_t arg;        // Argument value for PERIOD, THRESHOLD, INJECT
    bool has_arg;       // True if an argument was parsed
} parsed_cmd_t;

// Function prototypes
int parse_command_line(const char *line, parsed_cmd_t *cmd);
int handle_protocol_command(int client_fd, int vmonitor_fd, const parsed_cmd_t *cmd);

#endif // PROTOCOL_H