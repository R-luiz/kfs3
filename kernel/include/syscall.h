#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

#define SYSCALL_MAX_HANDLERS    8
#define SYSCALL_GET_MAGIC       0
#define SYSCALL_FORK            1
#define SYSCALL_WAIT            2
#define SYSCALL_EXIT            3
#define SYSCALL_GETUID          4
#define SYSCALL_SIGNAL          5
#define SYSCALL_KILL            6

uint32_t sys_fork(uint32_t pid_hint, uint32_t unused1, uint32_t unused2, uint32_t unused3);
uint32_t sys_wait(uint32_t pid, uint32_t unused1, uint32_t unused2, uint32_t unused3);
uint32_t sys_exit(uint32_t exit_code, uint32_t unused1, uint32_t unused2, uint32_t unused3);
uint32_t sys_getuid(uint32_t unused0, uint32_t unused1, uint32_t unused2, uint32_t unused3);
uint32_t sys_signal(uint32_t pid, uint32_t signal, uint32_t value, uint32_t unused3);
uint32_t sys_kill(uint32_t pid, uint32_t signal, uint32_t unused2, uint32_t unused3);

typedef uint32_t (*syscall_handler_t)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3);

void syscall_init(void);
int syscall_register(uint32_t number, syscall_handler_t handler);
uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3);

#endif /* SYSCALL_H */