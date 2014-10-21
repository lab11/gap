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

//////////////////////////
// SPI Stuff
//////////////////////////

static int cc2520_spi_add_to_bus(void)
{
    struct spi_master *spi_master;
    struct spi_device *spi_device;
    struct device *pdev;
    char buff[64];
    int status = 0;

    spi_master = spi_busnum_to_master(SPI_BUS);
    if (!spi_master) {
        ERR((KERN_ALERT "[cc2520] - spi_busnum_to_master(%d) returned NULL\n",
            SPI_BUS));
        ERR((KERN_ALERT "[cc2520] - Missing modprobe spi-bcm2708?\n"));
        return -1;
    }

    spi_device = spi_alloc_device(spi_master);
    if (!spi_device) {
        put_device(&spi_master->dev);
        ERR((KERN_ALERT "[cc2520] - spi_alloc_device() failed\n"));
        return -1;
    }

    spi_device->chip_select = SPI_BUS_CS0;

    /* Check whether this SPI bus.cs is already claimed */
    snprintf(buff, sizeof(buff), "%s.%u",
            dev_name(&spi_device->master->dev),
            spi_device->chip_select);

    pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);

    if (pdev) {
        if (pdev->driver != NULL) {
            ERR((KERN_INFO
                "[cc2520] - Driver [%s] already registered for %s. \
Nuking from orbit.\n",
                pdev->driver->name, buff));
        }
        else {
            ERR((KERN_INFO
                "[cc2520] - Previous driver registered with no loaded module. \
Nuking from orbit.\n"));
        }

        device_unregister(pdev);
    }

    spi_device->max_speed_hz = SPI_BUS_SPEED;
    spi_device->mode = SPI_MODE_0;
    spi_device->bits_per_word = 8;
    spi_device->irq = -1;

    spi_device->controller_state = NULL;
    spi_device->controller_data = NULL;
    strlcpy(spi_device->modalias, cc2520_name, SPI_NAME_SIZE);

    status = spi_add_device(spi_device);
    if (status < 0) {
        spi_dev_put(spi_device);
        ERR((KERN_ALERT "[cc2520] - spi_add_device() failed: %d\n",
            status));
    }

    put_device(&spi_master->dev);
    return status;
}

static int cc2520_spi_probe(struct spi_device *spi_device)
{
    ERR((KERN_INFO "[cc2520] - Inserting SPI protocol driver.\n"));
    state.spi_device = spi_device;
    return 0;
}

static int cc2520_spi_remove(struct spi_device *spi_device)
{
    ERR((KERN_INFO "[cc2520] - Removing SPI protocol driver."));
    state.spi_device = NULL;
    return 0;
}

static struct spi_driver cc2520_spi_driver = {
        .driver = {
            .name = cc2520_name,
            .owner = THIS_MODULE,
        },
        .probe = cc2520_spi_probe,
        .remove = cc2520_spi_remove,
};

int cc2520_plat_spi_init()
{
    int result;

    result = cc2520_spi_add_to_bus();
    if (result < 0)
        goto error;

    result = spi_register_driver(&cc2520_spi_driver);
    if (result < 0)
        goto error;

    return 0;

    error:
        spi_unregister_driver(&cc2520_spi_driver);
        return result;
}

void cc2520_plat_spi_free()
{
    if (state.spi_device)
        spi_unregister_device(state.spi_device);

    spi_unregister_driver(&cc2520_spi_driver);
}

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

    err = gpio_request_one(CC2520_0_RESET, GPIOF_DIR_OUT, NULL);
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

    err = gpio_request_one(CC2520_1_RESET, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_DEBUG_0, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_DEBUG_1, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_DEBUG_2, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_SPIE0, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    err = gpio_request_one(CC2520_SPIE1, GPIOF_DIR_OUT, NULL);
    if (err)
        goto fail;

    gpio_set_value(CC2520_DEBUG_0, 0);
    gpio_set_value(CC2520_DEBUG_1, 0);
    gpio_set_value(CC2520_DEBUG_2, 0);

    gpio_set_value(CC2520_SPIE0, 0);
    gpio_set_value(CC2520_SPIE1, 0);

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

    gpio_free(CC2520_DEBUG_0);
    gpio_free(CC2520_DEBUG_1);
    gpio_free(CC2520_DEBUG_2);

    gpio_free(CC2520_SPIE0);
    gpio_free(CC2520_SPIE1);
}
