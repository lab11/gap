#ifndef _nrf51822_H_
#define _nrf51822_H_

#define SPI_BUF_LEN 128
#define CHAR_DEVICE_BUFFER_LEN 256

struct nrf51822_dev {
	int id;

	unsigned int chipselect_demux_index;

	int pin_interrupt;
	unsigned int irq;

	struct cdev cdev;
	int devno;

	wait_queue_head_t to_user_queue;

	u8 spi_command_buffer[SPI_BUF_LEN];
	u8 spi_data_buffer[SPI_BUF_LEN];

	struct spi_transfer spi_tsfers[4];
	struct spi_message spi_msg;

	spinlock_t spi_spin_lock;
	unsigned long spi_spin_lock_flags;
	bool spi_pending;

	u8 buf_to_nrf51822[CHAR_DEVICE_BUFFER_LEN];
	u8 buf_to_user[CHAR_DEVICE_BUFFER_LEN];
	size_t buf_to_nrf51822_len;
	size_t buf_to_user_len;
};

struct nrf51822_config {
	dev_t chr_dev;
	unsigned int major;
	struct class* cl;

	u8 num_radios; // Number of radios on board.

	struct nrf51822_dev *radios;

	int debug_print;
};

int nrf51822_issue_simple_command(uint8_t command, struct nrf51822_dev *dev);
static void nrf51822_read_irq(struct nrf51822_dev *dev);

#endif