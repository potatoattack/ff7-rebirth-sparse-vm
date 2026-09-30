"""Arm MangoHud through a private, already watched config; no key or socket.

The config change happens before warm-up. autostart_log is relative to the
overlay's initialization, so using launcher age + warm-up gives a conservative
lower bound. The completed CSV, not that estimate, establishes the log window.
"""
import math
import os
from pathlib import Path
import re
import stat
import time

NAME = 'MangoHud.conf'
PREFIX = b'# Private FF7 benchmark capture trigger; do not edit during a run.\nautostart_log='

def contents(seconds=0):
    if not isinstance(seconds, int) or not 0 <= seconds < 2**31:
        raise RuntimeError('Invalid MangoHud autostart delay.')
    # Decimal with trailing spaces; leading zeroes would be parsed as octal.
    return PREFIX + str(seconds).ljust(10).encode() + b'\n'

def create(directory):
    path = directory / NAME
    with path.open('xb') as f:
        f.write(contents())
    return path

def watched_config(proc, path):
    st = path.stat()
    device = (os.major(st.st_dev) << 20) | os.minor(st.st_dev)
    result = {'path': str(path), 'inode': st.st_ino, 'device': st.st_dev,
              'kernel_device': device, 'matches': [], 'errors': []}
    for fd in (proc / 'fd').iterdir():
        try:
            if os.readlink(fd) != 'anon_inode:inotify':
                continue
            info = proc / 'fdinfo' / fd.name
            with info.open() as f:
                text = f.read(131073)
            if len(text) > 131072:
                result['errors'].append('Oversized inotify fdinfo: ' + fd.name)
                continue
            for line in text.splitlines():
                if not line.startswith('inotify '):
                    continue
                values = {k: int(v, 16) for k, v in re.findall(r'\b(wd|ino|sdev|mask):([0-9a-fA-F]+)', line)}
                if values.get('ino') == st.st_ino and values.get('sdev') == device and values.get('mask', 0) & 2:
                    result['matches'].append({'fd': fd.name, **values})
        except (OSError, ValueError) as exc:
            result['errors'].append({'fd': fd.name, 'error': str(exc)})
    return result

def verify(proc, env, directory):
    path = directory / NAME
    if env.get('MANGOHUD_CONFIGFILE') != str(path):
        raise RuntimeError('Game is missing its private MangoHud config. Close and relaunch through capture.py.')
    options = env.get('MANGOHUD_CONFIG', '').split(',')
    if 'read_cfg' not in options or any(x.split('=')[0] in ('control', 'autostart_log') for x in options):
        raise RuntimeError('MangoHud environment overrides the private logging trigger.')
    if path.is_symlink() or path.read_bytes() != contents():
        raise RuntimeError('Private logging config was changed or used already. Use a fresh launch.')
    game_path = proc / 'root' / str(path).lstrip('/')
    if not path.samefile(game_path):
        raise RuntimeError('Game and collector see different MangoHud config files.')
    return path, watched_config(proc, path)

def write_trigger(path, previous, seconds):
    # Preserve the inode watched by MangoHud. One pwrite avoids truncate/rewrite
    # and rename sequences that can expose a transient empty config.
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    try:
        st = os.fstat(fd)
        expected = contents(previous)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_size != len(expected):
            raise RuntimeError('Unexpected private MangoHud config type, owner or size.')
        if os.pread(fd, len(expected) + 1, 0) != expected:
            raise RuntimeError('Private MangoHud config changed before arming.')
        value = contents(seconds)
        if os.pwrite(fd, value, 0) != len(value):
            raise RuntimeError('Short write while arming MangoHud.')
    finally:
        os.close(fd)

def schedule(launch_ns, now_ns=None):
    now_ns = time.monotonic_ns() if now_ns is None else now_ns
    age = (now_ns - launch_ns) / 1e9
    if not math.isfinite(age) or not 0 <= age <= 600:
        raise RuntimeError('This capture needs a fresh launch within ten minutes; no rebuild is needed.')
    seconds = math.ceil(age + 32)
    return {'autostart_log': seconds, 'launch_age_seconds': age,
            'scheduled_monotonic_ns': now_ns,
            'earliest_start_monotonic_ns': launch_ns + seconds * 10**9,
            'start_timeout_seconds': seconds + 10,
            'timing_note': 'At least 30 seconds of warm-up; overlay initialization can delay start further. CSV timestamps establish the measured window.'}
