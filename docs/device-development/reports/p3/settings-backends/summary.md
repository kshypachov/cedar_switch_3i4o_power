## Summary

| Backend | wipe | clean boot Matter init, s | read back | commissionings ok | Matter init before commissioning, s (first / last / max) | Matter init with fabric, s (first / last / max) | commissioning, s (median / max) | fabric removal, s (median / max) | persistence |
|---|---|---|---|---|---|---|---|---|---|
| file | ok | 2.3 | yes | 9/10 | 3.2 / 19.0 / 49.4 | 8.2 / 48.0 / 50.8 | 64.8 / 75.0 | 29.7 / 60.8 | 23 counted / 23 boots |
| zms | ok | 0.6 | yes | 9/10 | 0.6 / 6.2 / 6.2 | 0.7 / 7.4 / 7.4 | 54.0 / 63.1 | 2.0 / 6.7 | 23 counted / 23 boots |
| nvs | ok | 0.8 | yes | 10/10 | 0.7 / 10.4 / 10.4 | 2.0 / 12.6 / 12.6 | 54.0 / 59.2 | 8.3 / 17.6 | 23 counted / 23 boots |
| fcb | ok | 3.1 | yes | 1/10 | 6.2 / 123.3 / 123.3 | 49.6 / 127.2 / 127.2 | 54.4 / 54.4 | 14.9 / 71.3 | 24 counted / 23 boots |

## file: per cycle

| cycle | Matter init before, s | window open, s | commissioning | Matter init with fabric, s | removal, s |
|---|---|---|---|---|---|
| 1 | 3.2 | 1.0 | ok 57.8 | 8.2 | 13.7 |
| 2 | 9.4 | 2.8 | ok 64.0 | 21.0 | 29.7 |
| 3 | 19.0 | 4.8 | ok 74.3 | 48.0 | — |
| 4 | 49.4 | — | FAILED None window | 50.8 | 52.6 |
| 5 | 3.9 | 1.5 | ok 58.2 | 9.8 | 14.6 |
| 6 | 14.5 | 4.0 | ok 69.5 | 23.5 | 33.4 |
| 7 | 21.1 | 5.3 | ok 75.0 | 40.6 | 51.2 |
| 8 | 10.1 | 1.1 | ok 57.5 | 8.6 | 14.1 |
| 9 | 9.3 | 2.9 | ok 64.8 | 20.5 | 27.7 |
| 10 | 19.0 | 4.7 | ok 72.5 | 48.0 | 60.8 |

storage_bench after the cycles:
```

```

storage warnings at boot (2), first: `uart:~$ uart:~$ [00:00:01.476,000] <err> littlefs: WEST_TOPDIR/modules/fs/littlefs/lfs.c:1386: Corrupted dir pair at {0x0, 0x1}`

## zms: per cycle

| cycle | Matter init before, s | window open, s | commissioning | Matter init with fabric, s | removal, s |
|---|---|---|---|---|---|
| 1 | 0.6 | — | FAILED None window | 0.7 | 1.1 |
| 2 | 0.7 | 1.4 | ok 47.5 | 1.3 | 1.6 |
| 3 | 1.3 | 2.5 | ok 47.8 | 1.7 | 1.5 |
| 4 | 1.4 | 3.4 | ok 49.1 | 2.4 | 1.8 |
| 5 | 2.1 | 5.0 | ok 51.3 | 3.1 | 1.6 |
| 6 | 2.7 | 7.0 | ok 54.0 | 3.9 | 2.3 |
| 7 | 3.7 | 8.9 | ok 56.0 | 4.4 | 3.3 |
| 8 | 4.5 | 10.3 | ok 58.0 | 5.5 | 3.4 |
| 9 | 4.9 | 11.2 | ok 59.3 | 6.7 | 4.8 |
| 10 | 6.2 | 14.6 | ok 63.1 | 7.4 | 6.7 |

storage_bench after the cycles:
```

```

## nvs: per cycle

| cycle | Matter init before, s | window open, s | commissioning | Matter init with fabric, s | removal, s |
|---|---|---|---|---|---|
| 1 | 0.7 | 1.3 | ok 49.5 | 2.0 | 17.6 |
| 2 | 1.5 | 1.5 | ok 50.4 | 3.0 | 3.5 |
| 3 | 2.9 | 1.8 | ok 51.5 | 4.2 | 4.5 |
| 4 | 4.0 | 2.0 | ok 52.4 | 5.3 | 5.6 |
| 5 | 5.0 | 2.3 | ok 53.5 | 6.7 | 6.7 |
| 6 | 6.2 | 2.5 | ok 54.4 | 7.9 | 7.7 |
| 7 | 7.2 | 2.8 | ok 55.6 | 9.0 | 8.8 |
| 8 | 8.2 | 3.0 | ok 57.1 | 10.2 | 9.9 |
| 9 | 9.3 | 3.2 | ok 58.2 | 11.4 | 11.0 |
| 10 | 10.4 | 3.5 | ok 59.2 | 12.6 | 12.1 |

storage_bench after the cycles:
```
settings backend: 11 entries, 3 rounds (best / mean, us)
scan (no match)    176300 /   177933
scan (all)         176200 /   176200
load_one (hit)     176400 /   176533  -> 0
load_one (miss)    176400 /   176466  -> 0
val_len (hit)      176300 /   176300  -> 0
val_len (miss)     176300 /   176533  -> 0
```

## fcb: per cycle

| cycle | Matter init before, s | window open, s | commissioning | Matter init with fabric, s | removal, s |
|---|---|---|---|---|---|
| 1 | 6.2 | 2.6 | ok 54.4 | 49.6 | 71.3 |
| 2 | 54.8 | 15.4 | FAILED 120.2  | 66.6 | 19.6 |
| 3 | 70.2 | 20.3 | FAILED 10.7 [1789304526.488] [41567:11431588:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 74.0 | 9.7 |
| 4 | 77.8 | 22.6 | FAILED 10.7 [1789304738.101] [41767:11434254:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 81.6 | 10.9 |
| 5 | 85.3 | 24.9 | FAILED 10.7 [1789304968.904] [41834:11436205:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 89.2 | 12.0 |
| 6 | 92.8 | 27.4 | FAILED 10.7 [1789305219.025] [41907:11438492:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 96.7 | 13.2 |
| 7 | 100.4 | 29.6 | FAILED 10.7 [1789305488.119] [41989:11440654:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 104.2 | 14.4 |
| 8 | 107.9 | 32.0 | FAILED 10.7 [1789305806.426] [42095:11442987:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 112.1 | 15.5 |
| 9 | 115.9 | 34.4 | FAILED 10.7 [1789306114.883] [42379:11447985:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 119.2 | 22.4 |
| 10 | 123.3 | 36.7 | FAILED 10.7 [1789306447.661] [42633:11452848:chip] [TOO] Pairing Failure: src/protocols/secure_channel/PASESession.cpp:312: CHIP Error 0x00000032: Timeout | 127.2 | 18.0 |

storage_bench after the cycles:
```
settings backend: 16 entries, 3 rounds (best / mean, us)
scan (no match)   2404800 /  2407966
scan (all)        2405000 /  2407633
load_one (hit)    2401100 /  2404466  -> 0
load_one (miss)   2405000 /  2408200  -> 0
val_len (hit)     2401100 /  2402433  -> 0
val_len (miss)    2405400 /  2407100  -> 0
```
