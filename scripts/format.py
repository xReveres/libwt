"""Format first-party sources and check the same file set in CI."""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def files(*patterns):
    return sorted(
        {
            str(path.relative_to(ROOT))
            for pattern in patterns
            for path in ROOT.glob(pattern)
            if path.is_file()
        }
    )


def tool(name):
    executable = shutil.which(name)
    if not executable:
        raise RuntimeError(f"{name} not found; see README.md#code-style")
    return executable


def run(command):
    print(" ".join(command[:3]), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    check = parser.parse_args().check

    clang = tool("clang-format-20")
    version = subprocess.check_output([clang, "--version"], text=True)
    if not re.search(r"version 20(?:\.|\s)", version):
        raise RuntimeError(f"clang-format 20 required, found: {version.strip()}")
    cpp = files(
        "include/**/*.hpp",
        "src/**/*.hpp",
        "src/**/*.cpp",
        "tests/**/*.cpp",
        "examples/**/*.cpp",
        "bindings/**/*.cpp",
    )
    run([clang, "--dry-run", "--Werror", *cpp] if check else [clang, "-i", *cpp])

    ruff = tool("ruff")
    python = files("tests/**/*.py", "scripts/**/*.py")
    run([ruff, "format", *(["--check"] if check else []), *python])

    prettier = tool("prettier")
    web = files(
        "bindings/**/*.js",
        "bindings/**/*.ts",
        "bindings/**/*.json",
        "tests/**/*.js",
        "examples/**/*.js",
        "examples/**/*.html",
        ".github/**/*.yml",
        "CMakePresets.json",
        "package.json",
        ".prettierrc.json",
        "README.md",
        "AGENTS.md",
        "docs/**/*.md",
        "third_party/README.md",
    )
    run([prettier, "--check" if check else "--write", *web])

    cmake = tool("cmake-format")
    cmake_files = files(
        "CMakeLists.txt",
        "bindings/**/CMakeLists.txt",
        "tests/**/CMakeLists.txt",
        "cmake/*.cmake.in",
    )
    run([cmake, "--check" if check else "--in-place", *cmake_files])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
