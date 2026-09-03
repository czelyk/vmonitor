#ifndef VMONITOR_UAPI_H
#define VMONITOR_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define VMONITOR_IOC_MAGIC 'V'

struct vmonitor_sample {
    __u64 seq;
    __u64 timestamp_ns;
    __s32 value_mC;
    __u32 alarm;
};

struct vmonitor_status {
    __u32 running;
    __u32 period_ms;
    __s32 threshold_mC;

    __u64 produced_total;
    __u64 enqueued_total;
    __u64 dropped_total;
    __u64 read_total;

    __u32 queued;

    __u64 last_seq;
    __s32 last_value_mC;
    __u32 last_alarm;
};

#define VMONITOR_IOC_START       _IO(VMONITOR_IOC_MAGIC, 1)
#define VMONITOR_IOC_STOP        _IO(VMONITOR_IOC_MAGIC, 2)
#define VMONITOR_IOC_GET_STATUS  _IOR(VMONITOR_IOC_MAGIC, 3, struct vmonitor_status)

#endif
