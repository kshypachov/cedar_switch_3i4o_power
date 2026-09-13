"""Run shell commands over the board's telnet shell; print what comes back."""
import re, socket, sys, time
def run(cmds, host="192.168.88.14", wait=2.5):
    s = socket.create_connection((host, 23), timeout=5)
    def drain(t):
        s.settimeout(0.3); buf = b""; end = time.time() + t
        while time.time() < end:
            try:
                d = s.recv(4096)
                if not d: break
                buf += d; end = max(end, time.time() + 0.8)
            except socket.timeout:
                pass
        return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]|\xff[\xfb-\xfe].|\r", b"", buf).decode(errors="replace")
    drain(1.0)
    out = []
    for c in cmds:
        s.sendall(c.encode() + b"\r\n")
        out.append(f"### {c}\n{drain(wait)}")
    s.close()
    return "\n".join(out)
if __name__ == "__main__":
    print(run(sys.argv[1:]))
