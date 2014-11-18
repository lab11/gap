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
#include "interface.h"
#include "radio.h"
#include "sack.h"
#include "csma.h"
#include "lpl.h"
#include "debug.h"

struct cc2520_interface *interface_bottom[CC2520_NUM_DEVICES];

// Arrays to hold pin config data
const unsigned int RESET_PINS[] = {CC2520_0_RESET, CC2520_1_RESET};
const unsigned int FIFO_PINS[]  = {CC2520_0_FIFO, CC2520_1_FIFO};
const unsigned int FIFOP_PINS[] = {CC2520_0_FIFOP, CC2520_1_FIFOP};
const unsigned int CCA_PINS[]   = {CC2520_0_CCA, CC2520_1_CCA};
const unsigned int SFD_PINS[]   = {CC2520_0_SFD, CC2520_1_SFD};

static unsigned int major;
static unsigned int minor = CC2520_DEFAULT_MINOR;
static unsigned int num_devices = CC2520_NUM_DEVICES;
static struct class* cl;
struct cc2520_dev* cc2520_devices;

static u8 *tx_buf_c[CC2520_NUM_DEVICES];
static u8 *rx_buf_c[CC2520_NUM_DEVICES];
static size_t tx_pkt_len[CC2520_NUM_DEVICES];
static size_t rx_pkt_len[CC2520_NUM_DEVICES];

// Allows for only a single rx or tx
// to occur simultaneously.
static struct semaphore tx_sem[CC2520_NUM_DEVICES];
static struct semaphore rx_sem[CC2520_NUM_DEVICES];

// Used by the character driver
// to indicate when a blocking tx
// or rx has completed.
static struct semaphore tx_done_sem[CC2520_NUM_DEVICES];
static struct semaphore rx_done_sem[CC2520_NUM_DEVICES];

// Results, stored by the callbacks
static int tx_result[CC2520_NUM_DEVICES];

static wait_queue_head_t cc2520_interface_read_queue[CC2520_NUM_DEVICES];

static void cc2520_interface_tx_done(u8 status, struct cc2520_dev *dev);
static void cc2520_interface_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_address(struct cc2520_set_address_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data, struct cc2520_dev *dev);
static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data, int index);
static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data, int index);
static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data, int index);
static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data);


static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param);

///////////////////////
// Interface callbacks
///////////////////////

void cc2520_interface_tx_done(u8 status, struct cc2520_dev *dev)
{
	int index = dev->id;
	tx_result[index] = status;
	up(&tx_done_sem[index]);
}

void cc2520_interface_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;
	rx_pkt_len[index] = (size_t)len;
	memcpy(rx_buf_c[index], buf, len);
	wake_up(&cc2520_interface_read_queue[index]);
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

    //DBG((KERN_INFO "[cc2520] - sfd interrupt occurred at %lld, %d\n", (long long int)nanos, gpio_val));

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
static ssize_t interface_write(
	struct file *filp, const char *in_buf, size_t len, loff_t * off)
{
	int result;
	size_t pkt_len;
	struct cc2520_dev *dev = filp->private_data;
	int index = dev->id;

	DBG(KERN_INFO, "radio%d beginning write.\n", index);

	// Step 1: Get an exclusive lock on writing to the
	// radio.
	if (filp->f_flags & O_NONBLOCK) {
		result = down_trylock(&tx_sem[index]);
		if (result)
			return -EAGAIN;
	}
	else {
		result = down_interruptible(&tx_sem[index]);
		if (result)
			return -ERESTARTSYS;
	}
	DBG(KERN_INFO, "radio%d write lock obtained.\n", index);

	// Step 2: Copy the packet to the incoming buffer.
	pkt_len = min(len, (size_t)128);
	if (copy_from_user(tx_buf_c[index], in_buf, pkt_len)) {
		result = -EFAULT;
		goto error;
	}
	tx_pkt_len[index] = pkt_len;

	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(tx_buf_c[index], pkt_len, true);
	}

	// Step 3: Launch off into sending this packet,
	// wait for an asynchronous callback to occur in
	// the form of a semaphore.
	interface_bottom[index]->tx(tx_buf_c[index], pkt_len, dev);
	down(&tx_done_sem[index]);

	// Step 4: Finally return and allow other callers to write
	// packets.
	DBG(KERN_INFO, "radio%d wrote %d bytes.\n", index, pkt_len);
	up(&tx_sem[index]);
	return tx_result[index] ? tx_result[index] : pkt_len;

	error:
		up(&tx_sem[index]);
		return -EFAULT;
}

static ssize_t interface_read(struct file *filp, char __user *buf, size_t count,
			loff_t *offp)
{
	struct cc2520_dev *dev = filp->private_data;
	int index = dev->id;

	interruptible_sleep_on(&cc2520_interface_read_queue[index]);
	if (copy_to_user(buf, rx_buf_c[index], rx_pkt_len[index]))
		return -EFAULT;
	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(rx_buf_c[index], rx_pkt_len[index], false);
	}
	return rx_pkt_len[index];
}

static long interface_ioctl(struct file *filp,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param)
{
	struct cc2520_dev *dev = filp->private_data;
	int index = dev->id;

	switch (ioctl_num) {
		case CC2520_IO_RADIO_INIT:
			INFO(KERN_INFO, "radio%d starting.\n", index);
			cc2520_radio_start(dev);
			break;
		case CC2520_IO_RADIO_ON:
			INFO(KERN_INFO, "radio%d turning on.\n", index);
			cc2520_radio_on(dev);
			break;
		case CC2520_IO_RADIO_OFF:
			INFO(KERN_INFO, "radio%d turning off.\n", index);
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
			interface_ioctl_set_ack((struct cc2520_set_ack_data*) ioctl_param, index);
			break;
		case CC2520_IO_RADIO_SET_LPL:
			interface_ioctl_set_lpl((struct cc2520_set_lpl_data*) ioctl_param, index);
			break;
		case CC2520_IO_RADIO_SET_CSMA:
			interface_ioctl_set_csma((struct cc2520_set_csma_data*) ioctl_param, index);
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

struct file_operations cc2520_fops = {
	.read = interface_read,
	.write = interface_write,
	.unlocked_ioctl = interface_ioctl,
	.open = interface_open
};

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

static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data, int index)
{
	int result;
	struct cc2520_set_ack_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_ack_data));

	if (result) {
		ERR(KERN_INFO, "an error occurred setting soft ack\n");
		return;
	}

	INFO(KERN_INFO, "setting softack timeout: %d\n", ldata.timeout);
	cc2520_sack_set_timeout(ldata.timeout, index);
}

static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data, int index)
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
	cc2520_lpl_set_enabled(ldata.enabled, index);
	cc2520_lpl_set_listen_length(ldata.window, index);
	cc2520_lpl_set_wakeup_interval(ldata.interval, index);
}

static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data, int index)
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
	cc2520_csma_set_enabled(ldata.enabled, index);
	cc2520_csma_set_min_backoff(ldata.min_backoff, index);
	cc2520_csma_set_init_backoff(ldata.init_backoff, index);
	cc2520_csma_set_cong_backoff(ldata.cong_backoff, index);
}

/////////////////
// init/free
///////////////////
static int cc2520_setup_device(struct cc2520_dev *dev, int index){
	int err = 0;
	int devno = MKDEV(major, minor + index);
	struct device *device;

	dev->id = index;

	// Initialize pin configurations
	dev->reset = RESET_PINS[index];
	dev->fifo  = FIFO_PINS[index];
	dev->fifop = FIFOP_PINS[index];
	dev->cca   = CCA_PINS[index];
	dev->sfd   = SFD_PINS[index];

    // Initialize/add char device to kernel
	cdev_init(&dev->cdev, &cc2520_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &cc2520_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if(err){
		ERR(KERN_INFO, "Error while trying to add radio%d.\n", index);
		return err;
	}

	// Create the device in /dev
	device = device_create(cl, NULL, devno, NULL, "cc2520_%d", minor + index);
	if (device == NULL) {
		ERR(KERN_INFO, "Could not create device\n");
		// Clean up cdev:
		cdev_del(&dev->cdev);
		err = -ENODEV;
		return err;
	}

	INFO(KERN_INFO, "Created node radio%d\n", minor + index);

	return 0;
}

static void cc2520_destroy_device(struct cc2520_dev *dev, int index){
	device_destroy(cl, MKDEV(major, minor + index));
	free_irq(dev->fifop_irq, dev);
	free_irq(dev->sfd_irq, dev);
	cdev_del(&dev->cdev);
	return;
}

static void cc2520_cleanup_devices(int devices_to_destroy){
	int i;

	if(cc2520_devices){
		for(i = minor; i < devices_to_destroy; ++i){
			cc2520_destroy_device(&cc2520_devices[i], i);
		}
		kfree(cc2520_devices);
		cc2520_devices = NULL;
	}

	if(cl){
		class_destroy(cl);
	}

	unregister_chrdev_region(MKDEV(major, minor), num_devices);
	return;
}

int cc2520_interface_init()
{
	int result = 0;
	int i;
	int devices_to_destroy = 0;
	dev_t dev = 0;

	// Allocate a major number for this device
	result = alloc_chrdev_region(&dev, minor, num_devices, cc2520_name);
	if (result < 0) {
		ERR(KERN_INFO, "Could not allocate a major number\n");
		goto error;
	}
	major = MAJOR(dev);

	// Create device class
	cl = class_create(THIS_MODULE, cc2520_name);
	if (cl == NULL) {
		ERR(KERN_INFO, "Could not create device class\n");
		goto error;
	}

	// Allocate the array of devices
	cc2520_devices = (struct cc2520_dev *)kmalloc(
		num_devices * sizeof(struct cc2520_dev),
		GFP_KERNEL);
	if(cc2520_devices == NULL){
		ERR(KERN_INFO, "Could not allocate cc2520 devices\n");
		goto error;
	}

	// Register the character devices
	for(i = 0; i < num_devices; ++i){
		int irq = 0;
		int err = 0;

		result = cc2520_setup_device(&cc2520_devices[i], i);
		if(result) {
			devices_to_destroy = i;
			goto error;
		}

		interface_bottom[i]->tx_done = cc2520_interface_tx_done;
		interface_bottom[i]->rx_done = cc2520_interface_rx_done;

		sema_init(&tx_sem[i], 1);
		sema_init(&rx_sem[i], 1);

		sema_init(&tx_done_sem[i], 0);
		sema_init(&rx_done_sem[i], 0);

		init_waitqueue_head(&cc2520_interface_read_queue[i]);

		// set up the interrupts and GPIOs
			// Setup Interrupts
	    // Setup FIFOP GPIO Interrupt
	    irq = gpio_to_irq(cc2520_devices[i].fifop);
	    if (irq < 0) {
	        err = irq;
	        goto error;
	    }
		err = request_irq(
	        irq,
	        cc2520_fifop_handler,
	        IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
	        "fifopHandler",
	        &cc2520_devices[i]
	    );
	    if (err)
	        goto error;
	    cc2520_devices[i].fifop_irq = irq;
	    state.gpios.fifop_irq = irq;

	    // Setup SFD GPIO Interrupt
	    irq = gpio_to_irq(cc2520_devices[i].sfd);
	    if (irq < 0) {
	        err = irq;
	        goto error;
	    }
	    err = request_irq(
	        irq,
	        cc2520_sfd_handler,
	        IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
	        "sfdHandler",
	        &cc2520_devices[i]
	    );
	    if (err)
	        goto error;
	    cc2520_devices[i].sfd_irq = irq;
	    state.gpios.sfd_irq = irq;

		tx_buf_c[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!tx_buf_c[i]) {
			result = -EFAULT;
			goto error;
		}

		rx_buf_c[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!rx_buf_c[i]) {
			result = -EFAULT;
			goto error;
		}
	}

	INFO(KERN_INFO, "Char interface registered on %d\n", major);

	return 0;

	error:

	ERR(KERN_INFO, "Error interface init\n");

	cc2520_cleanup_devices(devices_to_destroy);
	for(i = 0; i < num_devices; ++i){
		if (rx_buf_c[i]) {
			kfree(rx_buf_c[i]);
			rx_buf_c[i] = 0;
		}

		if (tx_buf_c[i]) {
			kfree(tx_buf_c[i]);
			tx_buf_c[i] = 0;
		}
	}
	return result;
}

void cc2520_interface_free()
{
	int result;
	int i;

	cc2520_cleanup_devices(num_devices);

	INFO(KERN_INFO, "Removed character devices\n");

	for(i = 0; i < num_devices; ++i){
		result = down_interruptible(&tx_sem[i]);
		if (result) {
			ERR(KERN_INFO, "critical error occurred on free.");
		}

		result = down_interruptible(&rx_sem[i]);
		if (result) {
			ERR(KERN_INFO, "critical error occurred on free.");
		}

		if (rx_buf_c[i]) {
			kfree(rx_buf_c[i]);
			rx_buf_c[i] = 0;
		}

		if (tx_buf_c[i]) {
			kfree(tx_buf_c[i]);
			tx_buf_c[i] = 0;
		}
	}

	INFO(KERN_INFO, "Removed interface\n");
}
