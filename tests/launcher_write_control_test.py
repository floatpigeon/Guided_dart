"""Exercise the real launcher with a console producer and an external pause."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def until(predicate):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('launcher did not reach the requested state')


def exercise(launcher, detached):
    with tempfile.TemporaryDirectory(prefix='dart-launcher-write-') as directory:
        root = Path(directory)
        pause, stop, progress = (root / name for name in ('write.pause', 'stop', 'progress'))
        producer = root / 'producer.py'
        producer.write_text('''import os, pathlib, sys, time
root = pathlib.Path(sys.argv[1])
i = 0
while not (root / 'stop').exists():
    print(str(i) + 'x' * 4096, flush=True)
    (root / 'progress').write_text(str(i))
    i += 1
    time.sleep(0.002)
''')
        startup = root / 'startup.list'
        startup.write_text(f'{sys.executable} {producer} {root} &\n')
        args = [launcher, '--poll-ms', '20', '--heartbeat', '1', '--log-dir', str(root / 'logs')]
        if detached:
            args.append('-n')
        args.append(str(startup))
        proc = subprocess.Popen(args, env=dict(os.environ, DART_WRITE_PAUSE=str(pause)),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        console = root / 'logs' / (Path(sys.executable).name + '.console.log')
        try:
            until(lambda: console.exists() and console.stat().st_size > 0 and progress.exists())
            if detached:
                assert proc.wait(timeout=2) == 0
            pause.touch()
            time.sleep(0.15)
            old_progress = progress.read_text()
            console.unlink()
            launcher_log = root / 'logs/launcher.log'
            launcher_log.unlink(missing_ok=True)
            time.sleep(0.2)
            assert not console.exists() and not launcher_log.exists()
            until(lambda: progress.read_text() != old_progress)
            pause.unlink()
            until(lambda: console.exists() and console.stat().st_size > 0)
            if not detached:
                until(launcher_log.exists)
        finally:
            stop.touch()
            if proc.poll() is None:
                try:
                    proc.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    proc.wait(timeout=2)
            time.sleep(0.1)  # Let the detached collector consume EOF before removing its directory.


if __name__ == '__main__':
    for detached in (False, True):
        exercise(sys.argv[1], detached)
    print('launcher pause, resume and console consumption passed')
