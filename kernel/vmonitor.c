// SPDX-License-Identifier: GPL-2.0
/*
 * vmonitor.c - Basic character device infrastructure
 *
 * Scope of this task (feature/kernel-driver):
 *   - Register the device with alloc_chrdev_region()
 *   - Initialize cdev with cdev_init()
 *   - Register it with cdev_add()
 *   - Implement open() / release()
 *   - Allow only one process to open the device at a time (-EBUSY otherwise)
 *   - Clean up all allocated resources on module exit
 *
 * Explicitly NOT implemented here (future tasks):
 *   - Sample queue
 *   - Timer-driven data generation
 *   - ioctl
 *   - sysfs attributes
 *   - poll
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#define DRIVER_NAME "vmonitor"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("staj");
MODULE_DESCRIPTION("Virtual temperature monitor - basic character device infrastructure");

/* Aygit durumu: bu asamada sadece cdev ve tekli-erisim kilidi tutuluyor. */
struct vmonitor_dev {
    struct cdev cdev;
    atomic_t opened; /* 0 = kapali, 1 = acik -> tekli erisim kilidi */
};

static struct vmonitor_dev *vdev;
static dev_t vmonitor_devno;
static struct class *vmonitor_class;
static struct device *vmonitor_device;

/*
 * open(): Ayni anda sadece bir process cihazi acabilir.
 * atomic_cmpxchg(&opened, 0, 1): deger 0 ise 1 yap ve ESKI degeri dondur.
 * Eski deger 0 degilse (yani zaten 1 ise) baskasi acik demektir -> EBUSY.
 */
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

/*
 * release(): Kilidi serbest birakir, boylece bir sonraki open() basarili olur.
 */
static int vmonitor_release(struct inode *inode, struct file *filp)
{
    atomic_set(&vdev->opened, 0);
    pr_info(DRIVER_NAME ": kapatildi\n");
    return 0;
}

static const struct file_operations vmonitor_fops = {
    .owner   = THIS_MODULE,
    .open    = vmonitor_open,
    .release = vmonitor_release,
    /* read/write/unlocked_ioctl/poll: sonraki gorevlerde eklenecek */
};

static int __init vmonitor_init(void)
{
    int ret;

    vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
    if (!vdev)
        return -ENOMEM;

    atomic_set(&vdev->opened, 0);

    /* 1) Major/minor numara aralilgini dinamik olarak ayir */
    ret = alloc_chrdev_region(&vmonitor_devno, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": alloc_chrdev_region basarisiz\n");
        goto err_free_dev;
    }

    /* 2) cdev yapisini file_operations ile ilişkilendir */
    cdev_init(&vdev->cdev, &vmonitor_fops);
    vdev->cdev.owner = THIS_MODULE;

    /* 3) cdev'i kernel'e kaydet */
    ret = cdev_add(&vdev->cdev, vmonitor_devno, 1);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": cdev_add basarisiz\n");
        goto err_unregister;
    }

    /* /dev/vmonitor dugumunun otomatik olusmasi icin class + device.
     * (Bu, gorev listesinde ayri bir madde olarak sayilmadi ama open()/EBUSY
     * davranisini elle test edebilmek icin /dev/vmonitor dugumu gerekiyor.) */
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