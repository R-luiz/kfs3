#include "syscall.h"
#include "idt.h"
#include "process.h"

static syscall_handler_t syscall_table[SYSCALL_MAX_HANDLERS];

static uint32_t syscall_get_magic(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0x4B465334;
}

uint32_t sys_fork(uint32_t pid_hint, uint32_t unused1, uint32_t unused2, uint32_t unused3)
{
    process_t *source = process_current();
    process_t *child;

    (void)unused1;
    (void)unused2;
    (void)unused3;

    if (pid_hint != 0) {
        source = process_get_by_pid(pid_hint);
    }

    child = fork_process(source);
    return child != NULL ? child->pid : 0xFFFFFFFF;
}

uint32_t sys_wait(uint32_t pid, uint32_t unused1, uint32_t unused2, uint32_t unused3)
{
    process_t *parent = process_current();
    int exit_code = 0;
    int result;

    (void)unused1;
    (void)unused2;
    (void)unused3;

    if (parent == NULL) {
        parent = process_root();
    }

    result = process_wait(parent, pid, &exit_code);
    if (result < 0) {
        return 0xFFFFFFFF;
    }
    if (result == 0) {
        return 0;
    }
    return (uint32_t)exit_code;
}

uint32_t sys_exit(uint32_t exit_code, uint32_t unused1, uint32_t unused2, uint32_t unused3)
{
    (void)unused1;
    (void)unused2;
    (void)unused3;
    process_exit(process_current(), (int)exit_code);
    return exit_code;
}

uint32_t sys_getuid(uint32_t unused0, uint32_t unused1, uint32_t unused2, uint32_t unused3)
{
    (void)unused0;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    return process_getuid(process_current());
}

uint32_t sys_signal(uint32_t pid, uint32_t signal, uint32_t value, uint32_t unused3)
{
    (void)unused3;
    return send_signal_to_process(process_current(), pid, (int)signal, value) == 0 ? 0 : 0xFFFFFFFF;
}

uint32_t sys_kill(uint32_t pid, uint32_t signal, uint32_t unused2, uint32_t unused3)
{
    (void)unused2;
    (void)unused3;
    return kill_process(process_current(), pid, (int)signal) == 0 ? 0 : 0xFFFFFFFF;
}

static uint32_t syscall_not_implemented(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0xFFFFFFFF;
}

static void syscall_handler(cpu_registers_t *registers)
{
    registers->eax = syscall_dispatch(registers->eax, registers->ebx, registers->ecx, registers->edx, registers->esi);
}

void syscall_init(void)
{
    uint32_t index;

    for (index = 0; index < SYSCALL_MAX_HANDLERS; index++) {
        syscall_table[index] = syscall_not_implemented;
    }

    syscall_table[SYSCALL_GET_MAGIC] = syscall_get_magic;
    syscall_table[SYSCALL_FORK] = sys_fork;
    syscall_table[SYSCALL_WAIT] = sys_wait;
    syscall_table[SYSCALL_EXIT] = sys_exit;
    syscall_table[SYSCALL_GETUID] = sys_getuid;
    syscall_table[SYSCALL_SIGNAL] = sys_signal;
    syscall_table[SYSCALL_KILL] = sys_kill;
    idt_register_handler(SYSCALL_VECTOR, syscall_handler);
}

int syscall_register(uint32_t number, syscall_handler_t handler)
{
    if (number >= SYSCALL_MAX_HANDLERS || handler == NULL) {
        return -1;
    }

    syscall_table[number] = handler;
    return 0;
}

uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    if (number >= SYSCALL_MAX_HANDLERS) {
        return 0xFFFFFFFF;
    }

    return syscall_table[number](arg0, arg1, arg2, arg3);
}