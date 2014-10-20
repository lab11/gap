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

#include "nrf51822.h"
#include "bcp.h"
#include "ioctl.h"
#include "debug.h"

#define DRIVER_AUTHOR  "Brad Campbell <bradjc@umich.edu>"
#define DRIVER_DESC    "A driver for the nRF51822 BLE chip over SPI."
#define DRIVER_VERSION "0.1"



// Defines the level of debug output
uint8_t debug_print = DEBUG_PRINT_DBG;

//struct cc2520_state state;
const char nrf51822_name[] = "nRF51822";

struct cc2520_interface *interface_bottom;

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
struct spi_device* nrf51822_spi_device;
#define SPI_BUS 1
#define SPI_BUS_CS0 0
#define SPI_BUS_SPEED 500000

static u8 spi_buffer[128];

static struct spi_transfer spi_tsfer;
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

// Allows for only a single rx or tx
// to occur simultaneously.
// static struct semaphore tx_sem;
// static struct semaphore rx_sem;

// Used by the character driver
// to indicate when a blocking tx
// or rx has completed.
// static struct semaphore tx_done_sem;
// static struct semaphore rx_done_sem;

// Results, stored by the callbacks
// static int tx_result;


// Queue to wake up read() calls when data is available
DECLARE_WAIT_QUEUE_HEAD(nrf51822_to_user_queue);

// static void cc2520_interface_tx_done(u8 status);
// static void cc2520_interface_rx_done(u8 *buf, u8 len);

// static void interface_ioctl_set_channel(struct cc2520_set_channel_data *data);
// static void interface_ioctl_set_address(struct cc2520_set_address_data *data);
// static void interface_ioctl_set_txpower(struct cc2520_set_txpower_data *data);
// static void interface_ioctl_set_ack(struct cc2520_set_ack_data *data);
// static void interface_ioctl_set_lpl(struct cc2520_set_lpl_data *data);
// static void interface_ioctl_set_csma(struct cc2520_set_csma_data *data);
// static void interface_ioctl_set_print(struct cc2520_set_print_messages_data *data);


// static long interface_ioctl(struct file *file,
// 		 unsigned int ioctl_num,
// 		 unsigned long ioctl_param);

///////////////////////
// Interface callbacks
///////////////////////
// void cc2520_interface_tx_done(u8 status)
// {
// 	tx_result = status;
// 	up(&tx_done_sem);
// }

// void cc2520_interface_rx_done(u8 *buf, u8 len)
// {
// 	rx_pkt_len = (size_t)len;
// 	memcpy(rx_buf_c, buf, len);
// 	wake_up(&nrf51822_to_user_queue);
// }

////////////////////
// Implementation
////////////////////

// static void interface_print_to_log(char *buf, int len, bool is_write)
// {
// 	char print_buf[641];
// 	char *print_buf_ptr;
// 	int i;

// 	print_buf_ptr = print_buf;

// 	for (i = 0; i < len && i < 128; i++) {
// 		print_buf_ptr += sprintf(print_buf_ptr, " 0x%02X", buf[i]);
// 	}
// 	*(print_buf_ptr) = '\0';

// 	if (is_write)
// 		INFO((KERN_INFO "write: %s\n", print_buf));
// 	else
// 		INFO((KERN_INFO "read: %s\n", print_buf));
// }

// Not implemented
static ssize_t nrf51822_write(struct file *filp,
                               const char *in_buf,
                               size_t len,
                               loff_t * off)
{
	// int result;
	// size_t pkt_len;

	// DBG((KERN_INFO "beginning write\n"));

	// // Step 1: Get an exclusive lock on writing to the
	// // radio.
	// if (filp->f_flags & O_NONBLOCK) {
	// 	result = down_trylock(&tx_sem);
	// 	if (result)
	// 		return -EAGAIN;
	// }
	// else {
	// 	result = down_interruptible(&tx_sem);
	// 	if (result)
	// 		return -ERESTARTSYS;
	// }
	// DBG((KERN_INFO "write lock obtained.\n"));

	// // Step 2: Copy the packet to the incoming buffer.
	// pkt_len = min(len, (size_t)128);
	// if (copy_from_user(tx_buf_c, in_buf, pkt_len)) {
	// 	result = -EFAULT;
	// 	goto error;
	// }
	// tx_pkt_len = pkt_len;

	// if (debug_print >= DEBUG_PRINT_DBG) {
	// 	interface_print_to_log(tx_buf_c, pkt_len, true);
	// }

	// // Step 3: Launch off into sending this packet,
	// // wait for an asynchronous callback to occur in
	// // the form of a semaphore.
	// interface_bottom->tx(tx_buf_c, pkt_len);
	// down(&tx_done_sem);

	// // Step 4: Finally return and allow other callers to write
	// // packets.
	// DBG((KERN_INFO "wrote %d bytes.\n", pkt_len));
	// up(&tx_sem);
	// return tx_result ? tx_result : pkt_len;

	// error:
	// 	up(&tx_sem);
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

	// if (debug_print >= DEBUG_PRINT_DBG) {
	// 	interface_print_to_log(rx_buf_c, rx_pkt_len, false);
	// }

	return user_len;
}

static long nrf51822_ioctl(struct file *file,
							unsigned int ioctl_num,
							unsigned long ioctl_param)
{
	int result;

	switch (ioctl_num) {
		// case CC2520_IO_RADIO_INIT:
		// 	INFO((KERN_INFO "radio starting\n"));
		// 	cc2520_radio_start();
		// 	break;
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

struct file_operations fops = {
	.read = nrf51822_read,
	.write = nrf51822_write,
	.unlocked_ioctl = nrf51822_ioctl,
	.open = NULL,
	.release = NULL
};

/////////////////
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

	return nrf51822_issue_command(ldata.command);
}


/////////////////////
// Application logic
/////////////////////


//void nrf51822_



// The result of the interrupt is in spi_buffer
static void nrf51822_read_irq_done (void *arg) {
	int len;

	len = (int) arg;

	// Move the packet from the SPI buffer to the thing that can be read()
	memcpy(buf_to_user, spi_buffer, len);
	buf_to_user_len = len;

	// Release the lock on the SPI buffer
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	spi_pending = false;
	spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);

	// Notify the read() call that there is data for it.
	wake_up(&nrf51822_to_user_queue);
}


// There was some error when talking to the nRF51822.
// Done with SPI, release the lock
static void nrf51822_read_irq_error (void *arg) {
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	spi_pending = false;
	spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
}


// Now we can read the length byte and clock the bus for that many
// bytes. If the length == 0xFF, an error occurred and we should skip
// this.
static void nrf51822_read_irq_continue_2 (void *arg) {

	INFO(KERN_INFO, "Getting READ_IRQ data\n");

	if (spi_buffer[0] == 0xFF) {
		// transfer to read why the nRF51822 interrupted us.
		spi_tsfer.tx_buf = spi_buffer;
		spi_tsfer.rx_buf = spi_buffer;
		spi_tsfer.len = 0;
		spi_tsfer.cs_change = 1;

		spi_message_init(&spi_msg);
		spi_msg.complete = nrf51822_read_irq_error;
		spi_msg.context = NULL;
		spi_message_add_tail(&spi_tsfer, &spi_msg);

		spi_async(nrf51822_spi_device, &spi_msg);

	} else {

		// Setup a SPI transfer to read why the nRF51822 interrupted us.
		spi_tsfer.tx_buf = NULL;
		spi_tsfer.rx_buf = spi_buffer;
		spi_tsfer.len = spi_buffer[0];
		spi_tsfer.cs_change = 1;

		spi_message_init(&spi_msg);
		spi_msg.complete = nrf51822_read_irq_done;
		spi_msg.context = NULL;
		spi_message_add_tail(&spi_tsfer, &spi_msg);

		spi_async(nrf51822_spi_device, &spi_msg);
	}
}


// At this point we have told the nRF51822 that we want data about the
// interrupt. Now we need to clock the SPI lines for 1 byte so we know
// how much data to get back.
static void nrf51822_read_irq_continue_1 (void *arg) {

	INFO(KERN_INFO, "Getting READ_IRQ length\n");

	// Setup a SPI transfer to read why the nRF51822 interrupted us.
	spi_tsfer.tx_buf = NULL;
	spi_tsfer.rx_buf = spi_buffer;
	spi_tsfer.len = 1;
	spi_tsfer.cs_change = 0;

	spi_message_init(&spi_msg);
	spi_msg.complete = nrf51822_read_irq_continue_2;
	spi_msg.context = NULL;
	spi_message_add_tail(&spi_tsfer, &spi_msg);

	spi_async(nrf51822_spi_device, &spi_msg);
}



//
// Interrupts
//

static irqreturn_t nrf51822_interrupt_handler(int irq, void *dev_id)
{
    // int gpio_val;
    // struct timespec ts;
    // s64 nanos;

    // // NOTE: For now we're assuming no delay between SFD called
    // // and actual SFD received. The TinyOS implementations call
    // // for a few uS of delay, but it's likely not needed.
    // getrawmonotonic(&ts);
    // nanos = timespec_to_ns(&ts);
    // gpio_val = gpio_get_value(CC2520_SFD);

    // //DBG((KERN_INFO "sfd interrupt occurred at %lld, %d\n", (long long int)nanos, gpio_val));

    // cc2520_radio_sfd_occurred(nanos, gpio_val);


	INFO(KERN_INFO, "got interrupt from nRF51822\n");

	// Check if we can get a lock on the SPI rx/tx buffer. If not, ignore
	// this interrupt.
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	if (spi_pending) {
		// Something else is using the SPI. Just skip this interrupt.
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
		return IRQ_HANDLED;
	} else {
		// Claim the SPI buffer.
		spi_pending = true;
		spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);
	}


	// Setup a SPI transfer to read why the nRF51822 interrupted us.
	spi_tsfer.tx_buf = spi_buffer;
	spi_tsfer.rx_buf = NULL;
	spi_tsfer.len = 1;
	spi_tsfer.cs_change = 1;

	spi_buffer[0] = BCP_COMMAND_READ_IRQ;

	spi_message_init(&spi_msg);
	spi_msg.complete = nrf51822_read_irq_continue_1;
	spi_msg.context = NULL;
	spi_message_add_tail(&spi_tsfer, &spi_msg);

	spi_async(nrf51822_spi_device, &spi_msg);

	return IRQ_HANDLED;
}


// Called after a command byte has been sent to the nRF51822.
// Just need to release the SPI bus.
void nrf51822_issue_command_done(void *arg) {
	spin_lock_irqsave(&spi_spin_lock, spi_spin_lock_flags);
	spi_pending = false;
	spin_unlock_irqrestore(&spi_spin_lock, spi_spin_lock_flags);

	DBG(KERN_INFO, "Finished writing the command.\n");
}


int nrf51822_issue_command(uint8_t command) {

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

	// Actually issue the command.
	spi_tsfer.tx_buf = spi_buffer;
	spi_tsfer.rx_buf = NULL;
	spi_tsfer.len = 1;
	spi_tsfer.cs_change = 1;

	spi_buffer[0] = command;

	spi_message_init(&spi_msg);
	spi_msg.complete = nrf51822_issue_command_done;
	spi_msg.context = NULL;
	spi_message_add_tail(&spi_tsfer, &spi_msg);

	spi_async(nrf51822_spi_device, &spi_msg);

	return 0;
}




/////////////////////
// SPI
/////////////////////

static int nrf51822_spi_probe(struct spi_device *spi_device)
{
    ERR(KERN_INFO, "Inserting SPI protocol driver.\n");
    nrf51822_spi_device = spi_device;
    return 0;
}

static int nrf51822_spi_remove(struct spi_device *spi_device)
{
    ERR(KERN_INFO, "Removing SPI protocol driver.");
    nrf51822_spi_device = NULL;
    return 0;
}


// Configure SPI
static struct spi_driver nrf51822_spi_driver = {
	.driver = {
		.name = nrf51822_name,
		.owner = THIS_MODULE,
	},
	.probe = nrf51822_spi_probe,
	.remove = nrf51822_spi_remove,
};




/////////////////
// init/free
///////////////////

int init_module(void)
{
	int result;

	struct spi_master *spi_master;
	struct spi_device *spi_device = NULL;
	struct device *pdev;
	char buff[64];

	// interface_bottom->tx_done = cc2520_interface_tx_done;
	// interface_bottom->rx_done = cc2520_interface_rx_done;

	// sema_init(&tx_sem, 1);
	// sema_init(&rx_sem, 1);

	// sema_init(&tx_done_sem, 0);
	// sema_init(&rx_done_sem, 0);

	// tx_buf_c = kmalloc(CHAR_DEVICE_BUFFER_LEN, GFP_KERNEL);
	// if (!tx_buf_c) {
	// 	result = -EFAULT;
	// 	goto error;
	// }

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
	                     IRQF_TRIGGER_FALLING,
	                     "nrf51822_interrupt",
	                     NULL);
	if (result) goto error;


	//
	// SPI
	//

	// Setup the SPI device



	// Init the SPI lock
	spin_lock_init(&spi_spin_lock);

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		ERR(KERN_ALERT, "spi_busnum_to_master(%d) returned NULL\n", SPI_BUS);
		//ERR((KERN_ALERT "Missing modprobe spi-bcm2708?\n"));
		goto error;
	}

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) {
		put_device(&spi_master->dev);
		ERR(KERN_ALERT, "spi_alloc_device() failed\n");
		goto error;
	}

	spi_device->chip_select = SPI_BUS_CS0;

	/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff,
		     sizeof(buff),
		     "%s.%u",
	         dev_name(&spi_device->master->dev),
	         spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);

	if (pdev) {
		if (pdev->driver != NULL) {
			ERR(KERN_INFO,
			"Driver [%s] already registered for %s. \
			Nuking from orbit.\n",
			pdev->driver->name, buff);
		} else {
			ERR(KERN_INFO,
			"Previous driver registered with no loaded module. \
			Nuking from orbit.\n");
		}

		device_unregister(pdev);
	}

	spi_device->max_speed_hz = SPI_BUS_SPEED;
	spi_device->mode = SPI_MODE_0;
	spi_device->bits_per_word = 8;
	spi_device->irq = -1;

	spi_device->controller_state = NULL;
	spi_device->controller_data = NULL;
	strlcpy(spi_device->modalias, nrf51822_name, SPI_NAME_SIZE);

	result = spi_add_device(spi_device);
	if (result < 0) {
		spi_dev_put(spi_device);
		ERR(KERN_ALERT, "spi_add_device() failed: %d\n", result);
	}

	put_device(&spi_master->dev);


	// Register SPI
	result = spi_register_driver(&nrf51822_spi_driver);
    if (result < 0) {
    	ERR(KERN_INFO, "Could not register SPI driver.\n");
    	goto error;
    }





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

	// if (tx_buf_c) {
	// 	kfree(tx_buf_c);
	// 	tx_buf_c = 0;
	// }

	gpio_free(NRF51822_INTERRUPT_PIN);
    free_irq(nrf51822_irq, NULL);


	spi_unregister_device(spi_device);



    spi_unregister_driver(&nrf51822_spi_driver);





	return -1;
}

void cleanup_module(void)
{
	//int result;


    gpio_free(NRF51822_INTERRUPT_PIN);
    free_irq(nrf51822_irq, NULL);

    if (nrf51822_spi_device) {
        spi_unregister_device(nrf51822_spi_device);
    }

    spi_register_driver(&nrf51822_spi_driver);




	// result = down_interruptible(&tx_sem);
	// if (result) {
	// 	ERR(("critical error occurred on free."));
	// }

	// result = down_interruptible(&rx_sem);
	// if (result) {
	// 	ERR(("critical error occurred on free."));
	// }

	cdev_del(&char_d_cdev);
	unregister_chrdev(char_d_mm, nrf51822_name);
	device_destroy(cl, char_d_mm);
	class_destroy(cl);


	INFO(KERN_INFO, "Removed character device\n");

	if (buf_to_user) {
		kfree(buf_to_user);
		buf_to_user = NULL;
	}

	// if (tx_buf_c) {
	// 	kfree(tx_buf_c);
	// 	tx_buf_c = 0;
	// }
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);