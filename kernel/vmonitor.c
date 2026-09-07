// SPDX-License-Identifier: GPL-2.0
/*
 * vmonitor.c - Character device foundation + sample queue + read/write
 *
 * This task adds on top of the character device foundation:
 *   - A fixed-size (64 entry) circular queue of struct vmonitor_sample
 *   - A spinlock protecting all shared queue state and counters
 *   - read(): pop a sample from the queue, copy_to_user() it, remove it
 *   - write(): validate input, build a new sample (driver assigns seq),
 *     push it onto the queue (manual temperature injection)
 *   - Queue-full policy: drop the NEW sample, keep unread ones,
 *     increment dropped_total
 *
 * Explicitly NOT implemented here (future tasks):
 *   - Kernel timer / automatic sample generation
 *   - ioctl
 *   - sysfs attributes
 *   - poll / wait queue
 *   - START/STOP behavior
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "vmonitor_uapi.h"

#define DRIVER_NAME    "vmonitor"
#define QUEUE_CAPACITY 64

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION("Virtual temperature monitor - character device + sample queue");

/* ---- Fixed-size circular queue ---- */
struct vmonitor_queue {
    struct vmonitor_sample items[QUEUE_CAPACITY];
    unsigned int head;   /* index of the next sample to read */
    unsigned int tail;   /* index where the next sample will be written */
    unsigned int count;  /* number of samples currently queued */
};

/* ---- Device state ---- */
struct vmonitor_dev {
    struct cdev cdev;
    atomic_t opened; /* 0 = closed, 1 = open -> single-open lock */

    spinlock_t lock;              /* protects queue + counters below */
    struct vmonitor_queue queue;

    __u64 seq_counter;            /* next sequence number to assign */

    /* Queue-related counters (mirrors what GET_STATUS will expose later) */
    u64 produced_total;   /* samples created (write() calls that validated OK) */
    u64 enqueued_total;   /* samples successfully pushed onto the queue */
    u64 dropped_total;    /* samples dropped because the queue was full */
    u64 read_total;       /* samples successfully popped via read() */
};

static struct vmonitor_dev *vdev;
static dev_t vmonitor_devno;
static struct class *vmonitor_class;
static struct device *vmonitor_device;

/* ---- Queue helpers (caller MUST hold dev->lock) ---- */

static bool queue_is_full(struct vmonitor_queue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static bool queue_is_empty(struct vmonitor_queue *q)
{
    return q->count == 0;
}

/*
 * Push a new sample onto the queue.
 * Policy when full: DROP THE NEW SAMPLE, keep existing unread ones.
 * Returns true if pushed, false if dropped (queue was full).
 */
static bool queue_push(struct vmonitor_queue *q, const struct vmonitor_sample *s)
{
    if (queue_is_full(q))
        return false;

    q->items[q->tail] = *s;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;
    return true;
}

/*
 * Pop the oldest sample from the queue.
 * Returns true if a sample was popped, false if the queue was empty.
 */
static bool queue_pop(struct vmonitor_queue *q, struct vmonitor_sample *out)
{
    if (queue_is_empty(q))
        return false;

    *out = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    return true;
}

/* ---- file_operations ---- */

static int vmonitor_open(struct inode *inode, struct file *filp)
{
    if (atomic_cmpxchg(&vdev->opened, 0, 1) != 0) {
        pr_info(DRIVER_NAME ": open reddedildi, cihaz zaten acik (EBUSY)\n");
        return -EBUSY;
    }

    filp->private_data = vdev;
    pr_info(DRIVER_NAME ": acildi\n");
    return 0;
}

static int vmonitor_release(struct inode *inode, struct file *filp)
{
    atomic_set(&vdev->opened, 0);
    pr_info(DRIVER_NAME ": kapatildi\n");
    return 0;
}

/*
 * read(): pops one struct vmonitor_sample (24 bytes) from the queue and
 * copies it to userspace. If the queue is empty, returns 0 (EOF-style,
 * no blocking yet -- poll()/wait queue will be added in a later task).
 */
static ssize_t vmonitor_read(struct file *filp, char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;
    unsigned long flags;
    bool got;

    if (count < sizeof(sample))
        return -EINVAL;

    spin_lock_irqsave(&dev->lock, flags);
    got = queue_pop(&dev->queue, &sample);
    if (got)
        dev->read_total++;
    spin_unlock_irqrestore(&dev->lock, flags);

    if (!got)
        return 0; /* queue empty */

    if (copy_to_user(buf, &sample, sizeof(sample)))
        return -EFAULT;

    return sizeof(sample);
}

/*
 * write(): manual temperature injection. Userspace provides a
 * struct vmonitor_sample; the driver validates the size, copies it in,
 * assigns its own sequence number (userspace-provided seq is ignored),
 * and pushes it onto the queue.
 */
static ssize_t vmonitor_write(struct file *filp, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    struct vmonitor_dev *dev = filp->private_data;
    struct vmonitor_sample sample;
    unsigned long flags;
    bool pushed;

    /* Validate input size before touching userspace memory */
    if (count < sizeof(sample))
        return -EINVAL;

    if (copy_from_user(&sample, buf, sizeof(sample)))
        return -EFAULT;

    spin_lock_irqsave(&dev->lock, flags);

    /* Driver owns the sequence number, not the caller */
    sample.seq = ++dev->seq_counter;
    dev->produced_total++;

    pushed = queue_push(&dev->queue, &sample);
    if (pushed)
        dev->enqueued_total++;
    else
        dev->dropped_total++;

    spin_unlock_irqrestore(&dev->lock, flags);

    /* Per spec: a dropped sample is not an error from write()'s point of
     * view -- the write() call itself succeeded, the driver simply chose
     * not to overwrite unread data. */
    return sizeof(sample);
}

static const struct file_operations vmonitor_fops = {
    .owner   = THIS_MODULE,
    .open    = vmonitor_open,
    .release = vmonitor_release,
    .read    = vmonitor_read,
    .write   = vmonitor_write,
    /* unlocked_ioctl / poll: future tasks */
};

static int __init vmonitor_init(void)
{
    int ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);
    spin_lock_init(&vdev->lock);

    ret = alloc_chrdev_region(&vmonitor_devno, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": alloc_chrdev_region basarisiz\n");
        goto err_free_dev;
    }

    cdev_init(&vdev->cdev, &vmonitor_fops);
    vdev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&vdev->cdev, vmonitor_devno, 1);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": cdev_add basarisiz\n");
        goto err_unregister;
    }

    vmonitor_class = class_create(DRIVER_NAME);
    if (IS_ERR(vmonitor_class)) {
        ret = PTR_ERR(vmonitor_class);
        pr_err(DRIVER_NAME ": class_create basarisiz\n");
        goto err_cdev_del;
    }

    vmonitor_device = device_create(vmonitor_class, NULL, vmonitor_devno, NULL, DRIVER_NAME);
    if (IS_ERR(vmonitor_device)) {
        ret = PTR_ERR(vmonitor_device);
        pr_err(DRIVER_NAME ": device_create basarisiz\n");
        goto err_class_destroy;
    }

    pr_info(DRIVER_NAME ": yuklendi, major=%d minor=%d\n",
            MAJOR(vmonitor_devno), MINOR(vmonitor_devno));
    return 0;

err_class_destroy:
    class_destroy(vmonitor_class);
err_cdev_del:
    cdev_del(&vdev->cdev);
err_unregister:
    unregister_chrdev_region(vmonitor_devno, 1);
err_free_dev:
    kfree(vdev);
    return ret;
}

static void __exit vmonitor_exit(void)
{
    device_destroy(vmonitor_class, vmonitor_devno);
    class_destroy(vmonitor_class);
    cdev_del(&vdev->cdev);
    unregister_chrdev_region(vmonitor_devno, 1);
    kfree(vdev);
    pr_info(DRIVER_NAME ": kaldirildi\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);