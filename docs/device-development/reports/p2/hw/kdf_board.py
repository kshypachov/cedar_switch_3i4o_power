import http.client, json, re, subprocess, sys, threading, time
sys.path.insert(0, '/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench')
from bench_console import Console

BOARD = '192.168.88.14'
con = Console('console-2.log')
since = con.mark()
web = con.wait_for(r'web interface', since, 600)
print('web up:', web and con.lines_since(web[2])[0][1], flush=True)
time.sleep(25)

for n in (100, 1000, 3000):
    lat, m = con.probe(f'web_auth kdf {n}', r'pbkdf2-hmac-sha256 (\d+) iterations: (\d+) ms.*', 180)
    print('shell:', m.group(0) if m else f'no answer for {n}', flush=True)

def call(method, path, body=None, timeout=5):
    t0 = time.perf_counter()
    c = http.client.HTTPConnection(BOARD, 80, timeout=timeout)
    data = json.dumps(body).encode() if body is not None else None
    hdrs = {'Connection': 'close'}
    if data: hdrs['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=hdrs)
    r = c.getresponse(); b = r.read(); c.close()
    return r.status, b, time.perf_counter() - t0

stop = threading.Event()
http_probes, shell_probes = [], []
def http_prober():
    while not stop.is_set():
        t = time.perf_counter()
        try:
            s, _, dt = call('GET', '/api/v1/auth/state', timeout=3)
            http_probes.append((t, s, dt))
        except Exception as e:
            http_probes.append((t, type(e).__name__, None))
        time.sleep(1)
def shell_prober():
    while not stop.is_set():
        t = time.perf_counter()
        lat, m = con.probe('kernel uptime', r'Uptime: \d+ ms', 3)
        shell_probes.append((t, lat))
        time.sleep(1)

ping = subprocess.Popen(['ping', '-i', '0.3', '-W', '1000', BOARD], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
threads = [threading.Thread(target=http_prober), threading.Thread(target=shell_prober)]
for th in threads: th.start()
time.sleep(3)
t_login = time.perf_counter()
try:
    s, b, dt = call('POST', '/api/v1/auth/session', {'password': 'cedar-bench-P2-password'}, timeout=180)
    print('login:', s, f'{dt:.2f} s', flush=True)
except Exception as e:
    dt = time.perf_counter() - t_login
    print('login failed:', type(e).__name__, f'{dt:.2f} s', flush=True)
t_end = t_login + dt
time.sleep(3)
stop.set()
for th in threads: th.join()
ping.terminate()
ping_out = ping.communicate()[0]
replies = len(re.findall(r'bytes from', ping_out)); timeouts = len(re.findall(r'Request timeout', ping_out))
during = lambda rows: [r for r in rows if t_login <= r[0] <= t_end]
hp = during(http_probes); sp = during(shell_probes)
print('during login: http ok', sum(1 for r in hp if r[1] == 200), 'of', len(hp),
      '| shell answered', sum(1 for r in sp if r[1] is not None), 'of', len(sp),
      'max shell latency ms', max([round(r[1]*1000) for r in sp if r[1] is not None] or [None]), flush=True)
print('ping over whole window: replies', replies, 'timeouts', timeouts, flush=True)
out = con.collect('kernel thread list', quiet=1.5, timeout=15) or ''
blocks = out.split('\n\n')
for blk in re.split(r'\n(?= \*?0x)', out):
    if 'web_auth_kdf' in blk or 'http_server_tid' in blk or 'web_v1_jobs' in blk:
        print(blk.strip(), flush=True)
con.close()
