import http.client, json, sys, time
sys.path.insert(0, '/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench')
from bench_console import Console

def state():
    t0 = time.perf_counter()
    try:
        c = http.client.HTTPConnection('192.168.88.14', 80, timeout=8)
        c.request('GET', '/api/v1/auth/state', headers={'Connection': 'close'})
        r = c.getresponse(); body = r.read(); c.close()
        return r.status, body.decode(), round((time.perf_counter() - t0) * 1000)
    except Exception as e:
        return type(e).__name__, str(e), round((time.perf_counter() - t0) * 1000)

print('state:', state(), flush=True)
con = Console('diag-console.log')
time.sleep(1.0)
print('--- tail since open:', flush=True)
for cmd in ('kernel uptime', 'kernel thread list'):
    out = con.collect(cmd, quiet=1.5, timeout=15)
    print(f'--- {cmd}\n{out}', flush=True)
print('state again:', state(), flush=True)
con.close()
