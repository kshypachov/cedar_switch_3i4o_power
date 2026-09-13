import http.client, json, re, subprocess, sys, threading, time
sys.path.insert(0, '/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench')
from bench_console import Console

BOARD = '192.168.88.14'
PASSWORD = 'cedar-bench-P2-changed'

def pct(xs, p):
    s = sorted(xs); rank = max(1, -(-len(s) * p // 100)); return s[int(rank) - 1]

def summary(name, xs, errors=()):
    if xs:
        print(f"{name}: n={len(xs)} p50={pct(xs,50)*1000:.1f} p95={pct(xs,95)*1000:.1f} max={max(xs)*1000:.1f} ms errors={list(errors)}", flush=True)
    else:
        print(f"{name}: no samples, errors={list(errors)}", flush=True)

def baseline_p0(url, extra=()):
    # Exactly the P0 method: 15 sequential curl requests, sorted, 8th and 14th.
    times = []
    for _ in range(15):
        out = subprocess.run(['curl', '-s', '-m', '8', '-o', '/dev/null', '-w', '%{time_total}', *extra, url], capture_output=True, text=True).stdout
        times.append(float(out or 'nan'))
    s = sorted(times)
    print(f"curl {url}: min={s[0]*1000:.1f} p50={s[7]*1000:.1f} p95={s[13]*1000:.1f} max={s[14]*1000:.1f} ms", flush=True)

ping = subprocess.run(['ping', '-c', '20', '-i', '0.3', '-W', '2000', BOARD], capture_output=True, text=True).stdout
print('ping:', ping.strip().splitlines()[-2:], flush=True)

c = http.client.HTTPConnection(BOARD, 80, timeout=30)
c.request('POST', '/api/v1/auth/session', body=json.dumps({'password': PASSWORD}), headers={'Content-Type': 'application/json'})
r = c.getresponse(); r.read(); cookie = r.getheader('Set-Cookie').split(';')[0]; c.close()

baseline_p0(f'http://{BOARD}/')
baseline_p0(f'http://{BOARD}/api/v1/auth/state')
baseline_p0(f'http://{BOARD}/api/v1/system/status', ('-H', f'Cookie: {cookie}'))

def close_mode(n=15):
    xs = []
    for _ in range(n):
        t = time.perf_counter()
        c = http.client.HTTPConnection(BOARD, 80, timeout=30)
        c.request('GET', '/api/v1/system/status', headers={'Cookie': cookie, 'Connection': 'close'})
        r = c.getresponse(); r.read(); c.close()
        xs.append(time.perf_counter() - t)
    return xs
summary('python Connection: close x15', close_mode())

def keepalive(n=50):
    xs, errs = [], []
    c = http.client.HTTPConnection(BOARD, 80, timeout=30)
    for _ in range(n):
        t = time.perf_counter()
        try:
            c.request('GET', '/api/v1/system/status', headers={'Cookie': cookie})
            r = c.getresponse(); r.read()
            xs.append(time.perf_counter() - t)
        except Exception as e:
            errs.append(type(e).__name__)
            c.close(); c = http.client.HTTPConnection(BOARD, 80, timeout=30)
    c.close()
    return xs, errs
xs, errs = keepalive()
summary('keep-alive one connection x50', xs, errs)

def clients(k, mode):
    lat, errs, lock = [], [], threading.Lock()
    def worker():
        if mode == 'keepalive':
            x, e = keepalive(25)
        else:
            x, e = [], []
            for _ in range(25):
                t = time.perf_counter()
                try:
                    out = subprocess.run(['curl', '-s', '-m', '8', '-o', '/dev/null', '-w', '%{http_code}', '-H', f'Cookie: {cookie}', f'http://{BOARD}/api/v1/system/status'], capture_output=True, text=True).stdout
                    if out != '200': e.append(out)
                    else: x.append(time.perf_counter() - t)
                except Exception as ex:
                    e.append(type(ex).__name__)
        with lock:
            lat.extend(x); errs.extend(e)
    th = [threading.Thread(target=worker) for _ in range(k)]
    t0 = time.perf_counter()
    for t in th: t.start()
    for t in th: t.join()
    summary(f'{k} clients {mode} x25 each (wall {time.perf_counter()-t0:.1f}s)', lat, errs)

clients(2, 'keepalive')
clients(4, 'keepalive')
clients(2, 'curl')
clients(4, 'curl')

con = Console('console-latency.log')
time.sleep(0.5)
print(con.collect('net conn', quiet=1.5, timeout=10), flush=True)
con.close()
