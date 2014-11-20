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

#include "debug.h"

#define DRIVER_AUTHOR  "Neal Jackson <nealjack@umich.edu>"
#define DRIVER_DESC    "A driver for the GAP Beaglebone Black \"Zigbeag\" cape SPI control."
#define DRIVER_VERSION "0.1"

static void gapspi_cs_mux(int id);

const char gapspi_name[] = "gapspi";

// Device and Pin config
int num_demux_ctrl_pins;
int demux_ctrl_pins[3];

// Defines the level of debug output
uint8_t debug_print = DEBUG_PRINT_ERR;

struct spi_device* gapspi_spi_device;

// Use this function to set the DEMUX
static void gapspi_cs_mux(int id)
{
	int i;
	for (i = 0; i < num_demux_ctrl_pins; ++i) {
		if (id & (1<<i)) {
			gpio_set_value(demux_ctrl_pins[i], 1);
		} else {
			gpio_set_value(demux_ctrl_pins[i], 0);
		}
	}
}

int gap_spi_async(struct spi_message * message, int dev_id)
{
	// Save the device id (which CS line to use) in the state
	// variable of the message.
	message->state = (void*) dev_id;
	return spi_async(gapspi_spi_device, message);
}

int gap_spi_sync(struct spi_message * message, int dev_id)
{
	message->state = (void*) dev_id;
	return spi_sync(gapspi_spi_device, message);
}

EXPORT_SYMBOL(gap_spi_async);
EXPORT_SYMBOL(gap_spi_sync);

/////////////////////
// SPI
/////////////////////

// Intercept the "transfer message" function to allow for
// setting the CS MUX before the message goes out.
int (*real_transfer_one_message)(struct spi_master *master,
                                 struct spi_message *mesg);

int our_transfer_one_message (struct spi_master *master,
                               struct spi_message *mesg) {
	int cs_device_id;

	// Sse the stored value in mesg to set the mux
	cs_device_id = (int) (mesg->state);
	gapspi_cs_mux(cs_device_id);

	return real_transfer_one_message(master, mesg);
}

static int gapspi_spi_probe(struct spi_device *spi_device)
{
	struct device_node *np = spi_device->dev.of_node;
	const __be32 *prop;
	int i;
	int err;

    INFO(KERN_INFO, "Inserting SPI protocol driver.\n");

   	//
	// Setup GPIO SPI mux pins
	//
	// Get the parameters for the driver from the device tree
	prop = of_get_property(np, "num-csmux-pins", NULL);
	if (!prop) {
		ERR(KERN_ALERT, "Got NULL for the number of CS pins.\n");
		return -EINVAL;
	}
	num_demux_ctrl_pins = be32_to_cpup(prop);
	INFO(KERN_INFO, "Number of DEMUX ctrl pins %i\n", num_demux_ctrl_pins);

	for (i=0; i<num_demux_ctrl_pins; i++) {
		char buf[64];

		snprintf(buf, 64, "csmux%i-gpio", i);
		demux_ctrl_pins[i] = of_get_named_gpio(np, buf, 0);

		if (!gpio_is_valid(demux_ctrl_pins[i])) {
			ERR(KERN_ALERT, "gpio csmux%i is not valid\n", i);
			return -EINVAL;
		}
		err = devm_gpio_request_one(&spi_device->dev, demux_ctrl_pins[i], GPIOF_OUT_INIT_LOW, "buf");
		if (err) return -EINVAL;
	}

	// Splice in our transfer one message function so that we can
	// set the mux before the packet is transfered.
	real_transfer_one_message = spi_device->master->transfer_one_message;
	spi_device->master->transfer_one_message = our_transfer_one_message;

    gapspi_spi_device = spi_device;

    return 0;
}

static int gapspi_spi_remove(struct spi_device *spi_device)
{
    INFO(KERN_INFO, "Removing SPI protocol driver.");

	spi_device->master->transfer_one_message = real_transfer_one_message;

    gapspi_spi_device = NULL;
    return 0;
}

static const struct spi_device_id gapspi_ids[] = {
	{"gapspi", },
	{},
};
MODULE_DEVICE_TABLE(spi, gapspi_ids);

static const struct of_device_id gapspi_of_ids[] = {
	{.compatible = "lab11,gapspi", },
	{},
};
MODULE_DEVICE_TABLE(of, gapspi_of_ids);

// Configure SPI
static struct spi_driver gapspi_spi_driver = {
	.driver = {
		.name = gapspi_name,
		.owner = THIS_MODULE,
		.bus = &spi_bus_type,
		.of_match_table = of_match_ptr(gapspi_of_ids),
	},
	.probe = gapspi_spi_probe,
	.remove = gapspi_spi_remove,
	.id_table = gapspi_ids,
};

module_spi_driver(gapspi_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
