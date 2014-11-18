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

uint8_t debug_print;

struct cc2520_state state;
const char cc2520_name[] = "cc2520";

struct cc2520_interface interface_to_unique[CC2520_NUM_DEVICES];
struct cc2520_interface unique_to_lpl[CC2520_NUM_DEVICES];
struct cc2520_interface lpl_to_csma[CC2520_NUM_DEVICES];
struct cc2520_interface csma_to_sack[CC2520_NUM_DEVICES];
struct cc2520_interface sack_to_radio[CC2520_NUM_DEVICES];

void setup_bindings(void)
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		radio_top[i] = &sack_to_radio[i];
		sack_bottom[i] = &sack_to_radio[i];
		sack_top[i] = &csma_to_sack[i];
		csma_bottom[i] = &csma_to_sack[i];
		csma_top[i] = &lpl_to_csma[i];
		lpl_bottom[i] = &lpl_to_csma[i];
		lpl_top[i] = &unique_to_lpl[i];
		unique_bottom[i] = &unique_to_lpl[i];
		unique_top[i] = &interface_to_unique[i];
		interface_bottom[i] = &interface_to_unique[i];
	}
}

//int init_module()
static int cc2520_probe(struct platform_device *pltf)
{
	int err = 0;

	debug_print = DEBUG_PRINT_DBG;

	// Make sure that gapspi.ko is loaded first
	request_module("gapspi");

	setup_bindings();

	memset(&state, 0, sizeof(struct cc2520_state));

	INFO((KERN_INFO "[CC2520] - Loading kernel module v%s\n", DRIVER_VERSION));

	return 0;

	err = cc2520_plat_gpio_init();
	if (err) {
		ERR((KERN_ALERT "[CC2520] - gpio driver error. aborting.\n"));
		goto error6;
	}

	err = cc2520_interface_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - char driver error. aborting.\n"));
		goto error5;
	}

	err = cc2520_radio_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - radio init error. aborting.\n"));
		goto error4;
	}

	err = cc2520_lpl_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - lpl init error. aborting.\n"));
		goto error3;
	}

	err = cc2520_sack_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - sack init error. aborting.\n"));
		goto error2;
	}

	err = cc2520_csma_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - csma init error. aborting.\n"));
		goto error1;
	}

	err = cc2520_unique_init();
	if (err) {
		ERR((KERN_ALERT "[cc2520] - unique init error. aborting.\n"));
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
		return -1;
}

//void cleanup_module()
static int cc2520_remove(struct platform_device *pltf)
{
	destroy_workqueue(state.wq);
	cc2520_interface_free();
	cc2520_plat_gpio_free();
	INFO((KERN_INFO "[cc2520] - Unloading kernel module\n"));

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
