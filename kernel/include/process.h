#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "paging.h"

#define PROCESS_MAX_COUNT                  16
#define PROCESS_SIGNAL_QUEUE_CAPACITY      16
#define PROCESS_SOCKET_CAPACITY            8
#define PROCESS_PAGE_REGION_BASE           0x02000000U
#define PROCESS_VIRTUAL_STRIDE             0x00020000U

#define PROCESS_RIGHT_SIGNAL               0x00000001U
#define PROCESS_RIGHT_WAIT                 0x00000002U
#define PROCESS_RIGHT_MEMORY               0x00000004U
#define PROCESS_RIGHT_ALL                  0xFFFFFFFFU

#define PROCESS_SIGNAL_KILL                9
#define PROCESS_SIGNAL_TERM                15
#define PROCESS_SIGNAL_USER                16
#define PROCESS_SIGNAL_SOCKET              17

typedef enum process_status {
    PROCESS_STATUS_READY = 0,
    PROCESS_STATUS_RUN = 1,
    PROCESS_STATUS_WAITING = 2,
    PROCESS_STATUS_ZOMBIE = 3,
    PROCESS_STATUS_THREAD = 4,
} process_status_t;

typedef struct process_pending_signal {
    int signal;
    uint32_t value;
} process_pending_signal_t;

typedef struct process_socket {
    uint32_t values[PROCESS_SOCKET_CAPACITY];
    uint32_t head;
    uint32_t tail;
} process_socket_t;

typedef struct process_context {
    uint32_t ebx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t eip;
} process_context_t;

struct process;
typedef struct process process_t;
typedef void (*process_exec_fn_t)(process_t *process);
typedef void (*process_signal_handler_t)(process_t *process, int signal, uint32_t value);

typedef struct process_memory {
    page_directory_t *process_page_directory;
    uint32_t page_size;
    uint32_t virtual_base;
    uint32_t virtual_page;
    uint32_t process_page;
    uint32_t data_start;
    uint32_t data_end;
    uint32_t bss_start;
    uint32_t bss_end;
    uint32_t heap_start;
    uint32_t heap_break;
    uint32_t heap_end;
    uint32_t stack_base;
    uint32_t stack_top;
    uint32_t physical_page;
    uint32_t data_page;
    uint32_t bss_page;
    uint32_t heap_page;
    uint32_t stack_page;
    uint32_t process_page_table;
    uint32_t code_size;
} process_memory_t;

struct process {
    uint32_t pid;
    process_status_t status;
    uint32_t owner;
    uint32_t rights;
    process_t *parent;
    process_t *children;
    process_t *next_sibling;
    process_t *next;
    process_memory_t memory;
    process_pending_signal_t pending_process_signals[PROCESS_SIGNAL_QUEUE_CAPACITY];
    uint32_t signal_head;
    uint32_t signal_tail;
    process_socket_t process_socket;
    process_exec_fn_t entry;
    process_signal_handler_t signal_handler;
    process_context_t context;
    uint32_t cpu_ticks;
    uint32_t run_count;
    uint32_t switch_count;
    uint32_t last_signal;
    uint32_t last_signal_value;
    uint32_t last_socket_value;
    uint32_t runtime_state;
    uint32_t runtime_value;
    uint32_t runtime_arg0;
    uint32_t runtime_arg1;
    int exit_code;
    uint32_t wait_target_pid;
    int used;
};

void process_init(void);
process_t *process_root(void);
process_t *process_current(void);
process_t *process_first(void);
process_t *process_get_by_pid(uint32_t pid);
uint32_t process_count(void);
const char *process_status_name(process_status_t status);

process_t *process_create_owned(uint32_t owner, process_exec_fn_t function, uint32_t size);
process_t *exec_fn(uint32_t addr, process_exec_fn_t function, uint32_t size);
process_t *fork_process(process_t *source);
void process_yield(void);
uint32_t process_context_switch_count(void);

int alloc_process_page(process_t *process, uint32_t virtual_page, uint32_t *physical_page);
int process_memory_map(process_t *process);
int process_memory_alloc(process_t *process, uint32_t size);
void *process_mmap(process_t *process, uint32_t size);

int process_signal(process_t *process, process_signal_handler_t handler);
int queue_process_signal(process_t *process, int signal, uint32_t value);
int deliver_process_signal(process_t *process);
int send_signal_to_process(process_t *sender, uint32_t pid, int signal, uint32_t value);
int kill_process(process_t *sender, uint32_t pid, int signal);
int process_kill(process_t *sender, uint32_t pid, int signal);

int socket_create(process_t *process);
int socket_send(process_t *sender, uint32_t pid, uint32_t value);
int socket_recv(process_t *process, uint32_t *value);
uint32_t process_signal_pending_count(const process_t *process);
uint32_t process_socket_pending_count(const process_t *process);

int process_wait(process_t *parent, uint32_t pid, int *exit_code);
int wait_process(process_t *parent, uint32_t pid, int *exit_code);
void process_exit(process_t *process, int exit_code);
uint32_t process_getuid(process_t *process);

void scheduler_init(void);
void scheduler_tick(void);
process_t *pick_next_process(void);
process_t *schedule_next_process(void);
uint32_t scheduler_run_pending(uint32_t max_ticks);
uint32_t scheduler_force_ticks(uint32_t count);
uint32_t scheduler_tick_count(void);
int multitasking_enabled(void);

void process_context_switch(process_context_t *from, process_context_t *to);

process_t *process_spawn_demo_counter(uint32_t owner, uint32_t seed, uint32_t limit);
int process_spawn_demo_socket_pair(uint32_t owner, uint32_t value, uint32_t *sender_pid, uint32_t *receiver_pid);

#endif /* PROCESS_H */