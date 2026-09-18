// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "vmonitor_uapi.h"

#define DEVICE_PATH "/dev/vmonitor"

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s read\n"
            "  %s inject <temperature_mC>\n"
            "  %s start\n"
            "  %s stop\n"
            "  %s status\n",
            program,
            program,
            program,
            program,
            program);
}

static int open_device(void)
{
    int fd;

    fd = open(DEVICE_PATH, O_RDWR);

    if (fd < 0) {
        fprintf(stderr,
                "Failed to open %s: %s\n",
                DEVICE_PATH,
                strerror(errno));
        return -1;
    }

    return fd;
}

static int command_read(int fd)
{
    struct vmonitor_sample sample;
    ssize_t result;

    memset(&sample, 0, sizeof(sample));

    result = read(fd, &sample, sizeof(sample));

    if (result < 0) {
        fprintf(stderr,
                "read() failed: %s\n",
                strerror(errno));
        return 1;
    }

    if (result == 0) {
        printf("No sample available.\n");
        return 0;
    }

    if ((size_t)result != sizeof(sample)) {
        fprintf(stderr,
                "Unexpected read size: %zd bytes "
                "(expected %zu bytes)\n",
                result,
                sizeof(sample));
        return 1;
    }

    printf("Sample:\n");
    printf("  seq          : %llu\n",
           (unsigned long long)sample.seq);
    printf("  timestamp_ns : %llu\n",
           (unsigned long long)sample.timestamp_ns);
    printf("  value_mC     : %d\n",
           (int)sample.value_mC);
    printf("  alarm        : %u\n",
           (unsigned int)sample.alarm);

    return 0;
}

static int parse_temperature(const char *text, int32_t *value)
{
    char *end;
    long parsed;

    errno = 0;

    end = NULL;
    parsed = strtol(text, &end, 10);

    if (errno != 0) {
        fprintf(stderr,
                "Invalid temperature '%s': %s\n",
                text,
                strerror(errno));
        return -1;
    }

    if (end == text || *end != '\0') {
        fprintf(stderr,
                "Invalid temperature: %s\n",
                text);
        return -1;
    }

    if (parsed < INT32_MIN || parsed > INT32_MAX) {
        fprintf(stderr,
                "Temperature is outside int32 range: %s\n",
                text);
        return -1;
    }

    *value = (int32_t)parsed;

    return 0;
}

static int command_inject(int fd, const char *argument)
{
    struct vmonitor_sample sample;
    int32_t temperature;
    ssize_t result;

    if (parse_temperature(argument, &temperature) < 0)
        return 1;

    memset(&sample, 0, sizeof(sample));

    /*
     * Only value_mC is supplied by userspace.
     *
     * seq, timestamp_ns and alarm are driver-owned
     * and should be overwritten by the kernel driver.
     */
    sample.value_mC = temperature;

    result = write(fd, &sample, sizeof(sample));

    if (result < 0) {
        fprintf(stderr,
                "write() failed: %s\n",
                strerror(errno));
        return 1;
    }

    if ((size_t)result != sizeof(sample)) {
        fprintf(stderr,
                "Unexpected write size: %zd bytes "
                "(expected %zu bytes)\n",
                result,
                sizeof(sample));
        return 1;
    }

    printf("Injected temperature: %d mC\n",
           (int)temperature);

    return 0;
}

static int command_start(int fd)
{
    if (ioctl(fd, VMONITOR_IOC_START) < 0) {
        fprintf(stderr,
                "VMONITOR_IOC_START failed: %s\n",
                strerror(errno));
        return 1;
    }

    printf("Automatic sample generation started.\n");

    return 0;
}

static int command_stop(int fd)
{
    if (ioctl(fd, VMONITOR_IOC_STOP) < 0) {
        fprintf(stderr,
                "VMONITOR_IOC_STOP failed: %s\n",
                strerror(errno));
        return 1;
    }

    printf("Automatic sample generation stopped.\n");

    return 0;
}

static void print_status(const struct vmonitor_status *status)
{
    printf("Driver status:\n");

    printf("  running          : %u\n",
           (unsigned int)status->running);

    printf("  period_ms        : %u\n",
           (unsigned int)status->period_ms);

    printf("  threshold_mC     : %d\n",
           (int)status->threshold_mC);

    printf("  produced_total   : %llu\n",
           (unsigned long long)status->produced_total);

    printf("  enqueued_total   : %llu\n",
           (unsigned long long)status->enqueued_total);

    printf("  dropped_total    : %llu\n",
           (unsigned long long)status->dropped_total);

    printf("  read_total       : %llu\n",
           (unsigned long long)status->read_total);

    printf("  queued           : %u\n",
           (unsigned int)status->queued);

    printf("  last_seq         : %llu\n",
           (unsigned long long)status->last_seq);

    printf("  last_value_mC    : %d\n",
           (int)status->last_value_mC);

    printf("  last_alarm       : %u\n",
           (unsigned int)status->last_alarm);
}

static int command_status(int fd)
{
    struct vmonitor_status status;

    memset(&status, 0, sizeof(status));

    if (ioctl(fd,
              VMONITOR_IOC_GET_STATUS,
              &status) < 0) {
        fprintf(stderr,
                "VMONITOR_IOC_GET_STATUS failed: %s\n",
                strerror(errno));
        return 1;
    }

    print_status(&status);

    return 0;
}

int main(int argc, char *argv[])
{
    int fd;
    int result;

    /*
     * Compile-time check for the required UAPI sample size.
     */
    _Static_assert(sizeof(struct vmonitor_sample) == 24,
                   "struct vmonitor_sample must be exactly 24 bytes");

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "inject") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else {
        if (argc != 2) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (strcmp(argv[1], "read") != 0 &&
        strcmp(argv[1], "inject") != 0 &&
        strcmp(argv[1], "start") != 0 &&
        strcmp(argv[1], "stop") != 0 &&
        strcmp(argv[1], "status") != 0) {

        fprintf(stderr,
                "Unknown command: %s\n",
                argv[1]);

        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    fd = open_device();

    if (fd < 0)
        return EXIT_FAILURE;

    if (strcmp(argv[1], "read") == 0) {
        result = command_read(fd);

    } else if (strcmp(argv[1], "inject") == 0) {
        result = command_inject(fd, argv[2]);

    } else if (strcmp(argv[1], "start") == 0) {
        result = command_start(fd);

    } else if (strcmp(argv[1], "stop") == 0) {
        result = command_stop(fd);

    } else {
        result = command_status(fd);
    }

    if (close(fd) < 0) {
        fprintf(stderr,
                "close() failed: %s\n",
                strerror(errno));

        if (result == 0)
            result = 1;
    }

    return result == 0
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}