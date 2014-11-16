#ifndef _GAPSPI_H_
#define _GAPSPI_H_

extern int gap_spi_async(struct spi_message * message, int dev_id);
extern int gap_spi_sync(struct spi_message * message, int dev_id);

#endif