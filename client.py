import socket
import sys
import time
import random
import termios
import tty
import select

HOST = "127.0.0.1"
PORT = 9999

FPS = 60
PING_INTERVAL = 0.5

SYNC_CODE = 0

INPUT_UP    = 1 << 0
INPUT_DOWN  = 1 << 1
INPUT_LEFT  = 1 << 2
INPUT_RIGHT = 1 << 3
INPUT_A     = 1 << 4
INPUT_B     = 1 << 5

def gen_token():
    t = (random.randint(1, 127) | 1) & 0x7F
    if t == 0:
        t = 1
    return t

def read_key_once():
    dr, _, _ = select.select([sys.stdin], [], [], 0)
    if dr:
        return sys.stdin.read(1)
    return None

def keys_to_input(ch):
    v = 0
    if ch is None:
        return 0
    if ch == 'w': v |= INPUT_UP
    if ch == 's': v |= INPUT_DOWN
    if ch == 'a': v |= INPUT_LEFT
    if ch == 'd': v |= INPUT_RIGHT
    if ch == 'j': v |= INPUT_A
    if ch == 'k': v |= INPUT_B
    if ch == 'q': raise KeyboardInterrupt
    return v & 0xFF

def recv_one(sock):
    try:
        d = sock.recv(1)
        if d:
            return d[0]
        return None
    except BlockingIOError:
        return None

def send_one(sock, b):
    try:
        sock.send(bytes([b & 0xFF]))
        return True
    except BlockingIOError:
        return False

def main():
    host = HOST
    port = PORT
    if len(sys.argv) > 1:
        host = sys.argv[1]
    if len(sys.argv) > 2:
        port = int(sys.argv[2])

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    s.setblocking(False)

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setcbreak(fd)

    token = gen_token()
    remote_token = None
    local_player_id = 0

    phase = "ESTABLISH"
    last_ping = 0.0
    sent_sync = False

    remote_last = 0
    local_last = 0

    print(f"Connected to {host}:{port}")
    print("Controls: WASD + J/K, Q quit")

    try:
        while True:
            now = time.time()

            b = recv_one(s)
            while b is not None:
                if phase == "ESTABLISH":
                    if b == token:
                        token = gen_token()
                        remote_token = None
                    else:
                        remote_token = b
                        local_player_id = 0 if token < remote_token else 1
                        phase = "SYNC_SEND"
                        sent_sync = False
                elif phase == "SYNC_SEND":
                    if b == SYNC_CODE:
                        phase = "INGAME"
                elif phase == "INGAME":
                    remote_last = b
                b = recv_one(s)

            if phase == "ESTABLISH":
                if now - last_ping >= PING_INTERVAL:
                    send_one(s, token)
                    last_ping = now

                sys.stdout.write(
                    f"\rPhase=ESTABLISH token=0x{token:02X} remote={'--' if remote_token is None else f'0x{remote_token:02X}'}   "
                )
                sys.stdout.flush()

            elif phase == "SYNC_SEND":
                if not sent_sync:
                    if send_one(s, SYNC_CODE):
                        sent_sync = True

                sys.stdout.write(
                    f"\rPhase=SYNC localId={local_player_id} token=0x{token:02X} remote=0x{remote_token:02X}   "
                )
                sys.stdout.flush()

            else:
                ch = read_key_once()
                local_last = keys_to_input(ch)

                send_one(s, local_last)

                sys.stdout.write(
                    f"\rPhase=INGAME local=0x{local_last:02X} remote=0x{remote_last:02X} localId={local_player_id}   "
                )
                sys.stdout.flush()

            time.sleep(1 / FPS)

    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        try:
            s.close()
        except:
            pass
        print("\nDisconnected")

if __name__ == "__main__":
    main()
