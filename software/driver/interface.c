<<<<<<< HEAD
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

#include "ioctl.h"
#include "cc2520.h"
#include "interface.h"
#include "radio.h"
#include "sack.h"
#include "csma.h"
#include "lpl.h"
#include "debug.h"

struct cc2520_interface *interface_bottom;

// Arrays to hold pin config data
// TODO ask if this is best practice
const unsigned int RESET_PINS[] = {CC2520_0_RESET, CC2520_1_RESET};
const unsigned int CS_PINS[]    = {CC2520_SPI_CS0, CC2520_SPI_CS1};
const unsigned int FIFO_PINS[]  = {CC2520_0_FIFO, CC2520_1_FIFO};
const unsigned int FIFOP_PINS[] = {CC2520_0_FIFOP, CC2520_1_FIFOP};
const unsigned int CCA_PINS[]   = {CC2520_0_CCA, CC2520_1_CCA};
const unsigned int SFD_PINS[]   = {CC2520_0_SFD, CC2520_1_SFD};

static unsigned int major;
static unsigned int minor = CC2520_DEFAULT_MINOR;
static unsigned int num_devices = CC2520_NUM_DEVICES;
static struct class* cl;
struct cc2520_dev* cc2520_devices;

static u8 *tx_buf_c;
static u8 *rx_buf_c;
static size_t tx_pkt_len;
static size_t rx_pkt_len;

// Allows for only a single rx or tx
// to occur simultaneously.
static struct semaphore tx_sem;
static struct semaphore rx_sem;

// Used by the character driver
// to indicate when a blocking tx
// or rx has completed.
static struct semaphore tx_done_sem;
static struct semaphore rx_done_sem;

// Results, stored by the callbacks
static int tx_result;

DECLARE_WAIT_QUEUE_HEAD(cc2520_interface_read_queue);

static void cc2520_interface_tx_done(u8 status);
static void cc2520_interface_rx_done(u8 *buf, u8 len);

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data);
static void interface_ioctl_set_address(struct cc2520_set_address_data *data);
static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data);
static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data);
static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data);
static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data);
static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data);


static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param);

///////////////////////
// Interface callbacks
///////////////////////
void cc2520_interface_tx_done(u8 status)
{
	tx_result = status;
	up(&tx_done_sem);
}

void cc2520_interface_rx_done(u8 *buf, u8 len)
{
	rx_pkt_len = (size_t)len;
	memcpy(rx_buf_c, buf, len);
	wake_up(&cc2520_interface_read_queue);
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

    cc2520_radio_sfd_occurred(nanos, gpio_val);
    return IRQ_HANDLED;
}

static irqreturn_t cc2520_fifop_handler(int irq, void *dev_id)
{
	struct cc2520_dev *dev = dev_id;
    if (gpio_get_value(dev->fifop) == 1) {
        DBG((KERN_INFO "[cc2520] - fifop interrupt occurred\n"));
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
		INFO((KERN_INFO "[cc2520] - write: %s\n", print_buf));
	else
		INFO((KERN_INFO "[cc2520] - read: %s\n", print_buf));
}

// Should accept a 6LowPAN frame, no longer than 127 bytes.
static ssize_t interface_write(
	struct file *filp, const char *in_buf, size_t len, loff_t * off)
{
	int result;
	size_t pkt_len;

	DBG((KERN_INFO "[cc2520] - beginning write\n"));

	// Step 1: Get an exclusive lock on writing to the
	// radio.
	if (filp->f_flags & O_NONBLOCK) {
		result = down_trylock(&tx_sem);
		if (result)
			return -EAGAIN;
	}
	else {
		result = down_interruptible(&tx_sem);
		if (result)
			return -ERESTARTSYS;
	}
	DBG((KERN_INFO "[cc2520] - write lock obtained.\n"));

	// Step 2: Copy the packet to the incoming buffer.
	pkt_len = min(len, (size_t)128);
	if (copy_from_user(tx_buf_c, in_buf, pkt_len)) {
		result = -EFAULT;
		goto error;
	}
	tx_pkt_len = pkt_len;

	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(tx_buf_c, pkt_len, true);
	}

	// Step 3: Launch off into sending this packet,
	// wait for an asynchronous callback to occur in
	// the form of a semaphore.
	interface_bottom->tx(tx_buf_c, pkt_len);
	down(&tx_done_sem);

	// Step 4: Finally return and allow other callers to write
	// packets.
	DBG((KERN_INFO "[cc2520] - wrote %d bytes.\n", pkt_len));
	up(&tx_sem);
	return tx_result ? tx_result : pkt_len;

	error:
		up(&tx_sem);
		return -EFAULT;
}

static ssize_t interface_read(struct file *filp, char __user *buf, size_t count,
			loff_t *offp)
{
	interruptible_sleep_on(&cc2520_interface_read_queue);
	if (copy_to_user(buf, rx_buf_c, rx_pkt_len))
		return -EFAULT;
	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(rx_buf_c, rx_pkt_len, false);
	}
	return rx_pkt_len;
}

static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param)
{
	switch (ioctl_num) {
		case CC2520_IO_RADIO_INIT:
			INFO((KERN_INFO "[cc2520] - radio starting\n"));
			cc2520_radio_start();
			break;
		case CC2520_IO_RADIO_ON:
			INFO((KERN_INFO "[cc2520] - radio turning on\n"));
			cc2520_radio_on();
			break;
		case CC2520_IO_RADIO_OFF:
			INFO((KERN_INFO "[cc2520] - radio turning off\n"));
			cc2520_radio_off();
			break;
		case CC2520_IO_RADIO_SET_CHANNEL:
			interface_ioctl_set_channel((struct cc2520_set_channel_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_ADDRESS:
			interface_ioctl_set_address((struct cc2520_set_address_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_TXPOWER:
			interface_ioctl_set_txpower((struct cc2520_set_txpower_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_ACK:
			interface_ioctl_set_ack((struct cc2520_set_ack_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_LPL:
			interface_ioctl_set_lpl((struct cc2520_set_lpl_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_CSMA:
			interface_ioctl_set_csma((struct cc2520_set_csma_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_PRINT:
			interface_ioctl_set_print((struct cc2520_set_print_messages_data*) ioctl_param);
			break;
	}

	return 0;
}

static int interface_open(struct inode *inode, struct file *filp)
{
	int err = 0;
	int irq = 0;
	struct cc2520_dev *dev;
	dev = container_of(inode->i_cdev, struct cc2520_dev, cdev);
	filp->private_data = dev;
	INFO((KERN_INFO "[BLAH] - opening radio%d\n", dev->id));

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
    state.gpios.fifop_irq = irq;

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
    if (err)
        goto error;
    dev->sfd_irq = irq;
    state.gpios.sfd_irq = irq;

	return 0;

	error:

	ERR((KERN_INFO "[cc2520] - Failed to setup gpio irq for radio%d.\n", dev->id));

	if(dev->fifop_irq)
		free_irq(dev->fifop_irq, dev);
	if(dev->sfd_irq)
		free_irq(dev->sfd_irq, dev);

	return err;
}

int interface_release(struct inode *inode, struct file *filp){
	struct cc2520_dev *dev = filp->private_data;
	free_irq(dev->fifop_irq, dev);
	free_irq(dev->sfd_irq, dev);
	return 0;
}

struct file_operations cc2520_fops = {
	.read = interface_read,
	.write = interface_write,
	.unlocked_ioctl = interface_ioctl,
	.open = interface_open,
	.release = interface_release
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
		ERR((KERN_ALERT "[cc2520] - an error occurred setting print messages\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting debug message print: %i", ldata.debug_level));

	debug_print = ldata.debug_level;
}

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data)
{
	int result;
	struct cc2520_set_channel_data ldata;

	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_channel_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the channel\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - Setting channel to %d\n", ldata.channel));
	cc2520_radio_set_channel(ldata.channel);
}

static void interface_ioctl_set_address(struct cc2520_set_address_data *data)
{
	int result;
	struct cc2520_set_address_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_address_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the address\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting addr: %d ext_addr: %lld pan_id: %d\n",
		ldata.short_addr, ldata.extended_addr, ldata.pan_id));
	cc2520_radio_set_address(ldata.short_addr, ldata.extended_addr, ldata.pan_id);
}

static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data)
{
	int result;
	struct cc2520_set_txpower_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_txpower_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the txpower\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting txpower: %d\n", ldata.txpower));
	cc2520_radio_set_txpower(ldata.txpower);
}

static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data)
{
	int result;
	struct cc2520_set_ack_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_ack_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting soft ack\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting softack timeout: %d\n", ldata.timeout));
	cc2520_sack_set_timeout(ldata.timeout);
}

static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data)
{
	int result;
	struct cc2520_set_lpl_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_lpl_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting lpl\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting lpl enabled: %d, window: %d, interval: %d\n",
		ldata.enabled, ldata.window, ldata.interval));
	cc2520_lpl_set_enabled(ldata.enabled);
	cc2520_lpl_set_listen_length(ldata.window);
	cc2520_lpl_set_wakeup_interval(ldata.interval);
}

static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data)
{
	int result;
	struct cc2520_set_csma_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_csma_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting csma\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting csma enabled: %d, min_backoff: %d, init_backoff: %d, cong_backoff_ %d\n",
		ldata.enabled, ldata.min_backoff, ldata.init_backoff, ldata.cong_backoff));
	cc2520_csma_set_enabled(ldata.enabled);
	cc2520_csma_set_min_backoff(ldata.min_backoff);
	cc2520_csma_set_init_backoff(ldata.init_backoff);
	cc2520_csma_set_cong_backoff(ldata.cong_backoff);
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
	dev->cs    = CS_PINS[index];
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
		ERR((KERN_INFO "[cc2520] - Error while trying to add radio%d.\n", index));
		return err;
	}

	// Create the device in /dev
	device = device_create(cl, NULL, devno, NULL, "radio%d", minor + index);
	if (device == NULL) {
		ERR((KERN_INFO "[cc2520] - Could not create device\n"));
		// Clean up cdev:
		cdev_del(&dev->cdev);
		err = -ENODEV;
		return err;
	}

	INFO((KERN_INFO "[cc2520] - Created node radio%d\n", minor + index));

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
	// 	TODO: switch some of this over to be part of device struct,
	//	make everything else play happily together:
	interface_bottom->tx_done = cc2520_interface_tx_done;
	interface_bottom->rx_done = cc2520_interface_rx_done;

	sema_init(&tx_sem, 1);
	sema_init(&rx_sem, 1);

	sema_init(&tx_done_sem, 0);
	sema_init(&rx_done_sem, 0);
	
	tx_buf_c = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
	if (!tx_buf_c) {
		result = -EFAULT;
		goto error;
	}

	rx_buf_c = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
	if (!rx_buf_c) {
		result = -EFAULT;
		goto error;
	}
	// END TODO

	// Allocate a major number for this device
	result = alloc_chrdev_region(&dev, minor, num_devices, cc2520_name);
	if (result < 0) {
		ERR((KERN_INFO "[cc2520] - Could not allocate a major number\n"));
		goto error;
	}
	major = MAJOR(dev);

	// Create device class
	cl = class_create(THIS_MODULE, cc2520_name);
	if (cl == NULL) {
		ERR((KERN_INFO "[cc2520] - Could not create device class\n"));
		goto error;
	}

	// Allocate the array of devices
	cc2520_devices = (struct cc2520_dev *)kmalloc(
		num_devices * sizeof(struct cc2520_dev), 
		GFP_KERNEL);
	if(cc2520_devices == NULL){
		ERR((KERN_INFO "[cc2520] - Could not allocate cc2520 devices\n"));
		goto error;
	}

	// Register the character devices
	for(i = 0; i < num_devices; ++i){
		result = cc2520_setup_device(&cc2520_devices[i], i);
		if(result) {
			devices_to_destroy = i;
			goto error;
		}
	}
	INFO((KERN_INFO "[cc2520] - Char interface registered on %d\n", major));

	return 0;

	error:

	cc2520_cleanup_devices(devices_to_destroy);

	if (rx_buf_c) {
		kfree(rx_buf_c);
		rx_buf_c = 0;
	}

	if (tx_buf_c) {
		kfree(tx_buf_c);
		tx_buf_c = 0;
	}

	return result;
}

void cc2520_interface_free()
{
	int result;

	result = down_interruptible(&tx_sem);
	if (result) {
		ERR(("[cc2520] - critical error occurred on free."));
	}

	result = down_interruptible(&rx_sem);
	if (result) {
		ERR(("[cc2520] - critical error occurred on free."));
	}

	cc2520_cleanup_devices(num_devices);

	INFO((KERN_INFO "[cc2520] - Removed character devices\n"));

	if (rx_buf_c) {
		kfree(rx_buf_c);
		rx_buf_c = 0;
	}

	if (tx_buf_c) {
		kfree(tx_buf_c);
		tx_buf_c = 0;
	}
}
=======
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
// TODO ask if this is best practice
const unsigned int RESET_PINS[] = {CC2520_0_RESET, CC2520_1_RESET};
const unsigned int CS_PINS[]    = {CC2520_SPI_CS0, CC2520_SPI_CS1};
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
static int tx_result;

DECLARE_WAIT_QUEUE_HEAD(cc2520_interface_read_queue);

static void cc2520_interface_tx_done(u8 status);
static void cc2520_interface_rx_done(u8 *buf, u8 len);

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data);
static void interface_ioctl_set_address(struct cc2520_set_address_data *data);
static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data);
static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data);
static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data);
static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data);
static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data);


static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param);

///////////////////////
// Interface callbacks
///////////////////////
void cc2520_interface_tx_done(u8 status)
{
	tx_result = status;
	up(&tx_done_sem);
}

void cc2520_interface_rx_done(u8 *buf, u8 len)
{
	rx_pkt_len = (size_t)len;
	memcpy(rx_buf_c, buf, len);
	wake_up(&cc2520_interface_read_queue);
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

    cc2520_radio_sfd_occurred(nanos, gpio_val);
    return IRQ_HANDLED;
}

static irqreturn_t cc2520_fifop_handler(int irq, void *dev_id)
{
	struct cc2520_dev *dev = dev_id;
    if (gpio_get_value(dev->fifop) == 1) {
        DBG((KERN_INFO "[cc2520] - fifop interrupt occurred\n"));
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
		INFO((KERN_INFO "[cc2520] - write: %s\n", print_buf));
	else
		INFO((KERN_INFO "[cc2520] - read: %s\n", print_buf));
}

// Should accept a 6LowPAN frame, no longer than 127 bytes.
static ssize_t interface_write(
	struct file *filp, const char *in_buf, size_t len, loff_t * off)
{
	int result;
	size_t pkt_len;
	struct cc2520_dev *dev = filp->private_data;
	int index = dev->id;

	DBG((KERN_INFO "[cc2520] - beginning write\n"));

	// Step 1: Get an exclusive lock on writing to the
	// radio.
	if (filp->f_flags & O_NONBLOCK) {
		result = down_trylock(&tx_sem);
		if (result)
			return -EAGAIN;
	}
	else {
		result = down_interruptible(&tx_sem);
		if (result)
			return -ERESTARTSYS;
	}
	DBG((KERN_INFO "[cc2520] - write lock obtained.\n"));

	// Step 2: Copy the packet to the incoming buffer.
	pkt_len = min(len, (size_t)128);
	if (copy_from_user(tx_buf_c[index], in_buf, pkt_len)) {
		result = -EFAULT;
		goto error;
	}
	tx_pkt_len = pkt_len;

	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(tx_buf_c[index], pkt_len, true);
	}

	// Step 3: Launch off into sending this packet,
	// wait for an asynchronous callback to occur in
	// the form of a semaphore.
	interface_bottom[index]->tx(tx_buf_c[index], pkt_len);
	down(&tx_done_sem);

	// Step 4: Finally return and allow other callers to write
	// packets.
	DBG((KERN_INFO "[cc2520] - wrote %d bytes.\n", pkt_len));
	up(&tx_sem);
	return tx_result ? tx_result : pkt_len;

	error:
		up(&tx_sem);
		return -EFAULT;
}

static ssize_t interface_read(struct file *filp, char __user *buf, size_t count,
			loff_t *offp)
{
	interruptible_sleep_on(&cc2520_interface_read_queue);
	if (copy_to_user(buf, rx_buf_c, rx_pkt_len))
		return -EFAULT;
	if (debug_print >= DEBUG_PRINT_DBG) {
		interface_print_to_log(rx_buf_c, rx_pkt_len, false);
	}
	return rx_pkt_len;
}

static long interface_ioctl(struct file *file,
		 unsigned int ioctl_num,
		 unsigned long ioctl_param)
{
	switch (ioctl_num) {
		case CC2520_IO_RADIO_INIT:
			INFO((KERN_INFO "[cc2520] - radio starting\n"));
			cc2520_radio_start();
			break;
		case CC2520_IO_RADIO_ON:
			INFO((KERN_INFO "[cc2520] - radio turning on\n"));
			cc2520_radio_on();
			break;
		case CC2520_IO_RADIO_OFF:
			INFO((KERN_INFO "[cc2520] - radio turning off\n"));
			cc2520_radio_off();
			break;
		case CC2520_IO_RADIO_SET_CHANNEL:
			interface_ioctl_set_channel((struct cc2520_set_channel_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_ADDRESS:
			interface_ioctl_set_address((struct cc2520_set_address_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_TXPOWER:
			interface_ioctl_set_txpower((struct cc2520_set_txpower_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_ACK:
			interface_ioctl_set_ack((struct cc2520_set_ack_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_LPL:
			interface_ioctl_set_lpl((struct cc2520_set_lpl_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_CSMA:
			interface_ioctl_set_csma((struct cc2520_set_csma_data*) ioctl_param);
			break;
		case CC2520_IO_RADIO_SET_PRINT:
			interface_ioctl_set_print((struct cc2520_set_print_messages_data*) ioctl_param);
			break;
	}

	return 0;
}

static int interface_open(struct inode *inode, struct file *filp)
{
	int err = 0;
	int irq = 0;
	struct cc2520_dev *dev;
	dev = container_of(inode->i_cdev, struct cc2520_dev, cdev);
	filp->private_data = dev;
	INFO((KERN_INFO "[BLAH] - opening radio%d\n", dev->id));

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
    state.gpios.fifop_irq = irq;

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
    if (err)
        goto error;
    dev->sfd_irq = irq;
    state.gpios.sfd_irq = irq;

	return 0;

	error:

	ERR((KERN_INFO "[cc2520] - Failed to setup gpio irq for radio%d.\n", dev->id));

	if(dev->fifop_irq)
		free_irq(dev->fifop_irq, dev);
	if(dev->sfd_irq)
		free_irq(dev->sfd_irq, dev);

	return err;
}

int interface_release(struct inode *inode, struct file *filp){
	struct cc2520_dev *dev = filp->private_data;
	free_irq(dev->fifop_irq, dev);
	free_irq(dev->sfd_irq, dev);
	return 0;
}

struct file_operations cc2520_fops = {
	.read = interface_read,
	.write = interface_write,
	.unlocked_ioctl = interface_ioctl,
	.open = interface_open,
	.release = interface_release
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
		ERR((KERN_ALERT "[cc2520] - an error occurred setting print messages\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting debug message print: %i", ldata.debug_level));

	debug_print = ldata.debug_level;
}

static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data)
{
	int result;
	struct cc2520_set_channel_data ldata;

	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_channel_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the channel\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - Setting channel to %d\n", ldata.channel));
	cc2520_radio_set_channel(ldata.channel);
}

static void interface_ioctl_set_address(struct cc2520_set_address_data *data)
{
	int result;
	struct cc2520_set_address_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_address_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the address\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting addr: %d ext_addr: %lld pan_id: %d\n",
		ldata.short_addr, ldata.extended_addr, ldata.pan_id));
	cc2520_radio_set_address(ldata.short_addr, ldata.extended_addr, ldata.pan_id);
}

static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data)
{
	int result;
	struct cc2520_set_txpower_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_txpower_data));

	if (result) {
		ERR((KERN_ALERT "[cc2520] - an error occurred setting the txpower\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting txpower: %d\n", ldata.txpower));
	cc2520_radio_set_txpower(ldata.txpower);
}

static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data)
{
	int result;
	struct cc2520_set_ack_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_ack_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting soft ack\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting softack timeout: %d\n", ldata.timeout));
	cc2520_sack_set_timeout(ldata.timeout);
}

static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data)
{
	int result;
	struct cc2520_set_lpl_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_lpl_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting lpl\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting lpl enabled: %d, window: %d, interval: %d\n",
		ldata.enabled, ldata.window, ldata.interval));
	cc2520_lpl_set_enabled(ldata.enabled);
	cc2520_lpl_set_listen_length(ldata.window);
	cc2520_lpl_set_wakeup_interval(ldata.interval);
}

static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data)
{
	int result;
	struct cc2520_set_csma_data ldata;
	result = copy_from_user(&ldata, data, sizeof(struct cc2520_set_csma_data));

	if (result) {
		ERR((KERN_INFO "[cc2520] - an error occurred setting csma\n"));
		return;
	}

	INFO((KERN_INFO "[cc2520] - setting csma enabled: %d, min_backoff: %d, init_backoff: %d, cong_backoff_ %d\n",
		ldata.enabled, ldata.min_backoff, ldata.init_backoff, ldata.cong_backoff));
	cc2520_csma_set_enabled(ldata.enabled);
	cc2520_csma_set_min_backoff(ldata.min_backoff);
	cc2520_csma_set_init_backoff(ldata.init_backoff);
	cc2520_csma_set_cong_backoff(ldata.cong_backoff);
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
	dev->cs    = CS_PINS[index];
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
		ERR((KERN_INFO "[cc2520] - Error while trying to add radio%d.\n", index));
		return err;
	}

	// Create the device in /dev
	device = device_create(cl, NULL, devno, NULL, "radio%d", minor + index);
	if (device == NULL) {
		ERR((KERN_INFO "[cc2520] - Could not create device\n"));
		// Clean up cdev:
		cdev_del(&dev->cdev);
		err = -ENODEV;
		return err;
	}

	INFO((KERN_INFO "[cc2520] - Created node radio%d\n", minor + index));

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
		ERR((KERN_INFO "[cc2520] - Could not allocate a major number\n"));
		goto error;
	}
	major = MAJOR(dev);

	// Create device class
	cl = class_create(THIS_MODULE, cc2520_name);
	if (cl == NULL) {
		ERR((KERN_INFO "[cc2520] - Could not create device class\n"));
		goto error;
	}

	// Allocate the array of devices
	cc2520_devices = (struct cc2520_dev *)kmalloc(
		num_devices * sizeof(struct cc2520_dev), 
		GFP_KERNEL);
	if(cc2520_devices == NULL){
		ERR((KERN_INFO "[cc2520] - Could not allocate cc2520 devices\n"));
		goto error;
	}

	// Register the character devices
	for(i = 0; i < num_devices; ++i){
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

		tx_buf_c[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!tx_buf_c) {
			result = -EFAULT;
			goto error;
		}

		rx_buf_c[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!rx_buf_c) {
			result = -EFAULT;
			goto error;
		}
	}
	INFO((KERN_INFO "[cc2520] - Char interface registered on %d\n", major));

	return 0;

	error:

	cc2520_cleanup_devices(devices_to_destroy);

	if (rx_buf_c) {
		kfree(rx_buf_c);
		rx_buf_c = 0;
	}

	if (tx_buf_c) {
		kfree(tx_buf_c);
		tx_buf_c = 0;
	}

	return result;
}

void cc2520_interface_free()
{
	int result;

	result = down_interruptible(&tx_sem);
	if (result) {
		ERR(("[cc2520] - critical error occurred on free."));
	}

	result = down_interruptible(&rx_sem);
	if (result) {
		ERR(("[cc2520] - critical error occurred on free."));
	}

	cc2520_cleanup_devices(num_devices);

	INFO((KERN_INFO "[cc2520] - Removed character devices\n"));

	if (rx_buf_c) {
		kfree(rx_buf_c);
		rx_buf_c = 0;
	}

	if (tx_buf_c) {
		kfree(tx_buf_c);
		tx_buf_c = 0;
	}
}
>>>>>>> a016e64c043afb8234c0b367435a00dd775d544e
