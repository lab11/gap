#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/wait.h>

#include "ioctl.h"
#include "cc2520.h"
#include "unique.h"
#include "interface.h"
#include "radio.h"
#include "sack.h"
#include "csma.h"
#include "lpl.h"
#include "debug.h"

static ssize_t interface_write(struct file *filp, const char *in_buf, size_t len, loff_t * off);
static ssize_t interface_read(struct file *filp, char __user *buf, size_t count,
			loff_t *offp);
static long interface_ioctl(struct file *filp,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param);
static int interface_open(struct inode *inode, struct file *filp);

static irqreturn_t cc2520_sfd_handler(int irq, void *dev_id);
static irqreturn_t cc2520_fifop_handler(int irq, void *dev_id);

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_address(struct cc2520_set_address_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data);


static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param);

struct file_operations cc2520_fops = {
	.read = interface_read,
	.write = interface_write,
	.unlocked_ioctl = interface_ioctl,
	.open = interface_open
};


int cc2520_interface_init(struct cc2520_dev *dev)
{
	int irq = 0;
	int err = 0;
	int devno = MKDEV(config.major, dev->id);
	struct device *device;

    // Initialize/add char device to kernel
	cdev_init(&dev->cdev, &cc2520_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &cc2520_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err) {
		ERR(KERN_INFO, "Error while trying to add radio%d.\n", dev->id);
		return err;
	}

	// Create the device in /dev
	device = device_create(config.cl, NULL, devno, NULL, "cc2520_%d", dev->id);
	if (device == NULL) {
		ERR(KERN_INFO, "Could not create device\n");
		// Clean up cdev:
		cdev_del(&dev->cdev);
		err = -ENODEV;
		return err;
	}

	INFO(KERN_INFO, "Created node cc2520_%d\n", dev->id);

	sema_init(&dev->tx_sem, 1);
	sema_init(&dev->rx_sem, 1);

	sema_init(&dev->tx_done_sem, 0);
	sema_init(&dev->rx_done_sem, 0);

	init_waitqueue_head(&dev->cc2520_interface_read_queue);

	// set up the interrupts and GPIOs
		// Setup Interrupts
    // Setup FIFOP GPIO Interrupt
    irq = gpio_to_irq(dev->fifop);
    if (irq < 0) {
        err = irq;
        goto error;
    }
	err = request_irq(
        irq,
        cc2520_fifop_handler,
        IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
        "fifopHandler",
        dev
    );
    if (err)
        goto error;
    dev->fifop_irq = irq;

    // Setup SFD GPIO Interrupt
    irq = gpio_to_irq(dev->sfd);
    if (irq < 0) {
        err = irq;
        goto error;
    }
    err = request_irq(
        irq,
        cc2520_sfd_handler,
        IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
        "sfdHandler",
        dev
    );
    if (err) {
        goto error;
    }
    dev->sfd_irq = irq;

	INFO(KERN_INFO, "Char interface registered on %d\n", config.major);

	return 0;

	error:

		ERR(KERN_INFO, "Error interface init\n");

		device_destroy(config.cl, MKDEV(config.major, dev->id));
		cdev_del(&dev->cdev);

		if (dev->fifop_irq) {
			free_irq(dev->fifop_irq, dev);
		}
		if (dev->sfd_irq) {
			free_irq(dev->sfd_irq, dev);
		}

		return err;
}

void cc2520_interface_free(struct cc2520_dev *dev)
{
	int result;

	device_destroy(config.cl, MKDEV(config.major, dev->id));
	free_irq(dev->fifop_irq, dev);
	free_irq(dev->sfd_irq, dev);
	cdev_del(&dev->cdev);

	INFO(KERN_INFO, "Removed character devices\n");

	result = down_interruptible(&dev->tx_sem);
	if (result) {
		ERR(KERN_INFO, "critical error occurred on free.");
	}

	result = down_interruptible(&dev->rx_sem);
	if (result) {
		ERR(KERN_INFO, "critical error occurred on free.");
	}

	INFO(KERN_INFO, "Removed interface\n");
}


///////////////////////
// Interface callbacks
///////////////////////

void cc2520_interface_tx_done(u8 status, struct cc2520_dev *dev)
{
	dev->tx_result = status;
	up(&dev->tx_done_sem);
}

void cc2520_interface_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	dev->rx_pkt_len = (size_t)len;
	memcpy(dev->rx_buf_c, buf, len);
	wake_up(&dev->cc2520_interface_read_queue);
}

//////////////////////////
// Interrupt Handles
/////////////////////////

static irqreturn_t cc2520_sfd_handler(int irq, void *dev_id)
{
    int gpio_val;
    struct timespec ts;
    struct cc2520_dev *dev = dev_id;
    s64 nanos;

    // NOTE: For now we're assuming no delay between SFD called
    // and actual SFD received. The TinyOS implementations call
    // for a few uS of delay, but it's likely not needed.
    getrawmonotonic(&ts);
    nanos = timespec_to_ns(&ts);
    gpio_val = gpio_get_value(dev->sfd);

    cc2520_radio_sfd_occurred(nanos, gpio_val, dev);
    return IRQ_HANDLED;
}

static irqreturn_t cc2520_fifop_handler(int irq, void *dev_id)
{
	struct cc2520_dev *dev = dev_id;
    if (gpio_get_value(dev->fifop) == 1) {
        DBG(KERN_INFO, "fifop%d interrupt occurred\n", dev->id);
        cc2520_radio_fifop_occurred(dev);
    }
    return IRQ_HANDLED;
}

////////////////////
// Implementation
////////////////////

static void interface_print_to_log(char *buf, int len, bool is_write)
{
	char print_buf[641];
	char *print_buf_ptr;
	int i;

	print_buf_ptr = print_buf;

	for (i = 0; i < len && i < 128; i++) {
		print_buf_ptr += sprintf(print_buf_ptr, " 0x%02X", buf[i]);
	}
	*(print_buf_ptr) = '\0';

	if (is_write)
		INFO(KERN_INFO, "write: %s\n", print_buf);
	else
		INFO(KERN_INFO, "read: %s\n", print_buf);
}

// Should accept a 6LowPAN frame, no longer than 127 bytes.
static ssize_t interface_write(struct file *filp, const char *in_buf, size_t len, loff_t * off)
{
	int result;
	size_t pkt_len;
	struct cc2520_dev *dev = filp->private_data;

	DBG(KERN_INFO, "radio%d beginning write.\n", dev->id);

	// Step 1: Get an exclusive lock on writing to the
	// radio.
	if (filp->f_flags & O_NONBLOCK) {
		result = down_trylock(&dev->tx_sem);
		if (result)
			return -EAGAIN;
	}
	else {
		result = down_interruptible(&dev->tx_sem);
		if (result)
			return -ERESTARTSYS;
	}
	DBG(KERN_INFO, "radio%d write lock obtained.\n", dev->id);

	// Step 2: Copy the packet to the incoming buffer.
	pkt_len = min(len, (size_t)128);
	if (copy_from_user(dev->tx_buf_c, in_buf, pkt_len)) {
		result = -EFAULT;
		goto error;
	}
	dev->tx_pkt_len = pkt_len;

	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(dev->tx_buf_c, pkt_len, true);
	}

	// Step 3: Launch off into sending this packet,
	// wait for an asynchronous callback to occur in
	// the form of a semaphore.
	cc2520_unique_tx(dev->tx_buf_c, pkt_len, dev);
	down(&dev->tx_done_sem);

	// Step 4: Finally return and allow other callers to write
	// packets.
	DBG(KERN_INFO, "radio%d wrote %d bytes.\n", dev->id, pkt_len);
	up(&dev->tx_sem);
	return dev->tx_result ? dev->tx_result : pkt_len;

	error:
		up(&dev->tx_sem);
		return -EFAULT;
}

static ssize_t interface_read(struct file *filp, char __user *buf, size_t count,
			loff_t *offp)
{
	struct cc2520_dev *dev = filp->private_data;

	interruptible_sleep_on(&dev->cc2520_interface_read_queue);
	if (copy_to_user(buf, dev->rx_buf_c, dev->rx_pkt_len))
		return -EFAULT;
	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(dev->rx_buf_c, dev->rx_pkt_len, false);
	}
	return dev->rx_pkt_len;
}

static long interface_ioctl(struct file *filp,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param)
{
	struct cc2520_dev *dev = filp->private_data;

	switch (ioctl_num) {
		case CC2520_IO_RADIO_INIT:
			INFO(KERN_INFO, "radio%d starting.\n", dev->id);
			cc2520_radio_start(dev);
			break;
		case CC2520_IO_RADIO_ON:
			INFO(KERN_INFO, "radio%d turning on.\n", dev->id);
			cc2520_radio_on(dev);
			break;
		case CC2520_IO_RADIO_OFF:
			INFO(KERN_INFO, "radio%d turning off.\n", dev->id);
			cc2520_radio_off(dev);
			break;
		case CC2520_IO_RADIO_SET_CHANNEL:
			interface_ioctl_set_channel((struct cc2520_set_channel_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_ADDRESS:
			interface_ioctl_set_address((struct cc2520_set_address_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_TXPOWER:
			interface_ioctl_set_txpower((struct cc2520_set_txpower_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_ACK:
			interface_ioctl_set_ack((struct cc2520_set_ack_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_LPL:
			interface_ioctl_set_lpl((struct cc2520_set_lpl_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_CSMA:
			interface_ioctl_set_csma((struct cc2520_set_csma_data*) ioctl_param, dev);
			break;
		case CC2520_IO_RADIO_SET_PRINT:
			interface_ioctl_set_print((struct cc2520_set_print_messages_data*) ioctl_param);
			break;
	}

	return 0;
}

static int interface_open(struct inode *inode, struct file *filp)
{
	struct cc2520_dev *dev;
	dev = container_of(inode->i_cdev, struct cc2520_dev, cdev);
	filp->private_data = dev;
	DBG(KERN_INFO, "opening radio%d.\n", dev->id);

	return 0;
}



/////////////////
// IOCTL Handlers
///////////////////
static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data)
{
	int result;
	struct cc2520_set_print_messages_data ldata;

	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_print_messages_data));

	if (result) {
		ERR(KERN_ALERT, "an error occurred setting print messages\n");
		return;
	}

	INFO(KERN_INFO, "setting debug message print: %i", ldata.debug_level);

	debug_print = ldata.debug_level;
}

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_channel_data ldata;

	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_channel_data));

	if (result) {
		ERR(KERN_ALERT, "an error occurred setting the channel\n");
		return;
	}

	INFO(KERN_INFO, "Setting channel to %d\n", ldata.channel);
	cc2520_radio_set_channel(ldata.channel, dev);
}

static void interface_ioctl_set_address(struct cc2520_set_address_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_address_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_address_data));

	if (result) {
		ERR(KERN_ALERT, "an error occurred setting the address\n");
		return;
	}

	INFO(KERN_INFO, "setting addr: %d ext_addr: %lld pan_id: %d\n",
		ldata.short_addr, ldata.extended_addr, ldata.pan_id);
	cc2520_radio_set_address(ldata.short_addr, ldata.extended_addr, ldata.pan_id, dev);
}

static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_txpower_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_txpower_data));

	if (result) {
		ERR(KERN_ALERT, "an error occurred setting the txpower\n");
		return;
	}

	INFO(KERN_INFO, "setting txpower: %d\n", ldata.txpower);
	cc2520_radio_set_txpower(ldata.txpower, dev);
}

static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_ack_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_ack_data));

	if (result) {
		ERR(KERN_INFO, "an error occurred setting soft ack\n");
		return;
	}

	INFO(KERN_INFO, "setting softack timeout: %d\n", ldata.timeout);
	cc2520_sack_set_timeout(ldata.timeout, dev);
}

static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_lpl_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_lpl_data));

	if (result) {
		ERR(KERN_INFO, "an error occurred setting lpl\n");
		return;
	}

	INFO(KERN_INFO, "setting lpl enabled: %d, window: %d, interval: %d\n",
		ldata.enabled, ldata.window, ldata.interval);
	cc2520_lpl_set_enabled(ldata.enabled, dev);
	cc2520_lpl_set_listen_length(ldata.window, dev);
	cc2520_lpl_set_wakeup_interval(ldata.interval, dev);
}

static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data, struct cc2520_dev *dev)
{
	int result;
	struct cc2520_set_csma_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_csma_data));

	if (result) {
		ERR(KERN_INFO, "an error occurred setting csma\n");
		return;
	}

	INFO(KERN_INFO, "setting csma enabled: %d, min_backoff: %d, init_backoff: %d, cong_backoff_ %d\n",
		ldata.enabled, ldata.min_backoff, ldata.init_backoff, ldata.cong_backoff);
	cc2520_csma_set_enabled(ldata.enabled, dev);
	cc2520_csma_set_min_backoff(ldata.min_backoff, dev);
	cc2520_csma_set_init_backoff(ldata.init_backoff, dev);
	cc2520_csma_set_cong_backoff(ldata.cong_backoff, dev);
}
