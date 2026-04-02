#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HWMON_PATH "/sys/class/hwmon"
#define MAX_PATH PATH_MAX

static ssize_t read_fd_first_line(int fd, char *buf, size_t bufsize) {
    ssize_t n = read(fd, buf, bufsize - 1);
    if (n <= 0) return -1;

    char *nl = memchr(buf, '\n', (size_t)n);
    if (nl) n = (ssize_t)(nl - buf);
    buf[n] = '\0';
    return n;
}

static ssize_t read_file_first_line(const char *path, char *buf, size_t bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read_fd_first_line(fd, buf, bufsize);
    close(fd);
    return n;
}

static ssize_t read_file_first_line_at(int dirfd, const char *name, char *buf, size_t bufsize) {
    int fd = openat(dirfd, name, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read_fd_first_line(fd, buf, bufsize);
    close(fd);
    return n;
}

int read_temp(const char *path) {
    if (strncmp(path, "/sys/class/hwmon/", strlen("/sys/class/hwmon/")) != 0)
        return -1;

    char buf[32];
    if (read_file_first_line(path, buf, sizeof(buf)) <= 0)
        return -1;

    char *end = NULL;
    long temp = strtol(buf, &end, 10);
    if (end == buf || *end != '\0')
        return -1;

    printf("%ld°C\n", temp / 1000);
    return 0;
}

int main(void) {

    char cache_path[MAX_PATH];
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg)
        snprintf(cache_path, sizeof(cache_path), "%s/cpu_temp_path", xdg);
    else
        snprintf(cache_path, sizeof(cache_path), "/run/user/%d/cpu_temp_path", getuid());

    char path[MAX_PATH];

    // Try cache
    if (read_file_first_line(cache_path, path, sizeof(path)) > 0) {
        if (read_temp(path) == 0) return 0;
        unlink(cache_path);
    }

    // Scan
    DIR *dir = opendir(HWMON_PATH);
    if (!dir) return 1;

    struct dirent *entry;

    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "hwmon", 5)) continue;

        char base[MAX_PATH];
        snprintf(base, sizeof(base), "%s/%s", HWMON_PATH, entry->d_name);

        int hwfd = open(base, O_RDONLY | O_DIRECTORY);
        if (hwfd < 0) continue;

        char name[64];
        if (read_file_first_line_at(hwfd, "name", name, sizeof(name)) <= 0) {
            close(hwfd);
            continue;
        }

        if (strcmp(name, "coretemp") != 0) {
            close(hwfd);
            continue;
        }

        int sensor_order[] = {1, 2, 3};

        for (int idx = 0; idx < 3; idx++) {
            int i = sensor_order[idx];
            char label_name[16];
            snprintf(label_name, sizeof(label_name), "temp%d_label", i);

            char buf[64];
            if (read_file_first_line_at(hwfd, label_name, buf, sizeof(buf)) <= 0) continue;

            if (strcmp(buf, "Package id 0") == 0) {
                snprintf(path, sizeof(path), "%s/%s/temp%d_input", HWMON_PATH, entry->d_name, i);

                int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd >= 0) {
                    write(fd, path, strlen(path));
                    close(fd);
                }

                close(hwfd);
                closedir(dir);
                return read_temp(path);
            }
        }

        close(hwfd);
    }

    fprintf(stderr, "No matching sensor found\n");

    closedir(dir);
    return 1;
}
