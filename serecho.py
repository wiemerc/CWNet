#!/usr/bin/env python3

import socket
import sys


s = socket.socket(socket.AF_UNIX)
s.connect('/Users/consti/var/run/vbox/com1.sock')
try:
    while True:
        line = sys.stdin.readline()
        s.send(line.encode() + b'\x00')
        if line[0] == '.':
            break

        print("message sent, waiting for answer")
        msg = []
        while True:
            ch = s.recv(1)
            if ch == b'\x00':
                print(''.join(msg), flush = True)
                break
            else:
                msg.append(ch.decode())

finally:
    s.close()
