#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/version.h>

#include "../include/vmonitor_uapi.h"

#define DEVICE_NAME "vmonitor"
#define CLASS_NAME  "vmonitor"

#define VMONITOR_QUEUE_CAPACITY 64

#define VMONITOR_DEFAULT_PERIOD_MS     1000U
#define VMONITOR_DEFAULT_THRESHOLD_MC 35000

#define VMONITOR_TEMP_MIN_MC  20000
#define VMONITOR_TEMP_MAX_MC  40000
#define VMONITOR_TEMP_STEP_MC 1000

struct vmonitor_device {
    dev_t devno;
    struct cdev cdev;
    struct class *class;
    struct device *device;

    atomic_t opened;

    spinlock_t lock;
    struct mutex read_lock;
    struct mutex control_lock;

    wait_queue_head_t read_wait;

    struct timer_list timer;

    struct vmonitor_sample queue[VMONITOR_QUEUE_CAPACITY];
    unsigned int q_head;
    unsigned int q_tail;
    unsigned int q_count;

    bool running;
    u32 period_ms;
    s32 threshold_mC;

    s32 next_value_mC;

    u64 next_seq;

    u64 produced_total;
    u64 enqueued_total;
    u64 dropped_total;
    u64 read_total;

    u64 last_seq;
    s32 last_value_mC;
    u32 last_alarm;
};

static struct vmonitor_device vmon;

/*
 * Must be called with vmon.lock held.
 *
 * The function creates a new sample, updates global statistics and
 * attempts to enqueue the sample.
 *
 * Queue policy:
 *     DROP NEW
 *
 * If the queue is full, unread samples already present in the queue
 * are preserved and the newly produced sample is discarded.
 */
static bool vmonitor_produce_sample_locked(s32 value_mC)
{
    struct vmonitor_sample sample;

    sample.seq = vmon.next_seq++;
    sample.timestamp_ns = ktime_get_ns();
    sample.value_mC = value_mC;
    sample.alarm = (value_mC >= vmon.threshold_mC) ? 1U : 0U;

    vmon.produced_total++;

    vmon.last_seq = sample.seq;
    vmon.last_value_mC = sample.value_mC;
    vmon.last_alarm = sample.alarm;

    if (vmon.q_count == VMONITOR_QUEUE_CAPACITY) {
        vmon.dropped_total++;
        return false;
    }

    vmon.queue[vmon.q_tail] = sample;

    vmon.q_tail++;
    if (vmon.q_tail == VMONITOR_QUEUE_CAPACITY)
        vmon.q_tail = 0;

    vmon.q_count++;
    vmon.enqueued_total++;

    return true;
}

static void vmonitor_timer_callback(struct timer_list *timer)
{
    unsigned long flags;
    bool enqueued = false;

    (void)timer;

    spin_lock_irqsave(&vmon.lock, flags);

    if (!vmon.running) {
        spin_unlock_irqrestore(&vmon.lock, flags);
        return;
    }

    enqueued = vmonitor_produce_sample_locked(vmon.next_value_mC);

    vmon.next_value_mC += VMONITOR_TEMP_STEP_MC;

    if (vmon.next_value_mC > VMONITOR_TEMP_MAX_MC)
        vmon.next_value_mC = VMONITOR_TEMP_MIN_MC;

    /*
     * mod_timer() is safe from timer/atomic context.
     *
     * Rearming while holding the same state lock also serializes this
     * operation with period_ms updates.
     */
    mod_timer(&vmon.timer,
              jiffies + msecs_to_jiffies(vmon.period_ms));

    spin_unlock_irqrestore(&vmon.lock, flags);

    if (enqueued)
        wake_up_interruptible(&vmon.read_wait);
}

static int vmonitor_open(struct inode *inode, struct file *file)
{
    (void)inode;

    if (atomic_cmpxchg(&vmon.opened, 0, 1) != 0)
        return -EBUSY;

    file->private_data = &vmon;

    return 0;
}

static int vmonitor_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;

    atomic_set(&vmon.opened, 0);

    return 0;
}

static ssize_t vmonitor_read(struct file *file,
                             char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    struct vmonitor_sample sample;
    unsigned long flags;
    int ret;

    (void)ppos;

    if (count != sizeof(struct vmonitor_sample))
        return -EINVAL;

    /*
     * Serialize readers so a sample is not removed from the queue until
     * copy_to_user() has succeeded.
     */
    ret = mutex_lock_interruptible(&vmon.read_lock);
    if (ret)
        return ret;

    for (;;) {
        spin_lock_irqsave(&vmon.lock, flags);

        if (vmon.q_count != 0) {
            sample = vmon.queue[vmon.q_head];

            spin_unlock_irqrestore(&vmon.lock, flags);
            break;
        }

        spin_unlock_irqrestore(&vmon.lock, flags);

        if (file->f_flags & O_NONBLOCK) {
            mutex_unlock(&vmon.read_lock);
            return -EAGAIN;
        }

        ret = wait_event_interruptible(
            vmon.read_wait,
            READ_ONCE(vmon.q_count) != 0
        );

        if (ret) {
            mutex_unlock(&vmon.read_lock);
            return ret;
        }
    }

    if (copy_to_user(buf, &sample, sizeof(sample))) {
        mutex_unlock(&vmon.read_lock);
        return -EFAULT;
    }

    /*
     * Advance the queue only after the sample has successfully reached
     * userspace.
     */
    spin_lock_irqsave(&vmon.lock, flags);

    vmon.q_head++;
    if (vmon.q_head == VMONITOR_QUEUE_CAPACITY)
        vmon.q_head = 0;

    vmon.q_count--;
    vmon.read_total++;

    spin_unlock_irqrestore(&vmon.lock, flags);

    mutex_unlock(&vmon.read_lock);

    return sizeof(sample);
}

static ssize_t vmonitor_write(struct file *file,
                              const char __user *buf,
                              size_t count,
                              loff_t *ppos)
{
    struct vmonitor_sample user_sample;
    unsigned long flags;
    bool enqueued;

    (void)file;
    (void)ppos;

    if (count != sizeof(struct vmonitor_sample))
        return -EINVAL;

    if (copy_from_user(&user_sample, buf, sizeof(user_sample)))
        return -EFAULT;

    /*
     * Userspace supplies the requested temperature value.
     *
     * seq, timestamp_ns and alarm are generated by the kernel so all
     * samples obey the same ordering and alarm rules.
     */
    spin_lock_irqsave(&vmon.lock, flags);

    enqueued = vmonitor_produce_sample_locked(user_sample.value_mC);

    spin_unlock_irqrestore(&vmon.lock, flags);

    if (enqueued)
        wake_up_interruptible(&vmon.read_wait);

    return sizeof(user_sample);
}

static long vmonitor_ioctl(struct file *file,
                           unsigned int cmd,
                           unsigned long arg)
{
    struct vmonitor_status status;
    unsigned long flags;

    (void)file;

    switch (cmd) {
    case VMONITOR_IOC_START:
        mutex_lock(&vmon.control_lock);

        spin_lock_irqsave(&vmon.lock, flags);

        if (!vmon.running) {
            vmon.running = true;

            mod_timer(&vmon.timer,
                      jiffies + msecs_to_jiffies(vmon.period_ms));
        }

        spin_unlock_irqrestore(&vmon.lock, flags);

        mutex_unlock(&vmon.control_lock);

        return 0;

    case VMONITOR_IOC_STOP:
        mutex_lock(&vmon.control_lock);

        spin_lock_irqsave(&vmon.lock, flags);
        vmon.running = false;
        spin_unlock_irqrestore(&vmon.lock, flags);

        /*
         * Never call del_timer_sync() while holding vmon.lock because
         * the timer callback also acquires that lock.
         */
        del_timer_sync(&vmon.timer);

        mutex_unlock(&vmon.control_lock);

        return 0;

    case VMONITOR_IOC_GET_STATUS:
        memset(&status, 0, sizeof(status));

        spin_lock_irqsave(&vmon.lock, flags);

        status.running = vmon.running ? 1U : 0U;
        status.period_ms = vmon.period_ms;
        status.threshold_mC = vmon.threshold_mC;

        status.produced_total = vmon.produced_total;
        status.enqueued_total = vmon.enqueued_total;
        status.dropped_total = vmon.dropped_total;
        status.read_total = vmon.read_total;

        status.queued = vmon.q_count;

        status.last_seq = vmon.last_seq;
        status.last_value_mC = vmon.last_value_mC;
        status.last_alarm = vmon.last_alarm;

        spin_unlock_irqrestore(&vmon.lock, flags);

        if (copy_to_user((void __user *)arg,
                         &status,
                         sizeof(status)))
            return -EFAULT;

        return 0;

    default:
        return -ENOTTY;
    }
}

static __poll_t vmonitor_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    unsigned long flags;

    poll_wait(file, &vmon.read_wait, wait);

    spin_lock_irqsave(&vmon.lock, flags);

    if (vmon.q_count != 0)
        mask |= EPOLLIN | EPOLLRDNORM;

    spin_unlock_irqrestore(&vmon.lock, flags);

    return mask;
}

/* ------------------------------------------------------------------------- */
/* sysfs: period_ms                                                          */
/* ------------------------------------------------------------------------- */

static ssize_t period_ms_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    unsigned long flags;
    u32 period;

    (void)dev;
    (void)attr;

    spin_lock_irqsave(&vmon.lock, flags);
    period = vmon.period_ms;
    spin_unlock_irqrestore(&vmon.lock, flags);

    return sysfs_emit(buf, "%u\n", period);
}

static ssize_t period_ms_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    unsigned long flags;
    u32 period;
    int ret;

    (void)dev;
    (void)attr;

    ret = kstrtou32(buf, 0, &period);
    if (ret)
        return ret;

    if (period == 0)
        return -EINVAL;

    mutex_lock(&vmon.control_lock);

    spin_lock_irqsave(&vmon.lock, flags);

    vmon.period_ms = period;

    /*
     * Changing period_ms while stopped must not start the generator.
     * If running, update the next timer expiration immediately.
     */
    if (vmon.running) {
        mod_timer(&vmon.timer,
                  jiffies + msecs_to_jiffies(vmon.period_ms));
    }

    spin_unlock_irqrestore(&vmon.lock, flags);

    mutex_unlock(&vmon.control_lock);

    return count;
}

static DEVICE_ATTR_RW(period_ms);

/* ------------------------------------------------------------------------- */
/* sysfs: threshold_mC                                                       */
/* ------------------------------------------------------------------------- */

static ssize_t threshold_mC_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    unsigned long flags;
    s32 threshold;

    (void)dev;
    (void)attr;

    spin_lock_irqsave(&vmon.lock, flags);
    threshold = vmon.threshold_mC;
    spin_unlock_irqrestore(&vmon.lock, flags);

    return sysfs_emit(buf, "%d\n", threshold);
}

static ssize_t threshold_mC_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    unsigned long flags;
    int threshold;
    int ret;

    (void)dev;
    (void)attr;

    ret = kstrtoint(buf, 0, &threshold);
    if (ret)
        return ret;

    spin_lock_irqsave(&vmon.lock, flags);
    vmon.threshold_mC = (s32)threshold;
    spin_unlock_irqrestore(&vmon.lock, flags);

    return count;
}

static DEVICE_ATTR_RW(threshold_mC);

static struct attribute *vmonitor_attrs[] = {
    &dev_attr_period_ms.attr,
    &dev_attr_threshold_mC.attr,
    NULL
};

static const struct attribute_group vmonitor_attr_group = {
    .attrs = vmonitor_attrs,
};

/* ------------------------------------------------------------------------- */

static const struct file_operations vmonitor_fops = {
    .owner          = THIS_MODULE,
    .open           = vmonitor_open,
    .release        = vmonitor_release,
    .read           = vmonitor_read,
    .write          = vmonitor_write,
    .unlocked_ioctl = vmonitor_ioctl,
    .poll           = vmonitor_poll,
    .llseek         = no_llseek,
};

static int __init vmonitor_init(void)
{
    int ret;

    memset(&vmon, 0, sizeof(vmon));

    atomic_set(&vmon.opened, 0);

    spin_lock_init(&vmon.lock);
    mutex_init(&vmon.read_lock);
    mutex_init(&vmon.control_lock);

    init_waitqueue_head(&vmon.read_wait);

    vmon.running = true;
    vmon.period_ms = VMONITOR_DEFAULT_PERIOD_MS;
    vmon.threshold_mC = VMONITOR_DEFAULT_THRESHOLD_MC;

    vmon.next_value_mC = VMONITOR_TEMP_MIN_MC;
    vmon.next_seq = 1;

    timer_setup(&vmon.timer, vmonitor_timer_callback, 0);

    ret = alloc_chrdev_region(&vmon.devno, 0, 1, DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&vmon.cdev, &vmonitor_fops);
    vmon.cdev.owner = THIS_MODULE;

    ret = cdev_add(&vmon.cdev, vmon.devno, 1);
    if (ret)
        goto err_unregister;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    vmon.class = class_create(CLASS_NAME);
#else
    vmon.class = class_create(THIS_MODULE, CLASS_NAME);
#endif

    if (IS_ERR(vmon.class)) {
        ret = PTR_ERR(vmon.class);
        vmon.class = NULL;
        goto err_cdev;
    }

    vmon.device = device_create(vmon.class,
                                NULL,
                                vmon.devno,
                                NULL,
                                DEVICE_NAME);

    if (IS_ERR(vmon.device)) {
        ret = PTR_ERR(vmon.device);
        vmon.device = NULL;
        goto err_class;
    }

    ret = sysfs_create_group(&vmon.device->kobj,
                             &vmonitor_attr_group);
    if (ret)
        goto err_device;

    mod_timer(&vmon.timer,
              jiffies + msecs_to_jiffies(vmon.period_ms));

    pr_info("vmonitor: loaded major=%d minor=%d\n",
            MAJOR(vmon.devno),
            MINOR(vmon.devno));

    return 0;

err_device:
    device_destroy(vmon.class, vmon.devno);

err_class:
    class_destroy(vmon.class);

err_cdev:
    cdev_del(&vmon.cdev);

err_unregister:
    unregister_chrdev_region(vmon.devno, 1);

    return ret;
}

static void __exit vmonitor_exit(void)
{
    unsigned long flags;

    mutex_lock(&vmon.control_lock);

    spin_lock_irqsave(&vmon.lock, flags);
    vmon.running = false;
    spin_unlock_irqrestore(&vmon.lock, flags);

    del_timer_sync(&vmon.timer);

    mutex_unlock(&vmon.control_lock);

    sysfs_remove_group(&vmon.device->kobj,
                       &vmonitor_attr_group);

    device_destroy(vmon.class, vmon.devno);
    class_destroy(vmon.class);

    cdev_del(&vmon.cdev);

    unregister_chrdev_region(vmon.devno, 1);

    pr_info("vmonitor: unloaded\n");
}

module_init(vmonitor_init);
module_exit(vmonitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("vmonitor team");
MODULE_DESCRIPTION("Virtual temperature monitor character device");
MODULE_VERSION("1.0");
