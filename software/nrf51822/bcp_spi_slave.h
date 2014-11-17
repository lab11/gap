#ifndef BCP_SPI_SLAVE_H__
#define BCP_SPI_SLAVE_H__

// State machine for the SPI Slave driver.
typedef enum {
	SPI_SLAVE_STATE_WAIT_FOR_COMMAND,
	SPI_SLAVE_STATE_RUN_COMMAND
} spi_slave_state_e;


#define SPI_BUF_LEN  64

void spi_slave_notify();

#endif