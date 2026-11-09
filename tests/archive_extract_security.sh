#!/usr/bin/env bash
# Archive extraction must not create paths outside its destination, and opted-in
# tar hardlinks must refer to regular files inside the destination.
set -euo pipefail

INTERP="${INTERP:-build/release/nari}"
python3 - "$INTERP" <<'PY'
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import zipfile

interpreter = sys.argv[1]

def add_file(tar, name, data=b"proof"):
    entry = tarfile.TarInfo(name)
    entry.size = len(data)
    tar.addfile(entry, io.BytesIO(data))

def add_link(tar, name, target, kind):
    entry = tarfile.TarInfo(name)
    entry.type = kind
    entry.linkname = str(target)
    tar.addfile(entry)

def extract(root, archive, dest, allow_links=False):
    script = root / "extract.nari"
    options = ", {allow_links: true}" if allow_links else ""
    script.write_text(
        'import { Archive } from "std/archive";\n'
        f'let result = Archive.extract("{archive}", "{dest}"{options});\n'
        'print(result.is_ok());\n'
    )
    run = subprocess.run([interpreter, str(script)], capture_output=True, text=True, timeout=10)
    if run.returncode != 0 or run.stdout.strip() not in ("true", "false"):
        raise AssertionError(f"unexpected interpreter result: {run.returncode} {run.stdout} {run.stderr}")
    return run.stdout.strip() == "true"

with tempfile.TemporaryDirectory(prefix="nari-archive-security-") as temp:
    root = pathlib.Path(temp)
    outside = root / "outside"
    outside.mkdir()

    # An existing destination symlink must not be traversed, even when
    # allow_links is false and the archive contains only a regular file.
    dest = root / "existing-link"
    dest.mkdir()
    (dest / "jump").symlink_to(outside, target_is_directory=True)
    archive = root / "existing-link.tar"
    with tarfile.open(archive, "w") as tar:
        add_file(tar, "jump/created/proof.txt")
    assert not extract(root, archive, dest)
    assert not (outside / "created").exists(), "created a directory outside dest through an existing symlink"

    # An opted-in archive symlink must not be traversed by later entries.
    dest = root / "archive-link"
    archive = root / "archive-link.tar"
    with tarfile.open(archive, "w") as tar:
        add_link(tar, "jump", outside, tarfile.SYMTYPE)
        add_file(tar, "jump/created/proof.txt")
    assert not extract(root, archive, dest, allow_links=True)
    assert not (outside / "created").exists(), "created a directory outside dest through an archive symlink"

    # Tar hardlinks should work when explicitly allowed and confined to dest.
    dest = root / "hardlinks"
    archive = root / "hardlinks.tar"
    with tarfile.open(archive, "w") as tar:
        add_file(tar, "original.txt", b"original data")
        add_link(tar, "copy.txt", "original.txt", tarfile.LNKTYPE)
    assert not extract(root, archive, root / "hardlinks-disabled")
    assert extract(root, archive, dest, allow_links=True)
    assert (dest / "copy.txt").read_bytes() == b"original data"
    assert (dest / "copy.txt").samefile(dest / "original.txt")

    # A hardlink target cannot escape the extraction root.
    archive = root / "outside-hardlink.tar"
    with tarfile.open(archive, "w") as tar:
        add_link(tar, "copy.txt", "../outside/secret.txt", tarfile.LNKTYPE)
    assert not extract(root, archive, root / "outside-hardlink", allow_links=True)

    # A hardlink may refer only to a file from this archive, not to a file
    # already present in the extraction destination.
    dest = root / "preexisting-target"
    dest.mkdir()
    (dest / "private.txt").write_text("keep")
    archive = root / "preexisting-target.tar"
    with tarfile.open(archive, "w") as tar:
        add_link(tar, "copy.txt", "private.txt", tarfile.LNKTYPE)
    assert not extract(root, archive, dest, allow_links=True)
    assert (dest / "private.txt").read_text() == "keep"
    assert not (dest / "copy.txt").exists()

    # Replacing a destination file must not truncate an inode also linked
    # outside the extraction destination.
    secret = outside / "secret.txt"
    secret.write_text("keep")
    dest = root / "preexisting-hardlink"
    dest.mkdir()
    os.link(secret, dest / "proof.txt")
    archive = root / "preexisting-hardlink.tar"
    with tarfile.open(archive, "w") as tar:
        add_file(tar, "proof.txt", b"new data")
    assert extract(root, archive, dest)
    assert secret.read_text() == "keep"
    assert (dest / "proof.txt").read_bytes() == b"new data"
    assert not (dest / "proof.txt").samefile(secret)

    # A symlink inside dest is not a valid hardlink source, even when links
    # are enabled for the archive.
    archive = root / "symlink-hardlink.tar"
    with tarfile.open(archive, "w") as tar:
        add_link(tar, "source.txt", outside / "secret.txt", tarfile.SYMTYPE)
        add_link(tar, "copy.txt", "source.txt", tarfile.LNKTYPE)
    assert not extract(root, archive, root / "symlink-hardlink", allow_links=True)

    # Regular zip extraction still creates nested directories and file data.
    archive = root / "regular.zip"
    dest = root / "regular-zip"
    with zipfile.ZipFile(archive, "w") as zip_archive:
        zip_archive.writestr("nested/proof.txt", "zip data")
    assert extract(root, archive, dest)
    assert (dest / "nested" / "proof.txt").read_text() == "zip data"

print("archive extraction security: 9 cases passed")
PY
