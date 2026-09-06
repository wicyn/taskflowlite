"""Check changed project sources; manual and initial runs check all tracked sources.

Style changes are validated separately and do not force unrelated source rewrites.
Set CLANG_FORMAT to the local clang-format-18 executable when it is not on PATH.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = {"taskflowlite", "test", "examples", "benchmarks"}
EXTENSIONS = {".cpp", ".hpp", ".h"}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def paths(output):
    return [os.fsdecode(item) for item in output.split(b"\0") if item]


def is_source(name):
    path = Path(name)
    return (
        path.parts[0] in SOURCE_ROOTS
        and path.suffix in EXTENSIONS
        and not name.startswith(("benchmarks/taskflow/", "test/catch2/"))
        and path.name not in {"catch_amalgamated.cpp", "catch_amalgamated.hpp"}
    )


def changed_files(event_name, event):
    if event_name == "pull_request":
        base = event["pull_request"]["base"]["sha"]
        head = event["pull_request"]["head"]["sha"]
        base = git("merge-base", base, head).decode().strip()
    elif event_name == "push" and event.get("before", "").strip("0"):
        base, head = event["before"], event["after"]
        # A force-push can leave the previous commit outside the fetched history.
        available = subprocess.run(
            ["git", "cat-file", "-e", base + "^{commit}"],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if available.returncode:
            return paths(git("ls-files", "-z"))
    else:
        return paths(git("ls-files", "-z"))
    return paths(git("diff", "--name-only", "--diff-filter=ACMR", "-z", base, head, "--"))


def select_format_files(changed, existing):
    return sorted({name for name in changed if name in existing and is_source(name)})


def check(mode, changed, build_dir):
    tracked = paths(git("ls-files", "-z"))
    existing = {name for name in tracked if (ROOT / name).is_file()}
    if mode == "format":
        selected = select_format_files(changed, existing)
        formatter = [os.environ.get("CLANG_FORMAT", "clang-format-18"),
                     "--style=file:" + str(ROOT / ".clang-format")]
        # Validate the configuration even when no C++ source changed.
        # Golden style tests in test_format_style.py guard readability separately.
        subprocess.run([*formatter, "--dump-config"], cwd=ROOT,
                       stdout=subprocess.DEVNULL, check=True)
        command = [*formatter, "--dry-run", "--Werror"]
    else:
        # Headers and build/check configuration can affect multiple translation units.
        full = any(
            Path(name).suffix in {".hpp", ".h", ".cmake", ".yml", ".py"}
            or Path(name).name in {"CMakeLists.txt", ".clang-tidy"}
            for name in changed
        )
        database = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
        units = set()
        for entry in database:
            file = Path(entry["file"])
            if not file.is_absolute():
                file = Path(entry["directory"]) / file
            try:
                name = file.resolve().relative_to(ROOT).as_posix()
            except ValueError:
                continue
            if name in existing and is_source(name) and (full or name in changed):
                units.add(name)
        selected = sorted(units)
        command = [
            "clang-tidy-18", "-p", str(build_dir),
            "--header-filter=" + str(ROOT / "taskflowlite") + "/.*",
            "--checks=-*,bugprone-*,performance-*,modernize-use-nullptr,modernize-use-override",
            "--warnings-as-errors=bugprone-*",
        ]
    print(f"{mode}: {len(selected)} file(s)", flush=True)
    failed = []
    for name in selected:
        print(name, flush=True)
        result = subprocess.run([*command, str(ROOT / name)], cwd=ROOT)
        if result.returncode:
            failed.append(name)
    if failed:
        raise SystemExit(f"{mode}: failed for {len(failed)} file(s): " + ", ".join(failed))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("format", "tidy"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    with open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8") as stream:
        event = json.load(stream)
    check(args.mode, changed_files(os.environ["GITHUB_EVENT_NAME"], event), args.build_dir.resolve())


if __name__ == "__main__":
    main()
