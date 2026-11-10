#!/usr/bin/env python3
"""Check portable package imports use npkg's complete SHA-256 store layout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def make_package(directory, value):
    directory.mkdir(parents=True)
    (directory / "nari.toml").write_text(
        'packageFormat = 1\nname = "fixture/math"\nversion = "1.0.0"\n'
        '[exports]\n"." = "value.nari"\n', encoding="utf-8"
    )
    (directory / "value.nari").write_text(
        f"export func answer() {{ return {value}; }}\n", encoding="utf-8"
    )


def write_lock(project, integrity, recorded_path):
    (project / "nari.lock.toml").write_text(
        'lockfileVersion = 1\nroot = "app@1.0.0"\n'
        '[packages."fixture/math"]\nversion = "1.0.0"\n'
        f'integrity = {json.dumps(integrity)}\n'
        f'storePath = {json.dumps(recorded_path.as_posix())}\n', encoding="utf-8"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", required=True)
    args = parser.parse_args()
    interpreter = str(Path(args.interpreter).resolve())

    with tempfile.TemporaryDirectory(prefix="nari-package-store-") as temporary:
        root = Path(temporary)
        project = root / "project"
        project.mkdir()
        (project / "nari.toml").write_text(
            'packageFormat = 1\nname = "app"\nversion = "1.0.0"\n'
            'entry = "main.nari"\n[dependencies]\n'
            '"fixture/math" = { version = "1.0.0" }\n', encoding="utf-8"
        )
        (project / "main.nari").write_text(
            'import math from "fixture/math";\nprint(math.answer());\n', encoding="utf-8"
        )
        home = root / "nari-home"
        store = home / "store" / "pkg"
        digest = hashlib.sha256(b"package store fixture").hexdigest()
        installed = store / f"fixture-math@1.0.0-{digest}"
        make_package(installed, 42)
        missing = root / "another-machine" / "package"
        environment = dict(os.environ, NARI_HOME=str(home))

        def run(expected=None):
            result = subprocess.run(
                [interpreter, "main.nari"], cwd=project, env=environment,
                capture_output=True, text=True, timeout=20,
            )
            if expected is None:
                assert 0 < result.returncode < 128, (result.returncode, result.stdout, result.stderr)
            else:
                assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
                assert result.stdout.strip() == str(expected), result.stdout

        write_lock(project, "sha256-" + digest, missing)
        run(42)
        write_lock(project, "sha256-" + digest.upper(), missing)
        run(42)

        shortened = store / f"fixture-math@1.0.0-{digest[:8]}"
        installed.rename(shortened)
        write_lock(project, "sha256-" + digest, missing)
        run()
        write_lock(project, "sha256-" + digest[:8], missing)
        run()
        make_package(installed, 42)
        make_package(store / ("fixture-math@1.0.0-" + "g" * 64), 126)
        write_lock(project, "sha256-" + "g" * 64, missing)
        run()
        write_lock(project, "fnv1a-" + digest, missing)
        run()

        recorded = root / "recorded-package"
        make_package(recorded, 84)
        write_lock(project, "sha256-" + digest, recorded)
        run(84)

    print("package store resolution checks passed")


if __name__ == "__main__":
    main()
