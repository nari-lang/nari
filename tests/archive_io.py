#!/usr/bin/env python3
"""Check sparse archive extraction limits and file write errors."""

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def sparse_tar(path, entries):
    # Old GNU sparse headers keep a 64-bit logical size separately from the
    # few stored bytes, so tests do not need gigabytes of archive data.
    with path.open("wb") as archive:
        for name, offset, size, data in entries:
            entry = tarfile.TarInfo(name)
            entry.type = tarfile.GNUTYPE_SPARSE
            entry.size = len(data)
            header = bytearray(entry.tobuf(format=tarfile.GNU_FORMAT))
            header[386:398] = tarfile.itn(offset, 12, tarfile.GNU_FORMAT)
            header[398:410] = tarfile.itn(len(data), 12, tarfile.GNU_FORMAT)
            header[483:495] = tarfile.itn(size, 12, tarfile.GNU_FORMAT)
            header[148:156] = b" " * 8
            header[148:156] = f"{sum(header):06o}\0 ".encode("ascii")
            archive.write(header)
            archive.write(data)
            archive.write(b"\0" * (-len(data) % 512))
        archive.write(b"\0" * 1024)


def extract(interpreter, root, archive, destination, maximum, before_exec=None):
    script = root / "extract.nari"
    script.write_text(
        'import { Archive } from "std/archive";\n'
        f'let r = Archive.extract({json.dumps(str(archive))}, {json.dumps(str(destination))}, '
        f'{{max_bytes: {maximum}}});\nprint(r.is_ok());\n', encoding="utf-8")
    result = subprocess.run([interpreter, str(script)], capture_output=True,
                            text=True, timeout=20, preexec_fn=before_exec)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert result.stdout.strip() in ("true", "false"), result.stdout
    return result.stdout.strip() == "true"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", required=True)
    args = parser.parse_args()
    interpreter = args.interpreter

    with tempfile.TemporaryDirectory(prefix="nari-archive-io-") as temporary:
        root = Path(temporary)
        archive = root / "sparse.tar"
        sparse_tar(archive, [("holes.bin", 4096, 8192, b"proof")])
        assert extract(interpreter, root, archive, root / "small", 8192)
        output = root / "small" / "holes.bin"
        assert output.stat().st_size == 8192, "trailing sparse hole was lost"
        with output.open("rb") as stream:
            assert stream.read(16) == b"\0" * 16
            stream.seek(4096)
            assert stream.read(5) == b"proof"
        assert not extract(interpreter, root, archive, root / "limited", 4096), "sparse file bypassed max_bytes"

        sparse_tar(archive, [("one.bin", 4096, 8192, b"a"), ("two.bin", 4096, 8192, b"b")])
        assert not extract(interpreter, root, archive, root / "aggregate", 12000), "sparse files bypassed aggregate max_bytes"

        offset = (1 << 31) + 4096
        sparse_tar(archive, [("large.bin", offset, offset + 5, b"proof")])
        assert extract(interpreter, root, archive, root / "large", offset + 5), "64-bit sparse offset failed"
        output = root / "large" / "large.bin"
        assert output.stat().st_size == offset + 5
        with output.open("rb") as stream:
            stream.seek(offset)
            assert stream.read(5) == b"proof", "64-bit offset was truncated"

        if os.name == "posix" and os.environ.get("NARI_WINDOWS_TEST") != "1":
            import resource
            import signal

            def limit_file_size():
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))

            with tarfile.open(archive, "w") as output_archive:
                entry = tarfile.TarInfo("buffered.bin")
                entry.size = 2000
                output_archive.addfile(entry, io.BytesIO(b"x" * 2000))
            assert not extract(interpreter, root, archive, root / "full", 4000, limit_file_size), "buffered write failure was ignored"

    print("archive I/O checks passed")


if __name__ == "__main__":
    main()
