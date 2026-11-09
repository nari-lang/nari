"""GDB commands for Nari source and bytecode function entry debugging.

Load with: gdb -q -x gdb/nari.py --args ./build/release/nari file.nari
"""

import pathlib
import gdb


_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
_names = set()
_entry_breakpoint = None


def _inferior_string(accessor):
    # The release executable retains these C ABI accessors even without DWARF.
    return gdb.parse_and_eval("(char *)" + accessor + "()").string()


def _entry_name():
    # Reading an inferior register is safe in Breakpoint.stop(). Calling an
    # inferior function here can leave GDB unable to resume after stop() says
    # to continue past an unrelated Nari function.
    frame = gdb.selected_frame()
    architecture = frame.architecture().name()
    if "x86-64" in architecture or "i386:x86-64" in architecture:
        register = "rdi"
    elif "aarch64" in architecture:
        register = "x0"
    else:
        raise gdb.GdbError("unsupported GDB architecture: %s" % architecture)
    pointer = frame.read_register(register).cast(gdb.lookup_type("char").pointer())
    return pointer.string()


def _source_path(source):
    if source.startswith("<std>/"):
        module = source[len("<std>/") :]
        root = (_REPO_ROOT / "src" / "stdlib" / "std").resolve()
        path = (root / (module + ".nari")).resolve()
        return path if path.is_relative_to(root) else None
    if source == "<embedded_stdlib>":
        return _REPO_ROOT / "src" / "stdlib" / "std" / "prelude.nari"
    if source.startswith("<") and source.endswith(">"):
        return None
    return pathlib.Path(source)


class _EntryBreakpoint(gdb.Breakpoint):
    def __init__(self):
        super().__init__("nari_gdb_function_entry", internal=True)
        self.silent = True

    def stop(self):
        try:
            name = _entry_name()
        except gdb.error as exc:
            gdb.write("Nari GDB: unable to read function name: %s\n" % exc, gdb.STDERR)
            return True
        if name not in _names:
            return False
        gdb.write("Nari function: %s\n" % name)
        return True


def _require_entry():
    try:
        if gdb.selected_frame().name() == "nari_gdb_function_entry":
            return
    except gdb.error:
        pass
    raise gdb.GdbError("stop at a nari-break breakpoint first")


class NariBreak(gdb.Command):
    """nari-break NAME -- stop when the named Nari function or module begins."""

    def __init__(self):
        super().__init__("nari-break", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, arg, from_tty):
        global _entry_breakpoint
        args = gdb.string_to_argv(arg)
        if len(args) != 1:
            raise gdb.GdbError("usage: nari-break NAME")
        if _entry_breakpoint is None and gdb.selected_inferior().pid:
            raise gdb.GdbError("set the first nari-break before running the inferior")
        name = args[0]
        _names.add(name)
        # Set before run. Source-level entry hooks are opt-in, and the JIT is
        # disabled so direct compiled calls cannot skip an entry hook.
        gdb.execute("set environment NARI_GDB_FUNCTIONS 1", to_string=True)
        gdb.execute("set environment NARI_DISABLE_JIT 1", to_string=True)
        if _entry_breakpoint is None:
            _entry_breakpoint = _EntryBreakpoint()
        gdb.write("Nari breakpoint set: %s\n" % name)


class NariDisassemble(gdb.Command):
    """nari-disassemble -- show source for the stopped Nari function or module."""

    def __init__(self):
        super().__init__("nari-disassemble", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        if arg.strip():
            raise gdb.GdbError("usage: nari-disassemble")
        _require_entry()
        source = _inferior_string("nari_gdb_event_source")
        path = _source_path(source)
        if path is not None and path.is_file():
            try:
                content = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
            else:
                gdb.write("Nari source: %s\n" % source)
                gdb.write(content)
                if not content.endswith("\n"):
                    gdb.write("\n")
                return
        gdb.write("Nari source unavailable (%s); showing bytecode.\n" % source)
        gdb.execute("call (void)nari_gdb_dump_bytecode()")


class NariBytecode(gdb.Command):
    """nari-bytecode -- disassemble the stopped Nari function or module bytecode."""

    def __init__(self):
        super().__init__("nari-bytecode", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        if arg.strip():
            raise gdb.GdbError("usage: nari-bytecode")
        _require_entry()
        gdb.execute("call (void)nari_gdb_dump_bytecode()")


NariBreak()
NariDisassemble()
NariBytecode()
