#!/usr/bin/env python3
"""Build the isolated RT-Smart probe using paths in build-cross/CMakeCache.txt."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    here = Path(__file__).resolve().parent
    repo = here.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=repo / "build-cross/CMakeCache.txt")
    parser.add_argument("--output", type=Path, default=repo / "build/sc038hgs_raw")
    args = parser.parse_args()
    cache = {}
    for line in args.cache.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    sdk = Path(cache["K230_SDK_ROOT"])
    mpp = sdk / "src/rtsmart/mpp"
    abi_path = repo / "include/firmware/src/camera/k230/mcm_abi.json"
    for relative, expected in json.loads(abi_path.read_text()).items():
        if sha256(mpp / relative) != expected:
            raise SystemExit(f"{relative} changed: review MCM ABI/semantics before updating {abi_path}")
    prefix = str(Path(cache["K230_TOOLCHAIN_DIR"]) / "riscv64-unknown-linux-musl-")
    libs = sorted((mpp / "userapps/lib").glob("lib*.a"))
    if not libs:
        raise SystemExit("No SDK MPP static libraries found")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-march=rv64imafdcv", "-mabi=lp64d", "-mcmodel=medany", "-Os",
             "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
             "-fdata-sections", "-include", str(sdk / "include/generated/autoconf.h")]
    for path in (sdk / "include", mpp / "include", mpp / "include/comm",
                 mpp / "include/ioctl", mpp / "userapps/api", mpp / "userapps/api/framework"):
        flags += ["-I", str(path)]
    obj = output / "capture.o"
    elf = output / "sc038hgs_raw.elf"
    compile_command = [prefix + "gcc", *flags, "-c", str(here / "capture.c"), "-o", str(obj)]
    link_command = [prefix + "g++", "-march=rv64imafdcv", "-mabi=lp64d", "-mcmodel=medany",
                    str(obj), "-static", "-T", cache["K230_LINK_SCRIPT"],
                    "-Wl,--gc-sections", "-Wl,--wrap=ioctl", "-Wl,--start-group",
                    *map(str, libs), "-Wl,--end-group", "-lpthread", "-lm", "-o", str(elf)]
    subprocess.run(compile_command, check=True)
    subprocess.run(link_command, check=True)
    manifest = {"status": "cross-compiled; hardware capture and packing not verified",
                "mcm_abi_sha256": sha256(abi_path),
                "source_sha256": sha256(here / "capture.c"), "elf_sha256": sha256(elf),
                "sdk_vicap_sha256": sha256(mpp / "userapps/lib/libvicap.a"),
                "kernel_vicap_sha256": sha256(mpp / "kernel/lib/libvicap.a"),
                "compile": compile_command, "link": link_command}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(elf)


if __name__ == "__main__":
    main()
