"""Talk to a ucxapp TCP bridge (hw.bridge tcps, matter.bridge tcps): docs/INTEGRATION.md, way 2."""

import queue
import socket
import sys
import threading


class BridgeClient:
    """One command at a time; EVT lines go to on_event from a reader thread."""

    def __init__(self, host="127.0.0.1", port=7540, on_event=print):
        self._sock = socket.create_connection((host, port))
        self._replies = queue.Queue()
        self._on_event = on_event
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for raw in self._sock.makefile("r", encoding="utf-8", newline="\n"):
            line = raw.rstrip("\r\n")
            if line.startswith("EVT"):
                self._on_event(line)
            elif line:
                self._replies.put(line)
        self._replies.put(None)

    def command(self, line, timeout=10):
        """Send one line, return its reply line (OK ... / ERR ...)."""
        self._sock.sendall((line + "\n").encode())
        reply = self._replies.get(timeout=timeout)
        if reply is None:
            raise ConnectionError("bridge closed")
        return reply

    def close(self):
        self._sock.close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7540
    client = BridgeClient(port=port, on_event=lambda e: print("event:", e))
    try:
        for text in sys.stdin:
            if text.strip():
                print(client.command(text.strip()))
    finally:
        client.close()
