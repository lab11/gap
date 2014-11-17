#include <stdint.h>


#define INTERRUPT_EVENT_QUEUE_LEN 4


typedef struct {
	uint8_t  buffer[64];
	uint8_t len;
	uint8_t  interrupt_event;
} interrupt_event_queue_item_t;


uint32_t interrupt_event_queue_add (uint8_t interrupt_event,
                                    uint8_t len,
                                    uint8_t* data);

uint16_t interrupt_event_queue_get (uint8_t* interrupt_event, uint8_t* data);
