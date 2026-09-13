import http.client, json, subprocess, sys, time
sys.path.insert(0, '/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench')
from bench_console import Console

BOARD = '192.168.88.14'
PASSWORD = 'cedar-bench-P2-changed'
CLI = "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"

def call(method, path, body=None, headers=None, timeout=30):
    t0 = time.perf_counter()
    c = http.client.HTTPConnection(BOARD, 80, timeout=timeout)
    hdrs = {'Connection': 'close', **(headers or {})}
    data = None
    if body is not None:
        data = json.dumps(body).encode(); hdrs['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=hdrs)
    r = c.getresponse(); b = r.read(); c.close()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, b, time.perf_counter() - t0

s, h, b, dt = call('POST', '/api/v1/auth/session', {'password': PASSWORD})
cookie = h['set-cookie'].split(';')[0]
print('before reset: login', s, f'{dt:.2f}s', '| session', call('GET', '/api/v1/auth/session', headers={'Cookie': cookie})[0], flush=True)
old_boot = json.loads(call('GET', '/api/v1/system/status', headers={'Cookie': cookie})[2])['boot_id']

con = Console('console-reboot.log')
since = con.mark()
rst = subprocess.run([CLI, '-c', 'port=swd', 'sn=002F002B3233510739363634', 'mode=NORMAL', '-hardRst'], capture_output=True, text=True).stdout
print('reset:', 'Hard reset is performed' in rst, flush=True)
web = con.wait_for(r'web interface', since, 180)
print('web line:', web and con.lines_since(web[2])[0][1].split('<inf> ')[-1], flush=True)
warn = [l for _, l in con.lines_since(since) if 'setup is open' in l]
print('setup-open warning after reset:', bool(warn), flush=True)
con.close()

deadline = time.time() + 90
while time.time() < deadline:
    try:
        s, _, b, _ = call('GET', '/api/v1/auth/state', timeout=3)
        if s == 200:
            break
    except Exception:
        time.sleep(1)
print('auth state after reset:', s, b.decode(), flush=True)
s, _, b, _ = call('GET', '/api/v1/auth/session', headers={'Cookie': cookie})
print('old cookie after reset:', s, json.loads(b)['error']['code'], flush=True)
s, h, b, dt = call('POST', '/api/v1/auth/session', {'password': PASSWORD})
print('login after reset:', s, f'{dt:.2f}s', flush=True)
cookie = h['set-cookie'].split(';')[0]
new_boot = json.loads(call('GET', '/api/v1/system/status', headers={'Cookie': cookie})[2])['boot_id']
print('boot_id changed:', old_boot != new_boot, old_boot, '->', new_boot, flush=True)
s, _, b, _ = call('POST', '/api/v1/auth/session', {'password': 'cedar-bench-P2-password'})
print('first password after reset:', s, flush=True)
