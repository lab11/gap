#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/list.h>

#include "unique.h"
#include "packet.h"
#include "cc2520.h"
#include "debug.h"

struct node_list{
	struct list_head list;
	u64 src;
	u8 dsn;
};

// struct list_head nodes[CC2520_NUM_DEVICES];

int cc2520_unique_init(struct cc2520_dev *dev)
{
	// int i;
	// for(i = 0; i < CC2520_NUM_DEVICES; ++i){

		INIT_LIST_HEAD(&dev->nodes);
	// }
	return 0;
}

void cc2520_unique_free(struct cc2520_dev *dev)
{
	// int i;

	// for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		struct node_list *tmp;
		struct list_head *pos, *q;

		list_for_each_safe(pos, q, &dev->nodes) {
			tmp = list_entry(pos, struct node_list, list);
			list_del(pos);
			kfree(tmp);
		}
	// }
}

static int cc2520_unique_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	return cc2520_lpl_tx(buf, len, dev);
}

static void cc2520_unique_tx_done(u8 status, struct cc2520_dev *dev)
{
	cc2520_interface_tx_done(status, dev);
}
//////////////////
static void cc2520_unique_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	struct node_list *tmp;
	u8 dsn;
	u64 src;
	bool found;
	bool drop;

	dsn = cc2520_packet_get_header(buf)->dsn;
	src = cc2520_packet_get_src(buf);

	found = false;
	drop = false;

	list_for_each_entry(tmp, &dev->nodes, list) {
		if (tmp->src == src) {
			found = true;
			if (tmp->dsn != dsn) {
				tmp->dsn = dsn;
			}
			else {
				drop = true;
			}
			break;
		}
	}

	if (!found) {
		tmp = kmalloc(sizeof(struct node_list), GFP_ATOMIC);
		if (tmp) {
			tmp->dsn = dsn;
			tmp->src = src;
			list_add(&(tmp->list), &dev->nodes);
			INFO(KERN_INFO, "unique%d found new mote: %lld\n", dev->id, src);
		}
		else {
			INFO(KERN_INFO, "unique%d alloc failed.\n", dev->id);
		}
	}

	if (!drop) {
		cc2520_interface_rx_done(buf, len, dev);
	}
}
