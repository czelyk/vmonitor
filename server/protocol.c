#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "protocol.h"

int parse_command_line(const char *line, parsed_cmd_t *cmd)
{
    if (!line || !cmd) {
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));

    char id_token[32] = {0};
    char cmd_token[32] = {0};
    char extra[32] = {0};
    int offset = 0;

    // 1. Read ID and Command token; record scanned character offset via %n
    if (sscanf(line, "%31s %31s%n", id_token, cmd_token, &offset) < 2) {
        return -1;
    }

    // 2. Reject negative signs ('-') or non-digit characters in the ID token
    for (int i = 0; id_token[i] != '\0'; i++) {
        if (!isdigit((unsigned char)id_token[i])) {
            return -1;
        }
    }

    char *endptr = NULL;
    unsigned long id_val = strtoul(id_token, &endptr, 10);
    if (*endptr != '\0' || id_val > UINT32_MAX) {
        return -1;
    }
    cmd->id = (uint32_t)id_val;

    // 3. Convert command token to uppercase
    for (int i = 0; cmd_token[i] != '\0'; i++) {
        cmd_token[i] = (char)toupper((unsigned char)cmd_token[i]);
    }

    const char *rest = line + offset;

    // 4. Zero-argument commands: Ensure no trailing arguments exist
    if (strcmp(cmd_token, "GET") == 0) {
        cmd->type = CMD_GET;
        if (sscanf(rest, "%31s", extra) == 1) {
            return -1;
        }
    } else if (strcmp(cmd_token, "START") == 0) {
        cmd->type = CMD_START;
        if (sscanf(rest, "%31s", extra) == 1) {
            return -1;
        }
    } else if (strcmp(cmd_token, "STOP") == 0) {
        cmd->type = CMD_STOP;
        if (sscanf(rest, "%31s", extra) == 1) {
            return -1;
        }
    }
    // 5. Single-argument commands: Validate exactly one parameter and reject trailing tokens
    else if (strcmp(cmd_token, "PERIOD") == 0) {
        cmd->type = CMD_PERIOD;
        if (sscanf(rest, "%d %31s", &cmd->arg, extra) != 1 || cmd->arg <= 0) {
            return -1;
        }
        cmd->has_arg = true;
    } else if (strcmp(cmd_token, "THRESHOLD") == 0) {
        cmd->type = CMD_THRESHOLD;
        if (sscanf(rest, "%d %31s", &cmd->arg, extra) != 1) {
            return -1;
        }
        cmd->has_arg = true;
    } else if (strcmp(cmd_token, "INJECT") == 0) {
        cmd->type = CMD_INJECT;
        if (sscanf(rest, "%d %31s", &cmd->arg, extra) != 1) {
            return -1;
        }
        cmd->has_arg = true;
    } else if (strcmp(cmd_token, "WATCH") == 0) {
        cmd->type = CMD_WATCH;
        if (sscanf(rest, "%d %31s", &cmd->arg, extra) != 1) {
            return -1;
        }
        // WATCH requires 0 (unsubscribe) or 1 (subscribe)
        if (cmd->arg != 0 && cmd->arg != 1) {
            return -1;
        }
        cmd->has_arg = true;
    } else {
        cmd->type = CMD_UNKNOWN;
        return -1;
    }

    return 0;
}