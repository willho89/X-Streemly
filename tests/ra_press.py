#!/usr/bin/env python3
# X-Streemly — test tool (CLAUDE.md §8 notes: "Driving RetroArch unattended").
# ra_press.py <port> <script>: press RetroPad buttons in RetroArch through its
# Network RetroPad (network_remote_enable). script: "wait:BUTTON[*hold_ms]"
# items separated by commas, waits in seconds; e.g. "8:UP,4:UP*900,1:A".
import socket, struct, sys, time

IDS = {"B": 0, "Y": 1, "SELECT": 2, "START": 3, "UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7,
       "A": 8, "X": 9, "L": 10, "R": 11, "L2": 12, "R2": 13}
RETRO_DEVICE_JOYPAD = 1

def send(sock, port, button, state):
    # struct remote_message { int port; int device; int index; int id; uint16_t state; } (+2 padding)
    sock.sendto(struct.pack("<iiiiHxx", 0, RETRO_DEVICE_JOYPAD, 0, IDS[button], state), ("127.0.0.1", port))

def main():
    port, script = int(sys.argv[1]), sys.argv[2]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for item in script.split(","):
        wait, what = item.split(":")
        hold = 120
        if "*" in what:
            what, h = what.split("*")
            hold = int(h)
        time.sleep(float(wait))
        print(f"{time.monotonic():.1f} press {what} for {hold} ms", flush=True)
        send(s, port, what, 1)
        time.sleep(hold / 1000)
        send(s, port, what, 0)

main()
