// memory.c
#include "memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/uio.h>
#include <unistd.h>

ssize_t mem_read(pid_t pid, const void *remote_addr, void *local_buf, size_t size) {
    struct iovec local  = { .iov_base = local_buf,  .iov_len = size  };
    struct iovec remote = { .iov_base = (void*)remote_addr, .iov_len = size  };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0);
}

uint64_t read_u64(pid_t pid, const void *addr) {
    uint64_t val = 0;
    mem_read(pid, addr, &val, 8);
    return val;
}

uint32_t read_u32(pid_t pid, const void *addr) {
    uint32_t val = 0;
    mem_read(pid, addr, &val, 4);
    return val;
}

float read_float(pid_t pid, const void *addr) {
    float val = 0.0f;
    mem_read(pid, addr, &val, 4);
    return val;
}

Vector3 read_vector3(pid_t pid, const void *addr) {
    Vector3 v = {0, 0, 0};
    mem_read(pid, addr, &v, 12);
    return v;
}

int read_armastring(pid_t pid, const void *addr, char *out, int max_len) {
    // ArmaString layout: +0x00 = vtable(8), +0x08 = length(4), +0x10 = data
    uint32_t len = read_u32(pid, addr + 8);
    if (len == 0 || len >= (uint32_t)max_len) len = max_len - 1;

    // Read the string data starting at +0x10
    ssize_t n = mem_read(pid, addr + 0x10, out, len);
    if (n < 0) { out[0] = '\0'; return -1; }
    out[len] = '\0';
    return len;
}

pid_t find_dayz_pid(void) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;

    struct dirent *entry;
    pid_t found = -1;

    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atoi(entry->d_name);
        if (pid <= 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char buf[256];
        int n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        // DayZ under Proton shows as "DayZ.exe" or "DayZ_BE.exe"
        // We want the main game process, not the BE service
        if (strstr(buf, "DayZ.exe") && !strstr(buf, "DayZ_BE.exe")) {
            found = pid;
            // Keep scanning to get the LAST one (usually the correct one)
            // As an improvement, you could check /proc/pid/status for threads
        }
    }
    closedir(proc);
    return found;
}
