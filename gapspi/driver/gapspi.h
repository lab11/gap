#ifndef _GAPSPI_H_
#define _GAPSPI_H_

extern int gap_spi_async(struct spi_device * spi,
				  		 struct spi_message * message);
extern int gap_spi_sync(struct spi_device * spi,
				  		 struct spi_message * message);

#endif