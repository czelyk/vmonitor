// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ioctl.h>

#include "vmonitor_uapi.h"

#define DRIVER_NAME "vmonitor"
#define QUEUE_CAPACITY 64

#define DEFAULT_THRESHOLD_MC 35000
#define DEFAULT_PERIOD_MS 1000

#define TEMP_MIN_MC 20000
#define TEMP_MAX_MC 40000
#define TEMP_STEP_MC 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION(
    "Virtual temperature monitor - character device, queue, timer and ioctl");

struct vmonitor_queue {
    struct vmonitor_sample items[QUEUE_CAPACITY];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
};

struct vmonitor_dev {
    struct cdev cdev;

    atomic_t opened;

    spinlock_t lock;
    struct mutex read_lock;

    struct vmonitor_queue queue;

    __u64 seq_counter;

    s32 threshold_mC;
    u32 period_ms;

    s32 generated_value_mC;

    struct timer_list sample_timer;
    bool running;

    u64 produced_total;
    u64 enqueued_total;
    u64 dropped_total;
    u64 read_total;

    u64 last_seq;
    s32 last_value_mC;
    u32 last_alarm;
};

static struct vmonitor_dev *vdev;

static dev_t vmonitor_devno;
static struct class *vmonitor_class;
static struct device *vmonitor_device;

/* ------------------------------------------------------------------------- */
/* Queue helpers                                                             */
/* Caller must hold dev->lock.                                               */
/* ------------------------------------------------------------------------- */

static bool queue_is_full(struct vmonitor_queue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static bool queue_is_empty(struct vmonitor_queue *q)
{
    return q->count == 0;
}

static bool queue_push(struct vmonitor_queue *q,
                       const struct vmonitor_sample *sample)
{
    if (queue_is_full(q))
        return false;

    q->items[q->tail] = *sample;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    return true;
}

static bool queue_peek(struct vmonitor_queue *q,
                       struct vmonitor_sample *sample)
{
    if (queue_is_empty(q))
        return false;

    *sample = q->items[q->head];

    return true;
}

static void queue_advance(struct vmonitor_queue *q)
{
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
}

/* ------------------------------------------------------------------------- */
/* Sample production                                                         */
/* Caller must hold dev->lock.                                               */
/* ------------------------------------------------------------------------- */

static void produce_sample_locked(struct vmonitor_dev *dev,
                                  s32 value_mC)
{
    struct vmonitor_sample sample;
    bool pushed;

    memset(&sample, 0, sizeof(sample));

    /*
     * Assign seq and timestamp while holding the same lock.
     * This keeps sequence/timestamp ordering consistent between
     * timer-generated and manually injected samples.
     */
    sample.seq = ++dev->seq_counter;
    sample.timestamp_ns = ktime_get_ns();
    sample.value_mC = value_mC;
    sample.alarm =
        (value_mC >= dev->threshold_mC) ? 1 : 0;

    dev->produced_total++;

    /*
     * "Last" describes the last sample produced, even if the
     * queue is full and that sample has to be dropped.
     */
    dev->last_seq = sample.seq;
    dev->last_value_mC = sample.value_mC;
    dev->last_alarm = sample.alarm;

    pushed = queue_push(&dev->queue, &sample);

    if (pushed)
        dev->enqueued_total++;
    else
        dev->dropped_total++;
}

/* ------------------------------------------------------------------------- */
/* Timer                                                                     */
/* ------------------------------------------------------------------------- */

static void vmonitor_timer_callback(struct timer_list *timer)
{
    struct vmonitor_dev *dev;
    unsigned long flags;
    u32 period_ms;
    bool rearm;

    dev = from_timer(dev, timer, sample_timer);

    spin_lock_irqsave(&dev->lock, flags);

    /*
     * STOP may have raced with a callback that was already scheduled.
     * If the driver is no longer running, do not generate or rearm.
     */
    if (!dev->running) {
        spin_unlock_irqrestore(&dev->lock, flags);
        return;
    }

    produce_sample_locked(dev, dev->generated_value_mC);

    if (dev->generated_value_mC >= TEMP_MAX_MC)
        dev->generated_value_mC = TEMP_MIN_MC;
    else
        dev->generated_value_mC += TEMP_STEP_MC;

    period_ms = dev->period_ms;
    rearm = dev->running;

    spin_unlock_irqrestore(&dev->lock, flags);

    if (rearm)
        mod_timer(&dev->sample_timer,
                  jiffies + msecs_to_jiffies(period_ms));
}

/* ------------------------------------------------------------------------- */
/* File operations                                                           */
/* ------------------------------------------------------------------------- */

static int vmonitor_open(struct inode *inode, struct file *filp)
{
    if (atomic_cmpxchg(&vdev->opened, 0, 1) != 0)
        return -EBUSY;

    filp->private_data = vdev;

    return 0;
}

static int vmonitor_release(struct inode *inode, struct file *filp)
{
    struct vmonitor_dev *dev = filp->private_data;

    atomic_set(&dev->opened, 0);

    return 0;
}

static ssize_t vmonitor_read(struct file *filp,
                             char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;
    unsigned long flags;
    bool available;
    ssize_t ret;

    if (count != sizeof(sample))
        return -EINVAL;

    if (mutex_lock_interruptible(&dev->read_lock))
        return -ERESTARTSYS;

    spin_lock_irqsave(&dev->lock, flags);

    available = queue_peek(&dev->queue, &sample);

    spin_unlock_irqrestore(&dev->lock, flags);

    if (!available) {
        ret = 0;
        goto out_unlock;
    }

    /*
     * copy_to_user() may sleep, therefore the spinlock must not
     * be held while copying.
     */
    if (copy_to_user(buf, &sample, sizeof(sample))) {
        ret = -EFAULT;
        goto out_unlock;
    }

    spin_lock_irqsave(&dev->lock, flags);

    queue_advance(&dev->queue);
    dev->read_total++;

    spin_unlock_irqrestore(&dev->lock, flags);

    ret = sizeof(sample);

out_unlock:
    mutex_unlock(&dev->read_lock);

    return ret;
}

static ssize_t vmonitor_write(struct file *filp,
                              const char __user *buf,
                              size_t count,
                              loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample input;
    unsigned long flags;

    if (count != sizeof(input))
        return -EINVAL;

    if (copy_from_user(&input, buf, sizeof(input)))
        return -EFAULT;

    /*
     * Only value_mC is accepted from userspace.
     * All other fields are assigned by the driver.
     */
    spin_lock_irqsave(&dev->lock, flags);

    produce_sample_locked(dev, input.value_mC);

    spin_unlock_irqrestore(&dev->lock, flags);

    /*
     * Per specification, dropping the new sample because the
     * queue is full does not make write() itself fail.
     */
    return sizeof(input);
}

/* ------------------------------------------------------------------------- */
/* ioctl                                                                     */
/* ------------------------------------------------------------------------- */

static long vmonitor_ioctl(struct file *filp,
                           unsigned int cmd,
                           unsigned long arg)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_status status;
    unsigned long flags;
    bool should_start;
    bool should_stop;
    u32 period_ms;

    /*
     * Commands outside our ioctl namespace are not supported.
     */
    if (_IOC_TYPE(cmd) != VMONITOR_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case VMONITOR_IOC_START:
        should_start = false;
        period_ms = 0;

        spin_lock_irqsave(&dev->lock, flags);

        /*
         * START is idempotent.
         */
        if (!dev->running) {
            dev->running = true;
            period_ms = dev->period_ms;
            should_start = true;
        }

        spin_unlock_irqrestore(&dev->lock, flags);

        if (should_start)
            mod_timer(&dev->sample_timer,
                      jiffies + msecs_to_jiffies(period_ms));

        return 0;

    case VMONITOR_IOC_STOP:
        should_stop = false;

        spin_lock_irqsave(&dev->lock, flags);

        /*
         * STOP is idempotent.
         */
        if (dev->running) {
            dev->running = false;
            should_stop = true;
        }

        spin_unlock_irqrestore(&dev->lock, flags);

        /*
         * del_timer_sync() may wait for a running callback,
         * so it must never be called while holding dev->lock.
         */
        if (should_stop)
            del_timer_sync(&dev->sample_timer);

        return 0;

    case VMONITOR_IOC_GET_STATUS:
        memset(&status, 0, sizeof(status));

        /*
         * Take one consistent snapshot while holding the lock.
         * copy_to_user() is deliberately performed afterwards.
         */
        spin_lock_irqsave(&dev->lock, flags);

        status.running = dev->running ? 1 : 0;
        status.period_ms = dev->period_ms;
        status.threshold_mC = dev->threshold_mC;

        status.produced_total = dev->produced_total;
        status.enqueued_total = dev->enqueued_total;
        status.dropped_total = dev->dropped_total;
        status.read_total = dev->read_total;

        status.queued = dev->queue.count;

        status.last_seq = dev->last_seq;
        status.last_value_mC = dev->last_value_mC;
        status.last_alarm = dev->last_alarm;

        spin_unlock_irqrestore(&dev->lock, flags);

        if (copy_to_user((void __user *)arg,
                         &status,
                         sizeof(status)))
            return -EFAULT;

        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations vmonitor_fops = {
    .owner = THIS_MODULE,
    .open = vmonitor_open,
    .release = vmonitor_release,
    .read = vmonitor_read,
    .write = vmonitor_write,
    .unlocked_ioctl = vmonitor_ioctl,
};

/* ------------------------------------------------------------------------- */
/* Module initialization                                                     */
/* ------------------------------------------------------------------------- */

static int __init vmonitor_init(void)
{
    int ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);

    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);

    spin_lock_init(&vdev->lock);
    mutex_init(&vdev->read_lock);

    vdev->threshold_mC = DEFAULT_THRESHOLD_MC;
    vdev->period_ms = DEFAULT_PERIOD_MS;
    vdev->generated_value_mC = TEMP_MIN_MC;

    /*
     * Automatic generation starts when the module is loaded.
     */
    vdev->running = true;

    timer_setup(&vdev->sample_timer,
                vmonitor_timer_callback,
                0);

    ret = alloc_chrdev_region(&vmonitor_devno,
                              0,
                              1,
                              DRIVER_NAME);

    if (ret < 0)
        goto err_free_dev;

    cdev_init(&vdev->cdev, &vmonitor_fops);
    vdev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&vdev->cdev,
                   vmonitor_devno,
                   1);

    if (ret < 0)
        goto err_unregister;

    vmonitor_class = class_create(DRIVER_NAME);

    if (IS_ERR(vmonitor_class)) {
        ret = PTR_ERR(vmonitor_class);
        goto err_cdev_del;
    }

    vmonitor_device = device_create(vmonitor_class,
                                     NULL,
                                     vmonitor_devno,
                                     NULL,
                                     DRIVER_NAME);

    if (IS_ERR(vmonitor_device)) {
        ret = PTR_ERR(vmonitor_device);
        goto err_class_destroy;
    }

    mod_timer(&vdev->sample_timer,
              jiffies + msecs_to_jiffies(vdev->period_ms));

    pr_info(DRIVER_NAME
            ": loaded, major=%d minor=%d period=%u ms\n",
            MAJOR(vmonitor_devno),
            MINOR(vmonitor_devno),
            vdev->period_ms);

    return 0;

err_class_destroy:
    class_destroy(vmonitor_class);

err_cdev_del:
    cdev_del(&vdev->cdev);

err_unregister:
    unregister_chrdev_region(vmonitor_devno, 1);

err_free_dev:
    mutex_destroy(&vdev->read_lock);
    kfree(vdev);
    vdev = NULL;

    return ret;
}

/* ------------------------------------------------------------------------- */
/* Module cleanup                                                            */
/* ------------------------------------------------------------------------- */

static void __exit vmonitor_exit(void)
{
    unsigned long flags;

    /*
     * Prevent a running callback from rearming itself.
     */
    spin_lock_irqsave(&vdev->lock, flags);
    vdev->running = false;
    spin_unlock_irqrestore(&vdev->lock, flags);

    del_timer_sync(&vdev->sample_timer);

    device_destroy(vmonitor_class, vmonitor_devno);
    class_destroy(vmonitor_class);

    cdev_del(&vdev->cdev);

    unregister_chrdev_region(vmonitor_devno, 1);

    mutex_destroy(&vdev->read_lock);

    kfree(vdev);
    vdev = NULL;

    pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);