"""makes sure Windows native file cancellation doesn't die with named pipes"""

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", required=True)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this regression requires Windows")
    root = Path(__file__).resolve().parent.parent
    interpreter = str(Path(args.interpreter).resolve())
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class Overlapped(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                    ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]

    kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    kernel.CreateNamedPipeW.restype = wintypes.HANDLE
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateEventW.restype = wintypes.HANDLE
    kernel.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.ConnectNamedPipe.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped), ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    for operation, explicit_exit in ((op, flag) for op in ("read", "write", "append") for flag in ("0", "1")):
        name = rf"\\.\pipe\nari-file-shutdown-{uuid.uuid4().hex}"
        pipe = kernel.CreateNamedPipeW(name, 3 | 0x40000000, 0, 1, 4096, 4096, 0, None)
        if pipe == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        event = kernel.CreateEventW(None, True, False, None)
        if not event:
            kernel.CloseHandle(pipe)
            raise ctypes.WinError(ctypes.get_last_error())
        connected = Overlapped(hEvent=event)
        process = None
        try:
            if not kernel.ConnectNamedPipe(pipe, ctypes.byref(connected)) and ctypes.get_last_error() != 997:
                raise ctypes.WinError(ctypes.get_last_error())
            environment = dict(os.environ, NARI_TEST_PIPE=name, NARI_TEST_FILE_OP=operation, NARI_TEST_EXIT=explicit_exit)
            process = subprocess.Popen(
                [interpreter, "tests/windows/pending_file.nari"],
                cwd=root, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, 
                text=True, encoding="utf-8", errors="replace"
            )
            if kernel.WaitForSingleObject(event, 5000) != 0:
                raise AssertionError(f"{operation}: file worker did not connect to its pipe")
            # Keep the peer connected without reading or writing. The runtime
            # must finish before the server closes or permits I/O to progress.
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
                raise AssertionError(f"{operation}, explicit exit={explicit_exit}: shutdown timed out\n{output}") from None
            assert process.returncode == 0, (operation, process.returncode, output)
            assert "exit with pending Windows file I/O" in output, (operation, output)
            print(f"[pass] Windows pending file {operation} ({'process.exit' if explicit_exit == '1' else 'normal exit'})", flush=True)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()
            kernel.CancelIoEx(pipe, ctypes.byref(connected))
            transferred = wintypes.DWORD()
            kernel.GetOverlappedResult(pipe, ctypes.byref(connected), ctypes.byref(transferred), True)
            kernel.CloseHandle(event)
            kernel.CloseHandle(pipe)


if __name__ == "__main__":
    main()
