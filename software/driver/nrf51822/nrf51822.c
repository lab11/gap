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
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#include "../gapspi/gapspi.h"

#include "nrf51822.h"
#include "bcp.h"
#include "ioctl.h"
#include "debug.h"

#define DRIVER_AUTHOR  "Brad Campbell <bradjc@umich.edu>"
#define DRIVER_DESC    "A driver for the nRF51822 BLE chip over SPI."
#define DRIVER_VERSION "0.2"

// Defines the level of debug output
// uint8_t debug_print = DEBUG_PRINT_ERR;
const char nrf51822_name[] = "nRF51822";

// Properties of the character device
// static unsigned int major;
// static dev_t char_d_mm;
// // static struct cdev char_d_cdev;
// static struct class* cl;
// static struct device* de;

// GPIO for the interrupt from the nRF to us
// #define NRF51822_INTERRUPT_PIN 51   // P9_16

// Keep track of the irq assigned so it can be freed
// int nrf51822_irq = 0;

// SPI
// #define GAPSPI_CS_INDEX 2

// static u8 spi_command_buffer[128];
// static u8 spi_data_buffer[128];

// static struct spi_transfer spi_tsfers[4];
// static struct spi_message spi_msg;

// static spinlock_t spi_spin_lock;
// static unsigned long spi_spin_lock_flags;
// static bool spi_pending = false;

// // Buffers for holding data to/from userspace
// #define CHAR_DEVICE_BUFFER_LEN 256

// static u8 *buf_to_nrf51822;
// static u8 *buf_to_user;
// static size_t buf_to_nrf51822_len;
// static size_t buf_to_user_len;


// struct nrf51822_dev {
// 	int id;

// 	unsigned int chipselect_demux_index;

// 	int interrupt;
// 	unsigned int irq;

// 	struct cdev cdev;

// 	wait_queue_head_t to_user_queue;

// 	u8 spi_command_buffer[128];
// 	u8 spi_data_buffer[128];

// 	struct spi_transfer spi_tsfers[4];
// 	struct spi_message spi_msg;

// 	spinlock_t spi_spin_lock;
// 	unsigned long spi_spin_lock_flags;
// 	bool spi_pending = false;

// 	u8 buf_to_nrf51822[CHAR_DEVICE_BUFFER_LEN];
// 	u8 buf_to_user[CHAR_DEVICE_BUFFER_LEN];
// 	size_t buf_to_nrf51822_len;
// 	size_t buf_to_user_len;
// };

// struct nrf51822_config {
// 	dev_t chr_dev;
// 	unsigned int major;
// 	struct class* cl;

// 	u8 num_radios; // Number of radios on board.

// 	struct nrf51822_dev *radios;
// };

struct nrf51822_config config;


// Queue to wake up read() calls when data is available
// DECLARE_WAIT_QUEUE_HEAD(nrf51822_to_user_queue);




// Not implemented currently
static ssize_t nrf51822_write(struct file *filp,
                               const char *in_buf,
                               size_t len,
                               loff_t * off)
{
		return -EFAULT;
}

// Called by the read() command from user space
static ssize_t nrf51822_read(struct file *filp,
                              char __user *buf,
                              size_t count,
                              loff_t *offp)
{
	int result;
	int user_len;
	struct nrf51822_dev *dev = filp->private_data;

	// Wait for data to be ready to send to the user.
	wait_event_interruptible(dev->to_user_queue, (dev->buf_to_user_len > 0));

	// Copy the result from the nRF51822 to the user
	result  = copy_to_user(buf, dev->buf_to_user, dev->buf_to_user_len);
	if (result) {
		return -EFAULT;
	}

	user_len = dev->buf_to_user_len;

	// Reset the buffer length so the user_queue wait doesn't trigger
	dev->buf_to_user_len = 0;

	return user_len;
}

static long nrf51822_ioctl(struct file *file,
							unsigned int ioctl_num,
							unsigned long ioctl_param)
{
	int result;
	struct nrf51822_dev *dev = file->private_data;

	switch (ioctl_num) {
		case NRF51822_IOCTL_SET_DEBUG_VERBOSITY:
			result = nrf51822_ioctl_set_debug_verbosity((struct nrf51822_set_debug_verbosity_data*) ioctl_param);
			break;
		case NRF51822_IOCTL_SIMPLE_COMMAND:
			result = nrf51822_ioctl_simple_command((struct nrf51822_simple_command*) ioctl_param, dev);
			break;
		default:
			result = -ENOTTY;
	}

	return result;
}

static unsigned int nrf51822_poll (struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
	struct nrf51822_dev *dev = file->private_data;

	poll_wait(file, &dev->to_user_queue, wait);

	// always writable
	mask |= POLLOUT | POLLWRNORM;

	if (dev->buf_to_user_len > 0) {
		// readable
		mask |= POLLIN | POLLRDNORM;
	}

	return mask;
}

static int nrf51822_open(struct inode *inode, struct file *filp)
{
	struct nrf51822_dev *dev;
	dev = container_of(inode->i_cdev, struct nrf51822_dev, cdev);
	filp->private_data = dev;
	DBG(KERN_INFO, "opening radio%d.\n", dev->id);

	return 0;
}

struct file_operations fops = {
	.read = nrf51822_read,
	.write = nrf51822_write,
	.unlocked_ioctl = nrf51822_ioctl,
	.open = nrf51822_open,
	.release = NULL,
	.poll = nrf51822_poll
};

///////////////////
// IOCTL Handlers
///////////////////

// Change the print verbosity
static int nrf51822_ioctl_set_debug_verbosity(struct nrf51822_set_debug_verbosity_data *data)
{
	int result;
	struct nrf51822_set_debug_verbosity_data ldata;

	result = copy_from_user(&ldata, data, sizeof(struct nrf51822_set_debug_verbosity_data));

	if (result) {
		ERR(KERN_ALERT, "an error occurred setting print messages\n");
		return 0;
	}

	INFO(KERN_INFO, "setting debug message print: %i", ldata.debug_level);

	config.debug_print = ldata.debug_level;

	return 0;
}

// Issue a command to the nRF51822. These are 1 byte commands.
static int nrf51822_ioctl_simple_command(struct nrf51822_simple_command *data, struct nrf51822_dev *dev)
{
	int result;
	struct nrf51822_simple_command ldata;

	result = copy_from_user(&ldata, data, sizeof(struct nrf51822_simple_command));

	if (result) {
		ERR(KERN_ALERT, "an error occurred a command\n");
		return 0;
	}

	INFO(KERN_INFO, "issuing command: %i", ldata.command);

	return nrf51822_issue_simple_command(ldata.command, dev);
}


/////////////////////
// Application logic
/////////////////////

// Manually check if the interrupt line is high.
static void nrf51822_check_irq (struct nrf51822_dev *dev) {
	// Check if we need to read the IRQ again
	if (gpio_get_value(dev->pin_interrupt) == 1) {
		// Interrupt is still high.
		// Read again.

		// Put this delay here so the nRF51822 has time to set the SPI buffer
		// again.
		usleep_range(25, 50);

		nrf51822_read_irq(dev);
	}
}


// The result of the interrupt is in spi_buffer
static void nrf51822_read_irq_done (void *arg) {
	struct nrf51822_dev *dev = arg;
	int len;

	DBG(KERN_INFO, "Got IRQ data from nrf51822\n");

	// There is an issue where the nRF51822 needs 7.1us between CS and CLK.
	// We violate that currently, so the first byte may be invalid (0x00).
	// If the second byte was zero as well, that denotes an error.
	if (dev->spi_data_buffer[0] == 0 && dev->spi_data_buffer[1] == 0) {
		// This was an invalid transfer. Some error occurred.

		ERR(KERN_INFO, "First two bytes zero. Ignoring response from nRF51822:%i\n", dev->id);

		spin_lock_irqsave(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
		dev->spi_pending = false;
		spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);

		nrf51822_check_irq(dev);

		return;

	} else if (dev->spi_data_buffer[0] == 0) {
		// The first byte was an error. Skip it and use the second byte
		// as the length.
		len = dev->spi_data_buffer[1]+1;

		// Move the packet from the SPI buffer to the thing that can be read()
		memcpy(dev->buf_to_user, dev->spi_data_buffer, len);
		dev->buf_to_user_len = len;

	} else {
		// The first byte was the length
		len = dev->spi_data_buffer[0]+1;

		// Move the packet from the SPI buffer to the thing that can be read()
		memcpy(dev->buf_to_user, dev->spi_data_buffer, len);
		dev->buf_to_user_len = len;
	}

	// Release the lock on the SPI buffer
	spin_lock_irqsave(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	dev->spi_pending = false;
	spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);

	// Notify the read() call that there is data for it.
	wake_up(&dev->to_user_queue);

	nrf51822_check_irq(dev);
}

// Set up the SPI transfer to query the nRF51822 about why it interrupted
// us.
static void nrf51822_read_irq(struct nrf51822_dev *dev) {

	// Check if we can get a lock on the SPI rx/tx buffer. If not, ignore
	// this interrupt.
	spin_lock_irqsave(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	if (dev->spi_pending) {
		// Something else is using the SPI. Just skip this interrupt.
		spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
		return;
	} else {
		// Claim the SPI buffer.
		dev->spi_pending = true;
		spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	}

	DBG(KERN_INFO, "setup SPI transfer to investigate interrupt\n");

    // Clear the transfer buffers
    memset(dev->spi_tsfers, 0, sizeof(dev->spi_tsfers));

	// Setup SPI transfers.
	// The first byte is the command byte. Because we just want interrupt
	// data we send the READ_IRQ command. The next bytes are the response
	// from the nRF51822. In the future we will be able to read the correct
	// number of bytes from the based on the first response byte from the slave
    // (the length byte). Right now just read the maximum length.
	dev->spi_tsfers[0].tx_buf = dev->spi_command_buffer;
	dev->spi_tsfers[0].rx_buf = dev->spi_data_buffer;
	dev->spi_tsfers[0].len = 40;
	dev->spi_tsfers[0].cs_change = 1;

	dev->spi_command_buffer[0] = BCP_COMMAND_READ_IRQ;

	spi_message_init(&dev->spi_msg);
	dev->spi_msg.complete = nrf51822_read_irq_done;
	dev->spi_msg.context = dev;
	spi_message_add_tail(&dev->spi_tsfers[0], &dev->spi_msg);

	gap_spi_async(&dev->spi_msg, dev->chipselect_demux_index);
}


//
// Interrupts
//

static irqreturn_t nrf51822_interrupt_handler(int irq, void *dev_id)
{
    struct nrf51822_dev *dev = dev_id;
	INFO(KERN_INFO, "got interrupt from nRF51822:%i\n", dev->id);

	nrf51822_read_irq(dev);

	return IRQ_HANDLED;
}

// Called after a command byte has been sent to the nRF51822.
// Check if the nRF51822 sent us data.
void nrf51822_issue_simple_command_done(void *arg) {
	struct nrf51822_dev *dev = arg;
	bool received_valid_data = false;

	if (dev->spi_data_buffer[0] == 0 && dev->spi_data_buffer[1] == 0) {
		// No data to be transfered to the host.

	} else {
		int len;

		received_valid_data = true;

		if (dev->spi_data_buffer[0] == 0) {
			// The first byte was an error. Skip it and use the second byte
			// as the length.
			len = dev->spi_data_buffer[1]+1;

			// Move the packet from the SPI buffer to the thing that can be read()
			memcpy(dev->buf_to_user, dev->spi_data_buffer, len);
			dev->buf_to_user_len = len;

		} else {
			// The first byte was the length
			len = dev->spi_data_buffer[0]+1;

			// Move the packet from the SPI buffer to the thing that can be read()
			memcpy(dev->buf_to_user, dev->spi_data_buffer, len);
			dev->buf_to_user_len = len;
		}
	}


	spin_lock_irqsave(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	dev->spi_pending = false;
	spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);

	if (received_valid_data) {
		// Notify the read() call that there is data for it.
		wake_up(&dev->to_user_queue);
	}

	DBG(KERN_INFO, "Finished writing the command.\n");

	nrf51822_check_irq(dev);
}

// Send command to the nRF51822
int nrf51822_issue_simple_command(uint8_t command, struct nrf51822_dev *dev) {

	// Check if the SPI driver is in use
	spin_lock_irqsave(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	if (dev->spi_pending) {
		// Something else is using the SPI. Return that we can't issue the
		// command right now.
		spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
		return -EINTR;
	} else {
		// Claim the SPI buffer.
		dev->spi_pending = true;
		spin_unlock_irqrestore(&dev->spi_spin_lock, dev->spi_spin_lock_flags);
	}

	DBG(KERN_INFO, "Issuing command %i on radio %i\n", command, dev->id);

    // Clear the transfer buffers
    memset(dev->spi_tsfers, 0, sizeof(dev->spi_tsfers));

	// Actually issue the command.
	// Also be prepared to receive data
	dev->spi_tsfers[0].tx_buf = dev->spi_command_buffer;
	dev->spi_tsfers[0].rx_buf = dev->spi_data_buffer;
	dev->spi_tsfers[0].len = 40;
	dev->spi_tsfers[0].cs_change = 1;

	dev->spi_command_buffer[0] = command;

	spi_message_init(&dev->spi_msg);
	dev->spi_msg.complete = nrf51822_issue_simple_command_done;
	dev->spi_msg.context = dev;
	spi_message_add_tail(&dev->spi_tsfers[0], &dev->spi_msg);

	gap_spi_async(&dev->spi_msg, dev->chipselect_demux_index);

	return 0;
}


/////////////////
// init/free
///////////////////

static int nrf51822_probe(struct platform_device *pltf)
{
	struct device_node *np = pltf->dev.of_node;
	int result;
	const __be32 *prop;
	struct nrf51822_dev *dev;
	int i;
	int err;

	INFO(KERN_INFO, "Loading kernel module v%s\n", DRIVER_VERSION);

	// Make sure that gapspi.ko is loaded first
	request_module("gapspi");

	memset(&config, 0, sizeof(struct nrf51822_config));

	config.debug_print = DEBUG_PRINT_DBG;

	// Get the parameters for the driver from the device tree
	prop = of_get_property(np, "num-radios", NULL);
	if (!prop) {
		ERR(KERN_ALERT, "Got NULL for the number of radios.\n");
		goto error0;
	}
	config.num_radios = be32_to_cpup(prop);
	INFO(KERN_INFO, "Number of nRF51822 radios %i\n", config.num_radios);

	// Instantiate the correct number of radios
	config.radios = (struct nrf51822_dev*) kmalloc(config.num_radios * sizeof(struct nrf51822_dev), GFP_KERNEL);
	if (config.radios == NULL) {
		ERR(KERN_INFO, "Could not allocate nrf51822 devices\n");
		goto error0;
	}
	memset(config.radios, 0, config.num_radios * sizeof(struct nrf51822_dev));


	for (i=0; i<config.num_radios; i++) {
		char buf[64];

		dev = &config.radios[i];

		// Configure the GPIOs
		snprintf(buf, 64, "interrupt%i-gpio", i);
		dev->pin_interrupt = of_get_named_gpio(np, buf, 0);

		if (!gpio_is_valid(dev->pin_interrupt)) {
			ERR(KERN_ALERT, "interrupt gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->pin_interrupt, GPIOF_IN, "interrupt");
		if (err) goto error1;

		// Make this pin interruptable
		dev->irq = gpio_to_irq(dev->pin_interrupt);
		if (dev->irq < 0) {
			result = dev->irq;
			goto error1;
		}

		// Enable the interrupt
		result = request_irq(dev->irq,
		                     nrf51822_interrupt_handler,
		                     IRQF_TRIGGER_RISING,
		                     "nrf51822_interrupt",
		                     NULL);
		if (result) goto error1;

		// Get other properties
		snprintf(buf, 64, "radio%i-csmux", i);
		prop = of_get_property(np, buf, NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL for the csmux index.\n");
			goto error1;
		}
		dev->chipselect_demux_index = be32_to_cpup(prop);

		dev->id = i;
		dev->spi_pending = false;
		init_waitqueue_head(&dev->to_user_queue);

		INFO(KERN_INFO, "GPIO CONFIG radio:%i\n", i);
		INFO(KERN_INFO, "  INTERRUPT: %i\n", dev->pin_interrupt);
		INFO(KERN_INFO, "SETTINGS radio:%i\n", i);
		INFO(KERN_INFO, "  DEMUX: %i\n", dev->chipselect_demux_index);
	}

	// Allocate a major number for this device
	err = alloc_chrdev_region(&config.chr_dev, 0, config.num_radios, nrf51822_name);
	if (err < 0) {
		ERR(KERN_INFO, "Could not allocate a major number\n");
		goto error1;
	}
	config.major = MAJOR(config.chr_dev);

	// Create device class
	config.cl = class_create(THIS_MODULE, nrf51822_name);
	if (config.cl == NULL) {
		ERR(KERN_INFO, "Could not create device class\n");
		goto error2;
	}

	for (i=0; i<config.num_radios; i++) {
		struct device* de;
		dev = &config.radios[i];
		dev->devno = MKDEV(config.major, dev->id);

		// Register the character device
		cdev_init(&dev->cdev, &fops);
		dev->cdev.owner = THIS_MODULE;
		dev->cdev.ops = &fops;
		result = cdev_add(&dev->cdev, dev->devno, 1);
		if (result < 0) {
			ERR(KERN_INFO, "Unable to register char dev\n");
			goto error3;
		}
		INFO(KERN_INFO, "Char interface registered on %d\n", config.major);

		de = device_create(config.cl, NULL, dev->devno, NULL, "nrf51822_%d", dev->id);
		if (de == NULL) {
			ERR(KERN_INFO, "Could not create device %i\n", dev->id);
			goto error3;
		}
	}


	return 0;

	error3:
		class_destroy(config.cl);
	error2:
		unregister_chrdev_region(config.chr_dev, config.num_radios);
	error1:
		for (i=0; i<config.num_radios; i++) {
			if (config.radios[i].irq > 0) {
    			free_irq(config.radios[i].irq, NULL);
			}
		}
		kfree(config.radios);
	error0:
		return -1;




	//
	// Configure the buffer to transmit data to the user
	//

	// buf_to_user = kmalloc(CHAR_DEVICE_BUFFER_LEN, GFP_KERNEL);
	// if (!buf_to_user) {
	// 	result = -EFAULT;
	// 	goto error;
	// }

	//
	// Setup the interrupt from the nRF51822
	//

	// Setup the GPIO
	// result = gpio_request_one(NRF51822_INTERRUPT_PIN, GPIOF_DIR_IN, NULL);
	// if (result) goto error;

	// // Make this pin interruptable
	// nrf51822_irq = gpio_to_irq(NRF51822_INTERRUPT_PIN);
	// if (nrf51822_irq < 0) {
	// 	result = nrf51822_irq;
	// 	goto error;
	// }

	// // Enable the interrupt
	// result = request_irq(nrf51822_irq,
	//                      nrf51822_interrupt_handler,
	//                      IRQF_TRIGGER_RISING,
	//                      "nrf51822_interrupt",
	//                      NULL);
	// if (result) goto error;

    //
	// Configure the character device in /dev
	//

	// Allocate a major number for this device
	// result = alloc_chrdev_region(&char_d_mm, 0, 1, nrf51822_name);
	// if (result < 0) {
	// 	ERR(KERN_INFO, "Could not allocate a major number\n");
	// 	goto error;
	// }
	// major = MAJOR(char_d_mm);

	// // Register the character device
	// cdev_init(&char_d_cdev, &fops);
	// char_d_cdev.owner = THIS_MODULE;
	// result = cdev_add(&char_d_cdev, char_d_mm, 1);
	// if (result < 0) {
	// 	ERR(KERN_INFO, "Unable to register char dev\n");
	// 	goto error;
	// }
	// INFO(KERN_INFO, "Char interface registered on %d\n", major);

	// cl = class_create(THIS_MODULE, "nrf51822");
	// if (cl == NULL) {
	// 	ERR(KERN_INFO, "Could not create device class\n");
	// 	goto error;
	// }

	// // Create the device in /dev/nrf51822
	// // TODO: the 1 should not be hardcoded
	// de = device_create(cl, NULL, char_d_mm, NULL, "nrf51822_%d", 1);
	// if (de == NULL) {
	// 	ERR(KERN_INFO, "Could not create device\n");
	// 	goto error;
	// }

	// return 0;


	// error:

	// if (buf_to_user) {
	// 	kfree(buf_to_user);
	// 	buf_to_user = NULL;
	// }

	// gpio_free(NRF51822_INTERRUPT_PIN);
 //    free_irq(nrf51822_irq, NULL);

	// return -1;
}

static int nrf51822_remove(struct platform_device *pltf)
{
	int i;

	for (i=0; i<config.num_radios; i++) {
		struct nrf51822_dev *dev = &config.radios[i];
	//gpio_free(NRF51822_INTERRUPT_PIN);
		free_irq(dev->irq, NULL);
		cdev_del(&dev->cdev);

		unregister_chrdev(dev->devno, nrf51822_name);
		device_destroy(config.cl, dev->devno);
	}



	class_destroy(config.cl);

	INFO(KERN_INFO, "Removed character device\n");

	// if (buf_to_user) {
	// 	kfree(buf_to_user);
	// 	buf_to_user = NULL;
	// }
	return 0;
}

static const struct of_device_id nrf51822_of_ids[] = {
	{.compatible = "lab11,nrf51822", },
	{},
};
MODULE_DEVICE_TABLE(of, nrf51822_of_ids);

static struct platform_driver nrf51822_driver = {
	.driver = {
		.name = "gap-nrf51822",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nrf51822_of_ids),
	},
	.probe = nrf51822_probe,
	.remove = nrf51822_remove,
};

module_platform_driver(nrf51822_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
