import re, sys, time
sys.path.insert(0, '/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/tests/bench')
from bench_console import Console

out = sys.argv[1]
c = Console(out + '/console.log')
since = c.mark()
t0 = time.time()
banner = c.wait_for(r'Starting bootloader|MCUboot|Booting Zephyr', since, 400)
print('banner:', banner and f'+{banner[0]-t0:.1f}s', banner and banner[1].group(0), flush=True)
main = c.wait_for(r'Start main app', since, 400)
print('main:', main and f'+{main[0]-t0:.1f}s', flush=True)
web = c.wait_for(r'web interface', since, 400)
print('web:', web and f'+{web[0]-t0:.1f}s', web and c.lines_since(web[2])[0][1], flush=True)
time.sleep(40)
lines = c.lines_since(since)
print('--- errors/warnings')
for _, l in lines:
    if re.search(r'FATAL|[Ff]ault|<err>|<wrn>|ASSERT|panic|web_|registered|setup is open', l):
        print(l)
print('--- total lines', len(lines), 'reopens', c.reopen_count)
c.close()
