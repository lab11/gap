#!/usr/bin/env python3


print('here')
import asyncio

print('we')
import websockets

print('wat')

@asyncio.coroutine
def hello():
    websocket = yield from websockets.connect('ws://141.212.11.192:8764/')
    yield from websocket.send('BLE')
    while True:
        ble_packet = yield from websocket.recv()
        print(type(ble_packet))
        print(len(ble_packet))
#        print("< {}".format(ble_packet))

print('start')

asyncio.get_event_loop().run_until_complete(hello())
