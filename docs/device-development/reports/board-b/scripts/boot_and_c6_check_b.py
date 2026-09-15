"""Board B only (J-Link 000941000024, console /dev/cu.usbmodem5AE60208891).
Reset, capture the application's boot log, then read-only shell probes of the C6
link over ESP-Hosted SPI. Never sends wifi_ctrl reset (the hosted transport does
not recover from a C6 reset without an STM32 reboot)."""
import re, subprocess, sys, time
import serial
P = sys.argv[1]; LISTEN = float(sys.argv[2]) if len(sys.argv) > 2 else 90
UART = "/dev/cu.usbmodem5AE60208891"
JLINK = ["JLinkExe", "-USB", "000941000024", "-device", "STM32U585AI", "-if", "SWD",
         "-speed", "4000", "-autoconnect", "1", "-NoGui", "1", "-ExitOnError", "0"]
open(f"{P}/_reset.jlink", "w").write("r\ng\nq\n")
s = serial.Serial(); s.port = UART; s.baudrate = 115200; s.timeout = 0.1
s.dtr = False; s.rts = False; s.open(); s.reset_input_buffer()
log = bytearray()
def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        log.extend(s.read(4096))
subprocess.run(JLINK + ["-CommanderScript", f"{P}/_reset.jlink"], capture_output=True)
t0 = time.time(); pump(LISTEN)
text = log.decode("utf-8", "replace").replace("\r", "")
fallback = re.search(r"fallback MAC|EEPROM MAC invalid", text, re.I)
def send(cmd, wait):
    for ch in cmd + "\r":
        s.write(ch.encode()); time.sleep(0.02)
    pump(wait)
if not fallback:
    send("", 1); send("net iface", 4); send("wifi status", 5); send("wifi scan", 20)
s.close()
text = log.decode("utf-8", "replace").replace("\r", "")
open(f"{P}/board_b_app_boot.log", "w").write(text)
KEYS = r"Starting bootloader|Jumping|Booting Zephyr|MAC|IPv4|DHCP|web interface|cedar-|coprocessor|hosted|esp_hosted|priv event|SPI bus|handshake|spi_transceive|RPC|wifi|Wi-Fi|SSID|Scan|<err>|FATAL|fault"
print(f"{len(log)} bytes captured in {time.time()-t0:.0f} s; fallback MAC: {bool(fallback)}")
for line in text.splitlines():
    if re.search(KEYS, line, re.I):
        print(line[:200])
