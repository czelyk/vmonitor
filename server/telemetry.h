#ifndef TELEMETRY_H
#define TELEMETRY_H

int telemetry_init(void);
void telemetry_send_status(void);
void telemetry_close(void);

#endif