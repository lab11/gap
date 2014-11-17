#include <stdio.h>
#include <string.h>
#include <math.h>

#include "nrf_error.h"

#include "interrupt_event_queue.h"
#include "bcp_spi_slave.h"

interrupt_event_queue_item_t queue[INTERRUPT_EVENT_QUEUE_LEN];
uint8_t queue_head = 0;
uint8_t queue_tail = 0;
uint8_t items_in_queue = 0;


uint32_t interrupt_event_queue_add (uint8_t interrupt_event,
                                    uint8_t len,
                                    uint8_t* data) {
	interrupt_event_queue_item_t* queue_entry;

	if (items_in_queue == INTERRUPT_EVENT_QUEUE_LEN) {
		return NRF_ERROR_NO_MEM;
	}

	queue_entry = queue + queue_head;

	// Copy into the queue
	memcpy(queue_entry->buffer, data, fmin(len, 64));
	queue_entry->len = len;
	queue_entry->interrupt_event = interrupt_event;

	// Update counters
	items_in_queue++;
	queue_head = (queue_head + 1) % INTERRUPT_EVENT_QUEUE_LEN;

	// Notify the SPI layer that it should read from the queue to populate
	// the SPI buffer ahead of time.
	spi_slave_notify();

	return NRF_SUCCESS;
}

uint16_t interrupt_event_queue_get (uint8_t* interrupt_event, uint8_t* data) {
	uint8_t len;

	if (items_in_queue == 0) {
		return 0;
	}

	// Copy to the arguments
	*interrupt_event = queue[queue_tail].interrupt_event;
	memcpy(data, queue[queue_tail].buffer, fmin(queue[queue_tail].len, 64));
	len = queue[queue_tail].len;

	// Update queue state
	items_in_queue--;
	queue_tail = (queue_tail + 1) % INTERRUPT_EVENT_QUEUE_LEN;

	return len;
}
