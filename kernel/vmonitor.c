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

#include "vmonitor_uapi.h"

#define DRIVER_NAME "vmonitor"

#define QUEUE_CAPACITY 64

#define DEFAULT_THRESHOLD_MC 35000
#define DEFAULT_PERIOD_MS    1000

#define TEMP_MIN_MC  20000
#define TEMP_MAX_MC  40000
#define TEMP_STEP_MC 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION(
    "Virtual temperature monitor - character device, queue and timer");

/* ------------------------------------------------------------------------- */
/* Fixed-size circular queue                                                 */
/* ------------------------------------------------------------------------- */

struct vmonitor_queue {
    struct vmonitor_sample items[QUEUE_CAPACITY];

    unsigned int head;
    unsigned int tail;
    unsigned int count;
};

/* ------------------------------------------------------------------------- */
/* Device state                                                              */
/* ------------------------------------------------------------------------- */

struct vmonitor_dev {
    struct cdev cdev;

    /*
     * Single-open protection.
     * 0 = available
     * 1 = already opened
     */
    atomic_t opened;

    /*
     * Protects:
     *   queue
     *   counters
     *   seq_counter
     *   threshold_mC
     *   generated_value_mC
     */
    spinlock_t lock;

    struct vmonitor_queue queue;

    /*
     * Serializes the complete:
     *
     * peek -> copy_to_user -> queue advance
     *
     * read transaction.
     */
    struct mutex read_lock;

    /*
     * Sequence number assigned by the driver.
     */
    __u64 seq_counter;

    /*
     * Alarm threshold.
     *
     * Sysfs configurability is implemented in
     * a later task.
     */
    s32 threshold_mC;

    /*
     * Automatic generation period.
     *
     * Sysfs configurability is implemented in
     * a later task.
     */
    u32 period_ms;

    /*
     * Next virtual temperature value that the
     * timer will generate.
     */
    s32 generated_value_mC;

    /*
     * Automatic sample generation timer.
     */
    struct timer_list sample_timer;

    /*
     * Queue/status counters.
     */
    u64 produced_total;
    u64 enqueued_total;
    u64 dropped_total;
    u64 read_total;
};

static struct vmonitor_dev *vdev;

static dev_t vmonitor_devno;
static struct class *vmonitor_class;
static struct device *vmonitor_device;

/* ------------------------------------------------------------------------- */
/* Queue helpers                                                             */
/* Caller MUST hold dev->lock.                                               */
/* ------------------------------------------------------------------------- */

static bool queue_is_full(struct vmonitor_queue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static bool queue_is_empty(struct vmonitor_queue *q)
{
    return q->count == 0;
}

/*
 * Push a sample onto the queue.
 *
 * Queue-full policy:
 *
 * DROP THE NEW SAMPLE.
 *
 * Existing unread samples are never overwritten.
 */
static bool queue_push(struct vmonitor_queue *q,
                       const struct vmonitor_sample *sample)
{
    if (queue_is_full(q))
        return false;

    q->items[q->tail] = *sample;

    q->tail =
        (q->tail + 1) % QUEUE_CAPACITY;

    q->count++;

    return true;
}

/*
 * Copy the oldest sample without removing it.
 */
static bool queue_peek(struct vmonitor_queue *q,
                       struct vmonitor_sample *sample)
{
    if (queue_is_empty(q))
        return false;

    *sample = q->items[q->head];

    return true;
}

/*
 * Remove the oldest queue element.
 *
 * Caller must already know the queue is non-empty.
 */
static void queue_advance(struct vmonitor_queue *q)
{
    q->head =
        (q->head + 1) % QUEUE_CAPACITY;

    q->count--;
}

/* ------------------------------------------------------------------------- */
/* Sample generation helper                                                  */
/* Caller MUST hold dev->lock.                                               */
/* ------------------------------------------------------------------------- */

/*
 * Enqueue a fully driver-owned sample.
 *
 * This helper is used by automatic generation.
 *
 * timestamp_ns is supplied by the caller so the
 * timestamp can be obtained before entering the
 * critical section.
 */
static void generate_automatic_sample_locked(
    struct vmonitor_dev *dev,
    u64 timestamp_ns)
{
    struct vmonitor_sample sample;
    bool pushed;

    memset(&sample, 0, sizeof(sample));

    sample.seq = ++dev->seq_counter;
    sample.timestamp_ns = timestamp_ns;
    sample.value_mC = dev->generated_value_mC;

    sample.alarm =
        (sample.value_mC >= dev->threshold_mC)
            ? 1
            : 0;

    /*
     * Every generated sample counts as produced,
     * even if the queue is already full.
     */
    dev->produced_total++;

    pushed =
        queue_push(&dev->queue, &sample);

    if (pushed)
        dev->enqueued_total++;
    else
        dev->dropped_total++;

    /*
     * Required repeating temperature pattern:
     *
     * 20000
     * 21000
     * ...
     * 40000
     * 20000
     * ...
     */
    if (dev->generated_value_mC >= TEMP_MAX_MC)
        dev->generated_value_mC = TEMP_MIN_MC;
    else
        dev->generated_value_mC += TEMP_STEP_MC;
}

/* ------------------------------------------------------------------------- */
/* Timer                                                                     */
/* ------------------------------------------------------------------------- */

static void vmonitor_timer_callback(struct timer_list *timer)
{
    struct vmonitor_dev *dev;
    unsigned long flags;
    u32 period_ms;
    u64 timestamp_ns;

    dev = from_timer(dev, timer, sample_timer);

    /*
     * ktime_get_ns() provides a monotonic timestamp.
     */
    timestamp_ns = ktime_get_ns();

    /*
     * Timer callbacks must not sleep.
     *
     * Use the existing spinlock to protect shared
     * queue/state.
     */
    spin_lock_irqsave(&dev->lock, flags);

    generate_automatic_sample_locked(
        dev,
        timestamp_ns);

    /*
     * Take a local snapshot while holding the lock.
     *
     * A later sysfs task may allow period_ms to change.
     */
    period_ms = dev->period_ms;

    spin_unlock_irqrestore(&dev->lock, flags);

    /*
     * Rearm the timer.
     *
     * mod_timer() is safe for this timer-based
     * periodic scheduling pattern.
     */
    mod_timer(&dev->sample_timer,
              jiffies +
              msecs_to_jiffies(period_ms));
}

/* ------------------------------------------------------------------------- */
/* File operations                                                           */
/* ------------------------------------------------------------------------- */

static int vmonitor_open(struct inode *inode,
                         struct file *filp)
{
    if (atomic_cmpxchg(&vdev->opened,
                       0,
                       1) != 0) {

        pr_info(DRIVER_NAME
                ": open reddedildi, cihaz zaten acik (EBUSY)\n");

        return -EBUSY;
    }

    filp->private_data = vdev;

    pr_info(DRIVER_NAME ": acildi\n");

    return 0;
}

static int vmonitor_release(struct inode *inode,
                            struct file *filp)
{
    atomic_set(&vdev->opened, 0);

    pr_info(DRIVER_NAME ": kapatildi\n");

    return 0;
}

/*
 * read()
 *
 * Exactly one complete vmonitor_sample must be requested.
 *
 * The queue entry is removed only after copy_to_user()
 * succeeds.
 */
static ssize_t vmonitor_read(struct file *filp,
                             char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    struct vmonitor_dev *dev;
    struct vmonitor_sample sample;

    unsigned long flags;
    bool available;
    ssize_t ret;

    dev = filp->private_data;

    if (count != sizeof(sample))
        return -EINVAL;

    /*
     * A single open fd can still be shared between
     * multiple threads/processes, so serialize the
     * complete read transaction.
     */
    if (mutex_lock_interruptible(&dev->read_lock))
        return -ERESTARTSYS;

    /*
     * Peek under the queue spinlock.
     */
    spin_lock_irqsave(&dev->lock, flags);

    available =
        queue_peek(&dev->queue, &sample);

    spin_unlock_irqrestore(&dev->lock, flags);

    if (!available) {
        /*
         * Blocking read / wait queue is a later task.
         */
        ret = 0;
        goto out_unlock;
    }

    /*
     * Never hold a spinlock across copy_to_user().
     */
    if (copy_to_user(buf,
                     &sample,
                     sizeof(sample))) {

        /*
         * Sample remains queued.
         */
        ret = -EFAULT;
        goto out_unlock;
    }

    /*
     * Commit the queue removal only after the copy
     * succeeded.
     */
    spin_lock_irqsave(&dev->lock, flags);

    queue_advance(&dev->queue);
    dev->read_total++;

    spin_unlock_irqrestore(&dev->lock, flags);

    ret = sizeof(sample);

out_unlock:
    mutex_unlock(&dev->read_lock);

    return ret;
}

/*
 * write()
 *
 * Manual temperature injection.
 *
 * Userspace supplies a complete struct for uniform
 * UAPI sizing, but only value_mC is accepted from
 * userspace.
 *
 * seq, timestamp_ns and alarm remain driver-owned.
 */
static ssize_t vmonitor_write(struct file *filp,
                              const char __user *buf,
                              size_t count,
                              loff_t *ppos)
{
    struct vmonitor_dev *dev;
    struct vmonitor_sample sample;

    unsigned long flags;
    bool pushed;

    dev = filp->private_data;

    /*
     * Strict 24-byte sample semantics.
     */
    if (count != sizeof(sample))
        return -EINVAL;

    if (copy_from_user(&sample,
                       buf,
                       sizeof(sample))) {

        return -EFAULT;
    }

    /*
     * Driver-owned monotonic timestamp.
     */
    sample.timestamp_ns =
        ktime_get_ns();

    spin_lock_irqsave(&dev->lock, flags);

    /*
     * Driver owns sequence and alarm fields.
     */
    sample.seq =
        ++dev->seq_counter;

    sample.alarm =
        (sample.value_mC >= dev->threshold_mC)
            ? 1
            : 0;

    dev->produced_total++;

    pushed =
        queue_push(&dev->queue, &sample);

    if (pushed)
        dev->enqueued_total++;
    else
        dev->dropped_total++;

    spin_unlock_irqrestore(&dev->lock, flags);

    /*
     * Queue-full drop is not reported as a failed
     * write. The input itself was accepted.
     */
    return sizeof(sample);
}

static const struct file_operations vmonitor_fops = {
    .owner   = THIS_MODULE,
    .open    = vmonitor_open,
    .release = vmonitor_release,
    .read    = vmonitor_read,
    .write   = vmonitor_write,

    /*
     * unlocked_ioctl and poll are later tasks.
     */
};

/* ------------------------------------------------------------------------- */
/* Module initialization                                                     */
/* ------------------------------------------------------------------------- */

static int __init vmonitor_init(void)
{
    int ret;

    vdev =
        kzalloc(sizeof(*vdev),
                GFP_KERNEL);

    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);

    spin_lock_init(&vdev->lock);
    mutex_init(&vdev->read_lock);

    vdev->threshold_mC =
        DEFAULT_THRESHOLD_MC;

    vdev->period_ms =
        DEFAULT_PERIOD_MS;

    vdev->generated_value_mC =
        TEMP_MIN_MC;

    /*
     * Prepare timer before exposing the device.
     */
    timer_setup(&vdev->sample_timer,
                vmonitor_timer_callback,
                0);

    ret =
        alloc_chrdev_region(&vmonitor_devno,
                            0,
                            1,
                            DRIVER_NAME);

    if (ret < 0) {
        pr_err(DRIVER_NAME
               ": alloc_chrdev_region basarisiz\n");

        goto err_free_dev;
    }

    cdev_init(&vdev->cdev,
              &vmonitor_fops);

    vdev->cdev.owner =
        THIS_MODULE;

    ret =
        cdev_add(&vdev->cdev,
                 vmonitor_devno,
                 1);

    if (ret < 0) {
        pr_err(DRIVER_NAME
               ": cdev_add basarisiz\n");

        goto err_unregister;
    }

    vmonitor_class =
        class_create(DRIVER_NAME);

    if (IS_ERR(vmonitor_class)) {
        ret =
            PTR_ERR(vmonitor_class);

        pr_err(DRIVER_NAME
               ": class_create basarisiz\n");

        goto err_cdev_del;
    }

    vmonitor_device =
        device_create(vmonitor_class,
                      NULL,
                      vmonitor_devno,
                      NULL,
                      DRIVER_NAME);

    if (IS_ERR(vmonitor_device)) {
        ret =
            PTR_ERR(vmonitor_device);

        pr_err(DRIVER_NAME
               ": device_create basarisiz\n");

        goto err_class_destroy;
    }

    /*
     * Start automatic generation.
     *
     * START/STOP ioctl behavior itself remains a
     * future task. For this timer-focused task the
     * generator begins when the module is loaded.
     */
    mod_timer(&vdev->sample_timer,
              jiffies +
              msecs_to_jiffies(vdev->period_ms));

    pr_info(DRIVER_NAME
            ": yuklendi, major=%d minor=%d, period=%u ms\n",
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
    /*
     * Timer has not been armed on any failure path
     * above, so no synchronous timer deletion is
     * necessary here.
     */
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
    /*
     * Stop the timer synchronously before freeing
     * vdev. This guarantees the callback is no longer
     * running and cannot rearm itself after teardown.
     */
    del_timer_sync(&vdev->sample_timer);

    device_destroy(vmonitor_class,
                   vmonitor_devno);

    class_destroy(vmonitor_class);

    cdev_del(&vdev->cdev);

    unregister_chrdev_region(vmonitor_devno,
                             1);

    mutex_destroy(&vdev->read_lock);

    kfree(vdev);
    vdev = NULL;

    pr_info(DRIVER_NAME ": kaldirildi\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);
