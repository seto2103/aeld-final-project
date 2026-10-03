/**
 * @file status_led.c
 * @brief Character driver for the Raspberry Pi onboard ACT LED, used as the camera status LED.
 *
 * Based on the aesdchar driver, itself based on the "scull" driver from Linux Device Drivers.
 * Placeholder for Sprint 1: registers /dev/status_led with no LED control. GPIO control,
 * ioctl and blinking are added in Sprint 2.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>

#define DEVICE_NAME "status_led"

MODULE_AUTHOR("seto2103");
MODULE_DESCRIPTION("Raspberry Pi status LED character driver");
MODULE_LICENSE("Dual BSD/GPL");

static dev_t status_led_devno;
static struct cdev status_led_cdev;
static struct class *status_led_class;

static int status_led_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int status_led_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations status_led_fops = {
    .owner   = THIS_MODULE,
    .open    = status_led_open,
    .release = status_led_release,
};

static int __init status_led_init(void)
{
    struct device *dev;
    int result;

    result = alloc_chrdev_region(&status_led_devno, 0, 1, DEVICE_NAME);
    if (result < 0) {
        pr_err("status_led: can't get major number: %d\n", result);
        return result;
    }

    cdev_init(&status_led_cdev, &status_led_fops);
    status_led_cdev.owner = THIS_MODULE;
    result = cdev_add(&status_led_cdev, status_led_devno, 1);
    if (result) {
        pr_err("status_led: cdev_add failed: %d\n", result);
        goto fail_cdev;
    }

    /* Creating the device under a class makes devtmpfs create /dev/status_led */
    status_led_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(status_led_class)) {
        result = PTR_ERR(status_led_class);
        goto fail_class;
    }

    dev = device_create(status_led_class, NULL, status_led_devno, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        result = PTR_ERR(dev);
        goto fail_device;
    }

    pr_info("status_led: loaded, major %d\n", MAJOR(status_led_devno));
    return 0;

fail_device:
    class_destroy(status_led_class);
fail_class:
    cdev_del(&status_led_cdev);
fail_cdev:
    unregister_chrdev_region(status_led_devno, 1);
    return result;
}

static void __exit status_led_exit(void)
{
    device_destroy(status_led_class, status_led_devno);
    class_destroy(status_led_class);
    cdev_del(&status_led_cdev);
    unregister_chrdev_region(status_led_devno, 1);
    pr_info("status_led: unloaded\n");
}

module_init(status_led_init);
module_exit(status_led_exit);
