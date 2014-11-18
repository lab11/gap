#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/time.h>

#include "cc2520.h"
#include "radio.h"
#include "platform.h"
#include "debug.h"

//////////////////////////////
// Interface Initialization
//////////////////////////////

// Sets up the GPIO pins needed for the CC2520
int cc2520_plat_gpio_init()
{
    int err = 0;

    // Setup GPIO In/Out
    err = gpio_request_one(CC2520_0_FIFO, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_0_FIFOP, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_0_CCA, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_0_SFD, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_0_RESET, GPIOF_DIR_OUT | GPIOF_INIT_HIGH, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_1_FIFO, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_1_FIFOP, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_1_CCA, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_1_SFD, GPIOF_DIR_IN, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_1_RESET, GPIOF_DIR_OUT | GPIOF_INIT_HIGH, NULL);
    if (err)
        goto fail;

    return err;

    fail:
        ERR((KERN_ALERT "[cc2520] - failed to init GPIOs\n"));
        cc2520_plat_gpio_free();
        return err;
}

void cc2520_plat_gpio_free()
{
    gpio_free(CC2520_0_FIFO);
    gpio_free(CC2520_0_FIFOP);
    gpio_free(CC2520_0_CCA);
    gpio_free(CC2520_0_SFD);
    gpio_free(CC2520_0_RESET);

    gpio_free(CC2520_1_FIFO);
    gpio_free(CC2520_1_FIFOP);
    gpio_free(CC2520_1_CCA);
    gpio_free(CC2520_1_SFD);
    gpio_free(CC2520_1_RESET);
}
