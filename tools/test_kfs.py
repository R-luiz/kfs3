#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL_BIN = ROOT / "kernel.bin"
TEN_MIB = 10 * 1024 * 1024
PROMPT = "shell> "

EXPECTED_GDT = bytes([
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFA, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xF2, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xF2, 0xCF, 0x00,
])


@dataclass
class CheckResult:
    section: str
    name: str
    ok: bool
    detail: str = ""


class Reporter:
    def __init__(self) -> None:
        self.results: list[CheckResult] = []

    def add(self, section: str, name: str, ok: bool, detail: str = "") -> None:
        self.results.append(CheckResult(section, name, ok, detail))
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {section}: {name}")
        if detail and not ok:
            print(indent(trim_output(detail), prefix="    "))

    def passed(self) -> bool:
        return all(result.ok for result in self.results)

    def summary(self) -> None:
        total = len(self.results)
        passed = sum(1 for result in self.results if result.ok)
        failed = total - passed
        print()
        print(f"Summary: {passed}/{total} checks passed, {failed} failed")


def trim_output(text: str, limit: int = 800) -> str:
    cleaned = clean_output(text)
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[-limit:]


def indent(text: str, prefix: str = "  ") -> str:
    return "\n".join(prefix + line for line in text.splitlines())


def clean_output(text: str) -> str:
    return text.replace("\r", "").replace("\x00", "")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def run_command(command: list[str], timeout: int = 180) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )


class QemuSession:
    def __init__(self, qemu_binary: str, memory: str = "128M") -> None:
        self.qemu_binary = qemu_binary
        self.memory = memory
        self.proc: subprocess.Popen[bytes] | None = None
        self.master_fd: int | None = None
        self.transcript = ""

    def __enter__(self) -> "QemuSession":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def start(self) -> None:
        master_fd, slave_fd = pty.openpty()
        command = [
            self.qemu_binary,
            "-kernel",
            str(KERNEL_BIN),
            "-m",
            self.memory,
            "-nographic",
            "-serial",
            "stdio",
            "-monitor",
            "none",
        ]
        self.proc = subprocess.Popen(
            command,
            cwd=ROOT,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            close_fds=True,
        )
        os.close(slave_fd)
        self.master_fd = master_fd

    def close(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self.master_fd is not None:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
        self.proc = None
        self.master_fd = None

    def _read_chunk(self, timeout: float) -> str:
        if self.master_fd is None:
            return ""
        ready, _, _ = select.select([self.master_fd], [], [], timeout)
        if not ready:
            return ""
        try:
            data = os.read(self.master_fd, 4096)
        except OSError:
            return ""
        return data.decode("latin1", errors="ignore")

    def collect_until(self, patterns: str | list[str], timeout: float) -> str:
        if isinstance(patterns, str):
            pattern_list = [patterns]
        else:
            pattern_list = patterns

        start = len(self.transcript)
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            segment = self.transcript[start:]
            if any(pattern in segment for pattern in pattern_list):
                return segment

            if self.proc is not None and self.proc.poll() is not None:
                chunk = self._read_chunk(0)
                if chunk:
                    self.transcript += chunk
                    continue
                raise RuntimeError(trim_output(segment or self.transcript))

            chunk = self._read_chunk(0.1)
            if chunk:
                self.transcript += chunk

        raise TimeoutError(trim_output(self.transcript[start:]))

    def sendline(self, line: str) -> None:
        if self.master_fd is None:
            raise RuntimeError("QEMU session is not running")
        os.write(self.master_fd, (line + "\n").encode("ascii", errors="ignore"))

    def boot_to_shell(self) -> str:
        boot = self.collect_until("Press any key to continue...", timeout=30)
        self.sendline("")
        shell = self.collect_until(PROMPT, timeout=15)
        return boot + shell

    def run_command(self, command: str, timeout: float = 10) -> str:
        self.sendline(command)
        return self.collect_until(PROMPT, timeout=timeout)

    def run_until(self, command: str, patterns: str | list[str], timeout: float = 10) -> str:
        self.sendline(command)
        return self.collect_until(patterns, timeout=timeout)


def parse_hexdump_bytes(output: str) -> bytes:
    result: list[int] = []
    for line in clean_output(output).splitlines():
        match = re.search(r"0x[0-9A-Fa-f]+:\s*(.*?)\|", line)
        if not match:
            continue
        result.extend(int(token, 16) for token in re.findall(r"\b[0-9A-Fa-f]{2}\b", match.group(1)))
    return bytes(result)


def parse_peek_value(output: str) -> int | None:
    match = re.search(r"Value at\s+0x[0-9A-Fa-f]+:\s+0x([0-9A-Fa-f]+)", clean_output(output))
    if match is None:
        return None
    return int(match.group(1), 16)


def parse_alloc_address(output: str) -> int | None:
    match = re.search(r"Allocated\s+\d+\s+bytes\s+at\s+0x([0-9A-Fa-f]+)", clean_output(output))
    if match is None:
        return None
    return int(match.group(1), 16)


def gdt_matches_expected(actual: bytes) -> bool:
    if len(actual) < len(EXPECTED_GDT):
        return False

    normalized_actual = bytearray(actual[:len(EXPECTED_GDT)])
    normalized_expected = bytearray(EXPECTED_GDT)

    for offset in range(5, len(EXPECTED_GDT), 8):
        normalized_actual[offset] &= 0xFE
        normalized_expected[offset] &= 0xFE

    return bytes(normalized_actual) == bytes(normalized_expected)


def static_checks(report: Reporter) -> None:
    makefile = read_text(ROOT / "Makefile")
    gdt_header = read_text(ROOT / "kernel/include/gdt.h")
    gdt_source = read_text(ROOT / "kernel/src/gdt.c")
    paging_header = read_text(ROOT / "kernel/include/paging.h")
    paging_source = read_text(ROOT / "kernel/src/paging.c")
    pmm_header = read_text(ROOT / "kernel/include/pmm.h")
    kmalloc_header = read_text(ROOT / "kernel/include/kmalloc.h")
    vmalloc_header = read_text(ROOT / "kernel/include/vmalloc.h")
    panic_header = read_text(ROOT / "kernel/include/panic.h")
    main_source = read_text(ROOT / "kernel/src/main.c")
    shell_source = read_text(ROOT / "kernel/src/shell.c")
    create_image = read_text(ROOT / "tools/create_image.sh")

    required_flags = [
        "-fno-builtin",
        "-fno-exceptions",
        "-fno-stack-protector",
        "-nostdlib",
        "-nodefaultlibs",
        "-m elf_i386",
        "-m32",
    ]
    missing_flags = [flag for flag in required_flags if flag not in makefile]
    report.add(
        "General",
        "Freestanding build flags are present",
        not missing_flags,
        "Missing flags: " + ", ".join(missing_flags) if missing_flags else "",
    )

    report.add(
        "KFS2 mandatory",
        "GDT base address is configured at 0x00000800",
        "GDT_BASE_ADDRESS           0x00000800" in gdt_header,
    )

    descriptor_patterns = [
        r"gdt_set_gate\(1,\s*0,\s*0xFFFFFFFF,\s*0x9A,\s*0xCF\)",
        r"gdt_set_gate\(2,\s*0,\s*0xFFFFFFFF,\s*0x92,\s*0xCF\)",
        r"gdt_set_gate\(3,\s*0,\s*0xFFFFFFFF,\s*0x92,\s*0xCF\)",
        r"gdt_set_gate\(4,\s*0,\s*0xFFFFFFFF,\s*0xFA,\s*0xCF\)",
        r"gdt_set_gate\(5,\s*0,\s*0xFFFFFFFF,\s*0xF2,\s*0xCF\)",
        r"gdt_set_gate\(6,\s*0,\s*0xFFFFFFFF,\s*0xF2,\s*0xCF\)",
    ]
    report.add(
        "KFS2 mandatory",
        "GDT source defines kernel/user code, data, and stack descriptors",
        all(re.search(pattern, gdt_source) for pattern in descriptor_patterns),
    )

    report.add(
        "KFS2 mandatory",
        "LGDT is used to load the GDT",
        "lgdt" in gdt_source,
    )
    report.add(
        "KFS2 mandatory",
        "Boot stack symbols are exported",
        "global stack_bottom" in read_text(ROOT / "boot/boot.asm")
        and "global stack_top" in read_text(ROOT / "boot/boot.asm"),
    )
    report.add(
        "KFS2 mandatory",
        "Kernel stack printer is present",
        "=== Kernel Stack ===" in main_source and "Stack contents (64 bytes from ESP):" in main_source,
    )

    report.add(
        "KFS3 mandatory",
        "Paging flags include kernel and user access modes",
        all(token in paging_header for token in ["PAGE_KERNEL", "PAGE_USER_RO", "PAGE_USER_RW"]),
    )
    report.add(
        "KFS3 mandatory",
        "Paging implementation exposes map, unmap, and physical lookup helpers",
        all(token in paging_source for token in ["void paging_map_page", "void paging_unmap_page", "uint32_t paging_get_physical"]),
    )
    report.add(
        "KFS3 mandatory",
        "Physical memory manager exposes frame allocation helpers",
        all(token in pmm_header for token in ["pmm_alloc_frame", "pmm_free_frame", "pmm_alloc_frames", "pmm_free_frames"]),
    )
    report.add(
        "KFS3 mandatory",
        "Kernel heap helpers are exposed",
        all(token in kmalloc_header for token in ["kmalloc", "kfree", "ksize", "kbrk"]),
    )
    report.add(
        "KFS3 mandatory",
        "Virtual memory helpers are exposed",
        all(token in vmalloc_header for token in ["vmalloc", "vfree", "vsize", "vbrk"]),
    )
    report.add(
        "KFS3 mandatory",
        "Fatal and non-fatal panic helpers are exposed",
        all(token in panic_header for token in ["panic(", "warn(", "panic_at("] ),
    )

    report.add(
        "KFS2 bonus",
        "Shell implementation is present",
        all(token in shell_source for token in ["shell> ", "help", "reboot", "exit"]),
    )
    report.add(
        "KFS3 bonus",
        "Debug shell commands are implemented",
        all(token in shell_source for token in ["hexdump", "peek", "poke", "stack", "pagedir", "panic"]),
    )
    report.add(
        "General",
        "Image script stays within the 10 MiB subject limit",
        "bs=1M count=10" in create_image,
    )


def runtime_checks(report: Reporter, qemu_binary: str) -> None:
    with QemuSession(qemu_binary) as session:
        boot_output = clean_output(session.boot_to_shell())
        report.add(
            "KFS2 mandatory",
            "Boot-time kernel stack dump is printed",
            all(token in boot_output for token in ["=== Kernel Stack ===", "Range:", "ESP:", "Stack contents (64 bytes from ESP):"]),
            boot_output,
        )
        report.add(
            "KFS3 mandatory",
            "Boot sequence reports PMM, paging, heap, and vmalloc initialization",
            all(token in boot_output for token in [
                "Initializing Physical Memory Manager...",
                "Initializing Paging...",
                "Paging enabled (identity mapping)",
                "Initializing Kernel Heap...",
                "Initializing Virtual Memory Allocator...",
            ]),
            boot_output,
        )

        help_output = clean_output(session.run_command("help"))
        report.add(
            "KFS2 bonus",
            "Shell help lists the expected built-in and debug commands",
            all(token in help_output for token in ["help", "echo TEXT", "reboot", "meminfo", "stack", "panic"]),
            help_output,
        )

        echo_output = clean_output(session.run_command("echo KFS_TEST"))
        report.add(
            "KFS2 bonus",
            "Echo command works",
            "KFS_TEST" in echo_output,
            echo_output,
        )

        ls_output = clean_output(session.run_command("ls"))
        report.add(
            "KFS2 bonus",
            "ls command shows the project layout",
            all(token in ls_output for token in ["boot/", "kernel/", "tools/", "README.md"]),
            ls_output,
        )

        clear_output = clean_output(session.run_command("clear"))
        report.add(
            "KFS2 bonus",
            "clear command returns control to the shell",
            PROMPT.strip() in clear_output,
            clear_output,
        )

        gdt_dump_output = clean_output(session.run_command("hexdump 0x800 64"))
        gdt_bytes = parse_hexdump_bytes(gdt_dump_output)
        report.add(
            "KFS2 mandatory",
            "Runtime GDT at 0x800 matches the expected seven descriptors",
            gdt_matches_expected(gdt_bytes),
            gdt_dump_output,
        )

        stack_output = clean_output(session.run_command("stack"))
        report.add(
            "KFS2 bonus",
            "stack command prints a human-friendly dump",
            all(token in stack_output for token in ["=== Stack Dump ===", "ESP:", "EBP:", "Stack contents (64 bytes from ESP):"]),
            stack_output,
        )

        meminfo_before = clean_output(session.run_command("meminfo"))
        report.add(
            "KFS3 mandatory",
            "meminfo reports physical, heap, and virtual memory sections",
            all(token in meminfo_before for token in ["=== Physical Memory ===", "=== Kernel Heap ===", "=== Virtual Memory ==="]),
            meminfo_before,
        )

        alloc_output = clean_output(session.run_command("alloc 64"))
        report.add(
            "KFS3 mandatory",
            "kmalloc shell command allocates memory",
            "Allocated 64 bytes at 0x" in alloc_output,
            alloc_output,
        )

        kheap_output = clean_output(session.run_command("kheap"))
        report.add(
            "KFS3 mandatory",
            "kheap shows at least one used allocation after alloc",
            all(token in kheap_output for token in ["=== Kernel Heap Dump ===", "USED"]),
            kheap_output,
        )

        free_output = clean_output(session.run_command("free"))
        report.add(
            "KFS3 mandatory",
            "kfree shell path frees the last allocation",
            "Freeing 64 bytes at 0x" in free_output,
            free_output,
        )

        meminfo_after = clean_output(session.run_command("meminfo"))
        report.add(
            "KFS3 mandatory",
            "meminfo reflects that the allocation count returned to zero",
            "Allocs:    0" in meminfo_after,
            meminfo_after,
        )

        vmap_output = clean_output(session.run_command("vmap"))
        report.add(
            "KFS3 mandatory",
            "vmap exposes vmalloc and page-directory state",
            all(token in vmap_output for token in ["=== Virtual Memory Dump ===", "=== Page Directory Info ===", "Mapped page tables:"]),
            vmap_output,
        )

        scratch_alloc = clean_output(session.run_command("alloc 16"))
        scratch_addr = parse_alloc_address(scratch_alloc)
        peek_before = ""
        poke_output = ""
        peek_after = ""
        cleanup_output = ""

        if scratch_addr is not None:
            peek_before = clean_output(session.run_command(f"peek 0x{scratch_addr:08X}"))
            poke_output = clean_output(session.run_command(f"poke 0x{scratch_addr:08X} 0x07410742"))
            peek_after = clean_output(session.run_command(f"peek 0x{scratch_addr:08X}"))
            cleanup_output = clean_output(session.run_command("free"))

        report.add(
            "KFS3 bonus",
            "peek and poke can read and write memory through the shell",
            scratch_addr is not None
            and f"Wrote 0x07410742 to 0x{scratch_addr:08X}" in poke_output
            and "0x07410742" in peek_after,
            scratch_alloc + "\n" + peek_before + "\n" + poke_output + "\n" + peek_after + "\n" + cleanup_output,
        )

        pagedir_output = clean_output(session.run_command("pagedir"))
        report.add(
            "KFS3 bonus",
            "pagedir prints mapped page tables and permissions",
            all(token in pagedir_output for token in ["=== Page Directory ===", "Mapped page tables:", "VA:"]),
            pagedir_output,
        )

        warning_output = clean_output(session.run_command("alloc 9000000", timeout=15))
        report.add(
            "KFS3 mandatory",
            "Non-fatal warning path keeps the shell alive after an oversized allocation",
            all(token in warning_output for token in ["[WARNING] kbrk: Heap would exceed maximum size", "Allocation failed!", PROMPT.strip()]),
            warning_output,
        )

        halt_output = clean_output(session.run_until("exit", "System halted.", timeout=10))
        report.add(
            "KFS2 bonus",
            "exit leaves the shell and halts the system",
            all(token in halt_output for token in ["Exiting shell...", "System halted."]),
            halt_output,
        )

    with QemuSession(qemu_binary) as session:
        session.boot_to_shell()
        panic_output = clean_output(session.run_until("panic", "The system has been halted.", timeout=10))
        report.add(
            "KFS3 mandatory",
            "panic prints a fatal panic and halts",
            all(token in panic_output for token in ["Triggering test kernel panic...", "KERNEL PANIC", "The system has been halted."]),
            panic_output,
        )

    with QemuSession(qemu_binary) as session:
        session.boot_to_shell()
        reboot_output = clean_output(session.run_until("reboot", ["Rebooting...", "Press any key to continue..."], timeout=20))
        if "Press any key to continue..." not in reboot_output:
            reboot_output += clean_output(session.collect_until("Press any key to continue...", timeout=20))
        report.add(
            "KFS2 bonus",
            "reboot restarts the virtual machine back into the kernel",
            "Rebooting..." in reboot_output and reboot_output.count("Welcome to KFS-3!") >= 1,
            reboot_output,
        )


def general_checks(report: Reporter) -> None:
    report.add(
        "General",
        "kernel.bin was produced",
        KERNEL_BIN.exists(),
        str(KERNEL_BIN),
    )
    report.add(
        "General",
        "kernel.bin stays below 10 MiB",
        KERNEL_BIN.exists() and KERNEL_BIN.stat().st_size <= TEN_MIB,
        f"kernel.bin size: {KERNEL_BIN.stat().st_size} bytes" if KERNEL_BIN.exists() else "kernel.bin is missing",
    )


def build_kernel(report: Reporter) -> bool:
    clean_result = run_command(["make", "clean"])
    build_result = run_command(["make"])
    detail = clean_result.stdout + clean_result.stderr + build_result.stdout + build_result.stderr
    report.add("General", "Kernel builds successfully", build_result.returncode == 0, detail)
    return build_result.returncode == 0


def find_qemu(report: Reporter) -> str | None:
    qemu_binary = shutil.which("qemu-system-i386")
    report.add(
        "General",
        "qemu-system-i386 is available",
        qemu_binary is not None,
        "Install qemu-system-i386 to run runtime tests" if qemu_binary is None else qemu_binary,
    )
    return qemu_binary


def main() -> int:
    parser = argparse.ArgumentParser(description="Static and runtime checks for KFS-2 and KFS-3")
    parser.add_argument("--skip-build", action="store_true", help="Assume kernel.bin is already up to date")
    parser.add_argument("--skip-runtime", action="store_true", help="Run only static checks")
    args = parser.parse_args()

    reporter = Reporter()

    if not args.skip_build:
        if not build_kernel(reporter):
            reporter.summary()
            return 1

    general_checks(reporter)
    static_checks(reporter)

    if not args.skip_runtime:
        qemu_binary = find_qemu(reporter)
        if qemu_binary is not None:
            try:
                runtime_checks(reporter, qemu_binary)
            except Exception as exc:
                reporter.add("Runtime", "QEMU test harness completed", False, str(exc))

    reporter.summary()
    return 0 if reporter.passed() else 1


if __name__ == "__main__":
    sys.exit(main())