#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * These constants define the size of the simulated operating-system world.
 * They are compile-time macros, so the values are substituted by the preprocessor
 * before the compiler sees the code. This makes the data structures fixed-size,
 * which is common in small teaching examples and embedded systems.
 */
#define MAX_PROCESSES 16
#define MAX_ALLOCATIONS 16
#define MEMORY_SIZE 1024
#define LINE_SIZE 128
#define NAME_SIZE 24

/*
 * ProcessState is an enum that gives each process a named state.
 * In C, enums are integer values, and the compiler assigns 0, 1, 2, ... in order.
 * Using names instead of raw numbers makes the code easier to read and reason about.
 */
typedef enum {
    PROCESS_UNUSED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_FINISHED
} ProcessState;

/*
 * A Process object models one task in the simulated scheduler.
 * This is a struct, which groups related fields into a single object.
 * The field names are accessed with dot notation (for example, process.pid).
 */
typedef struct {
    int pid;
    char name[NAME_SIZE];
    int ticks_remaining;
    ProcessState state;
} Process;

/*
 * An Allocation describes one region of simulated memory.
 * The allocator keeps track of start address, size, and whether the slot is in use.
 */
typedef struct {
    int id;
    size_t address;
    size_t size;
    bool active;
} Allocation;

/*
 * Global arrays hold the state of the whole demo.
 * static means these variables have file scope and are not visible outside this file.
 * Their lifetime lasts for the entire program, like a persistent OS data structure.
 */
static Process processes[MAX_PROCESSES];
static Allocation allocations[MAX_ALLOCATIONS];
static int next_pid = 1;
static int next_allocation_id = 1;
static int scheduler_cursor = -1;

/*
 * Converts an enum value into a printable label such as "ready" or "running".
 * The function returns a string literal, which is a const char* pointer.
 * switch handles different enum cases; default covers unexpected values.
 */
static const char *state_name(ProcessState state)
{
    switch (state) {
    case PROCESS_READY:
        return "ready";
    case PROCESS_RUNNING:
        return "running";
    case PROCESS_FINISHED:
        return "finished";
    case PROCESS_UNUSED:
    default:
        return "unused";
    }
}

/*
 * parse_positive_int tries to convert a text string like "12" into an int.
 * strtol is a C library function that parses a long integer from a string.
 * The char **end pointer records where parsing stopped, so we can reject garbage suffixes.
 * We also validate bounds so the input matches the toy OS rules.
 */
static bool parse_positive_int(const char *text, int *result)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 100000) {
        return false;
    }

    *result = (int)value;
    return true;
}

/*
 * show_help prints the command menu for the shell.
 * puts writes a string to stdout and appends a newline automatically.
 */
static void show_help(void)
{
    puts("Commands:");
    puts("  help                 Show this help");
    puts("  about                Explain what this demo simulates");
    puts("  run NAME TICKS       Create a process that needs TICKS scheduler turns");
    puts("  ps                   List processes");
    puts("  tick [COUNT]         Run the scheduler for COUNT turns (default 1)");
    puts("  alloc BYTES          Allocate simulated RAM");
    puts("  free ID              Free an allocation by its ID");
    puts("  mem                  Show simulated memory usage");
    puts("  shutdown             Stop the demo");
}

/*
 * show_about explains that this is a simulation, not a real kernel.
 * It demonstrates concepts with plain C without touching actual OS-level privileges.
 */
static void show_about(void)
{
    puts("This is a userspace teaching model, not a bootable operating system.");
    puts("It demonstrates a command shell, process table, round-robin scheduler,");
    puts("and a small first-fit memory allocator without touching real system memory.");
}

/*
 * list_processes walks the process table and prints each active process.
 * The loop uses size_t because array indexes are unsigned and suitable for container sizes.
 * The += operator updates the index after each iteration.
 */
static void list_processes(void)
{
    bool found = false;

    puts(" PID  STATE      TICKS  NAME");
    for (size_t i = 0; i < MAX_PROCESSES; ++i) {
        const Process *process = &processes[i];
        if (process->state == PROCESS_UNUSED) {
            continue;
        }

        printf("%4d  %-10s %5d  %s\n", process->pid, state_name(process->state),
               process->ticks_remaining, process->name);
        found = true;
    }
    if (!found) {
        puts("(no processes)");
    }
}

/*
 * create_process validates user input, finds a free slot, and records the new process.
 * The function takes two strings: the process name and the tick count text.
 * In C, arrays decay to pointers when passed to functions, so char[] arguments are effectively char*.
 */
static void create_process(const char *name, const char *ticks_text)
{
    int ticks;
    size_t slot;

    if (name == NULL || ticks_text == NULL || strlen(name) >= NAME_SIZE ||
        !parse_positive_int(ticks_text, &ticks)) {
        puts("Usage: run NAME TICKS (name must be shorter than 24 characters; ticks 1-100000)");
        return;
    }

    for (slot = 0; slot < MAX_PROCESSES; ++slot) {
        if (processes[slot].state == PROCESS_UNUSED ||
            processes[slot].state == PROCESS_FINISHED) {
            break;
        }
    }
    if (slot == MAX_PROCESSES) {
        puts("Process table is full.");
        return;
    }

    /*
     * The assignment to pid uses the global counter and then increments it.
     * The ++ postfix operator returns the old value and updates the variable afterward.
     */
    processes[slot].pid = next_pid++;
    (void)snprintf(processes[slot].name, sizeof(processes[slot].name), "%s", name);
    processes[slot].ticks_remaining = ticks;
    processes[slot].state = PROCESS_READY;
    printf("Created process %s (PID %d), needing %d scheduler turn(s).\n",
           processes[slot].name, processes[slot].pid, ticks);
}

/*
 * run_scheduler simulates one or more scheduling turns.
 * It picks the next ready process in round-robin order, marks it running,
 * decreases its remaining ticks, and marks it finished when the count reaches zero.
 */
static void run_scheduler(const char *count_text)
{
    int count = 1;

    if (count_text != NULL && !parse_positive_int(count_text, &count)) {
        puts("Usage: tick [COUNT] (count must be 1-100000)");
        return;
    }

    for (int turn = 0; turn < count; ++turn) {
        int selected = -1;

        /*
         * This is a round-robin scan: begin at the next slot after the previous cursor,
         * wrap around the fixed-size array, and choose the first ready process.
         * The modulo operator % makes the index wrap back to zero.
         */
        for (size_t offset = 1; offset <= MAX_PROCESSES; ++offset) {
            size_t index = ((size_t)(scheduler_cursor + (int)offset)) % MAX_PROCESSES;
            if (processes[index].state == PROCESS_READY) {
                selected = (int)index;
                break;
            }
        }

        if (selected < 0) {
            if (turn == 0) {
                puts("Scheduler is idle: there are no ready processes.");
            }
            break;
        }

        Process *process = &processes[selected];
        scheduler_cursor = selected;
        process->state = PROCESS_RUNNING;
        printf("Tick %d: running %s (PID %d)\n", turn + 1, process->name, process->pid);
        --process->ticks_remaining;
        if (process->ticks_remaining == 0) {
            process->state = PROCESS_FINISHED;
            printf("       %s (PID %d) finished.\n", process->name, process->pid);
        } else {
            process->state = PROCESS_READY;
        }
    }
}

/*
 * range_is_free checks whether a requested memory block overlaps any existing allocation.
 * The start and end values are offsets in the simulated RAM array, so comparisons are done with size_t.
 */
static bool range_is_free(size_t start, size_t size)
{
    size_t end = start + size;

    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (!allocations[i].active) {
            continue;
        }
        size_t other_start = allocations[i].address;
        size_t other_end = other_start + allocations[i].size;
        if (start < other_end && other_start < end) {
            return false;
        }
    }
    return true;
}

/*
 * allocate_memory validates the requested byte count and then finds the first free region.
 * This is a simple first-fit allocator: it scans addresses from zero upward and uses the first
 * region that fits without overlapping other active allocations.
 */
static void allocate_memory(const char *size_text)
{
    int requested;
    size_t slot;

    if (size_text == NULL || !parse_positive_int(size_text, &requested)) {
        puts("Usage: alloc BYTES (bytes must be 1-100000)");
        return;
    }
    if ((size_t)requested > MEMORY_SIZE) {
        printf("Not enough simulated RAM: capacity is %d bytes.\n", MEMORY_SIZE);
        return;
    }

    for (slot = 0; slot < MAX_ALLOCATIONS; ++slot) {
        if (!allocations[slot].active) {
            break;
        }
    }
    if (slot == MAX_ALLOCATIONS) {
        puts("Allocation table is full.");
        return;
    }

    for (size_t address = 0; address + (size_t)requested <= MEMORY_SIZE; ++address) {
        if (range_is_free(address, (size_t)requested)) {
            allocations[slot].id = next_allocation_id++;
            allocations[slot].address = address;
            allocations[slot].size = (size_t)requested;
            allocations[slot].active = true;
            printf("Allocated %d byte(s) at address %zu (ID %d).\n",
                   requested, address, allocations[slot].id);
            return;
        }
    }

    puts("Allocation failed: simulated RAM is too fragmented or full.");
}

/*
 * free_memory removes one active allocation by ID.
 * The program searches the array of allocations and clears the active flag.
 * Once inactive, the slot can be reused by future allocations.
 */
static void free_memory(const char *id_text)
{
    int id;

    if (id_text == NULL || !parse_positive_int(id_text, &id)) {
        puts("Usage: free ID");
        return;
    }

    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (allocations[i].active && allocations[i].id == id) {
            allocations[i].active = false;
            printf("Freed allocation %d (%zu byte(s)).\n", id, allocations[i].size);
            return;
        }
    }
    printf("No active allocation with ID %d.\n", id);
}

/*
 * show_memory prints the current mock RAM map.
 * It adds the sizes of all active allocations to compute used memory,
 * and subtracts that from the total to find free memory.
 */
static void show_memory(void)
{
    size_t used = 0;

    printf("Simulated RAM: %d bytes total\n", MEMORY_SIZE);
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (!allocations[i].active) {
            continue;
        }
        printf("  ID %-3d  address %4zu  size %4zu bytes\n",
               allocations[i].id, allocations[i].address, allocations[i].size);
        used += allocations[i].size;
    }
    printf("  Used: %zu bytes; free: %zu bytes\n", used, MEMORY_SIZE - used);
}

/*
 * next_token extracts the next whitespace-delimited token from a string pointer.
 * The function takes a char** because it needs to update the caller's pointer as it advances.
 * It skips leading whitespace, then walks until it reaches a space/newline or the end of the string.
 * When it finds the end of a token, it writes a NUL terminator in place and returns the start pointer.
 */
static char *next_token(char **input)
{
    char *start;

    while (isspace((unsigned char)**input)) {
        ++*input;
    }
    if (**input == '\0') {
        return NULL;
    }

    start = *input;
    while (**input != '\0' && !isspace((unsigned char)**input)) {
        ++*input;
    }
    if (**input != '\0') {
        *(*input)++ = '\0';
    }
    return start;
}

/*
 * main is the entry point of the program. In C, every executable must have a main function.
 * Returning 0 reports successful completion to the operating system.
 */
int main(void)
{
    char line[LINE_SIZE];

    puts("Booting TinyOS demo...");
    puts("[boot] Initializing process table");
    puts("[boot] Setting up 1024 bytes of simulated RAM");
    puts("[boot] Starting command shell");
    puts("TinyOS is ready. Type 'help' for commands.");

    while (true) {
        char *cursor;
        char *command;
        char *argument;

        /*
         * fputs writes the prompt without adding a trailing newline.
         * fflush forces any buffered output to display immediately, which matters in an interactive shell.
         */
        fputs("tinyos> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            putchar('\n');
            break;
        }

        /*
         * Input lines are limited to LINE_SIZE bytes. If the user enters a longer line,
         * this code drains the rest of the line and ignores it instead of overflowing the buffer.
         */
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {
                ;
            }
            puts("Input too long; command ignored.");
            continue;
        }

        cursor = line;
        command = next_token(&cursor);
        if (command == NULL) {
            continue;
        }
        argument = next_token(&cursor);

        /*
         * This decision tree dispatches the user's command to a function.
         * strcmp compares two C strings and returns 0 when they match exactly.
         * The else-if chain is a standard way to evaluate multiple command names in sequence.
         */
        if (strcmp(command, "help") == 0) {
            show_help();
        } else if (strcmp(command, "about") == 0) {
            show_about();
        } else if (strcmp(command, "run") == 0) {
            create_process(argument, next_token(&cursor));
        } else if (strcmp(command, "ps") == 0) {
            list_processes();
        } else if (strcmp(command, "tick") == 0) {
            run_scheduler(argument);
        } else if (strcmp(command, "alloc") == 0) {
            allocate_memory(argument);
        } else if (strcmp(command, "free") == 0) {
            free_memory(argument);
        } else if (strcmp(command, "mem") == 0) {
            show_memory();
        } else if (strcmp(command, "shutdown") == 0) {
            puts("TinyOS demo stopped.");
            break;
        } else {
            printf("Unknown command: %s (type 'help' for commands)\n", command);
        }
    }

    return 0;
}
