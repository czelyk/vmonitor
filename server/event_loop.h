#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "../include/vmonitor_uapi.h"

/*
 * Run the single-threaded epoll server.
 *
 * server_fd:
 *     non-blocking TCP listening socket
 *
 * driver_fd:
 *     non-blocking /dev/vmonitor descriptor
 */
int event_loop_run(int server_fd, int driver_fd);

/*
 * Returns the most recently consumed driver sample.
 *
 * Returns:
 *   0  -> success
 *  -1  -> no sample available / invalid argument
 */
int event_loop_get_latest_sample(struct vmonitor_sample *out);

#endif
