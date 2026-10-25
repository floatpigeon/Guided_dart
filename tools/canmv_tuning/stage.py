#!/usr/bin/env python3
"""Back up the board interpreter and upload a verified replacement via GIO/MTP.

Only stages files: the running interpreter and its startup path stay intact.
Pass the storage-root URI containing boot.py, main.py and micropython.
"""

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def local_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(256 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def remote_hash(uri):
    digest = hashlib.sha256()
    with subprocess.Popen(["gio", "cat", uri], stdout=subprocess.PIPE) as process:
        for chunk in iter(lambda: process.stdout.read(256 * 1024), b""):
            digest.update(chunk)
        if process.wait() != 0:
            raise RuntimeError("Cannot read back " + uri)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("uri", help="MTP/GPhoto2 URI of the /sdcard storage root")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/canmv-tuning")
    args = parser.parse_args()
    if not args.uri.startswith(("gphoto2://", "mtp://")):
        parser.error("Expected a GIO MTP/GPhoto2 storage URI")
    build = args.build_dir.resolve()
    artifact = build / "micropython"
    metadata = json.loads((build / "BUILD_INFO.json").read_text())
    expected = local_hash(artifact)
    if expected != metadata["sha256"]:
        raise SystemExit("Interpreter differs from BUILD_INFO.json; rebuild first")
    uri = args.uri.rstrip("/")
    # Reject an incorrect storage root before uploading anything.
    for name in ("micropython", "boot.py", "main.py"):
        subprocess.run(["gio", "info", uri + "/" + name], check=True,
                       stdout=subprocess.DEVNULL)
    token = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    backup = build / "board-backups" / token
    backup.mkdir(parents=True)
    subprocess.run(["gio", "copy", "-T", uri + "/micropython",
                    str(backup / "micropython")], check=True)
    old_hash = local_hash(backup / "micropython")
    if old_hash != remote_hash(uri + "/micropython"):
        raise RuntimeError("Original interpreter backup failed readback verification")
    print("Original interpreter backed up:", backup, flush=True)
    stage_name = "micropython.dart-raw-" + token + ".new"
    subprocess.run(["gio", "copy", "-T", str(artifact), uri + "/" + stage_name],
                   check=True)
    if expected != remote_hash(uri + "/" + stage_name):
        raise RuntimeError("Uploaded interpreter failed readback verification")
    subprocess.run(["gio", "copy", "-T", str(build / "canmv_tune.py"),
                    uri + "/canmv_tune-" + token + ".py"], check=True)
    result = {
        "stage": "/sdcard/" + stage_name,
        "backup": str(backup / "micropython"),
        "old_sha256": old_hash,
        "sha256": expected,
        "board_previous": "/sdcard/micropython.before-dart-raw-" + token,
    }
    (build / "STAGED.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    print("Staged and verified. Activate by rename, then hard reboot; see README.")


if __name__ == "__main__":
    main()
