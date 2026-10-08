/* LD_PRELOAD shim: make Geekbench 5 see only 4 logical CPUs
 * (matching this container's cgroup cpu.max = 4 vCPU).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <sys/syscall.h>

#define FAKE_CPUS 4

static char *cpuinfo_buf = NULL;
static size_t cpuinfo_len = 0;
static int in_shim = 0;

static int build_cpuinfo(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return -1;
    char *data = NULL;
    size_t cap = 0, len = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (len + l + 1 > cap) {
            cap = (cap ? cap * 2 : 65536) + l;
            data = realloc(data, cap);
        }
        memcpy(data + len, line, l);
        len += l;
    }
    fclose(f);

    char *end = strstr(data, "\n\n");
    size_t blocklen = end ? (size_t)(end - data) + 1 : len;
    char *block = strndup(data, blocklen);

    cpuinfo_buf = malloc(blocklen * FAKE_CPUS + 256);
    cpuinfo_len = 0;

    for (int i = 0; i < FAKE_CPUS; i++) {
        char *copy = strdup(block);
        char *save = NULL;
        for (char *ln = strtok_r(copy, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(ln, "processor", 9))
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len,
                                       "processor\t: %d\n", i);
            else if (!strncmp(ln, "core id", 7))
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len,
                                       "core id\t\t: %d\n", i);
            else if (!strncmp(ln, "cpu cores", 9))
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len,
                                       "cpu cores\t: %d\n", FAKE_CPUS);
            else if (!strncmp(ln, "siblings", 8))
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len,
                                       "siblings\t: %d\n", FAKE_CPUS);
            else if (!strncmp(ln, "physical id", 11))
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len,
                                       "physical id\t: 0\n");
            else
                cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len, "%s\n", ln);
        }
        free(copy);
        cpuinfo_len += sprintf(cpuinfo_buf + cpuinfo_len, "\n");
    }
    free(block);
    free(data);
    return 0;
}

static int memfd_from(const char *content, size_t len)
{
    int fd = syscall(SYS_memfd_create, "fake", 0);
    if (fd < 0)
        return -1;
    if (write(fd, content, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static int fake_path_fd(const char *path)
{
    if (!path)
        return -1;
    if (!strcmp(path, "/proc/cpuinfo")) {
        if (!cpuinfo_buf && build_cpuinfo() != 0)
            return -1;
        return memfd_from(cpuinfo_buf, cpuinfo_len);
    }
    if (!strcmp(path, "/sys/devices/system/cpu/online") ||
        !strcmp(path, "/sys/devices/system/cpu/present") ||
        !strcmp(path, "/sys/devices/system/cpu/possible"))
        return memfd_from("0-3\n", 4);
    return -1;
}


/* ---------------- interception ---------------- */

static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_openat)(int, const char *, int, ...) = NULL;
static int (*real_openat64)(int, const char *, int, ...) = NULL;
static FILE *(*real_fopen)(const char *, const char *) = NULL;
static FILE *(*real_fopen64)(const char *, const char *) = NULL;
static long (*real_sysconf)(int) = NULL;

static int try_fake(const char *path, int *out_fd)
{
    if (in_shim)
        return 0;
    in_shim = 1;
    int fd = fake_path_fd(path);
    in_shim = 0;
    if (fd >= 0) {
        *out_fd = fd;
        return 1;
    }
    return 0;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, int);
    va_end(ap);
    int fd;
    if (try_fake(path, &fd))
        return fd;
    if (!real_open)
        real_open = dlsym(RTLD_NEXT, "open");
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, int);
    va_end(ap);
    int fd;
    if (try_fake(path, &fd))
        return fd;
    if (!real_open64)
        real_open64 = dlsym(RTLD_NEXT, "open64");
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, int);
    va_end(ap);
    int fd;
    if (path && path[0] == '/' && try_fake(path, &fd))
        return fd;
    if (!real_openat)
        real_openat = dlsym(RTLD_NEXT, "openat");
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, int);
    va_end(ap);
    int fd;
    if (path && path[0] == '/' && try_fake(path, &fd))
        return fd;
    if (!real_openat64)
        real_openat64 = dlsym(RTLD_NEXT, "openat64");
    return real_openat64(dirfd, path, flags, mode);
}

FILE *fopen(const char *path, const char *mode)
{
    int fd;
    if (try_fake(path, &fd))
        return fdopen(fd, mode);
    if (!real_fopen)
        real_fopen = dlsym(RTLD_NEXT, "fopen");
    return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    int fd;
    if (try_fake(path, &fd))
        return fdopen(fd, mode);
    if (!real_fopen64)
        real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    return real_fopen64(path, mode);
}

long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF)
        return FAKE_CPUS;
    if (!real_sysconf)
        real_sysconf = dlsym(RTLD_NEXT, "sysconf");
    return real_sysconf(name);
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
    (void)pid;
    if (cpusetsize < sizeof(cpu_set_t)) {
        errno = EINVAL;
        return -1;
    }
    CPU_ZERO(mask);
    for (int i = 0; i < FAKE_CPUS; i++)
        CPU_SET(i, mask);
    return 0;
}
