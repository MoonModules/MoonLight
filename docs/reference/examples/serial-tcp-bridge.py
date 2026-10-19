"""A serial port on a TCP port, for a Zigbee coordinator a VM cannot reach over USB: Zigbee2MQTT connects to tcp://<host>:<port> instead of a COM port.

One client at a time, since a coordinator has one consumer: a new connection replaces the old one, so a client that reconnects after a dropped link is never locked out by its own stale socket. The port runs at the given baud rate, 8N1, with no flow control. Each connection logs the bytes it moved in each direction when it closes, which is what tells a silent coordinator from a silent network.

A serial error ends the program, so the service wrapper (NSSM's AppExit Restart) restarts it once the port is back, for example after the USB device enumerates late at boot.

It needs pyserial in the Python that runs it; a service wants a fixed interpreter, so it runs from its own venv: python -m venv .venv, then .venv\\Scripts\\pip install pyserial.

Run: .venv\\Scripts\\python serial-tcp-bridge.py --port COM9 --tcp 20108
"""

import argparse
import os
import socket
import threading
import time

import serial


def log(text):
    print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}", flush=True)


class Connection:
    """One client's socket and the bytes moved for it, so its counts live and end with it."""

    def __init__(self, sock, address):
        self.sock = sock
        self.address = f"{address[0]}:{address[1]}"
        self.to_serial = 0
        self.to_tcp = 0


class Bridge:
    """The serial port and the one client it serves."""

    def __init__(self, port, baud):
        self.serial = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, xonxoff=False, rtscts=False, dsrdtr=False, timeout=0.1)
        self.client = None
        self.lock = threading.Lock()

    def serial_to_tcp(self):
        """Copy what the coordinator sends to the current client; with no client the bytes are dropped, as a closed port would."""
        while True:
            try:
                data = self.serial.read(4096)
            except serial.SerialException as e:
                log(f"serial error: {e}")
                os._exit(1)   # the whole program, which sys.exit in a thread would not end
            if not data:
                continue
            with self.lock:
                conn = self.client
            if conn is None:
                continue
            try:
                conn.sock.sendall(data)
                conn.to_tcp += len(data)
            except OSError:
                pass   # the client is going away; its reader closes it

    def serve(self, conn):
        """Copy what the client sends to the coordinator until the client closes or a newer one replaces it."""
        while True:
            try:
                data = conn.sock.recv(4096)
            except OSError:
                data = b""
            if not data:
                break
            try:
                self.serial.write(data)
            except serial.SerialException as e:
                log(f"serial error: {e}")
                os._exit(1)
            conn.to_serial += len(data)
        with self.lock:
            if self.client is conn:
                self.client = None
        conn.sock.close()
        log(f"{conn.address} closed: {conn.to_serial} bytes to serial, {conn.to_tcp} bytes to tcp")

    def accept(self, sock, address):
        """Make this the one client, closing the one before it."""
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn = Connection(sock, address)
        with self.lock:
            old, self.client = self.client, conn
        if old is not None:
            log("a new client replaces the previous one")
            try:
                old.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        log(f"{conn.address} connected")
        threading.Thread(target=self.serve, args=(conn,), daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True, help="the serial port, such as COM9 or /dev/ttyUSB0")
    ap.add_argument("--tcp", type=int, default=20108, help="the TCP port to listen on")
    ap.add_argument("--baud", type=int, default=115200, help="the coordinator's baud rate")
    args = ap.parse_args()

    bridge = Bridge(args.port, args.baud)
    threading.Thread(target=bridge.serial_to_tcp, daemon=True).start()
    server = socket.create_server(("0.0.0.0", args.tcp))
    log(f"{args.port} at {args.baud} 8N1 on tcp port {args.tcp}")
    while True:
        bridge.accept(*server.accept())


if __name__ == "__main__":
    main()
