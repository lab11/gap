#ifndef _IOCTL_H_
#define _IOCTL_H_

#include <asm/ioctl.h>
#include <linux/types.h>
#define BASE 0xCD

#ifndef __KERNEL__
#include <inttypes.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

struct nrf51822_set_debug_verbosity_data {
	u8 debug_level;
};

struct nrf51822_simple_command {
	u8 command;
};

//#define CC2520_IO_RADIO_INIT _IO(BASE, 0)
#define NRF51822_IOCTL_SET_DEBUG_VERBOSITY _IOW(BASE, 0, struct nrf51822_set_debug_verbosity_data)
#define NRF51822_IOCTL_SIMPLE_COMMAND      _IOW(BASE, 1, struct nrf51822_simple_command)


static void nrf51822_ioctl_set_debug_verbosity(struct nrf51822_set_debug_verbosity_data *data);
static void nrf51822_ioctl_simple_command(struct nrf51822_simple_command *data);

static long nrf51822_ioctl(struct file *file,
                           unsigned int ioctl_num,
                           unsigned long ioctl_param);

#endif