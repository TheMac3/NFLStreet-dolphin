#!/usr/bin/env python3
"""Build/run the synthetic HD cache metadata test using an existing Linux build.

Usage: test_hires_cache.py --build <private-cmake-build> --out <local/test-dir>
The real production resource manager is linked with section garbage collection;
renderer-dependent methods are excluded, and no game or GPU is needed.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    """Reuse production includes/defines, keeping generated objects under local/."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if "local" not in out.parts:
        parser.error("--out must be under a private local/ directory")
    out.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    commands = json.loads((args.build / "compile_commands.json").read_text())
    reference = next(row for row in commands if row["file"].endswith("/CustomResourceManager.cpp"))
    objects = []
    for name in ("Source/Core/VideoCommon/Assets/CustomResourceManager.cpp",
                 "Source/Core/VideoCommon/Assets/CustomAsset.cpp", "Tools/NSR/NSRRollbackNetplayTest.cpp"):
        command = shlex.split(reference["command"])
        index = command.index("-o")
        del command[index:index + 2]
        command.remove("-c")
        command.remove(reference["file"])
        obj = out / (Path(name).stem + ".o")
        objects.append(str(obj))
        command += ["-ffunction-sections", "-fdata-sections", "-UNDEBUG", "-c", str(root / name), "-o", str(obj)]
        if name.endswith("Test.cpp"):
            command += ["-Werror", "-DNSR_HIRES_CACHE_TEST"]
        subprocess.run(command, cwd=reference["directory"], check=True)
    binary = out / "hires-cache-test"
    subprocess.run(["g++", "-Wl,--gc-sections", *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
