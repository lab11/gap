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
	__be32 *prop;

	INFO(KERN_INFO, "Loading kernel module v%s\n", DRIVER_VERSION);

	// Make sure that gapspi.ko is loaded first
	request_module("gapspi");

	// Init
	memset(&state, 0, sizeof(struct cc2520_state));

	// Get the parameters for the driver from the device tree
	prop = of_get_property(np, "num-radios", NULL);
	if (!prop) {
		ERR(KERN_ALERT, "Got NULL for the number of radios.\n");
		goto error6;
	}
	config.num_radios = be32_to_cpup(prop);
	INFO(KERN_INFO, "Number of CC2520 radios %i\n", config.num_radios);

	// Instantiate the correct number of radios
	config.radios = (struct cc2520_dev*) kmalloc(config.num_radios * sizeof(struct cc2520_dev), GFP_KERNEL);
	if (config.radios == NULL){
		ERR(KERN_INFO, "Could not allocate cc2520 devices\n");
		goto error6;
	}



	for (i=0; i<config.num_radios; i++) {
		char buf[64];

		// Configure the GPIOs
		snprintf(buf, 64, "fifo%i-gpio", i);
		config.radios[i]->fifo = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "fifop%i-gpio", i);
		config.radios[i]->fifop = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "sfd%i-gpio", i);
		config.radios[i]->sfd = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "cca%i-gpio", i);
		config.radios[i]->cca = of_get_named_gpio(np, buf, 0);

		snprintf(buf, 64, "rst%i-gpio", i);
		config.radios[i]->rst = of_get_named_gpio(np, buf, 0);
	}




	setup_bindings();

	INFO(KERN_INFO, "Loading kernel module v%s\n", DRIVER_VERSION);

	return 0;

	// err = cc2520_plat_gpio_init();
	// if (err) {
	// 	ERR(KERN_ALERT, "gpio driver error. aborting.\n");
	// 	goto error6;
	// }

	err = cc2520_interface_init();
	if (err) {
		ERR(KERN_ALERT, "char driver error. aborting.\n");
		goto error5;
	}

	err = cc2520_radio_init();
	if (err) {
		ERR(KERN_ALERT, "radio init error. aborting.\n");
		goto error4;
	}

	err = cc2520_lpl_init();
	if (err) {
		ERR(KERN_ALERT, "lpl init error. aborting.\n");
		goto error3;
	}

	err = cc2520_sack_init();
	if (err) {
		ERR(KERN_ALERT, "sack init error. aborting.\n");
		goto error2;
	}

	err = cc2520_csma_init();
	if (err) {
		ERR(KERN_ALERT, "csma init error. aborting.\n");
		goto error1;
	}

	err = cc2520_unique_init();
	if (err) {
		ERR(KERN_ALERT, "unique init error. aborting.\n");
		goto error0;
	}

	state.wq = create_singlethread_workqueue(cc2520_name);

	return 0;

	error0:
		cc2520_csma_free();
	error1:
		cc2520_sack_free();
	error2:
		cc2520_lpl_free();
	error3:
		cc2520_radio_free();
	error4:
		cc2520_interface_free();
	error5:
		cc2520_plat_gpio_free();
	error6:



	kfree(config.radios);


		return -1;
}

//void cleanup_module()
static int cc2520_remove(struct platform_device *pltf)
{
	// destroy_workqueue(state.wq);
	// cc2520_interface_free();
	// cc2520_plat_gpio_free();
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
