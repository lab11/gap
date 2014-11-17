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

#include "../gapspi/gapspi.h"

#include "nrf51822.h"
#include "bcp.h"
#include "ioctl.h"
#include "debug.h"

#define DRIVER_AUTHOR  "Brad Campbell <bradjc@umich.edu>"
#define DRIVER_DESC    "A driver for the nRF51822 BLE chip over SPI."
#define DRIVER_VERSION "0.1"

// Defines the level of debug output
uint8_t debug_print = DEBUG_PRINT_ERR;
const char nrf51822_name[] = "nRF51822";

// Properties of the character device
static unsigned int major;
static dev_t char_d_mm;
static struct cdev char_d_cdev;
static struct class* cl;
static struct device* de;

// GPIO for the interrupt from the nRF to us
#define NRF51822_INTERRUPT_PIN 51   // P9_16

// Keep track of the irq assigned so it can be freed
int nrf51822_irq = 0;

// SPI
#define GAPSPI_CS_INDEX 2
// struct spi_device* nrf51822_spi_device;
// #define SPI_BUS 1
// #define SPI_BUS_CS0 0
// #define SPI_BUS_SPEED 8000000

static u8 spi_command_buffer[128];
static u8 spi_data_buffer[128];

static struct spi_transfer spi_tsfers[4];
static struct spi_message spi_msg;

static spinlock_t spi_spin_lock;
static unsigned long spi_spin_lock_flags;
static bool spi_pending = false;

// Buffers for holding data to/from userspace
#define CHAR_DEVICE_BUFFER_LEN 256

static u8 *buf_to_nrf51822;
static u8 *buf_to_user;
static size_t buf_to_nrf51822_len;
static size_t buf_to_user_len;

// Queue to wake up read() calls when data is available
DECLARE_WAIT_QUEUE_HEAD(nrf51822_to_user_queue);




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

	// Wait for data to be ready to send to the user.
	wait_event_interruptible(nrf51822_to_user_queue, (buf_to_user_len > 0));

	// Copy the result from the nRF51822 to the user
	result  = copy_to_user(buf, buf_to_user, buf_to_user_len);
	if (result) {
		return -EFAULT;
	}

	user_len = buf_to_user_len;

	// Reset the buffer length so the user_queue wait doesn't trigger
	buf_to_user_len = 0;

	return user_len;
}

static long nrf51822_ioctl(struct file *file,
							unsigned int ioctl_num,
							unsigned long ioctl_param)
{
	int result;

	switch (ioctl_num) {
		case NRF51822_IOCTL_SET_DEBUG_VERBOSITY:
			result = nrf51822_ioctl_set_debug_verbosity((struct nrf51822_set_debug_verbosity_data*) ioctl_param);
			break;
		case NRF51822_IOCTL_SIMPLE_COMMAND:
			result = nrf51822_ioctl_simple_command((struct nrf51822_simple_command*) ioctl_param);
			break;
		default:
			result = -ENOTTY;
	}

	return result;
}

static unsigned int nrf51822_poll (struct file *file, poll_table *wait)
{
  unsigned int mask = 0;

  poll_wait(file, &nrf51822_to_user_queue, wait);

  // always writable
  mask |= POLLOUT | POLLWRNORM;

  if (buf_to_user_len > 0) {
  	// readable
  	mask |= POLLIN | POLLRDNORM;
  }

  return mask;
}

struct file_operations fops = {
	.read = nrf51822_read,
	.write = nrf51822_write,
	.unlocked_ioctl = nrf51822_ioctl,
	.open = NULL,
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

	debug_print = ldata.debug_level;

	return 0;
}

// Issue a command to the nRF51822. These are 1 byte commands.
static int nrf51822_ioctl_simple_command(struct nrf51822_simple_command *data)
{
	int result;
	struct nrf51822_simple_command ldata;

	result = copy_from_user(&ldata, data, sizeof(struct nrf51822_simple_command));

	if (result) {
		ERR(KERN_ALERT, "an error occurred a command\n");
		return 0;
	}

	INFO(KERN_INFO, "issuing command: %i", ldata.command);

	return nrf51822_issue_simple_command(ldata.command);
}


/////////////////////
// Application logic
/////////////////////

// Manually check if the interrupt line is high.
static void nrf51822_check_irq (void) {
	// Check if we need to read the IRQ again
	if (gpio_get_value(NRF51822_INTERRUPT_PIN) == 1) {
		// Interrupt is still high.
		// Read again.

		// Put this delay here so the nRF51822 has time to set the SPI buffer
		// again.
		usleep_range(25, 50);

		nrf51822_read_irq();
	}
}


// The result of the interrupt is in spi_buffer
static void nrf51822_read_irq_done (void *arg) {
	int len;

	DBG(KERN_INFO, "Got IRQ data from nrf51822\n");

	// There is an issue where the nRF51822 needs 7.1us between CS and CLK.
	// We violate that currently, so the first byte may be invalid (0x00).
	// If the second byte was zero as well, that denotes an error.
	if (spi_data_buffer[0] == 0 && spi_data_buffer[1] == 0) {
		// This was an invalid transfer. Some error occurred.

		ERR(KERN_INFO, "First two bytes zero. Ignoring response from nRF51822\n");

		spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
		spi_pending = false;
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);

		nrf51822_check_irq();

		return;

	} else if (spi_data_buffer[0] == 0) {
		// The first byte was an error. Skip it and use the second byte
		// as the length.
		len = spi_data_buffer[1]+1;

		// Move the packet from the SPI buffer to the thing that can be read()
		memcpy(buf_to_user, spi_data_buffer, len);
		buf_to_user_len = len;

	} else {
		// The first byte was the length
		len = spi_data_buffer[0]+1;

		// Move the packet from the SPI buffer to the thing that can be read()
		memcpy(buf_to_user, spi_data_buffer, len);
		buf_to_user_len = len;
	}

	// Release the lock on the SPI buffer
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	spi_pending = false;
	spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);

	// Notify the read() call that there is data for it.
	wake_up(&nrf51822_to_user_queue);

	nrf51822_check_irq();
}

// Set up the SPI transfer to query the nRF51822 about why it interrupted
// us.
static void nrf51822_read_irq(void) {

	// Check if we can get a lock on the SPI rx/tx buffer. If not, ignore
	// this interrupt.
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	if (spi_pending) {
		// Something else is using the SPI. Just skip this interrupt.
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
		return;
	} else {
		// Claim the SPI buffer.
		spi_pending = true;
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
	}

	DBG(KERN_INFO, "setup SPI transfer to investigate interrupt\n");

    // Clear the transfer buffers
    memset(spi_tsfers, 0, sizeof(spi_tsfers));

	// Setup SPI transfers.
	// The first byte is the command byte. Because we just want interrupt
	// data we send the READ_IRQ command. The next bytes are the response
	// from the nRF51822. In the future we will be able to read the correct
	// number of bytes from the based on the first response byte from the slave
    // (the length byte). Right now just read the maximum length.
	spi_tsfers[0].tx_buf = spi_command_buffer;
	spi_tsfers[0].rx_buf = spi_data_buffer;
	spi_tsfers[0].len = 40;
	spi_tsfers[0].cs_change = 1;

	spi_command_buffer[0] = BCP_COMMAND_READ_IRQ;

	spi_message_init(&spi_msg);
	spi_msg.complete = nrf51822_read_irq_done;
	spi_msg.context = NULL;
	spi_message_add_tail(&spi_tsfers[0], &spi_msg);

	gap_spi_async(&spi_msg, GAPSPI_CS_INDEX);
}


//
// Interrupts
//

static irqreturn_t nrf51822_interrupt_handler(int irq, void *dev_id)
{
	INFO(KERN_INFO, "got interrupt from nRF51822\n");

	nrf51822_read_irq();

	return IRQ_HANDLED;
}

// Called after a command byte has been sent to the nRF51822.
// Check if the nRF51822 sent us data.
void nrf51822_issue_simple_command_done(void *arg) {
	bool received_valid_data = false;

	if (spi_data_buffer[0] == 0 && spi_data_buffer[1] == 0) {
		// No data to be transfered to the host.

	} else {
		int len;

		received_valid_data = true;

		if (spi_data_buffer[0] == 0) {
			// The first byte was an error. Skip it and use the second byte
			// as the length.
			len = spi_data_buffer[1]+1;

			// Move the packet from the SPI buffer to the thing that can be read()
			memcpy(buf_to_user, spi_data_buffer, len);
			buf_to_user_len = len;

		} else {
			// The first byte was the length
			len = spi_data_buffer[0]+1;

			// Move the packet from the SPI buffer to the thing that can be read()
			memcpy(buf_to_user, spi_data_buffer, len);
			buf_to_user_len = len;
		}
	}


	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	spi_pending = false;
	spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);

	if (received_valid_data) {
		// Notify the read() call that there is data for it.
		wake_up(&nrf51822_to_user_queue);
	}

	DBG(KERN_INFO, "Finished writing the command.\n");

	nrf51822_check_irq();
}

// Send command to the nRF51822
int nrf51822_issue_simple_command(uint8_t command) {

	// Check if the SPI driver is in use
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	if (spi_pending) {
		// Something else is using the SPI. Return that we can't issue the
		// command right now.
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
		return -EINTR;
	} else {
		// Claim the SPI buffer.
		spi_pending = true;
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
	}

	DBG(KERN_INFO, "Issuing command %i\n", command);

    // Clear the transfer buffers
    memset(spi_tsfers, 0, sizeof(spi_tsfers));

	// Actually issue the command.
	// Also be prepared to receive data
	spi_tsfers[0].tx_buf = spi_command_buffer;
	spi_tsfers[0].rx_buf = spi_data_buffer;
	spi_tsfers[0].len = 40;
	spi_tsfers[0].cs_change = 1;

	spi_command_buffer[0] = command;

	spi_message_init(&spi_msg);
	spi_msg.complete = nrf51822_issue_simple_command_done;
	spi_msg.context = NULL;
	spi_message_add_tail(&spi_tsfers[0], &spi_msg);

	gap_spi_async(&spi_msg, GAPSPI_CS_INDEX);

	return 0;
}

/////////////////////
// SPI
/////////////////////

// static int nrf51822_spi_probe(struct spi_device *spi_device)
// {
//     ERR(KERN_INFO, "Inserting SPI protocol driver.\n");
//     nrf51822_spi_device = spi_device;
//     return 0;
// }

// static int nrf51822_spi_remove(struct spi_device *spi_device)
// {
//     ERR(KERN_INFO, "Removing SPI protocol driver.");
//     nrf51822_spi_device = NULL;
//     return 0;
// }


// // Configure SPI
// static struct spi_driver nrf51822_spi_driver = {
// 	.driver = {
// 		.name = nrf51822_name,
// 		.owner = THIS_MODULE,
// 	},
// 	.probe = nrf51822_spi_probe,
// 	.remove = nrf51822_spi_remove,
// };




/////////////////
// init/free
///////////////////

int init_module(void)
{
	int result;

	// struct spi_master *spi_master;
	// struct spi_device *spi_device = NULL;
	// struct device *pdev;
	// char buff[64];

	// Make sure that gapspi.ko is loaded first
	request_module("gapspi");

	//
	// Configure the buffer to transmit data to the user
	//

	buf_to_user = kmalloc(CHAR_DEVICE_BUFFER_LEN, GFP_KERNEL);
	if (!buf_to_user) {
		result = -EFAULT;
		goto error;
	}

	//
	// Setup the interrupt from the nRF51822
	//

	// Setup the GPIO
	result = gpio_request_one(NRF51822_INTERRUPT_PIN, GPIOF_DIR_IN, NULL);
	if (result) goto error;

	// Make this pin interruptable
	nrf51822_irq = gpio_to_irq(NRF51822_INTERRUPT_PIN);
	if (nrf51822_irq < 0) {
		result = nrf51822_irq;
		goto error;
	}

	// Enable the interrupt
	result = request_irq(nrf51822_irq,
	                     nrf51822_interrupt_handler,
	                     IRQF_TRIGGER_RISING,
	                     "nrf51822_interrupt",
	                     NULL);
	if (result) goto error;


	//
	// SPI
	//
	// Setup the SPI device

	// Init the SPI lock
	// spin_lock_init(&spi_spin_lock);

	// spi_master = spi_busnum_to_master(SPI_BUS);
	// if (!spi_master) {
	// 	ERR(KERN_ALERT, "spi_busnum_to_master(%d) returned NULL\n", SPI_BUS);
	// 	//ERR((KERN_ALERT "Missing modprobe spi-bcm2708?\n"));
	// 	goto error;
	// }

	// spi_device = spi_alloc_device(spi_master);
	// if (!spi_device) {
	// 	put_device(&spi_master->dev);
	// 	ERR(KERN_ALERT, "spi_alloc_device() failed\n");
	// 	goto error;
	// }

	// spi_device->chip_select = SPI_BUS_CS0;

	// /* Check whether this SPI bus.cs is already claimed */
	// snprintf(buff,
	// 	     sizeof(buff),
	// 	     "%s.%u",
	//          dev_name(&spi_device->master->dev),
	//          spi_device->chip_select);

	// pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);

	// if (pdev) {
	// 	if (pdev->driver != NULL) {
	// 		ERR(KERN_INFO,
	// 		"Driver [%s] already registered for %s. \
	// 		Nuking from orbit.\n",
	// 		pdev->driver->name, buff);
	// 	} else {
	// 		ERR(KERN_INFO,
	// 		"Previous driver registered with no loaded module. \
	// 		Nuking from orbit.\n");
	// 	}

	// 	device_unregister(pdev);
	// }

	// spi_device->max_speed_hz = SPI_BUS_SPEED;
	// spi_device->mode = SPI_MODE_0;
	// spi_device->bits_per_word = 8;
	// spi_device->irq = -1;

	// spi_device->controller_state = NULL;
	// spi_device->controller_data = NULL;
	// strlcpy(spi_device->modalias, nrf51822_name, SPI_NAME_SIZE);

	// result = spi_add_device(spi_device);
	// if (result < 0) {
	// 	spi_dev_put(spi_device);
	// 	ERR(KERN_ALERT, "spi_add_device() failed: %d\n", result);
	// }

	// put_device(&spi_master->dev);


	// // Register SPI
	// result = spi_register_driver(&nrf51822_spi_driver);
 //    if (result < 0) {
 //    	ERR(KERN_INFO, "Could not register SPI driver.\n");
 //    	goto error;
 //    }

    //
	// Configure the character device in /dev
	//

	// Allocate a major number for this device
	result = alloc_chrdev_region(&char_d_mm, 0, 1, nrf51822_name);
	if (result < 0) {
		ERR(KERN_INFO, "Could not allocate a major number\n");
		goto error;
	}
	major = MAJOR(char_d_mm);

	// Register the character device
	cdev_init(&char_d_cdev, &fops);
	char_d_cdev.owner = THIS_MODULE;
	result = cdev_add(&char_d_cdev, char_d_mm, 1);
	if (result < 0) {
		ERR(KERN_INFO, "Unable to register char dev\n");
		goto error;
	}
	INFO(KERN_INFO, "Char interface registered on %d\n", major);

	cl = class_create(THIS_MODULE, "nrf51822");
	if (cl == NULL) {
		ERR(KERN_INFO, "Could not create device class\n");
		goto error;
	}

	// Create the device in /dev/nrf51822
	// TODO: the 1 should not be hardcoded
	de = device_create(cl, NULL, char_d_mm, NULL, "nrf51822_%d", 1);
	if (de == NULL) {
		ERR(KERN_INFO, "Could not create device\n");
		goto error;
	}

	return 0;


	error:

	if (buf_to_user) {
		kfree(buf_to_user);
		buf_to_user = NULL;
	}

	gpio_free(NRF51822_INTERRUPT_PIN);
    free_irq(nrf51822_irq, NULL);

	// spi_unregister_device(spi_device);
 //    spi_unregister_driver(&nrf51822_spi_driver);

	return -1;
}

void cleanup_module(void)
{
	gpio_free(NRF51822_INTERRUPT_PIN);
	free_irq(nrf51822_irq, NULL);

	// if (nrf51822_spi_device) {
	// 	spi_unregister_device(nrf51822_spi_device);
	// }

	// spi_unregister_driver(&nrf51822_spi_driver);

	cdev_del(&char_d_cdev);
	unregister_chrdev(char_d_mm, nrf51822_name);
	device_destroy(cl, char_d_mm);
	class_destroy(cl);

	INFO(KERN_INFO, "Removed character device\n");

	if (buf_to_user) {
		kfree(buf_to_user);
		buf_to_user = NULL;
	}
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
