#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/of_gpio.h>

#include "cc2520.h"
#include "radio.h"
#include "lpl.h"
#include "interface.h"
#include "sack.h"
#include "csma.h"
#include "unique.h"
#include "debug.h"

#define DRIVER_AUTHOR  "Andrew Robinson <androbin@umich.edu>, Neal Jackson <nealjack@umich.edu>"
#define DRIVER_DESC    "A driver for the Beaglebone Black \"Zigbeag\" cape."
#define DRIVER_VERSION "0.1"

uint8_t debug_print = DEBUG_PRINT_DBG;

struct cc2520_config config;
const char cc2520_name[] = "cc2520";

static int cc2520_probe(struct platform_device *pltf)
{
	struct device_node *np = pltf->dev.of_node;
	int err = 0;
	int i, j, step = 0;
	const __be32 *prop;
	struct cc2520_dev *dev;

	INFO(KERN_INFO, "Loading kernel module O v%s\n", DRIVER_VERSION);

	// Make sure that gapspi.ko is loaded first
	request_module("gapspi");

	// Init
	memset(&config, 0, sizeof(struct cc2520_config));

	// Get the parameters for the driver from the device tree
	prop = of_get_property(np, "num-radios", NULL);
	if (!prop) {
		ERR(KERN_ALERT, "Got NULL for the number of radios.\n");
		goto error0;
	}
	config.num_radios = be32_to_cpup(prop);
	INFO(KERN_INFO, "Number of CC2520 radios %i\n", config.num_radios);

	// Instantiate the correct number of radios
	config.radios = (struct cc2520_dev*) kmalloc(config.num_radios * sizeof(struct cc2520_dev), GFP_KERNEL);
	if (config.radios == NULL) {
		ERR(KERN_INFO, "Could not allocate cc2520 devices\n");
		goto error0;
	}
	memset(config.radios, 0, config.num_radios * sizeof(struct cc2520_dev));

	for (i=0; i<config.num_radios; i++) {
		char buf[64];

		dev = &config.radios[i];

		// Configure the GPIOs
		snprintf(buf, 64, "fifo%i-gpio", i);
		dev->fifo = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "fifop%i-gpio", i);
		dev->fifop = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "sfd%i-gpio", i);
		dev->sfd = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "cca%i-gpio", i);
		dev->cca = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "rst%i-gpio", i);
		dev->reset = of_get_named_gpio(np, buf, 0);


		if (!gpio_is_valid(dev->fifo)) {
			ERR(KERN_ALERT, "fifo gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->fifo, GPIOF_IN, "fifo");
		if (err) goto error1;

		if (!gpio_is_valid(dev->fifop)) {
			ERR(KERN_ALERT, "fifop gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->fifop, GPIOF_IN, "fifop");
		if (err) goto error1;

		if (!gpio_is_valid(dev->sfd)) {
			ERR(KERN_ALERT, "sfd gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->sfd, GPIOF_IN, "sfd");
		if (err) goto error1;

		if (!gpio_is_valid(dev->cca)) {
			ERR(KERN_ALERT, "cca gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->cca, GPIOF_IN, "cca");
		if (err) goto error1;

		if (!gpio_is_valid(dev->reset)) {
			ERR(KERN_ALERT, "reset gpio%i is not valid\n", i);
			goto error1;
		}
		err = devm_gpio_request_one(&pltf->dev, dev->reset, GPIOF_OUT_INIT_HIGH, "reset");
		if (err) goto error1;

		// Get other properties
		snprintf(buf, 64, "radio%i-csmux", i);
		prop = of_get_property(np, buf, NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL for the csmux index.\n");
			goto error1;
		}
		dev->chipselect_demux_index = be32_to_cpup(prop);

		snprintf(buf, 64, "radio%i-amplified", i);
		prop = of_get_property(np, buf, NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL when determining if the radio is amplified.\n");
			goto error1;
		}
		dev->amplified = be32_to_cpup(prop);

		dev->id = i;

		INFO(KERN_INFO, "GPIO CONFIG radio:%i\n", i);
		INFO(KERN_INFO, "  FIFO: %i\n", dev->fifo);
		INFO(KERN_INFO, "  FIFOP: %i\n", dev->fifop);
		INFO(KERN_INFO, "  SFD: %i\n", dev->sfd);
		INFO(KERN_INFO, "  CCA: %i\n", dev->cca);
		INFO(KERN_INFO, "  RST: %i\n", dev->reset);
		INFO(KERN_INFO, "SETTINGS radio:%i\n", i);
		INFO(KERN_INFO, "  DEMUX: %i\n", dev->chipselect_demux_index);
		INFO(KERN_INFO, "  AMP: %i\n", dev->amplified);
	}

	// Do the not-radio-specific init here

	// Allocate a major number for this device
	err = alloc_chrdev_region(&config.chr_dev, 0, config.num_radios, cc2520_name);
	if (err < 0) {
		ERR(KERN_INFO, "Could not allocate a major number\n");
		goto error1;
	}
	config.major = MAJOR(config.chr_dev);

	// Create device class
	config.cl = class_create(THIS_MODULE, cc2520_name);
	if (config.cl == NULL) {
		ERR(KERN_INFO, "Could not create device class\n");
		goto error2;
	}


	for (i=0; i<config.num_radios; i++) {

		step = 0;
		err = cc2520_interface_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "char driver error. aborting.\n");
			goto error;
		}

		step = 1;
		err = cc2520_radio_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "radio init error. aborting.\n");
			goto error;
		}

		step = 2;
		err = cc2520_lpl_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "lpl init error. aborting.\n");
			goto error;
		}

		step = 3;
		err = cc2520_sack_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "sack init error. aborting.\n");
			goto error;
		}

		step = 4;
		err = cc2520_csma_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "csma init error. aborting.\n");
			goto error;
		}

		step = 5;
		err = cc2520_unique_init(&config.radios[i]);
		if (err) {
			ERR(KERN_ALERT, "unique init error. aborting.\n");
			goto error;
		}
	}

	return 0;

	error:
		// Free all of the radios that have already been inited successfully.
		for (j=0; j<i; j++) {
			struct cc2520_dev *dev = &config.radios[j];
			cc2520_unique_free(dev);
			cc2520_csma_free(dev);
			cc2520_sack_free(dev);
			cc2520_lpl_free(dev);
			cc2520_radio_free(dev);
			cc2520_interface_free(dev);
		}
		// Do the radio that was in progress when the error occurred
		if (step > 4) cc2520_csma_free(&config.radios[i]);
		if (step > 3) cc2520_sack_free(&config.radios[i]);
		if (step > 2) cc2520_lpl_free(&config.radios[i]);
		if (step > 1) cc2520_radio_free(&config.radios[i]);
		if (step > 0) cc2520_interface_free(&config.radios[i]);


		class_destroy(config.cl);
	error2:
		unregister_chrdev_region(config.chr_dev, config.num_radios);
	error1:
		kfree(config.radios);
	error0:
		return -1;
}

//void cleanup_module()
static int cc2520_remove(struct platform_device *pltf)
{
	int i;

	for (i=0; i<config.num_radios; i++) {
		struct cc2520_dev *dev = &config.radios[i];
		cc2520_unique_free(dev);
		cc2520_csma_free(dev);
		cc2520_sack_free(dev);
		cc2520_lpl_free(dev);
		cc2520_radio_free(dev);
		cc2520_interface_free(dev);
	}

	class_destroy(config.cl);
	unregister_chrdev_region(config.chr_dev, config.num_radios);
	kfree(config.radios);

	INFO(KERN_INFO, "Unloading kernel module\n");

	return 0;
}

static const struct of_device_id cc2520_of_ids[] = {
	{.compatible = "lab11,cc2520", },
	{},
};
MODULE_DEVICE_TABLE(of, cc2520_of_ids);

static struct platform_driver cc2520_driver = {
	.driver = {
		.name = "gap-cc2520",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(cc2520_of_ids),
	},
	.probe = cc2520_probe,
	.remove = cc2520_remove,
};

module_platform_driver(cc2520_driver);


MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
