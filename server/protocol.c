#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <errno.h>

#include "../include/vmonitor_uapi.h"
#include "protocol.h"

// Default sysfs node paths created by vmonitor.ko
#define SYSFS_PERIOD_PATH    "/sys/class/vmonitor/vmonitor_device/period_ms"
#define SYSFS_THRESHOLD_PATH "/sys/class/vmonitor/vmonitor_device/threshold_mC"

/*
 * Helper function to write an integer to a sysfs attribute.
 * Opens, writes, and closes immediately to avoid blocking the event loop.
 */
static int write_sysfs_attribute(const char *path, int32_t val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", val);
    if (len <= 0) {
        close(fd);
        return -1;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    close(fd);

    return (written == len) ? 0 : -1;
}

/*
 * Parses a single null-terminated command line.
 * Format: <id> <COMMAND> [arg]
 * Returns: 0 on success, -1 on malformed or unknown syntax.
 */
int parse_command_line(const char *line, parsed_cmd_t *cmd)
{
    if (!line || !cmd) {
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));

    char cmd_token[32] = {0};
    char extra_garbage[32] = {0};

    // Scan ID and command name
    int matched = sscanf(line, "%u %31s", &cmd->id, cmd_token);
    if (matched < 2) {
        return -1;
    }

    // Convert command token to uppercase for strict case-insensitive handling
    for (int i = 0; cmd_token[i] != '\0'; i++) {
        cmd_token[i] = (char)toupper((unsigned char)cmd_token[i]);
    }

    // Identify commands and validate required arguments
    if (strcmp(cmd_token, "GET") == 0) {
        cmd->type = CMD_GET;
        if (sscanf(line, "%*u %*s %31s", extra_garbage) == 1) {
            return -1; // GET takes no arguments
        }
    } else if (strcmp(cmd_token, "START") == 0) {
        cmd->type = CMD_START;
        if (sscanf(line, "%*u %*s %31s", extra_garbage) == 1) {
            return -1; // START takes no arguments
        }
    } else if (strcmp(cmd_token, "STOP") == 0) {
        cmd->type = CMD_STOP;
        if (sscanf(line, "%*u %*s %31s", extra_garbage) == 1) {
            return -1; // STOP takes no arguments
        }
    } else if (strcmp(cmd_token, "PERIOD") == 0) {
        cmd->type = CMD_PERIOD;
        if (sscanf(line, "%*u %*s %d %31s", &cmd->arg, extra_garbage) != 1) {
            return -1; // Exactly one integer argument required
        }
        if (cmd->arg <= 0) {
            return -1; // Period must be strictly positive
        }
        cmd->has_arg = true;
    } else if (strcmp(cmd_token, "THRESHOLD") == 0) {
        cmd->type = CMD_THRESHOLD;
        if (sscanf(line, "%*u %*s %d %31s", &cmd->arg, extra_garbage) != 1) {
            return -1; // Exactly one integer argument required
        }
        cmd->has_arg = true;
    } else if (strcmp(cmd_token, "INJECT") == 0) {
        cmd->type = CMD_INJECT;
        if (sscanf(line, "%*u %*s %d %31s", &cmd->arg, extra_garbage) != 1) {
            return -1; // Exactly one integer argument required
        }
        cmd->has_arg = true;
    } else {
        cmd->type = CMD_UNKNOWN;
        return -1;
    }

    return 0;
}

/*
 * Dispatches the parsed command to the kernel driver or sysfs,
 * and sends the formatted RSP string back to client_fd.
 */
int handle_protocol_command(int client_fd, int vmonitor_fd, const parsed_cmd_t *cmd)
{
    if (!cmd) {
        return -1;
    }

    char response[512] = {0};
    struct vmonitor_status status;
    struct vmonitor_sample manual_sample;

    switch (cmd->type) {
        case CMD_START:
            // Send START ioctl to driver (defined in vmonitor_uapi.h)
            if (ioctl(vmonitor_fd, VMONITOR_IOC_START) < 0) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                snprintf(response, sizeof(response), "RSP %u OK\n", cmd->id);
            }
            break;

        case CMD_STOP:
            // Send STOP ioctl to driver (defined in vmonitor_uapi.h)
            if (ioctl(vmonitor_fd, VMONITOR_IOC_STOP) < 0) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                snprintf(response, sizeof(response), "RSP %u OK\n", cmd->id);
            }
            break;

        case CMD_GET:
            // Read driver statistics via ioctl
            memset(&status, 0, sizeof(status));
            if (ioctl(vmonitor_fd, VMONITOR_IOC_GET_STATUS, &status) < 0) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                // Formatting matches project specifications and test vectors
                snprintf(response, sizeof(response),
                         "RSP %u OK running=%u period_ms=%u threshold_mC=%d "
                         "produced_total=%llu enqueued_total=%llu dropped_total=%llu "
                         "read_total=%llu queued=%u last_seq=%llu last_value_mC=%d\n",
                         cmd->id, status.running, status.period_ms, status.threshold_mC,
                         (unsigned long long)status.produced_total,
                         (unsigned long long)status.enqueued_total,
                         (unsigned long long)status.dropped_total,
                         (unsigned long long)status.read_total,
                         status.queued,
                         (unsigned long long)status.last_seq,
                         status.last_value_mC);
            }
            break;

        case CMD_INJECT:
            // Manual data injection: Driver expects exactly 24 bytes (struct vmonitor_sample)
            memset(&manual_sample, 0, sizeof(manual_sample));
            manual_sample.value_mC = cmd->arg;

            if (write(vmonitor_fd, &manual_sample, sizeof(manual_sample)) != sizeof(manual_sample)) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                snprintf(response, sizeof(response), "RSP %u OK\n", cmd->id);
            }
            break;

        case CMD_PERIOD:
            if (write_sysfs_attribute(SYSFS_PERIOD_PATH, cmd->arg) < 0) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                snprintf(response, sizeof(response), "RSP %u OK\n", cmd->id);
            }
            break;

        case CMD_THRESHOLD:
            if (write_sysfs_attribute(SYSFS_THRESHOLD_PATH, cmd->arg) < 0) {
                snprintf(response, sizeof(response), "RSP %u ERR EIO\n", cmd->id);
            } else {
                snprintf(response, sizeof(response), "RSP %u OK\n", cmd->id);
            }
            break;

        default:
            snprintf(response, sizeof(response), "RSP %u ERR ENOTSUP\n", cmd->id);
            break;
    }

    // Direct transmission for this task; output buffer queuing is handled in next milestones
    ssize_t sent = send(client_fd, response, strlen(response), 0);
    return (sent >= 0) ? 0 : -1;
}