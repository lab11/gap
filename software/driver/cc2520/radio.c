#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include "../gapspi/gapspi.h"

#include "cc2520.h"
#include "radio.h"
#include "radio_config.h"
#include "interface.h"
#include "debug.h"

static u16 short_addr[CC2520_NUM_DEVICES];
static u64 extended_addr[CC2520_NUM_DEVICES];
static u16 pan_id[CC2520_NUM_DEVICES];
static u8 channel[CC2520_NUM_DEVICES];

const unsigned int GAP_SPI_CS_INDICES[] = {CC2520_CS_MUX_INDEX_0,
                                           CC2520_CS_MUX_INDEX_1};
static struct semaphore spi_sem;

const int is_amplified[] = {CC2520_AMP0, CC2520_AMP1};

static struct spi_message msg;
static struct spi_transfer tsfer;
static struct spi_transfer tsfer1;
static struct spi_transfer tsfer2;
static struct spi_transfer tsfer3;
static struct spi_transfer tsfer4;

static struct spi_message rx_msg;
static struct spi_transfer rx_tsfer;

static u8 *tx_buf[CC2520_NUM_DEVICES];
static u8 *rx_buf[CC2520_NUM_DEVICES];

static u8 *rx_out_buf[CC2520_NUM_DEVICES];
static u8 *rx_in_buf[CC2520_NUM_DEVICES];

static u8 *tx_buf_r[CC2520_NUM_DEVICES];
static u8 *rx_buf_r[CC2520_NUM_DEVICES];
static u8 tx_buf_r_len[CC2520_NUM_DEVICES];

static u64 sfd_nanos_ts[CC2520_NUM_DEVICES];

static spinlock_t radio_sl[CC2520_NUM_DEVICES];

static spinlock_t pending_rx_sl[CC2520_NUM_DEVICES];
static bool pending_rx[CC2520_NUM_DEVICES];

static spinlock_t rx_buf_sl[CC2520_NUM_DEVICES];

static int radio_state[CC2520_NUM_DEVICES];

static unsigned long flags[CC2520_NUM_DEVICES];
static unsigned long flags1[CC2520_NUM_DEVICES];

enum cc2520_radio_state_enum {
    CC2520_RADIO_STATE_IDLE,
    CC2520_RADIO_STATE_TX,
    CC2520_RADIO_STATE_TX_SFD_DONE,
    CC2520_RADIO_STATE_TX_SPI_DONE,
    CC2520_RADIO_STATE_TX_2_RX,
    CC2520_RADIO_STATE_CONFIG
};

static cc2520_status_t cc2520_radio_strobe(u8 cmd, struct cc2520_dev *dev);
static void cc2520_radio_writeRegister(u8 reg, u8 value, struct cc2520_dev *dev);
static void cc2520_radio_writeMemory(u16 mem_addr, u8 *value, u8 len, struct cc2520_dev *dev);

static int cc2520_radio_beginRx(struct cc2520_dev *dev);
static void cc2520_radio_continueRx(void *arg);
static void cc2520_radio_finishRx(void *arg);


static int cc2520_radio_tx(u8 *buf, u8 len, struct cc2520_dev *dev);
static int cc2520_radio_beginTx(struct cc2520_dev *dev);
static void cc2520_radio_continueTx_check(void *arg);
static void cc2520_radio_continueTx(void *arg);
static void cc2520_radio_completeTx(struct cc2520_dev *dev);
static void cc2520_radio_flushRx(struct cc2520_dev *dev);
static void cc2520_radio_continueFlushRx(void *arg);
static void cc2520_radio_completeFlushRx(void *arg);
static void cc2520_radio_flushTx(struct cc2520_dev *dev);
static void cc2520_radio_completeFlushTx(void *arg);

struct cc2520_interface *radio_top[CC2520_NUM_DEVICES];

// TODO: These methods are stupid
// and make things more confusing.
// Refactor them out.

void cc2520_radio_lock(int state, int index)
{
	spin_lock_irqsave(&radio_sl[index], flags1[index]);
	while (radio_state[index] != CC2520_RADIO_STATE_IDLE) {
		spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
		spin_lock_irqsave(&radio_sl[index], flags1[index]);
	}
	radio_state[index] = state;
	spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
}

void cc2520_radio_unlock(int index)
{
	spin_lock_irqsave(&radio_sl[index], flags1[index]);
	radio_state[index] = CC2520_RADIO_STATE_IDLE;
	spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
}

int cc2520_radio_tx_unlock_spi(int index)
{
	spin_lock_irqsave(&radio_sl[index], flags1[index]);
	if (radio_state[index] == CC2520_RADIO_STATE_TX) {
		radio_state[index] = CC2520_RADIO_STATE_TX_SPI_DONE;
		spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
		return 0;
	}
	else if (radio_state[index] == CC2520_RADIO_STATE_TX_SFD_DONE) {
		radio_state[index] = CC2520_RADIO_STATE_TX_2_RX;
		spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
		return 1;
	}
	spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
	return 0;
}

int cc2520_radio_tx_unlock_sfd(int index)
{
	spin_lock_irqsave(&radio_sl[index], flags1[index]);
	if (radio_state[index] == CC2520_RADIO_STATE_TX) {
		radio_state[index] = CC2520_RADIO_STATE_TX_SFD_DONE;
		spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
		return 0;
	}
	else if (radio_state[index] == CC2520_RADIO_STATE_TX_SPI_DONE) {
		radio_state[index] = CC2520_RADIO_STATE_TX_2_RX;
		spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
		return 1;
	}
	spin_unlock_irqrestore(&radio_sl[index], flags1[index]);
	return 0;
}

//////////////////////////////
// Initialization & On/Off
/////////////////////////////

int cc2520_radio_init()
{
	int result;
	int i;

	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		radio_top[i]->tx = cc2520_radio_tx;

		short_addr[i] = CC2520_DEF_SHORT_ADDR;
		extended_addr[i] = CC2520_DEF_EXT_ADDR;
		pan_id[i] = CC2520_DEF_PAN;
		channel[i] = CC2520_DEF_CHANNEL;

		spin_lock_init(&radio_sl[i]);
		spin_lock_init(&rx_buf_sl[i]);
		spin_lock_init(&pending_rx_sl[i]);

		radio_state[i] = CC2520_RADIO_STATE_IDLE;

		tx_buf[i] = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!tx_buf[i]) {
			result = -EFAULT;
			goto error;
		}

		rx_buf[i] = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!rx_buf[i]) {
			result = -EFAULT;
			goto error;
		}

		rx_out_buf[i] = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!rx_out_buf[i]) {
			result = -EFAULT;
			goto error;
		}

		rx_in_buf[i] = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!rx_in_buf[i]) {
			result = -EFAULT;
			goto error;
		}

		tx_buf_r[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!tx_buf_r[i]) {
			result = -EFAULT;
			goto error;
		}

		rx_buf_r[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!rx_buf_r[i]) {
			result = -EFAULT;
			goto error;
		}
	}

	sema_init(&spi_sem, 1);

	return 0;

	error:
		cc2520_radio_free();
		return result;
}

void cc2520_radio_free()
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		if (rx_buf_r[i]) {
			kfree(rx_buf_r[i]);
			rx_buf_r[i] = NULL;
		}

		if (tx_buf_r[i]) {
			kfree(tx_buf_r[i]);
			tx_buf_r[i] = NULL;
		}

		if (rx_buf[i]) {
			kfree(rx_buf[i]);
			rx_buf[i] = NULL;
		}

		if (tx_buf[i]) {
			kfree(tx_buf[i]);
			tx_buf[i] = NULL;
		}

		if (rx_in_buf[i]) {
			kfree(rx_in_buf[i]);
			rx_in_buf[i] = NULL;
		}

		if (rx_out_buf[i]) {
			kfree(rx_out_buf[i]);
			rx_out_buf[i] = NULL;
		}
	}
}

void cc2520_radio_start(struct cc2520_dev *dev)
{
	int index = dev->id;
	cc2520_radio_lock(CC2520_RADIO_STATE_CONFIG, index);
	tsfer.cs_change = 1;

	// 200uS Reset Pulse.
	gpio_set_value(dev->reset, 0);
	udelay(200);
	gpio_set_value(dev->reset, 1);
	udelay(200);

	// If radio is amplified with CC2591:
	if(is_amplified[index]){
		cc2520_radio_writeRegister(CC2520_TXPOWER, cc2520_txpower_amp.value, dev);
		cc2520_radio_writeRegister(CC2520_AGCCTRL1, cc2520_agcctrl1_amp.value, dev);
		cc2520_radio_writeRegister(CC2520_GPIOCTRL0, cc2520_gpioctrl0_amp.value, dev);
		cc2520_radio_writeRegister(CC2520_GPIOCTRL5, cc2520_gpioctrl5_amp.value, dev);
		cc2520_radio_writeRegister(CC2520_GPIOPOLARITY, cc2520_gpiopolarity_amp.value, dev);
		cc2520_radio_writeRegister(CC2520_TXCTRL, cc2520_txctrl_amp.value, dev);
	}
	else{
		cc2520_radio_writeRegister(CC2520_TXPOWER, cc2520_txpower_default.value, dev);
		cc2520_radio_writeRegister(CC2520_AGCCTRL1, cc2520_agcctrl1_default.value, dev);
	}
	cc2520_radio_writeRegister(CC2520_CCACTRL0, cc2520_ccactrl0_default.value, dev);
	cc2520_radio_writeRegister(CC2520_MDMCTRL0, cc2520_mdmctrl0_default.value, dev);
	cc2520_radio_writeRegister(CC2520_MDMCTRL1, cc2520_mdmctrl1_default.value, dev);
	cc2520_radio_writeRegister(CC2520_RXCTRL, cc2520_rxctrl_default.value, dev);
	cc2520_radio_writeRegister(CC2520_FSCTRL, cc2520_fsctrl_default.value, dev);
	cc2520_radio_writeRegister(CC2520_FSCAL1, cc2520_fscal1_default.value, dev);
	cc2520_radio_writeRegister(CC2520_ADCTEST0, cc2520_adctest0_default.value, dev);
	cc2520_radio_writeRegister(CC2520_ADCTEST1, cc2520_adctest1_default.value, dev);
	cc2520_radio_writeRegister(CC2520_ADCTEST2, cc2520_adctest2_default.value, dev);
	cc2520_radio_writeRegister(CC2520_FIFOPCTRL, cc2520_fifopctrl_default.value, dev);
	cc2520_radio_writeRegister(CC2520_FRMCTRL0, cc2520_frmctrl0_default.value, dev);
	cc2520_radio_writeRegister(CC2520_FRMFILT1, cc2520_frmfilt1_default.value, dev);
	cc2520_radio_writeRegister(CC2520_SRCMATCH, cc2520_srcmatch_default.value, dev);
	cc2520_radio_unlock(index);
}

void cc2520_radio_on(struct cc2520_dev *dev)
{
	int index = dev->id;
	cc2520_radio_lock(CC2520_RADIO_STATE_CONFIG, index);
	cc2520_radio_set_channel(channel[index] & CC2520_CHANNEL_MASK, dev);
	cc2520_radio_set_address(short_addr[index], extended_addr[index], pan_id[index], dev);
	cc2520_radio_strobe(CC2520_CMD_SRXON, dev);
	cc2520_radio_unlock(index);
}

void cc2520_radio_off(struct cc2520_dev *dev)
{
	int index = dev->id;

	cc2520_radio_lock(CC2520_RADIO_STATE_CONFIG, index);
	cc2520_radio_strobe(CC2520_CMD_SRFOFF, dev);
	cc2520_radio_unlock(index);
}

//////////////////////////////
// Configuration Commands
/////////////////////////////

bool cc2520_radio_is_clear(struct cc2520_dev *dev)
{
	return gpio_get_value(dev->cca) == 1;
}

void cc2520_radio_set_channel(int new_channel, struct cc2520_dev *dev)
{
	int index = dev->id;

	cc2520_freqctrl_t freqctrl;

	channel[index] = new_channel;
	freqctrl = cc2520_freqctrl_default;

	freqctrl.f.freq = 11 + 5 * (channel[index] - 11);

	cc2520_radio_writeRegister(CC2520_FREQCTRL, freqctrl.value, dev);
}

// Sets the short address
void cc2520_radio_set_address(u16 new_short_addr, u64 new_extended_addr, u16 new_pan_id, struct cc2520_dev *dev)
{
	char addr_mem[12];
	int index = dev->id;

	short_addr[index] = new_short_addr;
	extended_addr[index] = new_extended_addr;
	pan_id[index] = new_pan_id;

	memcpy(addr_mem, &extended_addr, 8);

	addr_mem[9] = (pan_id[index] >> 8) & 0xFF;
	addr_mem[8] = (pan_id[index]) & 0xFF;

	addr_mem[11] = (short_addr[index] >> 8) & 0xFF;
	addr_mem[10] = (short_addr[index]) & 0xFF;

	cc2520_radio_writeMemory(CC2520_MEM_ADDR_BASE, addr_mem, 12, dev);
}

void cc2520_radio_set_txpower(u8 power, struct cc2520_dev *dev)
{
	cc2520_txpower_t txpower;
	txpower = cc2520_txpower_default;

	txpower.f.pa_power = power;

	cc2520_radio_writeRegister(CC2520_TXPOWER, txpower.value, dev);
}

//////////////////////////////
// Callback Hooks
/////////////////////////////

// context: interrupt
void cc2520_radio_sfd_occurred(u64 nano_timestamp, u8 is_high, struct cc2520_dev *dev)
{
	int index = dev->id;
	// Store the SFD time for use later in timestamping
	// incoming/outgoing packets. To be used later...
	sfd_nanos_ts[index] = nano_timestamp;

	if (!is_high) {
		// SFD falling indicates TX completion
		// if we're currently in TX mode, unlock.
		if (cc2520_radio_tx_unlock_sfd(index)) {
			cc2520_radio_completeTx(dev);
		}
	}
}

// context: interrupt
void cc2520_radio_fifop_occurred(struct cc2520_dev *dev)
{
	int index = dev->id;

	spin_lock_irqsave(&pending_rx_sl[index], flags[index]);

	if (pending_rx[index]) {
		spin_unlock_irqrestore(&pending_rx_sl[index], flags[index]);
	}
	else {
		pending_rx[index] = true;
		spin_unlock_irqrestore(&pending_rx_sl[index], flags[index]);
		cc2520_radio_beginRx(dev);
	}
}

void cc2520_radio_reset(void)
{
	// TODO.
}

///////////////////////////////
// SPI Chip Select
///////////////////////////////


// void cc2520_cs_mux(int id){
// 	int i = 0;
// 	printk("pin config begin\n");
// 	if(id == 0){
// 		for(i = 0; i < CC2520_NUM_DEVICES; ++i){
// 			gpio_set_value(CS_ENABLE[i], 0);
// 			printk("pin disabled: %d\n", CS_ENABLE[i]);
// 		}
// 		return;
// 	}
// 	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
// 		if(id & (1<<i)){
// 			gpio_set_value(CS_ENABLE[i], 1);
// 			printk("pin enabled: %d\n", CS_ENABLE[i]);
// 		}
// 		else{
// 			gpio_set_value(CS_ENABLE[i], 0);
// 			printk("pin disabled: %d\n", CS_ENABLE[i]);
// 		}
// 	}
// }

//////////////////////////////
// Transmit Engine
/////////////////////////////

// context: process?
static int cc2520_radio_tx(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;

	DBG((KERN_INFO "[cc2520] - beginning write op.\n"));
	// capture exclusive radio rights to send
	// build the transmit command seq
	// write that packet!

	// 1- Stop Receiving
	// 2- Write Packet Header
	// 3- Turn on TX
	// 4- Write Rest of Packet
	// 5- On SFD falling edge give up lock

	// Beginning of TX critical section
	cc2520_radio_lock(CC2520_RADIO_STATE_TX, index);

	memcpy(tx_buf_r[index], buf, len);
	tx_buf_r_len[index] = len;

	cc2520_radio_beginTx(dev);
	return 0;
}

// Tx Part 1: Turn off the RF engine.
static int cc2520_radio_beginTx(struct cc2520_dev *dev)
{
	int status;
	int result = 0;
	int index = dev->id;

	tsfer1.tx_buf = tx_buf[index];
	tsfer1.rx_buf = rx_buf[index];
	tsfer1.len = 0;
	tsfer1.cs_change = 1;
	tx_buf[index][tsfer1.len++] = CC2520_CMD_SRFOFF;

	//result = down_interruptible(&spi_sem);
	//if(result)
	//	return -ERESTARTSYS;

	//cc2520_cs_mux(index);

	spi_message_init(&msg);
	msg.complete = cc2520_radio_continueTx_check;
	msg.context = dev;

	spi_message_add_tail(&tsfer1, &msg);

	status = gap_spi_async(&msg, GAP_SPI_CS_INDICES[index]);

	return result;
}

// Tx Part 2: Check for missed RX transmission
// and flush the buffer, actually write the data.
static void cc2520_radio_continueTx_check(void *arg)
{
	int status;
	int buf_offset;
	int i;
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	buf_offset = 0;

	tsfer1.tx_buf = tx_buf[index] + buf_offset;
	tsfer1.rx_buf = rx_buf[index]+ buf_offset;
	tsfer1.len = 0;
	tsfer1.cs_change = 1;

	if (gpio_get_value(dev->fifo) == 1) {
		INFO((KERN_INFO "[cc2520] - tx/rx race condition adverted.\n"));
		tx_buf[index][buf_offset + tsfer1.len++] = CC2520_CMD_SFLUSHRX;
	}

	tx_buf[index][buf_offset + tsfer1.len++] = CC2520_CMD_TXBUF;

	// Length + FCF
	for (i = 0; i < 3; i++)
		tx_buf[index][buf_offset + tsfer1.len++] = tx_buf_r[index][i];
	buf_offset += tsfer1.len;

	tsfer2.tx_buf = tx_buf[index] + buf_offset;
	tsfer2.rx_buf = rx_buf[index] + buf_offset;
	tsfer2.len = 0;
	tsfer2.cs_change = 1;
	tx_buf[index][buf_offset + tsfer2.len++] = CC2520_CMD_STXON;
	buf_offset += tsfer2.len;

	// We're keeping these two SPI transactions separated
	// in case we later want to encode timestamp
	// information in the packet itself after seeing SFD
	// flag.
	if (tx_buf_r_len[index] > 3) {
		tsfer3.tx_buf = tx_buf[index] + buf_offset;
		tsfer3.rx_buf = rx_buf[index] + buf_offset;
		tsfer3.len = 0;
		tsfer3.cs_change = 1;
		tx_buf[index][buf_offset + tsfer3.len++] = CC2520_CMD_TXBUF;
		for (i = 3; i < tx_buf_r_len[index]; i++)
			tx_buf[index][buf_offset + tsfer3.len++] = tx_buf_r[index][i];

		buf_offset += tsfer3.len;
	}

	tsfer4.tx_buf = tx_buf[index] + buf_offset;
	tsfer4.rx_buf = rx_buf[index] + buf_offset;
	tsfer4.len = 0;
	tsfer4.cs_change = 1;
	tx_buf[index][buf_offset + tsfer4.len++] = CC2520_CMD_REGISTER_READ | CC2520_EXCFLAG0;
	tx_buf[index][buf_offset + tsfer4.len++] = 0;

	spi_message_init(&msg);
	msg.complete = cc2520_radio_continueTx;
	msg.context = dev;

	spi_message_add_tail(&tsfer1, &msg);
	spi_message_add_tail(&tsfer2, &msg);

	if (tx_buf_r_len[index] > 3)
		spi_message_add_tail(&tsfer3, &msg);

	spi_message_add_tail(&tsfer4, &msg);

	status = gap_spi_async(&msg, GAP_SPI_CS_INDICES[index]);
}

static void cc2520_radio_continueTx(void *arg)
{
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	DBG((KERN_INFO "[cc2520] - tx spi write callback complete.\n"));

	if ((((u8*)tsfer4.rx_buf)[1] & CC2520_TX_UNDERFLOW) > 0) {
		cc2520_radio_flushTx(dev);
	}
	else if (cc2520_radio_tx_unlock_spi(index)) {
		// To prevent race conditions between the SPI engine and the
		// SFD interrupt we unlock in two stages. If this is the last
		// thing to complete we signal TX complete.
		cc2520_radio_completeTx(dev);
	}
}

static void cc2520_radio_flushTx(struct cc2520_dev *dev)
{
	int status;
	int index = dev->id;
	INFO((KERN_INFO "[cc2520] - tx underrun occurred.\n"));

	tsfer1.tx_buf = tx_buf[index];
	tsfer1.rx_buf = rx_buf[index];
	tsfer1.len = 0;
	tsfer1.cs_change = 1;
	tx_buf[index][tsfer1.len++] = CC2520_CMD_SFLUSHTX;
	tx_buf[index][tsfer1.len++] = CC2520_CMD_REGISTER_WRITE | CC2520_EXCFLAG0;
	tx_buf[index][tsfer1.len++] = 0;

	spi_message_init(&msg);
	msg.complete = cc2520_radio_completeFlushTx;
	msg.context = dev;

	spi_message_add_tail(&tsfer1, &msg);

	status = gap_spi_async(&msg, GAP_SPI_CS_INDICES[index]);
}

static void cc2520_radio_completeFlushTx(void *arg)
{
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	cc2520_radio_unlock(index);
	DBG((KERN_INFO "[cc2520] - write op complete.\n"));
	radio_top[index]->tx_done(-CC2520_TX_FAILED, dev);

	//up(&spi_sem);
}

static void cc2520_radio_completeTx(struct cc2520_dev *dev)
{
	int index = dev->id;

	cc2520_radio_unlock(index);
	DBG((KERN_INFO "[cc2520] - write op complete.\n"));
	radio_top[index]->tx_done(CC2520_TX_SUCCESS, dev);

	//up(&spi_sem);
}

//////////////////////////////
// Receiver Engine
/////////////////////////////

static int cc2520_radio_beginRx(struct cc2520_dev *dev)
{
	int status;
	int result = 0;
	int index = dev->id;

	rx_tsfer.tx_buf = rx_out_buf[index];
	rx_tsfer.rx_buf = rx_in_buf[index];
	rx_tsfer.len = 0;
	rx_out_buf[index][rx_tsfer.len++] = CC2520_CMD_RXBUF;
	rx_out_buf[index][rx_tsfer.len++] = 0;

	rx_tsfer.cs_change = 1;

	memset(rx_in_buf[index], 0, SPI_BUFF_SIZE);

	result = down_interruptible(&spi_sem);
	if(result)
		return -ERESTARTSYS;

	//cc2520_cs_mux(index);

	spi_message_init(&rx_msg);
	rx_msg.complete = cc2520_radio_continueRx;
	rx_msg.context = dev;
	spi_message_add_tail(&rx_tsfer, &rx_msg);

	status = gap_spi_async(&rx_msg, GAP_SPI_CS_INDICES[index]);

	return result;
}

static void cc2520_radio_continueRx(void *arg)
{
	int status;
	int i;
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	// Length of what we're reading is stored
	// in the received spi buffer, read from the
	// async operation called in beginRxRead.
	dev->len = rx_in_buf[index][1];

	if (dev->len > 127) {
		cc2520_radio_flushRx(dev);
	}
	else {
		rx_tsfer.len = 0;
		rx_out_buf[index][rx_tsfer.len++] = CC2520_CMD_RXBUF;
		for (i = 0; i < dev->len; i++)
			rx_out_buf[index][rx_tsfer.len++] = 0;

		rx_tsfer.cs_change = 1;

		spi_message_init(&rx_msg);
		rx_msg.complete = cc2520_radio_finishRx;
		// Platform dependent?
		rx_msg.context = dev;
		spi_message_add_tail(&rx_tsfer, &rx_msg);

		status = gap_spi_async(&rx_msg, GAP_SPI_CS_INDICES[index]);
	}
}

static void cc2520_radio_flushRx(struct cc2520_dev *dev)
{

	int status;
	int index = dev->id;

	INFO((KERN_INFO "[cc2520] - flush RX FIFO (part 1).\n"));

	rx_tsfer.len = 0;
	rx_tsfer.cs_change = 1;
	rx_out_buf[index][rx_tsfer.len++] = CC2520_CMD_SFLUSHRX;

	spi_message_init(&rx_msg);
	rx_msg.complete = cc2520_radio_continueFlushRx;
	rx_msg.context = dev;

	spi_message_add_tail(&rx_tsfer, &rx_msg);

	status = gap_spi_async(&rx_msg, GAP_SPI_CS_INDICES[index]);
}

// Flush RX twice. This is due to Errata Bug 1 and to try to fix an issue where
// the radio goes into a state where it no longer receives any packets after
// clearing the RX FIFO when a packet arrives at the same time a packet
// is being processed.
// Also, both the TinyOS and Contiki implementations do this.
static void cc2520_radio_continueFlushRx(void* arg)
{
	int status;
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	INFO((KERN_INFO "[cc2520] - flush RX FIFO (part 2).\n"));

	rx_tsfer.len = 0;
	rx_tsfer.cs_change = 1;
	rx_out_buf[index][rx_tsfer.len++] = CC2520_CMD_SFLUSHRX;

	spi_message_init(&rx_msg);
	rx_msg.complete = cc2520_radio_completeFlushRx;
	rx_msg.context = dev;

	spi_message_add_tail(&rx_tsfer, &rx_msg);

	status = gap_spi_async(&rx_msg, GAP_SPI_CS_INDICES[index]);
}

static void cc2520_radio_completeFlushRx(void *arg)
{
	struct cc2520_dev *dev = arg;
	int index = dev->id;

	spin_lock_irqsave(&pending_rx_sl[index], flags[index]);
	pending_rx[index] = false;
	spin_unlock_irqrestore(&pending_rx_sl[index], flags[index]);

	up(&spi_sem);
}

static void cc2520_radio_finishRx(void *arg)
{
	struct cc2520_dev *dev = arg;
	int index = dev->id;
	int len = dev->len;

	// we keep a lock on the RX buffer separately
	// to allow for another rx packet to pile up
	// behind the current one.
	spin_lock(&rx_buf_sl[index]);

	// Note: we place the len at the beginning
	// of the packet to make the interface symmetric
	// with the TX interface.
	rx_buf_r[index][0] = len;

	// Make sure to ignore the command return byte.
	memcpy(rx_buf_r[index] + 1, rx_in_buf[index] + 1, len);

	// Pass length of entire buffer to
	// upper layers.
	radio_top[index]->rx_done(rx_buf_r[index], len + 1, dev);

	DBG((KERN_INFO "[cc2520] - Read %d bytes from radio.\n", len));

	// For now if we received more than one RX packet we simply
	// clear the buffer, in the future we can move back to the scheme
	// where pending_rx is actually a FIFOP toggle counter and continue
	// to receive another packet. Only do this if it becomes a problem.
	// UPDATE
	// There are a few cases that can mean we should clear the buffer
	// and try again:
	//   - If FIFO is high that means there are more RX bytes to read.
	//   - If FIFO is low and FIFOP is high, that means RX overflow.
	if ((gpio_get_value(dev->fifo) == 1) ||
		((gpio_get_value(dev->fifo) == 0) && (gpio_get_value(dev->fifop) == 1))) {
		INFO((KERN_INFO "[cc2520] - more than one RX packet received, flushing buffer\n"));
		cc2520_radio_flushRx(dev);
	}
	else {
		// Allow for subsequent FIFOP
		spin_lock_irqsave(&pending_rx_sl[index], flags[index]);
		pending_rx[index] = false;
		spin_unlock_irqrestore(&pending_rx_sl[index], flags[index]);
	}

	up(&spi_sem);
}

void cc2520_radio_release_rx(int index)
{
	spin_unlock(&rx_buf_sl[index]);
}

//////////////////////////////
// Helper Routines
/////////////////////////////

// Memory address MUST be >= 200.
static void cc2520_radio_writeMemory(u16 mem_addr, u8 *value, u8 len, struct cc2520_dev *dev)
{
	int status;
	int i;
	int index = dev->id;
	int result = 0;

	tsfer.tx_buf = tx_buf[index];
	tsfer.rx_buf = rx_buf[index];
	tsfer.len = 0;

	tx_buf[index][tsfer.len++] = CC2520_CMD_MEMORY_WRITE | ((mem_addr >> 8) & 0xFF);
	tx_buf[index][tsfer.len++] = mem_addr & 0xFF;

	for (i=0; i<len; i++) {
		tx_buf[index][tsfer.len++] = value[i];
	}

	memset(rx_buf[index], 0, SPI_BUFF_SIZE);

	result = down_interruptible(&spi_sem);
	//cc2520_cs_mux(index);

	spi_message_init(&msg);
	msg.context = NULL;
	spi_message_add_tail(&tsfer, &msg);

	status = gap_spi_sync(&msg, GAP_SPI_CS_INDICES[index]);

	up(&spi_sem);
}

static void cc2520_radio_writeRegister(u8 reg, u8 value, struct cc2520_dev *dev)
{
	int status;
	int index = dev->id;
	int result = 0;

	tsfer.tx_buf = tx_buf[index];
	tsfer.rx_buf = rx_buf[index];
	tsfer.len = 0;

	if (reg <= CC2520_FREG_MASK) {
		tx_buf[index][tsfer.len++] = CC2520_CMD_REGISTER_WRITE | reg;
	}
	else {
		tx_buf[index][tsfer.len++] = CC2520_CMD_MEMORY_WRITE;
		tx_buf[index][tsfer.len++] = reg;
	}

	tx_buf[index][tsfer.len++] = value;

	memset(rx_buf[index], 0, SPI_BUFF_SIZE);

	result = down_interruptible(&spi_sem);
	//cc2520_cs_mux(index);

	spi_message_init(&msg);
	msg.context = NULL;
	spi_message_add_tail(&tsfer, &msg);

	status = gap_spi_sync(&msg, GAP_SPI_CS_INDICES[index]);

	up(&spi_sem);
}

static cc2520_status_t cc2520_radio_strobe(u8 cmd, struct cc2520_dev *dev)
{
	int status;
	cc2520_status_t ret;
	int index = dev->id;
	int result = 0;

	tsfer.tx_buf = tx_buf[index];
	tsfer.rx_buf = rx_buf[index];
	tsfer.len = 0;

	tx_buf[index][0] = cmd;
	tsfer.len = 1;

	memset(rx_buf[index], 0, SPI_BUFF_SIZE);

	result = down_interruptible(&spi_sem);
	//cc2520_cs_mux(index);

	spi_message_init(&msg);
	msg.context = NULL;
	spi_message_add_tail(&tsfer, &msg);

	status = gap_spi_sync(&msg, GAP_SPI_CS_INDICES[index]);

	ret.value = rx_buf[index][0];
	up(&spi_sem);
	return ret;
}
