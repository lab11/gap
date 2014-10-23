#!/usr/bin/env python3

import asyncio
import binascii
import json
import struct
import os

import websockets

queues = []

def read_from_lowapn (*args):
    global queues
    # Need to call the os.read to have non-blocking read
    d = os.read(args[0], 200)

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

                types = ['unused', 'Coilcube', 'sEHnsor', 'Impulse', 'Coilcube Splitcore', 'Gecko + Impulse', 'Buzz', 'Hot Spring']

                # Parse the data from nRF51822
                fields1 = struct.unpack('16B16B', d[8:40])
                #fields2 = struct.unpack('B', [d[58]])
                # bcp_len  = fields[0]
                # bcp_type = fields[1]
                # adv_mac_type = fields[2]
                src  = '{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}'.format(*fields1[0:16])
                dst  = '{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}'.format(*fields1[16:32])
                t = types[d[58]]
                # rssi = fields[9]
                # misc = fields[10]
                # data = d[11:]
                # name = 'unknown'

                # # Look for a name
                # idx = 0
                # while (len(data)-idx) > 2:
                #     l = data[idx]
                #     t = data[idx+1]
                #     if t == 0x09:
                #         # Found a name
                #         name = data[idx+2:idx+1+l].decode('ascii')
                #     idx += l + 1

                print(str(binascii.hexlify(d)));

                pkt = {#'mac': adv_mac,
                           #'rssi': rssi,
                           'data': str(binascii.hexlify(d)),
                           'src': src,
                           'dst': dst,
                           'name': t
                           }

                yield from websocket.send(json.dumps(pkt))
            except:
                pass

    except Exception as e:
        print(e)
        queues.remove(my_queue)

# Start websockets server
start_server = websockets.serve(websocket_handler, '0.0.0.0', 8763)
asyncio.get_event_loop().run_until_complete(start_server)

rfd = os.open('/tmp/lowpan_fifo', os.O_RDONLY | os.O_NONBLOCK)
asyncio.get_event_loop().add_reader(rfd, read_from_lowapn, rfd)

asyncio.get_event_loop().run_forever()


# b'`\x00\x00\x00\x00\x15\x11\x0f\x00\x00\x00\x00\x00\x00\x00\x00\xc2\x98\xe5CO\xf3\x99\xae \x01\x04p\x1f\x10\x13 \x00\x00\x00\x00\x00\x00\x00\x02Q\xc7\x0f\xa1\x00\x15\xeb\xc47aiOPJapXF\x01\xc4\x1e!@\xdc\x00'
# 600000000015110f
# 0000000000000000
# c298e5494ddfe7d9
# 200104701f101320
# 0000000000000002
# 51c7
# 0fa1
# 0015
# c194
# 3761694f504a61705846
# 04d6f900000000


