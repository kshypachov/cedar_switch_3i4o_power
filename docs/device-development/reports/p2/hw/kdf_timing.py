import http.client, json, threading, time

BOARD = '192.168.88.14'

def call(method, path, body=None, timeout=10):
    t0 = time.perf_counter()
    c = http.client.HTTPConnection(BOARD, 80, timeout=timeout)
    data = json.dumps(body).encode() if body is not None else None
    hdrs = {'Connection': 'close'}
    if data: hdrs['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=hdrs)
    r = c.getresponse(); b = r.read(); c.close()
    return r.status, b, time.perf_counter() - t0

probes = []
stop = threading.Event()
def prober():
    while not stop.is_set():
        t = time.perf_counter()
        try:
            s, _, dt = call('GET', '/api/v1/auth/state', timeout=3)
            probes.append((t, s, dt))
        except Exception as e:
            probes.append((t, type(e).__name__, None))
        time.sleep(1.0)

print('baseline GET:', [round(call('GET', '/api/v1/auth/state')[2] * 1000) for _ in range(3)], 'ms', flush=True)
th = threading.Thread(target=prober); th.start()
time.sleep(2.5)
t_login = time.perf_counter()
try:
    s, b, dt = call('POST', '/api/v1/auth/session', {'password': 'cedar-bench-P2-password'}, timeout=180)
    print('login:', s, json.loads(b).get('username', b[:60]), f'{dt:.1f} s', flush=True)
except Exception as e:
    dt = time.perf_counter() - t_login
    print('login failed:', type(e).__name__, f'{dt:.1f} s', flush=True)
time.sleep(2.5)
stop.set(); th.join()
t_end = t_login + dt
during = [p for p in probes if t_login <= p[0] <= t_end]
print('probes during login:', len(during), 'ok:', sum(1 for p in during if p[1] == 200),
      'failed:', sorted({str(p[1]) for p in during if p[1] != 200}), flush=True)
print('probes before/after ok:', sum(1 for p in probes if not (t_login <= p[0] <= t_end) and p[1] == 200),
      'of', sum(1 for p in probes if not (t_login <= p[0] <= t_end)), flush=True)
