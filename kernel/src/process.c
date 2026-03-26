#include "process.h"
#include "io.h"
#include "pmm.h"
#include "paging.h"

static process_t process_table[PROCESS_MAX_COUNT];
static page_directory_t process_page_directories[PROCESS_MAX_COUNT] __attribute__((aligned(4096)));
static process_context_t scheduler_context;
static process_t *process_head = NULL;
static process_t *current_process = NULL;
static process_t *kernel_process = NULL;
static process_t *scheduler_cursor = NULL;
static uint32_t next_pid = 1;
static uint32_t pending_scheduler_ticks = 0;
static uint32_t total_scheduler_ticks = 0;
static uint32_t total_context_switches = 0;
static int scheduler_enabled = 0;

static void process_bootstrap(void);

static void *process_memset(void *ptr, int value, size_t size)
{
    uint8_t *cursor = (uint8_t *)ptr;

    while (size-- > 0) {
        *cursor++ = (uint8_t)value;
    }

    return ptr;
}

static void *process_memcpy(void *dest, const void *src, size_t size)
{
    uint8_t *destination = (uint8_t *)dest;
    const uint8_t *source = (const uint8_t *)src;

    while (size-- > 0) {
        *destination++ = *source++;
    }

    return dest;
}

static uint32_t process_slot_index(const process_t *process)
{
    return (uint32_t)(process - process_table);
}

static uint32_t process_virtual_base_for_slot(uint32_t slot)
{
    return PROCESS_PAGE_REGION_BASE + slot * PROCESS_VIRTUAL_STRIDE;
}

static process_t *process_allocate_slot(void)
{
    uint32_t slot;

    for (slot = 1; slot < PROCESS_MAX_COUNT; slot++) {
        if (!process_table[slot].used) {
            process_memset(&process_table[slot], 0, sizeof(process_t));
            process_table[slot].used = 1;
            return &process_table[slot];
        }
    }

    return NULL;
}

static void process_link_global(process_t *process)
{
    process_t *tail;

    if (process_head == NULL) {
        process_head = process;
        return;
    }

    tail = process_head;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = process;
}

static void process_unlink_global(process_t *process)
{
    process_t *cursor = process_head;
    process_t *previous = NULL;

    while (cursor != NULL) {
        if (cursor == process) {
            if (previous == NULL) {
                process_head = cursor->next;
            } else {
                previous->next = cursor->next;
            }
            process->next = NULL;
            return;
        }
        previous = cursor;
        cursor = cursor->next;
    }
}

static void process_link_child(process_t *parent, process_t *child)
{
    child->parent = parent;
    child->next_sibling = NULL;

    if (parent == NULL) {
        return;
    }

    if (parent->children == NULL) {
        parent->children = child;
        return;
    }

    child->next_sibling = parent->children;
    parent->children = child;
}

static void process_unlink_child(process_t *process)
{
    process_t *cursor;
    process_t *previous = NULL;

    if (process == NULL || process->parent == NULL) {
        return;
    }

    cursor = process->parent->children;
    while (cursor != NULL) {
        if (cursor == process) {
            if (previous == NULL) {
                process->parent->children = cursor->next_sibling;
            } else {
                previous->next_sibling = cursor->next_sibling;
            }
            process->next_sibling = NULL;
            process->parent = NULL;
            return;
        }
        previous = cursor;
        cursor = cursor->next_sibling;
    }
}

static void process_reset_memory(process_memory_t *memory)
{
    process_memset(memory, 0, sizeof(process_memory_t));
    memory->page_size = PAGE_SIZE;
}

static void process_release_memory(process_t *process)
{
    if (process->memory.data_page != 0) {
        paging_unmap_page_in_directory(process->memory.process_page_directory, process->memory.data_start);
        pmm_free_frame(process->memory.data_page);
    }
    if (process->memory.bss_page != 0) {
        paging_unmap_page_in_directory(process->memory.process_page_directory, process->memory.bss_start);
        pmm_free_frame(process->memory.bss_page);
    }
    if (process->memory.heap_page != 0) {
        paging_unmap_page_in_directory(process->memory.process_page_directory, process->memory.heap_start);
        pmm_free_frame(process->memory.heap_page);
    }
    if (process->memory.stack_page != 0) {
        paging_unmap_page_in_directory(process->memory.process_page_directory, process->memory.stack_base);
        pmm_free_frame(process->memory.stack_page);
    }
    if (process->memory.process_page_directory != NULL && process->memory.process_page_table != 0) {
        uint32_t dir_idx = PAGE_DIR_INDEX(process->memory.virtual_base);
        process->memory.process_page_directory->entries[dir_idx] = 0;
        pmm_free_frame(process->memory.process_page_table);
    }

    process_reset_memory(&process->memory);
}

static void process_release(process_t *process)
{
    if (process == NULL || process == kernel_process) {
        return;
    }

    if (scheduler_cursor == process) {
        scheduler_cursor = process_head;
    }

    process_release_memory(process);
    process_unlink_child(process);
    process_unlink_global(process);
    process_memset(process, 0, sizeof(process_t));
}

static int process_can_signal(const process_t *sender, const process_t *target)
{
    if (target == NULL) {
        return 0;
    }
    if (sender == NULL) {
        return 1;
    }
    if (sender->owner == 0 || sender == target) {
        return 1;
    }
    return sender->owner == target->owner;
}

static void process_default_signal_handler(process_t *process, int signal, uint32_t value)
{
    process->last_signal = (uint32_t)signal;
    process->last_signal_value = value;

    if (signal == PROCESS_SIGNAL_TERM || signal == PROCESS_SIGNAL_KILL) {
        process_exit(process, 128 + signal);
    }
}

static void process_switch_to_kernel_space(void)
{
    if (kernel_process != NULL && kernel_process->memory.process_page_directory != NULL) {
        paging_switch_directory(kernel_process->memory.process_page_directory);
    }
}

static void process_bootstrap(void)
{
    process_t *process = current_process;

    if (process == NULL || process == kernel_process || process->entry == NULL) {
        process_switch_to_kernel_space();
        process_context_switch(&scheduler_context, &scheduler_context);
        return;
    }

    process->entry(process);
    process_exit(process, (int)process->runtime_value);
    process_yield();

    while (1) {
    }
}

static void process_initialize_context(process_t *process)
{
    uint32_t stack_pointer;

    process_memset(&process->context, 0, sizeof(process_context_t));
    stack_pointer = (process->memory.stack_top - 16U) & ~0x0FU;
    process->context.esp = stack_pointer;
    process->context.ebp = stack_pointer;
    process->context.eip = (uint32_t)process_bootstrap;
}

static process_t *process_create_internal(process_t *parent, uint32_t owner, uint32_t requested_base, process_exec_fn_t function, uint32_t size)
{
    process_t *process = process_allocate_slot();

    if (process == NULL) {
        return NULL;
    }

    process->pid = next_pid++;
    process->status = PROCESS_STATUS_READY;
    process->owner = owner;
    process->rights = PROCESS_RIGHT_ALL;
    process->entry = function;
    process->signal_handler = process_default_signal_handler;
    process->memory.virtual_base = requested_base;
    process->memory.code_size = size;

    if (socket_create(process) != 0 || process_memory_map(process) != 0) {
        process_release(process);
        return NULL;
    }

    process_initialize_context(process);

    process_link_child(parent, process);
    process_link_global(process);

    if (process->memory.data_page != 0) {
        *((uint32_t *)process->memory.data_page) = 0;
    }
    if (process->memory.bss_page != 0) {
        *((uint32_t *)process->memory.bss_page) = 0;
    }

    return process;
}

static void demo_counter_process(process_t *process)
{
    volatile uint32_t counter = process->runtime_arg0;
    volatile uint32_t iterations = 0;
    volatile uint32_t limit = process->runtime_arg1;

    while (counter < limit) {
        counter++;
        iterations++;
        *((uint32_t *)process->memory.data_start) = counter;
        *((uint32_t *)process->memory.bss_start) = iterations;
        process->runtime_value = counter;
        process->runtime_state = iterations;
        process_yield();
    }

    process_exit(process, (int)counter);
}

static void demo_socket_receiver_process(process_t *process)
{
    uint32_t value = 0;

    while (socket_recv(process, &value) != 0) {
        process_yield();
    }

    *((uint32_t *)process->memory.data_start) = value;
    process->last_socket_value = value;
    process_exit(process, (int)(value & 0xFFFF));
}

static void demo_socket_sender_process(process_t *process)
{
    while (socket_send(process, process->runtime_arg0, process->runtime_arg1) != 0) {
        process_yield();
    }

    process->runtime_state = 1;
    process_exit(process, 0);
}

void process_init(void)
{
    uint32_t slot;

    process_memset(process_table, 0, sizeof(process_table));
    process_memset(process_page_directories, 0, sizeof(process_page_directories));

    for (slot = 0; slot < PROCESS_MAX_COUNT; slot++) {
        process_reset_memory(&process_table[slot].memory);
    }

    kernel_process = &process_table[0];
    kernel_process->used = 1;
    kernel_process->pid = next_pid++;
    kernel_process->status = PROCESS_STATUS_THREAD;
    kernel_process->owner = 0;
    kernel_process->rights = PROCESS_RIGHT_ALL;
    kernel_process->memory.process_page_directory = paging_get_directory();

    process_head = kernel_process;
    current_process = kernel_process;
    scheduler_cursor = kernel_process;
    scheduler_enabled = 1;
    pending_scheduler_ticks = 0;
    total_scheduler_ticks = 0;
    total_context_switches = 0;
    process_memset(&scheduler_context, 0, sizeof(scheduler_context));
}

process_t *process_root(void)
{
    return kernel_process;
}

process_t *process_current(void)
{
    return current_process;
}

process_t *process_first(void)
{
    return process_head;
}

process_t *process_get_by_pid(uint32_t pid)
{
    process_t *cursor = process_head;

    while (cursor != NULL) {
        if (cursor->used && cursor->pid == pid) {
            return cursor;
        }
        cursor = cursor->next;
    }

    return NULL;
}

uint32_t process_count(void)
{
    process_t *cursor = process_head;
    uint32_t count = 0;

    while (cursor != NULL) {
        if (cursor->used) {
            count++;
        }
        cursor = cursor->next;
    }

    return count;
}

const char *process_status_name(process_status_t status)
{
    switch (status) {
        case PROCESS_STATUS_READY:
            return "READY";
        case PROCESS_STATUS_RUN:
            return "RUN";
        case PROCESS_STATUS_WAITING:
            return "WAIT";
        case PROCESS_STATUS_ZOMBIE:
            return "ZOMBIE";
        case PROCESS_STATUS_THREAD:
            return "THREAD";
        default:
            return "UNKNOWN";
    }
}

process_t *process_create_owned(uint32_t owner, process_exec_fn_t function, uint32_t size)
{
    return process_create_internal(current_process != NULL ? current_process : kernel_process, owner, 0, function, size);
}

process_t *exec_fn(uint32_t addr, process_exec_fn_t function, uint32_t size)
{
    uint32_t owner = current_process != NULL ? current_process->owner : 0;
    return process_create_internal(current_process != NULL ? current_process : kernel_process, owner, addr, function, size);
}

int alloc_process_page(process_t *process, uint32_t virtual_page, uint32_t *physical_page)
{
    uint32_t page = pmm_alloc_frame();

    if (process == NULL || page == 0) {
        return -1;
    }

    paging_map_page_in_directory(process->memory.process_page_directory, virtual_page, page, PAGE_USER_RW);
    process_memset((void *)page, 0, PAGE_SIZE);

    if (physical_page != NULL) {
        *physical_page = page;
    }

    return 0;
}

int process_memory_map(process_t *process)
{
    page_directory_t *kernel_directory;
    uint32_t slot;
    uint32_t dir_idx;

    if (process == NULL) {
        return -1;
    }

    slot = process_slot_index(process);
    kernel_directory = paging_get_kernel_directory();

    if (process->memory.virtual_base == 0) {
        process->memory.virtual_base = process_virtual_base_for_slot(slot);
    }

    process->memory.page_size = PAGE_SIZE;
    process->memory.virtual_page = process->memory.virtual_base;
    process->memory.process_page = process->memory.virtual_page;
    process->memory.data_start = process->memory.virtual_base;
    process->memory.data_end = process->memory.data_start + PAGE_SIZE;
    process->memory.bss_start = process->memory.virtual_base + PAGE_SIZE;
    process->memory.bss_end = process->memory.bss_start + PAGE_SIZE;
    process->memory.heap_start = process->memory.virtual_base + PAGE_SIZE * 2;
    process->memory.heap_break = process->memory.heap_start;
    process->memory.heap_end = process->memory.heap_start + PAGE_SIZE;
    process->memory.stack_base = process->memory.virtual_base + PAGE_SIZE * 3;
    process->memory.stack_top = process->memory.stack_base + PAGE_SIZE;
    process->memory.process_page_directory = &process_page_directories[slot];

    paging_copy_directory(process->memory.process_page_directory, kernel_directory);

    if (alloc_process_page(process, process->memory.data_start, &process->memory.data_page) != 0) {
        process_release_memory(process);
        return -1;
    }
    if (alloc_process_page(process, process->memory.bss_start, &process->memory.bss_page) != 0) {
        process_release_memory(process);
        return -1;
    }
    if (alloc_process_page(process, process->memory.heap_start, &process->memory.heap_page) != 0) {
        process_release_memory(process);
        return -1;
    }
    if (alloc_process_page(process, process->memory.stack_base, &process->memory.stack_page) != 0) {
        process_release_memory(process);
        return -1;
    }

    process->memory.physical_page = process->memory.data_page;
    dir_idx = PAGE_DIR_INDEX(process->memory.virtual_base);
    process->memory.process_page_table = PAGE_FRAME(process->memory.process_page_directory->entries[dir_idx]);
    return 0;
}

int process_memory_alloc(process_t *process, uint32_t size)
{
    if (process == NULL || size == 0) {
        return -1;
    }

    if (process->memory.heap_break + size > process->memory.heap_end) {
        return -1;
    }

    process->memory.heap_break += size;
    return 0;
}

void *process_mmap(process_t *process, uint32_t size)
{
    uint32_t old_break;

    if (process == NULL || size == 0) {
        return NULL;
    }

    old_break = process->memory.heap_break;
    if (process_memory_alloc(process, size) != 0) {
        return NULL;
    }

    return (void *)old_break;
}

int process_signal(process_t *process, process_signal_handler_t handler)
{
    if (process == NULL) {
        return -1;
    }

    process->signal_handler = handler != NULL ? handler : process_default_signal_handler;
    return 0;
}

int queue_process_signal(process_t *process, int signal, uint32_t value)
{
    uint32_t next_tail;
    uint32_t flags;

    if (process == NULL) {
        return -1;
    }

    flags = interrupts_save();
    next_tail = (process->signal_tail + 1) % PROCESS_SIGNAL_QUEUE_CAPACITY;
    if (next_tail == process->signal_head) {
        interrupts_restore(flags);
        return -1;
    }

    process->pending_process_signals[process->signal_tail].signal = signal;
    process->pending_process_signals[process->signal_tail].value = value;
    process->signal_tail = next_tail;
    interrupts_restore(flags);
    return 0;
}

int deliver_process_signal(process_t *process)
{
    int delivered = 0;

    if (process == NULL) {
        return 0;
    }

    while (process->signal_head != process->signal_tail) {
        process_pending_signal_t pending = process->pending_process_signals[process->signal_head];
        process->signal_head = (process->signal_head + 1) % PROCESS_SIGNAL_QUEUE_CAPACITY;

        process->last_signal = (uint32_t)pending.signal;
        process->last_signal_value = pending.value;
        if (process->signal_handler != NULL) {
            process->signal_handler(process, pending.signal, pending.value);
        } else {
            process_default_signal_handler(process, pending.signal, pending.value);
        }
        delivered++;

        if (process->status == PROCESS_STATUS_ZOMBIE) {
            break;
        }
    }

    return delivered;
}

int send_signal_to_process(process_t *sender, uint32_t pid, int signal, uint32_t value)
{
    process_t *target = process_get_by_pid(pid);

    if (!process_can_signal(sender, target)) {
        return -1;
    }

    return queue_process_signal(target, signal, value);
}

int kill_process(process_t *sender, uint32_t pid, int signal)
{
    return send_signal_to_process(sender, pid, signal, 0);
}

int process_kill(process_t *sender, uint32_t pid, int signal)
{
    return kill_process(sender, pid, signal);
}

int socket_create(process_t *process)
{
    if (process == NULL) {
        return -1;
    }

    process->process_socket.head = 0;
    process->process_socket.tail = 0;
    return 0;
}

int socket_send(process_t *sender, uint32_t pid, uint32_t value)
{
    process_t *target = process_get_by_pid(pid);
    uint32_t next_tail;
    (void)sender;

    if (target == NULL) {
        return -1;
    }

    next_tail = (target->process_socket.tail + 1) % PROCESS_SOCKET_CAPACITY;
    if (next_tail == target->process_socket.head) {
        return -1;
    }

    target->process_socket.values[target->process_socket.tail] = value;
    target->process_socket.tail = next_tail;
    return queue_process_signal(target, PROCESS_SIGNAL_SOCKET, value);
}

int socket_recv(process_t *process, uint32_t *value)
{
    if (process == NULL || value == NULL) {
        return -1;
    }

    if (process->process_socket.head == process->process_socket.tail) {
        return -1;
    }

    *value = process->process_socket.values[process->process_socket.head];
    process->process_socket.head = (process->process_socket.head + 1) % PROCESS_SOCKET_CAPACITY;
    process->last_socket_value = *value;
    return 0;
}

uint32_t process_signal_pending_count(const process_t *process)
{
    if (process == NULL) {
        return 0;
    }

    if (process->signal_tail >= process->signal_head) {
        return process->signal_tail - process->signal_head;
    }

    return PROCESS_SIGNAL_QUEUE_CAPACITY - process->signal_head + process->signal_tail;
}

uint32_t process_socket_pending_count(const process_t *process)
{
    if (process == NULL) {
        return 0;
    }

    if (process->process_socket.tail >= process->process_socket.head) {
        return process->process_socket.tail - process->process_socket.head;
    }

    return PROCESS_SOCKET_CAPACITY - process->process_socket.head + process->process_socket.tail;
}

process_t *fork_process(process_t *source)
{
    process_t *child;

    if (source == NULL || source == kernel_process || source->entry == NULL) {
        return NULL;
    }

    child = process_create_internal(source, source->owner, 0, source->entry, source->memory.code_size);
    if (child == NULL) {
        return NULL;
    }

    child->rights = source->rights;
    child->signal_handler = source->signal_handler;
    child->runtime_state = source->runtime_state;
    child->runtime_value = source->runtime_value;
    child->runtime_arg0 = source->runtime_arg0;
    child->runtime_arg1 = source->runtime_arg1;
    child->last_signal = source->last_signal;
    child->last_signal_value = source->last_signal_value;

    process_memcpy((void *)child->memory.data_page, (const void *)source->memory.data_page, PAGE_SIZE);
    process_memcpy((void *)child->memory.bss_page, (const void *)source->memory.bss_page, PAGE_SIZE);
    process_memcpy((void *)child->memory.heap_page, (const void *)source->memory.heap_page, PAGE_SIZE);
    child->memory.heap_break = child->memory.heap_start + (source->memory.heap_break - source->memory.heap_start);
    child->status = PROCESS_STATUS_READY;
    return child;
}

void process_yield(void)
{
    process_t *process = current_process;

    if (process == NULL || process == kernel_process) {
        return;
    }

    if (process->status == PROCESS_STATUS_RUN) {
        process->status = PROCESS_STATUS_READY;
    }

    total_context_switches++;
    process->switch_count++;
    process_context_switch(&process->context, &scheduler_context);
}

uint32_t process_context_switch_count(void)
{
    return total_context_switches;
}

int process_wait(process_t *parent, uint32_t pid, int *exit_code)
{
    process_t *child = process_get_by_pid(pid);

    if (child == NULL || (parent != kernel_process && child->parent != parent)) {
        return -1;
    }

    if (child->status != PROCESS_STATUS_ZOMBIE) {
        if (parent != NULL) {
            parent->wait_target_pid = pid;
            if (parent != kernel_process) {
                parent->status = PROCESS_STATUS_WAITING;
            }
        }
        return 0;
    }

    if (exit_code != NULL) {
        *exit_code = child->exit_code;
    }

    if (parent != NULL && parent->status == PROCESS_STATUS_WAITING) {
        parent->status = PROCESS_STATUS_THREAD;
    }

    process_release(child);
    return 1;
}

int wait_process(process_t *parent, uint32_t pid, int *exit_code)
{
    return process_wait(parent, pid, exit_code);
}

void process_exit(process_t *process, int exit_code)
{
    if (process == NULL || process == kernel_process) {
        return;
    }
    if (process->status == PROCESS_STATUS_ZOMBIE) {
        return;
    }

    process->exit_code = exit_code;
    process->status = PROCESS_STATUS_ZOMBIE;

    if (process->parent != NULL && process->parent->status == PROCESS_STATUS_WAITING &&
        (process->parent->wait_target_pid == 0 || process->parent->wait_target_pid == process->pid)) {
        process->parent->status = PROCESS_STATUS_READY;
    }
}

uint32_t process_getuid(process_t *process)
{
    if (process == NULL) {
        return 0;
    }
    return process->owner;
}

void scheduler_init(void)
{
    pending_scheduler_ticks = 0;
    total_scheduler_ticks = 0;
    scheduler_cursor = kernel_process;
    scheduler_enabled = 1;
}

void scheduler_tick(void)
{
    total_scheduler_ticks++;
    pending_scheduler_ticks++;
}

process_t *pick_next_process(void)
{
    process_t *candidate;
    process_t *start;

    if (!scheduler_enabled || process_head == NULL) {
        return NULL;
    }

    start = scheduler_cursor != NULL ? scheduler_cursor : process_head;
    candidate = start;

    do {
        candidate = candidate->next != NULL ? candidate->next : process_head;
        if (candidate->used && candidate->entry != NULL &&
            (candidate->status == PROCESS_STATUS_READY || candidate->status == PROCESS_STATUS_THREAD || candidate->status == PROCESS_STATUS_RUN)) {
            return candidate;
        }
    } while (candidate != start);

    return NULL;
}

process_t *schedule_next_process(void)
{
    process_t *next_process = pick_next_process();

    if (next_process == NULL) {
        return NULL;
    }

    scheduler_cursor = next_process;
    current_process = next_process;
    deliver_process_signal(next_process);

    if (next_process->status != PROCESS_STATUS_ZOMBIE && next_process->status != PROCESS_STATUS_WAITING) {
        next_process->status = PROCESS_STATUS_RUN;
        next_process->cpu_ticks++;
        next_process->run_count++;
        total_context_switches++;
        next_process->switch_count++;
        paging_switch_directory(next_process->memory.process_page_directory);
        process_context_switch(&scheduler_context, &next_process->context);
        process_switch_to_kernel_space();
        if (next_process->status == PROCESS_STATUS_RUN) {
            next_process->status = PROCESS_STATUS_READY;
        }
    }

    current_process = kernel_process;
    return next_process;
}

static void process_deliver_all_pending_signals(void)
{
    process_t *p = process_head;

    while (p != NULL) {
        if (p->used && p->signal_head != p->signal_tail) {
            deliver_process_signal(p);
        }
        p = p->next;
    }
}

uint32_t scheduler_run_pending(uint32_t max_ticks)
{
    uint32_t executed = 0;

    while (pending_scheduler_ticks > 0 && executed < max_ticks) {
        pending_scheduler_ticks--;
        process_deliver_all_pending_signals();
        schedule_next_process();
        executed++;
    }

    return executed;
}

uint32_t scheduler_force_ticks(uint32_t count)
{
    uint32_t index;

    for (index = 0; index < count; index++) {
        scheduler_tick();
        scheduler_run_pending(1);
    }

    return count;
}

uint32_t scheduler_tick_count(void)
{
    return total_scheduler_ticks;
}

int multitasking_enabled(void)
{
    return scheduler_enabled;
}

process_t *process_spawn_demo_counter(uint32_t owner, uint32_t seed, uint32_t limit)
{
    process_t *process = process_create_owned(owner, demo_counter_process, PAGE_SIZE);

    if (process == NULL) {
        return NULL;
    }

    process->runtime_arg0 = seed;
    process->runtime_arg1 = limit;
    return process;
}

int process_spawn_demo_socket_pair(uint32_t owner, uint32_t value, uint32_t *sender_pid, uint32_t *receiver_pid)
{
    process_t *receiver = process_create_owned(owner, demo_socket_receiver_process, PAGE_SIZE);
    process_t *sender;

    if (receiver == NULL) {
        return -1;
    }

    sender = process_create_owned(owner, demo_socket_sender_process, PAGE_SIZE);
    if (sender == NULL) {
        process_release(receiver);
        return -1;
    }

    sender->runtime_arg0 = receiver->pid;
    sender->runtime_arg1 = value;

    if (sender_pid != NULL) {
        *sender_pid = sender->pid;
    }
    if (receiver_pid != NULL) {
        *receiver_pid = receiver->pid;
    }

    return 0;
}