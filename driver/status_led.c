/**
 * @file status_led.c
 * @brief Character driver for the Raspberry Pi onboard ACT LED, used as the camera status LED.
 *
 * Based on the aesdchar driver, itself based on the "scull" driver from Linux Device Drivers.
 *
 * A platform driver that binds to the "aesd,status-led" device tree node added by
 * status-led-overlay.dts, and creates /dev/status_led when it binds. Writing "on", "off" or
 * "blink" sets the mode and reading returns it; ioctls in status_led_ioctl.h set the mode and the
 * blink period. Blinking runs from a self rearming kernel timer.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/uaccess.h>

#include "status_led_ioctl.h"

#define DEVICE_NAME "status_led"
/* Longest accepted write, "blink" plus a newline with room to spare */
#define WRITE_MAX 16

MODULE_AUTHOR("seto2103");
MODULE_DESCRIPTION("Raspberry Pi status LED character driver");
MODULE_LICENSE("Dual BSD/GPL");

struct status_led_dev {
    struct mutex lock;              /* Serializes mode and period changes, bind and unbind */
    struct timer_list timer;        /* Toggles the LED in blink mode */
    struct gpio_desc *gpio;         /* NULL while not bound */
    enum status_led_mode mode;
    unsigned int period_ms;         /* Full blink cycle; read by the timer with READ_ONCE */
    bool led_on;                    /* Current LED state in blink mode, used only by the timer */
    dev_t devno;
    struct cdev cdev;
    struct class *class;
};

/* The Pi has one ACT LED, so there is a single static instance */
static struct status_led_dev status_led;

static const char *const mode_names[] = {
    [STATUS_LED_MODE_OFF] = "off",
    [STATUS_LED_MODE_ON] = "on",
    [STATUS_LED_MODE_BLINK] = "blink",
};

static unsigned long half_period_jiffies(unsigned int period_ms)
{
    unsigned long j = msecs_to_jiffies(period_ms / 2);

    return j > 0 ? j : 1;
}

static void status_led_timer_fn(struct timer_list *t)
{
    struct status_led_dev *dev = from_timer(dev, t, timer);

    dev->led_on = !dev->led_on;
    /* Timer callbacks can't sleep; probe checked that this GPIO doesn't need to */
    gpiod_set_value(dev->gpio, dev->led_on);
    mod_timer(&dev->timer, jiffies + half_period_jiffies(READ_ONCE(dev->period_ms)));
}

/* Caller holds dev->lock and the device is bound */
static void status_led_set_mode(struct status_led_dev *dev, enum status_led_mode mode)
{
    /* Waits for a running callback, and stops the timer even though it rearms itself */
    del_timer_sync(&dev->timer);
    dev->mode = mode;

    switch (mode) {
    case STATUS_LED_MODE_OFF:
        gpiod_set_value_cansleep(dev->gpio, 0);
        break;
    case STATUS_LED_MODE_ON:
        gpiod_set_value_cansleep(dev->gpio, 1);
        break;
    case STATUS_LED_MODE_BLINK:
        dev->led_on = true;
        gpiod_set_value_cansleep(dev->gpio, 1);
        mod_timer(&dev->timer, jiffies + half_period_jiffies(dev->period_ms));
        break;
    }
}

static int status_led_open(struct inode *inode, struct file *filp)
{
    filp->private_data = container_of(inode->i_cdev, struct status_led_dev, cdev);
    return 0;
}

static int status_led_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t status_led_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct status_led_dev *dev = filp->private_data;
    char kbuf[WRITE_MAX];
    int len;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    if (dev->gpio == NULL) {
        mutex_unlock(&dev->lock);
        return -ENODEV;
    }
    len = scnprintf(kbuf, sizeof(kbuf), "%s\n", mode_names[dev->mode]);
    mutex_unlock(&dev->lock);

    /* Handles f_pos, so `cat` sees end of file after the mode line */
    return simple_read_from_buffer(buf, count, f_pos, kbuf, len);
}

static ssize_t status_led_write(struct file *filp, const char __user *buf, size_t count,
                                loff_t *f_pos)
{
    struct status_led_dev *dev = filp->private_data;
    char kbuf[WRITE_MAX];
    enum status_led_mode mode;
    char *cmd;

    if (count == 0 || count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';
    /* Drop the newline that echo adds */
    cmd = strim(kbuf);

    if (strcmp(cmd, "off") == 0)
        mode = STATUS_LED_MODE_OFF;
    else if (strcmp(cmd, "on") == 0)
        mode = STATUS_LED_MODE_ON;
    else if (strcmp(cmd, "blink") == 0)
        mode = STATUS_LED_MODE_BLINK;
    else
        return -EINVAL;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    if (dev->gpio == NULL) {
        mutex_unlock(&dev->lock);
        return -ENODEV;
    }
    status_led_set_mode(dev, mode);
    mutex_unlock(&dev->lock);
    return count;
}

static long status_led_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct status_led_dev *dev = filp->private_data;
    u32 __user *uarg = (u32 __user *)arg;
    long retval = 0;
    u32 value = 0;

    if (_IOC_TYPE(cmd) != STATUS_LED_IOC_MAGIC || _IOC_NR(cmd) > STATUS_LED_IOC_MAXNR)
        return -ENOTTY;

    /* Fetch and check the argument before taking the lock */
    if (cmd == STATUS_LED_IOCSMODE || cmd == STATUS_LED_IOCSPERIOD) {
        if (get_user(value, uarg))
            return -EFAULT;
        if (cmd == STATUS_LED_IOCSMODE && value > STATUS_LED_MODE_BLINK)
            return -EINVAL;
        if (cmd == STATUS_LED_IOCSPERIOD &&
            (value < STATUS_LED_PERIOD_MIN_MS || value > STATUS_LED_PERIOD_MAX_MS))
            return -EINVAL;
    }

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    if (dev->gpio == NULL) {
        mutex_unlock(&dev->lock);
        return -ENODEV;
    }

    switch (cmd) {
    case STATUS_LED_IOCSMODE:
        status_led_set_mode(dev, value);
        break;
    case STATUS_LED_IOCGMODE:
        value = dev->mode;
        break;
    case STATUS_LED_IOCSPERIOD:
        /* A running blink picks up the new period on its next toggle */
        WRITE_ONCE(dev->period_ms, value);
        break;
    case STATUS_LED_IOCGPERIOD:
        value = dev->period_ms;
        break;
    default:
        retval = -ENOTTY;
        break;
    }
    mutex_unlock(&dev->lock);

    if (retval == 0 && (cmd == STATUS_LED_IOCGMODE || cmd == STATUS_LED_IOCGPERIOD)) {
        if (put_user(value, uarg))
            retval = -EFAULT;
    }
    return retval;
}

static const struct file_operations status_led_fops = {
    .owner          = THIS_MODULE,
    .open           = status_led_open,
    .release        = status_led_release,
    .read           = status_led_read,
    .write          = status_led_write,
    .unlocked_ioctl = status_led_ioctl,
};

static int status_led_probe(struct platform_device *pdev)
{
    struct status_led_dev *dev = &status_led;
    struct gpio_desc *gpio;
    struct device *chardev;
    int result;

    if (dev->gpio != NULL)
        return -EBUSY;

    /* "led-gpios" in the device tree node; the LED starts off */
    gpio = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(gpio))
        return dev_err_probe(&pdev->dev, PTR_ERR(gpio), "can't get LED GPIO\n");
    if (gpiod_cansleep(gpio)) {
        dev_err(&pdev->dev, "LED GPIO can sleep, but the blink timer can't\n");
        return -EINVAL;
    }

    mutex_lock(&dev->lock);
    dev->gpio = gpio;
    dev->mode = STATUS_LED_MODE_OFF;
    dev->period_ms = STATUS_LED_PERIOD_DEFAULT_MS;
    mutex_unlock(&dev->lock);

    result = alloc_chrdev_region(&dev->devno, 0, 1, DEVICE_NAME);
    if (result < 0) {
        dev_err(&pdev->dev, "can't get major number: %d\n", result);
        goto fail_region;
    }

    cdev_init(&dev->cdev, &status_led_fops);
    dev->cdev.owner = THIS_MODULE;
    result = cdev_add(&dev->cdev, dev->devno, 1);
    if (result) {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", result);
        goto fail_cdev;
    }

    /* Creating the device under a class makes devtmpfs create /dev/status_led */
    dev->class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(dev->class)) {
        result = PTR_ERR(dev->class);
        goto fail_class;
    }

    chardev = device_create(dev->class, &pdev->dev, dev->devno, NULL, DEVICE_NAME);
    if (IS_ERR(chardev)) {
        result = PTR_ERR(chardev);
        goto fail_device;
    }

    dev_info(&pdev->dev, "bound to GPIO %d, /dev/%s major %d\n", desc_to_gpio(gpio), DEVICE_NAME,
             MAJOR(dev->devno));
    return 0;

fail_device:
    class_destroy(dev->class);
fail_class:
    cdev_del(&dev->cdev);
fail_cdev:
    unregister_chrdev_region(dev->devno, 1);
fail_region:
    mutex_lock(&dev->lock);
    dev->gpio = NULL;
    mutex_unlock(&dev->lock);
    return result;
}

static int status_led_remove(struct platform_device *pdev)
{
    struct status_led_dev *dev = &status_led;

    device_destroy(dev->class, dev->devno);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devno, 1);

    /* Files still open after this get -ENODEV, since the devm GPIO is released on return */
    mutex_lock(&dev->lock);
    status_led_set_mode(dev, STATUS_LED_MODE_OFF);
    dev->gpio = NULL;
    mutex_unlock(&dev->lock);

    dev_info(&pdev->dev, "unbound, LED off\n");
    return 0;
}

static const struct of_device_id status_led_of_match[] = {
    { .compatible = "aesd,status-led" },
    { }
};
MODULE_DEVICE_TABLE(of, status_led_of_match);

static struct platform_driver status_led_driver = {
    .probe  = status_led_probe,
    .remove = status_led_remove,
    .driver = {
        .name           = DEVICE_NAME,
        .of_match_table = status_led_of_match,
    },
};

static int __init status_led_init(void)
{
    int result;

    mutex_init(&status_led.lock);
    timer_setup(&status_led.timer, status_led_timer_fn, 0);

    result = platform_driver_register(&status_led_driver);
    if (result) {
        pr_err("status_led: platform_driver_register failed: %d\n", result);
        return result;
    }
    pr_info("status_led: loaded\n");
    return 0;
}

static void __exit status_led_exit(void)
{
    /* Unbinds the device first, which stops the timer and turns the LED off */
    platform_driver_unregister(&status_led_driver);
    pr_info("status_led: unloaded\n");
}

module_init(status_led_init);
module_exit(status_led_exit);
