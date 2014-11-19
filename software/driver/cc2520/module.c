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
#include "platform.h"
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

// struct cc2520_interface *interface_to_unique;
// struct cc2520_interface *unique_to_lpl;
// struct cc2520_interface *lpl_to_csma;
// struct cc2520_interface *csma_to_sack;
// struct cc2520_interface *sack_to_radio;

// void setup_bindings(void)
// {
// 	int i;
// 	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
// 		radio_top[i] = &sack_to_radio[i];
// 		sack_bottom[i] = &sack_to_radio[i];
// 		sack_top[i] = &csma_to_sack[i];
// 		csma_bottom[i] = &csma_to_sack[i];
// 		csma_top[i] = &lpl_to_csma[i];
// 		lpl_bottom[i] = &lpl_to_csma[i];
// 		lpl_top[i] = &unique_to_lpl[i];
// 		unique_bottom[i] = &unique_to_lpl[i];
// 		unique_top[i] = &interface_to_unique[i];
// 		interface_bottom[i] = &interface_to_unique[i];
// 	}
// }

static int cc2520_probe(struct platform_device *pltf)
{
	struct device_node *np = pltf->dev.of_node;
	int err = 0;
	int i, j, step = 0;
	const __be32 *prop;



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

	//for (i=0; i<config.num_radios; i++) {
	for (i=0; i<1; i++) {
		char buf[64];

		// // Configure the GPIOs
		// snprintf(buf, 64, "fifo%i-gpio", i);
		// config.radios[i].fifo = of_get_named_gpio(np, buf, 0);

		// snprintf(buf, 64, "fifop%i-gpio", i);
		// config.radios[i].fifop = of_get_named_gpio(np, buf, 0);

		// snprintf(buf, 64, "sfd%i-gpio", i);
		// config.radios[i].sfd = of_get_named_gpio(np, buf, 0);

		// snprintf(buf, 64, "cca%i-gpio", i);
		// config.radios[i].cca = of_get_named_gpio(np, buf, 0);

		// snprintf(buf, 64, "rst%i-gpio", i);
		// config.radios[i].reset = of_get_named_gpio(np, buf, 0);

		// // Get other properties
		// snprintf(buf, 64, "radio%i-csmux", i);
		// prop = of_get_property(np, buf, NULL);
		// if (!prop) {
		// 	ERR(KERN_ALERT, "Got NULL for the csmux index.\n");
		// 	goto error1;
		// }
		// config.radios[i].chipselect_demux_index = be32_to_cpup(prop);

		// snprintf(buf, 64, "radio%i-amplified", i);
		// prop = of_get_property(np, buf, NULL);
		// if (!prop) {
		// 	ERR(KERN_ALERT, "Got NULL when determining if the radio is amplified.\n");
		// 	goto error1;
		// }
		// config.radios[i].amplified = be32_to_cpup(prop);

		// config.radios[i].id = i;



		// Configure the GPIOs
		config.radios[0].fifo = of_get_named_gpio(np, "fifo0-gpio", 0);
		INFO(KERN_INFO, "great\n");
		// config.radios[0].fifop = of_get_named_gpio(np, "fifop0-gpio", 0);
		// config.radios[0].sfd = of_get_named_gpio(np, "sfd0-gpio", 0);
		// config.radios[0].cca = of_get_named_gpio(np, "cca0-gpio", 0);
		// config.radios[0].reset = of_get_named_gpio(np, "rst0-gpio", 0);

		// Get other properties
		prop = of_get_property(np, "radio0-csmux", NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL for the csmux index.\n");
			goto error1;
		}
		config.radios[0].chipselect_demux_index = be32_to_cpup(prop);

		prop = of_get_property(np, "radio0-amplified", NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL when determining if the radio is amplified.\n");
			goto error1;
		}
		config.radios[0].amplified = be32_to_cpup(prop);

		config.radios[0].id = i;

		config.radios[1].fifo = of_get_named_gpio(np, "fifo1-gpio", 0);
		// config.radios[1].fifop = of_get_named_gpio(np, "fifop1-gpio", 0);
		// config.radios[1].sfd = of_get_named_gpio(np, "sfd1-gpio", 0);
		// config.radios[1].cca = of_get_named_gpio(np, "cca1-gpio", 0);
		// config.radios[1].reset = of_get_named_gpio(np, "rst1-gpio", 0);

		// Get other properties
		prop = of_get_property(np, "radio1-csmux", NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL for the csmux index.\n");
			goto error1;
		}
		config.radios[1].chipselect_demux_index = be32_to_cpup(prop);

		prop = of_get_property(np, "radio1-amplified", NULL);
		if (!prop) {
			ERR(KERN_ALERT, "Got NULL when determining if the radio is amplified.\n");
			goto error1;
		}
		config.radios[1].amplified = be32_to_cpup(prop);

		config.radios[1].id = i;




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




	//setup_bindings();

	//return 0;

	// err = cc2520_plat_gpio_init();
	// if (err) {
	// 	ERR(KERN_ALERT, "gpio driver error. aborting.\n");
	// 	goto error6;
	// }

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

	//config.wq = create_singlethread_workqueue(cc2520_name);

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
	// destroy_workqueue(state.wq);
	// cc2520_interface_free();
	// cc2520_plat_gpio_free();

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
