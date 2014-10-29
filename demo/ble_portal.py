#!/usr/bin/env python3

import asyncio
import binascii
import json
import struct
import os

import websockets

queues = []

def read_from_nrf51822 (*args):
    global queues
    # Need to call the os.read to have non-blocking read
    d = os.read(args[0], 100)

    # Insert the data into the queue back to the websockets handler
    for q in queues:
        q.put_nowait(d)


@asyncio.coroutine
def websocket_handler (websocket, path):
    global count, queues

    try:
        my_queue = asyncio.Queue()
        queues.append(my_queue)

        while True:
            try:
                d = yield from my_queue.get()

                # Parse the data from nRF51822
                fields = struct.unpack('BBB6BbB', d[0:11])
                bcp_len  = fields[0]
                bcp_type = fields[1]
                adv_mac_type = fields[2]
                adv_mac  = '{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}'.format(*reversed(fields[3:9]))
                rssi = fields[9]
                misc = fields[10]
                data = d[11:]
                name = 'unknown'

                # Look for a name
                idx = 0
                while (len(data)-idx) > 2:
                    l = data[idx]
                    t = data[idx+1]
                    if t == 0x09:
                        # Found a name
                        name = data[idx+2:idx+1+l].decode('ascii')
                    idx += l + 1

                adv_pkt = {'mac': adv_mac,
                           'rssi': rssi,
                           'data': str(binascii.hexlify(data)),
                           'name': name}

                yield from websocket.send(json.dumps(adv_pkt))
            except struct.error:
                pass
            except: 
                pass

    except:
        queues.remove(my_queue)

# Start websockets server
start_server = websockets.serve(websocket_handler, '0.0.0.0', 8764)
asyncio.get_event_loop().run_until_complete(start_server)

rfd = os.open('/dev/nrf51822_1', os.O_RDONLY | os.O_NONBLOCK)
asyncio.get_event_loop().add_reader(rfd, read_from_nrf51822, rfd)

asyncio.get_event_loop().run_forever()
