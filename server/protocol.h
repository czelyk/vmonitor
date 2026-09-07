#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// Supported protocol command types
typedef enum {
    CMD_UNKNOWN = 0,
    CMD_GET,
    CMD_START,
    CMD_STOP,
    CMD_PERIOD,
    CMD_THRESHOLD,
    CMD_INJECT,
    CMD_WATCH
} cmd_type_t;

// Structure holding parsed command data
typedef struct {
    uint32_t id;        // Request ID (<id>)
    cmd_type_t type;    // Parsed command enum
    int32_t arg;        // Argument for PERIOD, THRESHOLD, INJECT, WATCH
    bool has_arg;       // True if an argument was parsed
} parsed_cmd_t;

/*
 * Parses a single ASCII command line.
 * Format: <id> <COMMAND> [arg]
 * Returns: 0 on success, -1 on syntax error or unsupported command.
 */
int parse_command_line(const char *line, parsed_cmd_t *cmd);

#endif // PROTOCOL_H