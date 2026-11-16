#!/usr/bin/env python3
"""Check propagation diagnostics, compiled modules and formatting semantics."""

import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    _ = parser.add_argument("--interpreter", required=True)
    _ = parser.add_argument("--compiler", default=os.environ.get("NARIC"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    interpreter = Path(args.interpreter).resolve()
    compiler = Path(args.compiler).resolve() if args.compiler else interpreter.with_name("naric" + interpreter.suffix)

    def run(command, error=None, cwd=root):
        result = subprocess.run(
            [str(item) for item in command], cwd=cwd, capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=30,
        )
        if error is None:
            assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
        else:
            assert 0 < result.returncode < 128, (command, result.returncode, result.stdout, result.stderr)
            assert error in result.stdout + result.stderr, (command, result.stdout, result.stderr)
        return result.stdout

    diagnostics = {
        "test_propagation_top_level.nari": "Error: SyntaxError: JSON.parse failed",
        "test_propagation_interpolation_top_level.nari": "Error: boom",
        "test_propagation_wrong_type.nari": "postfix '?' requires a Result",
        "test_propagation_option.nari": "postfix '?' requires a Result",
        "test_propagation_wrong_variant.nari": "postfix '?' requires a Result",
        "test_propagation_return_type.nari": "TypeError",
        "test_propagation_compact_ternary.nari": "Expected ';' or newline",
    }
    for name, message in diagnostics.items():
        _ = run([interpreter, root / "tests/expect_fail" / name], error=message)

    for source in sorted((root / "tests/fmt").glob("*.input.nari")):
        expected = source.with_name(source.name.replace(".input.nari", ".expected.nari"))
        assert run([interpreter, "fmt", source]) == expected.read_text(encoding="utf-8"), source

    with tempfile.TemporaryDirectory(prefix="nari-propagation-") as temporary:
        directory = Path(temporary)
        crlf = directory / "crlf.nari"
        crlf.write_bytes(b"// comment\r\nfunc load(result) { return Ok(result?); }\r\n/* first\r\nsecond */\r\n")
        first_format = run([interpreter, "fmt", crlf])
        crlf.write_text(first_format, encoding="utf-8")
        assert run([interpreter, "fmt", crlf]) == first_format, "CRLF comments changed on repeated formatting"
        formatted_root = directory / "formatted/tests"
        (formatted_root / "expect_pass").mkdir(parents=True)
        (formatted_root / "lib").mkdir()
        _ = shutil.copyfile(root / "tests/lib/assert.nari", formatted_root / "lib/assert.nari")
        for name in ("test_error_propagation.nari", "test_error_propagation_strict.nari"):
            source = root / "tests/expect_pass" / name
            compiled = directory / (source.stem + ".naric")
            _ = run([compiler, "-o", compiled, source])
            _ = run([interpreter, compiled], cwd=directory)
            formatted = formatted_root / "expect_pass" / name
            _ = formatted.write_text(run([interpreter, "fmt", source]), encoding="utf-8")
            expected = formatted.read_text(encoding="utf-8")
            actual = run([interpreter, "fmt", formatted])
            assert actual == expected, "".join(difflib.unified_diff(
                expected.splitlines(keepends=True), actual.splitlines(keepends=True),
                fromfile=name, tofile="formatted again",
            ))
            _ = run([interpreter, formatted], cwd=directory)

        module_source = directory / "module.nari"
        module_bytecode = directory / "module.naric"
        _ = module_source.write_text(
            'func forward(result) { return Ok(result? + 1); }\n', encoding="utf-8"
        )
        _ = run([compiler, "--no-stdlib", "-o", module_bytecode, module_source])
        importer = directory / "importer.nari"
        _ = importer.write_text(
            f'import {json.dumps(module_bytecode.as_posix())};\n' +
            'if (forward(Ok(41)).unwrap() != 42) { panic("compiled module success failed"); }\n' +
            'if (forward(Err("module")).unwrap_err() != "module") { panic("compiled module Err failed"); }\n',
            encoding="utf-8",
        )
        _ = run([interpreter, importer], cwd=directory)

    print("error propagation diagnostics, bytecode and formatter checks passed")


if __name__ == "__main__":
    main()
