#include "kill.h"
#include "console.h"
#include "process.h"

static int parse_pid(const char *s, uint64_t *out) {
    if (*s < '0' || *s > '9') return 0;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 100000) return 0;
        ++s;
    }
    if (*s && *s != ' ') return 0;
    *out = v;
    return 1;
}

void kill_run(char *args) {
    while (args && *args == ' ') ++args;
    uint64_t pid;
    if (!args || !*args || !parse_pid(args, &pid) || pid == 0) {
        console_print("usage: kill <pid>   (see 'ps')\n");
        return;
    }
    nexus_process_t *p = process_get(pid);
    if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE) {
        console_print("kill: no such process\n");
        return;
    }
    if (p == process_current()) {
        console_print("kill: refusing to kill the running process\n");
        return;
    }
    /* The scheduler never resumes a thread whose process is a zombie and reaps
     * it (address space, pages, handles) once the thread is off the CPU. */
    console_print(process_exit(pid) ? "process terminated\n" : "kill: could not terminate process\n");
}
