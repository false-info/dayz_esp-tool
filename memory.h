// memory.h
#pragma once
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    float x, y, z;
} Vector3;

// Core memory operations
ssize_t mem_read(pid_t pid, const void *remote_addr, void *local_buf, size_t size);

// Typed helpers
uint64_t  read_u64(pid_t pid, const void *addr);
uint32_t  read_u32(pid_t pid, const void *addr);
float     read_float(pid_t pid, const void *addr);
Vector3   read_vector3(pid_t pid, const void *addr);
int       read_armastring(pid_t pid, const void *addr, char *out, int max_len);

// Process discovery
pid_t find_dayz_pid(void);
