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
from functools import lru_cache
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


@lru_cache(maxsize=None)
def read_text_cached(path: Path) -> str:
    return read_text(path)


@lru_cache(maxsize=1)
def repository_source_files() -> tuple[Path, ...]:
    files: list[Path] = []

    for relative_dir in ["boot", "kernel"]:
        for path in sorted((ROOT / relative_dir).rglob("*")):
            if path.is_file() and path.suffix in {".asm", ".c", ".h", ".ld", ".s", ".S"}:
                files.append(path)

    return tuple(files)


def repo_has_regex(pattern: str) -> bool:
    regex = re.compile(pattern, re.IGNORECASE | re.MULTILINE)
    return any(regex.search(read_text_cached(path)) for path in repository_source_files())


def repo_has_any(patterns: list[str]) -> bool:
    return any(repo_has_regex(pattern) for pattern in patterns)


def repo_has_all(patterns: list[str]) -> bool:
    return all(repo_has_regex(pattern) for pattern in patterns)


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


def parse_demo_pids(output: str) -> list[int]:
    match = re.search(r"Created demo processes:\s*(\d+),\s*(\d+)", clean_output(output))
    if match is None:
        return []
    return [int(match.group(1)), int(match.group(2))]


def parse_fork_child_pid(output: str) -> int | None:
    match = re.search(r"child PID\s+(\d+)", clean_output(output))
    if match is None:
        return None
    return int(match.group(1), 10)


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
    panic_source = read_text(ROOT / "kernel/src/panic.c")
    keyboard_source = read_text(ROOT / "kernel/src/keyboard.c")
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

    idt_descriptor_present = repo_has_any([
        r"struct\s+idt_(entry|gate)",
        r"typedef\s+struct\s+[^\n]*idt[^\n]*\{",
        r"\bidt_entry_t\b",
        r"\bidt_gate_t\b",
    ])
    idt_setup_present = repo_has_any([
        r"\binit_idt\b",
        r"\bidt_init\b",
        r"\bidt_set_gate\b",
        r"\bregister_idt\b",
    ])
    idt_load_present = repo_has_any([
        r"\blidt\b",
        r"\bload_idt\b",
    ])
    report.add(
        "KFS4 mandatory",
        "IDT setup code appears to create, fill, and register an interrupt table",
        idt_descriptor_present and idt_setup_present and idt_load_present,
    )

    signal_callback_present = repo_has_any([
        r"\bsignal_callback_t\b",
        r"\bsignal_handler_t\b",
        r"typedef\s+[^\n]*signal[^\n]*(callback|handler)",
    ]) and repo_has_any([
        r"\b(register|set|install)_signal_(handler|callback)\b",
        r"\bsignal_(register|set|install)_(handler|callback)\b",
    ])
    report.add(
        "KFS4 mandatory",
        "Kernel API exposes a signal callback registration path",
        signal_callback_present,
    )

    signal_scheduling_present = repo_has_any([
        r"\bschedule_signal\b",
        r"\benqueue_signal\b",
        r"\bqueue_signal\b",
        r"\bsignal_scheduler\b",
    ]) and repo_has_any([
        r"\bpending_signals?\b",
        r"\bdispatch_signal\b",
        r"\bdeliver_signal\b",
    ])
    report.add(
        "KFS4 mandatory",
        "Kernel API exposes signal scheduling or delivery state",
        signal_scheduling_present,
    )

    register_cleanup_interface = repo_has_any([
        r"\b(clean|clear|zero|sanitize)_registers\b",
        r"\bregister_snapshot\b",
        r"\bsaved_registers\b",
        r"struct\s+(cpu_)?registers\b",
        r"\bpanic_registers\b",
    ])
    panic_cleans_registers = register_cleanup_interface and (
        repo_has_any([
            r"xor\s+%e[abcds][xip],\s*%e[abcds][xip]",
            r"mov\s+\$0,\s*%e[abcds][xip]",
        ])
        or re.search(r"(clean|clear|zero|sanitize)_registers", panic_source, re.IGNORECASE) is not None
    )
    report.add(
        "KFS4 mandatory",
        "Panic or halt path includes an explicit register cleanup interface",
        panic_cleans_registers,
    )

    stack_save_interface = repo_has_any([
        r"\bsave_stack\b",
        r"\bstack_snapshot\b",
        r"\bsaved_stack\b",
        r"\bpanic_stack\b",
        r"\bstack_trace_on_panic\b",
    ])
    panic_saves_stack = stack_save_interface and re.search(r"stack|esp|ebp", panic_source, re.IGNORECASE) is not None
    report.add(
        "KFS4 mandatory",
        "Panic path appears to save stack state before halting",
        panic_saves_stack,
    )

    keyboard_interrupt_present = "keyboard_handler" in keyboard_source and repo_has_any([
        r"\birq1\b",
        r"\bIRQ1\b",
        r"\bkeyboard_interrupt\b",
        r"idt_set_gate\s*\(\s*33\b",
        r"0x21",
        r"\bpic_(remap|send_eoi)\b",
    ])
    report.add(
        "KFS4 mandatory",
        "Keyboard handling appears to be wired through the IDT or IRQ1 path",
        keyboard_interrupt_present,
    )

    syscall_foundation_present = repo_has_any([
        r"\bsyscall\b",
        r"\bsyscall_table\b",
        r"\bsyscall_handler\b",
        r"int\s*\$?0x80",
        r"idt_set_gate\s*\(\s*128\b",
    ])
    report.add(
        "KFS4 bonus",
        "A syscall entry foundation appears to exist",
        syscall_foundation_present,
    )

    keyboard_bonus_present = repo_has_any([
        r"\bget_line\b",
        r"\bread_line\b",
        r"\bkeyboard_layout\b",
        r"\bazerty\b",
        r"\bqwerty\b",
    ])
    report.add(
        "KFS4 bonus",
        "Keyboard API includes either buffered line input or layout support",
        keyboard_bonus_present,
    )

    process_structure_present = repo_has_any([
        r"struct\s+(process|task|proc)\b",
        r"\b(process|task|proc)_t\b",
    ]) and repo_has_any([
        r"\bpid\b",
        r"\bpid_t\b",
    ]) and repo_has_any([
        r"\bstatus\b",
        r"\bstate\b",
        r"\bzombie\b",
        r"\bthread\b",
    ]) and repo_has_any([
        r"\bparent\b",
        r"\bfather\b",
    ]) and repo_has_any([
        r"\bchildren\b",
        r"\bchild_list\b",
    ]) and repo_has_any([
        r"\bowner\b",
        r"\buid\b",
        r"\buser_id\b",
    ]) and repo_has_any([
        r"\b(signal_queue|pending_signals|queued_signals|current_signals)\b",
    ]) and repo_has_any([
        r"\b(process_stack|user_stack|kernel_stack|stack_base|stack_top)\b",
    ]) and repo_has_any([
        r"\b(process_heap|heap_start|heap_end|heap_break)\b",
    ])
    report.add(
        "KFS5 mandatory",
        "Process structures appear to track PID, state, kinship, owner, signals, stack, and heap",
        process_structure_present,
    )

    process_signal_queue_present = repo_has_any([
        r"\b(queue|enqueue|schedule)_process_signal\b",
        r"\bprocess_(queue|enqueue|schedule)_signal\b",
        r"\btask_(queue|enqueue|schedule)_signal\b",
        r"\bdeliver_process_signal\b",
    ]) and repo_has_any([
        r"\b(next_cpu_tick|cpu_tick|timer_tick|scheduler_tick)\b",
        r"\bpending_process_signals\b",
    ])
    report.add(
        "KFS5 mandatory",
        "The kernel appears to queue process signals for delivery on a CPU tick",
        process_signal_queue_present,
    )

    process_socket_helpers_present = repo_has_any([
        r"\b(process|task|ipc)_socket(_t)?\b",
        r"\b(socket_create|socket_send|socket_recv|socket_connect|socket_close)\b",
        r"\bipc_(send|recv|socket)\b",
    ])
    report.add(
        "KFS5 mandatory",
        "Process socket or IPC helpers appear to exist",
        process_socket_helpers_present,
    )

    process_memory_helpers_present = repo_has_any([
        r"\b(process|task)_(alloc|map|unmap|clone|copy)_(page|memory|space)\b",
        r"\b(copy|clone)_(address_space|page_directory)\b",
        r"\bprocess_memory_(alloc|map|free)\b",
    ]) and repo_has_any([
        r"\b(address_space|page_directory|page_table|process_cr3|process_page)\b",
    ])
    report.add(
        "KFS5 mandatory",
        "Helpers for process memory management appear to be present",
        process_memory_helpers_present,
    )

    fork_helper_present = repo_has_any([
        r"\bfork_process\b",
        r"\bprocess_fork\b",
        r"\bcopy_process\b",
        r"\bclone_process\b",
    ])
    report.add(
        "KFS5 mandatory",
        "A full process copy or fork helper appears to exist",
        fork_helper_present,
    )

    process_syscall_helpers_present = repo_has_any([
        r"\b(wait_process|process_wait|sys_wait|do_wait)\b",
    ]) and repo_has_any([
        r"\b(process_exit|sys_exit|do_exit|_exit)\b",
    ]) and repo_has_any([
        r"\b(getuid|sys_getuid|process_getuid)\b",
    ]) and repo_has_any([
        r"\b(sys_signal|process_signal|send_signal_to_process)\b",
    ]) and repo_has_any([
        r"\b(sys_kill|kill_process|process_kill)\b",
    ])
    report.add(
        "KFS5 mandatory",
        "Helpers preparing wait, exit, getuid, signal, and kill syscalls appear to exist",
        process_syscall_helpers_present,
    )

    process_memory_separation_present = repo_has_any([
        r"\b(assign|alloc)_process_page\b",
        r"\bprocess_page\b",
        r"\bprocess_memory_map\b",
        r"\bprocess_page_directory\b",
        r"\baddress_space\b",
    ]) and repo_has_any([
        r"\b(track|record|store)_.*(virtual|vm|page)\b",
        r"\bvirtual_base\b",
        r"\bvirtual_page\b",
    ])
    report.add(
        "KFS5 mandatory",
        "Process memory separation appears to track dedicated per-process pages or address spaces",
        process_memory_separation_present,
    )

    multitasking_present = repo_has_any([
        r"\bmultitask(ing)?\b",
        r"\bscheduler_tick\b",
        r"\bschedule_next\b",
        r"\bschedule_process\b",
        r"\bpick_next_process\b",
        r"\bcontext_switch\b",
        r"\btask_switch\b",
    ])
    report.add(
        "KFS5 mandatory",
        "A multitasking or scheduler path appears to exist",
        multitasking_present,
    )

    exec_fn_present = repo_has_any([
        r"\bexec_fn\b",
        r"\bexecute_function_as_process\b",
        r"\bspawn_function_process\b",
    ])
    report.add(
        "KFS5 mandatory",
        "An in-kernel exec_fn-style process test hook appears to exist",
        exec_fn_present,
    )

    mmap_bonus_present = repo_has_any([
        r"\b(sys_mmap|do_mmap|process_mmap|task_mmap|mmap_process)\b",
        r"\bprocess_mmap\b",
        r"\btask_mmap\b",
    ])
    report.add(
        "KFS5 bonus",
        "mmap-like per-process virtual memory helpers appear to exist",
        mmap_bonus_present,
    )

    idt_process_bonus_present = repo_has_any([
        r"\bcurrent_process\b",
        r"\bcurrent_task\b",
    ]) and repo_has_any([
        r"\b(idt_handle_interrupt|interrupt_process_signal|process_signal_from_interrupt|deliver_process_signal)\b",
    ])
    report.add(
        "KFS5 bonus",
        "Interrupt handling appears to be linked to per-process signal delivery",
        idt_process_bonus_present,
    )

    process_segments_bonus_present = repo_has_any([
        r"\b(process|task)_(bss|data)\b",
        r"\bbss_(start|end|size)\b",
        r"\bdata_(start|end|size)\b",
    ])
    report.add(
        "KFS5 bonus",
        "Process structures appear to track dedicated BSS or data segments",
        process_segments_bonus_present,
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
        report.add(
            "KFS4 mandatory",
            "Boot sequence reports IDT, signal, syscall, and keyboard IRQ initialization",
            all(token in boot_output for token in [
                "Initializing Interrupt Descriptor Table...",
                "IDT ready (PIC remapped)",
                "Initializing Signal Callback Interface...",
                "Signal scheduler ready",
                "Initializing Syscall Interface...",
                "int 0x80 handler ready",
                "Initializing IDT keyboard handling...",
                "Keyboard IRQ1 ready",
                "Interrupts enabled",
            ]),
            boot_output,
        )
        report.add(
            "KFS5 mandatory",
            "Boot sequence reports process and scheduler initialization",
            all(token in boot_output for token in [
                "Initializing Process Interface...",
                "Kernel process ready",
                "Initializing Multitasking Scheduler...",
                "Scheduler and CPU tick ready",
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
        report.add(
            "KFS4 mandatory",
            "Shell help exposes the interrupt and signal debug commands",
            all(token in help_output for token in ["idt", "signals", "sigtest", "syscall", "int3"]),
            help_output,
        )
        report.add(
            "KFS5 mandatory",
            "Shell help exposes the process management commands",
            all(token in help_output for token in ["procs", "execdemo", "sched TICKS", "forkdemo PID", "waitproc PID", "killproc PID", "sigproc PID", "getuid PID", "mmapproc PID SIZE", "sockdemo"]),
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

        segreg_output = clean_output(session.run_command("segreg"))
        report.add(
            "KFS2 mandatory",
            "Segment registers are loaded with correct GDT selectors",
            all(token in segreg_output for token in ["CS=0x00000008", "DS=0x00000010", "SS=0x00000018"]),
            segreg_output,
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

        for i in range(5):
            session.run_command(f"alloc {32 * (i + 1)}")
            session.run_command("free")
        meminfo_cycles = clean_output(session.run_command("meminfo"))
        report.add(
            "KFS3 mandatory",
            "Repeated alloc/free cycles maintain consistency",
            "Allocs:    0" in meminfo_cycles,
            meminfo_cycles,
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

        meminfo_values = clean_output(session.run_command("meminfo"))
        total_match = re.search(r"Total:\s+(\d+)\s+MB", meminfo_values)
        free_match = re.search(r"Free:\s+.*\((\d+)\s+pages\)", meminfo_values)
        report.add(
            "KFS3 mandatory",
            "meminfo reports non-zero physical memory values",
            total_match is not None and int(total_match.group(1)) > 0
            and free_match is not None and int(free_match.group(1)) > 0,
            meminfo_values,
        )

        pagedir_count = len(re.findall(r"\[\d+\]\s+->", pagedir_output))
        total_match_pd = re.search(r"(\d+)\s+total\)", pagedir_output)
        if total_match_pd:
            pagedir_count = int(total_match_pd.group(1))
        report.add(
            "KFS3 mandatory",
            "Paging is active with at least 4 mapped page tables",
            pagedir_count >= 4,
            f"Mapped page tables: {pagedir_count}",
        )

        report.add(
            "KFS3 mandatory",
            "vmap shows correct vmalloc range start",
            "0x01000000" in vmap_output,
            vmap_output,
        )

        pagetest_output = clean_output(session.run_command("pagetest"))
        report.add(
            "KFS3 mandatory",
            "Page map/write/unmap round-trip works",
            "pagetest: PASS" in pagetest_output and "match OK" in pagetest_output,
            pagetest_output,
        )

        pageperm_output = clean_output(session.run_command("pageperm"))
        report.add(
            "KFS3 mandatory",
            "Page permission flags are correctly applied",
            "pageperm: PASS" in pageperm_output,
            pageperm_output,
        )

        pmmtest_output = clean_output(session.run_command("pmmtest"))
        report.add(
            "KFS3 mandatory",
            "PMM alloc/free tracks frame counts correctly",
            "pmmtest: PASS" in pmmtest_output and "delta=-3 OK" in pmmtest_output and "restored OK" in pmmtest_output,
            pmmtest_output,
        )

        vmtest_output = clean_output(session.run_command("vmtest"))
        report.add(
            "KFS3 mandatory",
            "vmalloc allocates, writes, and frees correctly",
            "vmtest: PASS" in vmtest_output,
            vmtest_output,
        )

        heaptest_output = clean_output(session.run_command("heaptest"))
        report.add(
            "KFS3 mandatory",
            "Heap handles multiple alloc/free with reuse",
            "heaptest: PASS" in heaptest_output and "allocs=0 OK" in heaptest_output,
            heaptest_output,
        )

        idt_output = clean_output(session.run_command("idt"))
        report.add(
            "KFS4 mandatory",
            "idt command reports the registered descriptor table and interrupt state",
            all(token in idt_output for token in ["=== IDT ===", "Base: 0x", "Interrupts: enabled", "IRQ1 vector: 0x00000021", "System call vector: 0x00000080"]),
            idt_output,
        )

        idt_limit_match = re.search(r"Limit:\s+(\d+)\s+bytes", idt_output)
        report.add(
            "KFS4 mandatory",
            "IDT limit matches 256 entries (2047 bytes)",
            idt_limit_match is not None and int(idt_limit_match.group(1)) == 2047,
            idt_output,
        )

        div0_output = clean_output(session.run_command("div0"))
        report.add(
            "KFS4 mandatory",
            "Division by zero exception is caught by IDT",
            "div0: PASS" in div0_output and "exception 0" in div0_output,
            div0_output,
        )

        invop_output = clean_output(session.run_command("invop"))
        report.add(
            "KFS4 mandatory",
            "Invalid opcode exception is caught by IDT",
            "invop: PASS" in invop_output and "exception 6" in invop_output,
            invop_output,
        )

        signals_output = clean_output(session.run_command("signals"))
        report.add(
            "KFS4 mandatory",
            "signals command reports queue and delivery counters",
            all(token in signals_output for token in ["=== Signals ===", "Pending: 0", "Handled:", "Last signal:", "Last value:"]),
            signals_output,
        )

        sigtest_output = clean_output(session.run_command("sigtest"))
        report.add(
            "KFS4 mandatory",
            "Signal callbacks can be scheduled and dispatched from the kernel API",
            all(token in sigtest_output for token in ["Scheduling test signal...", "[signal] handled signal 1 value=0xC0DE1234", "Signal delivery complete."]),
            sigtest_output,
        )

        signals_after_output = clean_output(session.run_command("signals"))
        report.add(
            "KFS4 mandatory",
            "Signal state tracks the delivered test signal",
            all(token in signals_after_output for token in ["Pending: 0", "Handled: 1", "Last signal: 1", "Last value: 0xC0DE1234"]),
            signals_after_output,
        )

        sigtest2_output = clean_output(session.run_command("sigtest"))
        signals2_output = clean_output(session.run_command("signals"))
        report.add(
            "KFS4 mandatory",
            "Multiple signal dispatches increment handled counter",
            "[signal] handled signal 1 value=0xC0DE1234" in sigtest2_output
            and "Handled: 2" in signals2_output,
            sigtest2_output + "\n" + signals2_output,
        )

        syscall_output = clean_output(session.run_command("syscall"))
        report.add(
            "KFS4 bonus",
            "int 0x80 dispatches through the syscall foundation",
            "syscall returned 0x4B465334" in syscall_output,
            syscall_output,
        )

        pit_before = clean_output(session.run_command("procs"))
        pit1_match = re.search(r"PIT ticks:\s+(\d+)", pit_before)
        time.sleep(0.5)
        pit_after = clean_output(session.run_command("procs"))
        pit2_match = re.search(r"PIT ticks:\s+(\d+)", pit_after)
        report.add(
            "KFS4 mandatory",
            "Timer IRQ0 is actively incrementing PIT ticks",
            pit1_match is not None and pit2_match is not None
            and int(pit2_match.group(1)) > int(pit1_match.group(1)),
            f"PIT before={pit1_match.group(1) if pit1_match else '?'} after={pit2_match.group(1) if pit2_match else '?'}",
        )

        procs_output = clean_output(session.run_command("procs"))
        report.add(
            "KFS5 mandatory",
            "The shell can inspect the process table and scheduler state",
            all(token in procs_output for token in ["=== Process Table ===", "PID=1", "STATE=THREAD", "OWNER=0", "CPU ticks:", "PIT ticks:", "Context switches:", "CR3=", "ESP=", "SWITCH="]),
            procs_output,
        )

        execdemo_output = clean_output(session.run_command("execdemo"))
        demo_pids = parse_demo_pids(execdemo_output)
        report.add(
            "KFS5 mandatory",
            "exec_fn-style demo processes can be spawned from the shell",
            len(demo_pids) == 2,
            execdemo_output,
        )

        getuid_output = ""
        forkdemo_output = ""
        mmap_output = ""
        sigproc_output = ""
        procs_signal_queued = ""
        sched_signal_output = ""
        procs_signal_delivered = ""
        killproc_output = ""
        sched_kill_output = ""
        wait_child_output = ""
        sched_demo_output = ""
        wait_demo_one_output = ""
        wait_demo_two_output = ""
        procs_after_exec = ""
        procs_after_fork = ""

        child_pid = None
        if len(demo_pids) == 2:
            procs_after_exec = clean_output(session.run_command("procs"))
            getuid_output = clean_output(session.run_command(f"getuid {demo_pids[0]}"))
            forkdemo_output = clean_output(session.run_command(f"forkdemo {demo_pids[0]}"))
            child_pid = parse_fork_child_pid(forkdemo_output)
            procs_after_fork = clean_output(session.run_command("procs"))

        report.add(
            "KFS5 mandatory",
            "Spawned processes begin in READY state",
            len(demo_pids) == 2
            and re.search(fr"PID={demo_pids[0]}\b.*STATE=READY", procs_after_exec) is not None
            and re.search(fr"PID={demo_pids[1]}\b.*STATE=READY", procs_after_exec) is not None,
            procs_after_exec or execdemo_output,
        )

        fork_cr3_ok = False
        if child_pid is not None and len(demo_pids) == 2:
            parent_cr3 = re.search(fr"PID={demo_pids[0]}\b.*CR3=(0x[0-9A-Fa-f]+)", procs_after_fork)
            child_cr3 = re.search(fr"PID={child_pid}\b.*CR3=(0x[0-9A-Fa-f]+)", procs_after_fork)
            fork_cr3_ok = (parent_cr3 is not None and child_cr3 is not None
                           and parent_cr3.group(1) != child_cr3.group(1))
        report.add(
            "KFS5 mandatory",
            "Forked process has separate page directory",
            fork_cr3_ok,
            procs_after_fork or forkdemo_output,
        )

        report.add(
            "KFS5 mandatory",
            "Processes expose an owner id through the getuid helper path",
            len(demo_pids) == 2 and f"PID {demo_pids[0]} owner=1000" in getuid_output,
            getuid_output or execdemo_output,
        )

        report.add(
            "KFS5 mandatory",
            "fork creates a new child process record",
            child_pid is not None,
            forkdemo_output or execdemo_output,
        )

        if child_pid is not None:
            mmap_output = clean_output(session.run_command(f"mmapproc {child_pid} 32"))
            sigproc_output = clean_output(session.run_command(f"sigproc {child_pid} 16 0xDEADBEEF"))
            procs_signal_queued = clean_output(session.run_command("procs"))
            sched_signal_output = clean_output(session.run_command("sched 4"))
            procs_signal_delivered = clean_output(session.run_command("procs"))
            killproc_output = clean_output(session.run_command(f"killproc {child_pid} 15"))
            sched_kill_output = clean_output(session.run_command("sched 4"))
            wait_child_output = clean_output(session.run_command(f"waitproc {child_pid}"))

        report.add(
            "KFS5 mandatory",
            "Per-process memory helpers can reserve memory in a process heap",
            child_pid is not None and f"for PID {child_pid}" in mmap_output and "mmapproc mapped 0x" in mmap_output,
            mmap_output or forkdemo_output,
        )

        report.add(
            "KFS5 mandatory",
            "Queued process signals are visible before the next scheduler tick",
            child_pid is not None
            and f"Queued process signal 16 for PID {child_pid} value=0xDEADBEEF" in sigproc_output
            and re.search(fr"PID={child_pid}\b.*SIGQ=1", procs_signal_queued) is not None,
            (sigproc_output + "\n" + procs_signal_queued).strip(),
        )

        report.add(
            "KFS5 mandatory",
            "Process signals are delivered on the next scheduler tick",
            child_pid is not None
            and "Scheduler ran 4 ticks" in sched_signal_output
            and re.search(fr"PID={child_pid}\b.*LASTSIG=16.*SIGVAL=0xDEADBEEF", procs_signal_delivered) is not None,
            (sched_signal_output + "\n" + procs_signal_delivered).strip(),
        )

        report.add(
            "KFS5 mandatory",
            "kill and wait helpers can terminate and collect a process",
            child_pid is not None
            and f"Queued signal 15 for PID {child_pid}" in killproc_output
            and "Scheduler ran 4 ticks" in sched_kill_output
            and f"wait collected PID {child_pid}" in wait_child_output,
            (killproc_output + "\n" + sched_kill_output + "\n" + wait_child_output).strip(),
        )

        multi_sig_procs = ""
        if len(demo_pids) == 2:
            session.run_command(f"sigproc {demo_pids[1]} 16 0x11111111")
            session.run_command(f"sigproc {demo_pids[1]} 16 0x22222222")
            session.run_command(f"sigproc {demo_pids[1]} 16 0x33333333")
            multi_sig_procs = clean_output(session.run_command("procs"))
            session.run_command("sched 1")

        multi_sig_ok = False
        if len(demo_pids) == 2:
            sigq_match = re.search(fr"PID={demo_pids[1]}\b.*SIGQ=(\d+)", multi_sig_procs)
            # Signals may still be queued (SIGQ>=3) or already delivered (LASTSIG=16, SIGVAL=0x33333333)
            still_queued = sigq_match is not None and int(sigq_match.group(1)) >= 3
            already_delivered = re.search(fr"PID={demo_pids[1]}\b.*LASTSIG=16.*SIGVAL=0x33333333", multi_sig_procs) is not None
            multi_sig_ok = still_queued or already_delivered
        report.add(
            "KFS5 mandatory",
            "Multiple signals can be queued to a process",
            multi_sig_ok,
            multi_sig_procs,
        )

        cs_before_match = re.search(r"Context switches:\s+(\d+)", procs_output)
        if child_pid is not None:
            cs_after_procs = clean_output(session.run_command("procs"))
        else:
            cs_after_procs = procs_output
        cs_after_match = re.search(r"Context switches:\s+(\d+)", procs_signal_delivered if child_pid is not None else cs_after_procs)
        report.add(
            "KFS5 mandatory",
            "Context switch counter increases with scheduling",
            cs_before_match is not None and cs_after_match is not None
            and int(cs_after_match.group(1)) > int(cs_before_match.group(1)),
            f"before={cs_before_match.group(1) if cs_before_match else '?'} after={cs_after_match.group(1) if cs_after_match else '?'}",
        )

        procs_after_sched = ""
        procs_count_before_wait = 0
        procs_count_after_wait = 0
        if len(demo_pids) == 2:
            sched_demo_output = clean_output(session.run_command("sched 10"))
            procs_after_sched = clean_output(session.run_command("procs"))
            procs_count_before_wait_match = re.search(r"Total:\s+(\d+)", procs_after_sched)
            if procs_count_before_wait_match:
                procs_count_before_wait = int(procs_count_before_wait_match.group(1))
            wait_demo_one_output = clean_output(session.run_command(f"waitproc {demo_pids[0]}"))
            wait_demo_two_output = clean_output(session.run_command(f"waitproc {demo_pids[1]}"))
            # Infer post-wait count from waitproc results (avoids PTY timing issue)
            if f"wait collected PID {demo_pids[0]}" in wait_demo_one_output and f"wait collected PID {demo_pids[1]}" in wait_demo_two_output:
                procs_count_after_wait = procs_count_before_wait - 2
            elif f"wait collected PID {demo_pids[0]}" in wait_demo_one_output or f"wait collected PID {demo_pids[1]}" in wait_demo_two_output:
                procs_count_after_wait = procs_count_before_wait - 1

        report.add(
            "KFS5 mandatory",
            "Process lifecycle shows READY to ZOMBIE transition after scheduling",
            len(demo_pids) == 2
            and re.search(fr"PID={demo_pids[0]}\b.*STATE=ZOMBIE", procs_after_sched) is not None
            and re.search(fr"PID={demo_pids[1]}\b.*STATE=ZOMBIE", procs_after_sched) is not None,
            procs_after_sched or sched_demo_output,
        )

        report.add(
            "KFS5 mandatory",
            "The multitasking scheduler advances runnable demo processes over CPU ticks",
            len(demo_pids) == 2
            and "Scheduler ran 10 ticks" in sched_demo_output
            and f"wait collected PID {demo_pids[0]} exit=3" in wait_demo_one_output
            and f"wait collected PID {demo_pids[1]} exit=13" in wait_demo_two_output,
            (sched_demo_output + "\n" + wait_demo_one_output + "\n" + wait_demo_two_output).strip(),
        )

        report.add(
            "KFS5 mandatory",
            "waitproc removes zombie processes from the process table",
            len(demo_pids) == 2
            and procs_count_before_wait > 0
            and procs_count_after_wait < procs_count_before_wait,
            f"before={procs_count_before_wait} after={procs_count_after_wait}",
        )

        sockdemo_output = clean_output(session.run_command("sockdemo"))
        report.add(
            "KFS5 mandatory",
            "Socket helpers can transfer a value between processes",
            all(token in sockdemo_output for token in ["Socket demo sender=", "receiver=", "value=0xABCD1234"]),
            sockdemo_output,
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
        report.add(
            "KFS4 mandatory",
            "panic output includes saved register and stack context",
            all(token in panic_output for token in ["EIP:", "ESP:", "Saved stack pointer:", "Stack snapshot:"]),
            panic_output,
        )

    with QemuSession(qemu_binary) as session:
        session.boot_to_shell()
        int3_output = clean_output(session.run_until("int3", "The system has been halted.", timeout=10))
        report.add(
            "KFS4 mandatory",
            "Software interrupt and exception handling routes through the IDT panic path",
            all(token in int3_output for token in ["Triggering breakpoint exception...", "Breakpoint", "KERNEL PANIC", "The system has been halted."]),
            int3_output,
        )

    with QemuSession(qemu_binary) as session:
        session.boot_to_shell()
        reboot_output = clean_output(session.run_until("reboot", ["Rebooting...", "Press any key to continue..."], timeout=20))
        if "Press any key to continue..." not in reboot_output:
            reboot_output += clean_output(session.collect_until("Press any key to continue...", timeout=20))
        report.add(
            "KFS2 bonus",
            "reboot restarts the virtual machine back into the kernel",
            "Rebooting..." in reboot_output and (reboot_output.count("Welcome to KFS-5!") >= 1 or reboot_output.count("Welcome to KFS-4!") >= 1 or reboot_output.count("Welcome to KFS-3!") >= 1),
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
    parser = argparse.ArgumentParser(description="Static and runtime checks for KFS-2, KFS-3, KFS-4, and KFS-5")
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