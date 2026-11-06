#!/usr/bin/env python3
"""Check ESPHome formatting, explicit instance access, and field visibility.

LLVM 21.1.8 supplies both clang-format and its matching Python/libclang bindings.
The default semantic profile covers host-testable code. --firmware-build adds
the ESP32 adapters using an existing ESPHome/ESP-IDF compilation database.
ESP32 flag normalization follows ESPHome's MIT-licensed script/clang-tidy;
see third_party/esphome/NOTICE.md and LICENSE for provenance and notices.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LLVM_VERSION = "21.1.8"
SUFFIXES = {".h", ".hpp", ".cc", ".cpp"}


def sources():
    return sorted(
        path for directory in (ROOT / "components", ROOT / "tests")
        for path in directory.rglob("*")
        if path.suffix in SUFFIXES and "meshcore_ed25519" not in path.parts
    )


def llvm_prefix():
    if value := os.environ.get("LLVM_PREFIX"):
        return Path(value)
    for name in ("llvm-config-21", "llvm-config"):
        if executable := shutil.which(name):
            return Path(subprocess.check_output([executable, "--prefix"], text=True).strip())
    if shutil.which("brew"):
        return Path(subprocess.check_output(["brew", "--prefix", "llvm@21"], text=True).strip())
    raise RuntimeError("Install LLVM 21.1.8 with Python bindings, or set LLVM_PREFIX")


def load_clang(prefix):
    bindings = sorted((prefix / "lib").glob("python*/site-packages/clang/cindex.py"))
    if bindings:
        sys.path.insert(0, str(bindings[0].parents[1]))
    from clang import cindex
    libraries = list((prefix / "lib").glob("libclang.dylib")) or list((prefix / "lib").glob("libclang.so*"))
    if not libraries:
        raise RuntimeError(f"libclang not found under {prefix}")
    cindex.Config.set_library_file(str(libraries[0]))
    return cindex


def spelling_location(cindex, location):
    file = cindex.c_object_p()
    line, column, offset = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
    cindex.conf.lib.clang_getSpellingLocation(
        location, ctypes.byref(file), ctypes.byref(line), ctypes.byref(column), ctypes.byref(offset)
    )
    return (Path(cindex.File(file).name).resolve() if file else None, line.value, column.value, offset.value)


def compiler_includes(compiler):
    result = subprocess.run(
        [compiler, "-E", "-x", "c++", "-", "-v"], input="", text=True, capture_output=True, check=True
    )
    section = result.stderr.split("#include <...> search starts here:", 1)[1].split("End of search list.", 1)[0]
    return [line.strip() for line in section.splitlines() if line.strip() and "(framework directory)" not in line]


def include_tree():
    directory = ROOT / "build/cpp-style/include/esphome/components"
    directory.mkdir(parents=True, exist_ok=True)
    for component in (ROOT / "components").iterdir():
        if component.is_dir():
            link = directory / component.name
            if link.is_symlink() and link.resolve() != component.resolve():
                link.unlink()  # Stale after the checkout or its dependency moved.
            if not link.is_symlink():
                link.symlink_to(component, target_is_directory=True)
    return directory.parents[1]


def host_units():
    compiler = os.environ.get("CXX", "clang++")
    args = ["-std=c++17", "-I" + str(ROOT / "tests/fakes"), "-I" + str(include_tree())]
    args += ["-I" + str(path) for path in (ROOT / "components").iterdir() if path.is_dir()]
    for path in compiler_includes(compiler):
        args += ["-isystem", path]
    openssl = os.environ.get("OPENSSL_PREFIX")
    if not openssl and shutil.which("brew"):
        openssl = subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
    if openssl:
        args += ["-I" + str(Path(openssl) / "include")]
    units = list((ROOT / "components/meshcore_core").glob("*.cpp"))
    units += list((ROOT / "tests").glob("*.cpp"))
    units += [ROOT / path for path in (
        "components/meshcore/ed25519_identity.cpp", "components/meshcore/sx126x_radio.cpp",
        "components/meshcore/nvs_room_store.cpp", "components/meshcore/room_runtime.cpp",
        "tests/fakes/esphome/components/spi/spi.h",
        "tests/fakes/esphome/core/automation.h",
        "tests/fakes/esphome/core/component.h",
        "tests/fakes/esphome/core/log.h",
    )]
    return [(path, ["-x", "c++", *args]) for path in sorted(units)]


def firmware_units(build):
    """Use the real target headers with Clang's 32-bit Xtensa analysis surrogate.

    Matches ESPHome's documented clang-tidy approach; this is syntax analysis,
    never an alternative target compiler or evidence of hardware correctness.
    """
    build = build.resolve()
    database = json.loads((build / "build/compile_commands.json").read_text())
    units = []
    owned = set(sources())
    includes = None
    omit = {
        "-mlongcalls", "-mtext-section-literals", "-fstrict-volatile-bitfields", "-freorder-blocks",
        "-fno-tree-switch-conversion", "-fno-shrink-wrap", "-fno-jump-tables", "-mno-target-align",
    }
    for entry in database:
        file = Path(entry["file"])
        try:
            relative = file.relative_to(build / "src/esphome")
        except ValueError:
            continue
        source = ROOT / relative
        if source not in owned or source.suffix not in {".cpp", ".cc"}:
            continue
        command = entry.get("arguments") or shlex.split(entry["command"])
        if includes is None:
            includes = compiler_includes(command[0])
        args = ["--target=i386-unknown-linux-gnu", "-U__i386__", "-U__x86_64__", "-D__XTENSA__",
                "-D__XTENSA_EL__", "-D_LIBC", "-D_GLIBCXX_HAVE_TLS", "-nostdinc", "-nostdinc++",
                "-I" + str(include_tree()), "-Wno-unknown-attributes"]
        skip = False
        for arg in command[1:]:
            if skip:
                skip = False
            elif arg in {"-o", "-MF", "-MT", "-MQ"}:
                skip = True
            elif arg in omit or arg in {"-c", "-MD", "-MMD", "-MP"} or arg == str(file):
                continue
            elif arg.startswith(("-W", "-fdiagnostics", "-std=")):
                continue
            elif arg.startswith("-I") and not Path(arg[2:]).is_absolute():
                args.append("-I" + str(Path(entry["directory"]) / arg[2:]))
            else:
                args.append(arg)
        args.append("-std=gnu++20")
        for path in includes:
            args += ["-isystem", path]
        units.append((source, args))
    if not units:
        raise RuntimeError("No owned C++ translation units found in firmware compilation database")
    return units


def inspect(cindex, unit, owned):
    """Return semantic findings and byte insertions, never regex-based rewrites."""
    findings = set()
    insertions = set()
    seen = {Path(include.include.name).resolve() for include in unit.get_includes()} & owned
    kind = cindex.CursorKind

    def visit(cursor):
        location = cursor.location
        path = Path(location.file.name).resolve() if location.file else None
        if path is not None and path not in owned:
            return
        if path in owned:
            seen.add(path)
            reference = cursor.referenced
            if cursor.kind in {kind.FIELD_DECL, kind.VAR_DECL} and cursor.access_specifier == cindex.AccessSpecifier.PROTECTED:
                findings.add((path, location.line, location.column, "protected-field", cursor.spelling))
            if path.is_relative_to(ROOT / "components") and cursor.access_specifier == cindex.AccessSpecifier.PUBLIC:
                parent = cursor.semantic_parent
                class_state = parent and parent.kind in {kind.CLASS_DECL, kind.CLASS_TEMPLATE}
                mutable_static = cursor.kind == kind.VAR_DECL and not cursor.type.is_const_qualified()
                if class_state and (cursor.kind == kind.FIELD_DECL or mutable_static):
                    findings.add((path, location.line, location.column, "private-class-state", cursor.spelling))
            if cursor.kind == kind.MEMBER_REF_EXPR and reference is not None:
                instance = reference.kind == kind.FIELD_DECL or (
                    reference.kind == kind.CXX_METHOD and not reference.is_static_method()
                )
                # libclang omits the implicit object; an explicit receiver has
                # an expression child (this, another object, a call, etc.).
                implicit = not any(child.kind.is_expression() for child in cursor.get_children())
                if instance and implicit:
                    findings.add((path, location.line, location.column, "explicit-this", cursor.spelling))
                    file, line, column, offset = spelling_location(cindex, cursor.extent.start)
                    # Only offer a fix where the spelling is an actual source
                    # token. Macro-generated access must be corrected by hand.
                    content = path.read_bytes()
                    name = cursor.spelling.encode()
                    end = offset + len(name)
                    if name and file == path and offset >= cursor.extent.start.offset and content[offset:end] == name and (
                        end == len(content) or not (content[end:end + 1].isalnum() or content[end:end + 1] == b"_")
                    ):
                        insertions.add((path, offset))
        for child in cursor.get_children():
            visit(child)

    visit(unit.cursor)
    return findings, insertions, seen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="format and insert explicit this-> where safe")
    parser.add_argument("--format-only", action="store_true")
    parser.add_argument("--firmware-build", type=Path, help="add adapters from an ESPHome build directory")
    options = parser.parse_args()
    os.chdir(ROOT)
    prefix = llvm_prefix()
    formatter = prefix / "bin/clang-format"
    version = subprocess.check_output([str(formatter), "--version"], text=True)
    if f"version {LLVM_VERSION}" not in version:
        raise RuntimeError(f"Expected clang-format {LLVM_VERSION}; found {version.strip()}")
    owned = set(sources())
    if not options.format_only:
        cindex = load_clang(prefix)
        index = cindex.Index.create()
        findings, insertions, seen, errors = set(), set(), set(), []
        units = host_units()
        if options.firmware_build:
            units += firmware_units(options.firmware_build)
        for source, args in units:
            unit = index.parse(str(source), args=args)
            errors += [str(d) for d in unit.diagnostics if d.severity >= cindex.Diagnostic.Error]
            found, fixes, covered = inspect(cindex, unit, owned)
            findings |= found
            insertions |= fixes
            seen |= covered
        if errors:
            for error in sorted(set(errors)):
                print(error, file=sys.stderr)
            raise RuntimeError("Semantic parse failed; no fixes applied")
        if options.fix:
            for path in sorted({path for path, _ in insertions}):
                content = path.read_bytes()
                for offset in sorted({offset for file, offset in insertions if file == path}, reverse=True):
                    content = content[:offset] + b"this->" + content[offset:]
                path.write_bytes(content)
            # Re-run after fixing to verify templates and macro cases too.
            findings = {f for f in findings if f[3] != "explicit-this"}
        for path, line, column, rule, name in sorted(findings):
            print(f"{path.relative_to(ROOT)}:{line}:{column}: {rule}: {name}")
        print(f"Semantic coverage: {len(seen)}/{len(owned)} owned files ({len(units)} translation units).", flush=True)
        if missing := owned - seen:
            print("Not analyzed: " + ", ".join(str(path.relative_to(ROOT)) for path in sorted(missing)), flush=True)
        if not options.firmware_build:
            print("Platform-only adapters require --firmware-build; naming/design rules also require review.", flush=True)
    else:
        findings = set()
    result = subprocess.run([str(formatter), "-i" if options.fix else "--dry-run", "--Werror", *map(str, sorted(owned))])
    if options.fix and not options.format_only:
        # Checking again avoids claiming success for an access that could not
        # safely be rewritten (e.g. generated by a third-party macro).
        command = [sys.executable, __file__]
        if options.firmware_build:
            command += ["--firmware-build", str(options.firmware_build)]
        return subprocess.call(command)
    return int(bool(findings) or result.returncode != 0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError, ImportError) as error:
        print(f"C++ style check failed: {error}", file=sys.stderr)
        sys.exit(1)
