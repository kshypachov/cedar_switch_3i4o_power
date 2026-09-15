"""make_queue.py <out queue> : bases first, then trials, a base repeat every ~12 trials per backend.
Only images that built and passed the .check are queued; numbering continues from existing trial dirs."""
import sys
from pathlib import Path

S = Path(__file__).resolve().parent
T = Path("/Volumes/Programming/Zephyr/zephyr_latest/cedar_switch_3in4out_power/docs/device-development/reports/p3/tuning")
C = T.parent / "settings-backends/configs"
ORDER = {
    "zms": ["cache-512", "cache-1024", "cache-2048", "cache-4096", "cache-128", "sectors-16", "sectors-8", "sectors-4",
            "sectors-64", "no_ll_delete-y", "lookup_cache-n", "cache_for_settings-n", "ll_cache-n", "load_subtree_path-n",
            "sector_mult-2", "collision_bits-2", "block_size-64", "block_size-256", "no_double_write-y",
            "spi_freq-40M", "fast_read-off", "spi_interrupt-y", "nor_sleep_wait-n", "nor_sleep_erase-5"],
    "file": ["max_lines-64", "max_lines-96", "max_lines-48", "max_lines-256", "read_size-16", "read_size-32", "read_size-64",
             "read_size-1024", "prog_size-16", "prog_size-64", "cache_size-256", "cache_size-512", "cache_size-4096",
             "lookahead-32", "lookahead-1024", "block_cycles-100", "block_cycles-off", "partition-1M", "fc_heap-16384",
             "num_files-4", "spi_freq-40M", "fast_read-off", "spi_interrupt-y", "nor_sleep_wait-n", "nor_sleep_erase-5"],
}
BASE_FILES = {"zms": [C / "zms.conf", C / "settings-on-storage-lfs.overlay"], "file": [C / "file.conf"]}
BASE_BUILD = {"zms": S / "builds/zms-base2", "file": S / "builds/file-base2"}
BASES_WANTED = 3
REPEAT_EVERY = 12


def trial_file(backend, name):
    for sub in (backend, "shared"):
        for ext in (".conf", ".overlay"):
            p = S / "trials" / sub / (name + ext)
            if p.exists():
                return p
    return None


def built(backend, name):
    b = S / "builds" / f"{backend}-{name}"
    chk = Path(str(b) + ".check")
    return (b / "cedar_switch_3in4out_power/zephyr/zephyr.signed.bin").exists() and chk.exists() and "NOT APPLIED" not in chk.read_text()


lines = []
for backend in ("zms", "file"):
    root = T / backend
    existing = sorted(p.name for p in root.iterdir() if p.is_dir()) if root.exists() else []
    n = len(existing)
    bases = sum(1 for e in existing if e.endswith("-base"))

    def add(name, kind, build, change, files):
        global n
        n += 1
        lines.append("|".join([backend, f"{n:03d}-{name}", kind, str(build), change, " ".join(map(str, files))]))

    while bases < BASES_WANTED:
        add("base", "base", BASE_BUILD[backend], "", BASE_FILES[backend])
        bases += 1
    since_base = 0
    for name in ORDER[backend]:
        f = trial_file(backend, name)
        if f is None or not built(backend, name):
            print(f"skip {backend} {name}: not built", file=sys.stderr)
            continue
        change = " ".join(l.strip().rstrip(";") for l in f.read_text().splitlines()
                          if l.strip().startswith("CONFIG_") or " = " in l or "delete-property" in l)
        add(name, "trial", S / "builds" / f"{backend}-{name}", change, [f])
        since_base += 1
        if since_base == REPEAT_EVERY:
            add("base", "base", BASE_BUILD[backend], "", BASE_FILES[backend])
            since_base = 0
    if since_base:
        add("base", "base", BASE_BUILD[backend], "", BASE_FILES[backend])
Path(sys.argv[1]).write_text("\n".join(lines) + "\n")
print("\n".join(lines))
