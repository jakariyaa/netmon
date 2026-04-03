#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HWMON_PATH "/sys/class/hwmon"
#define HWMON_PREFIX "/sys/class/hwmon/"
#define HWMON_PREFIX_LEN (sizeof(HWMON_PREFIX) - 1)
#define MAX_PATH PATH_MAX
#define FAN_RESCAN_BACKOFF_SEC 30

static int read_fan_rpm_from_path(const char *fan_path, int *rpm);
static int find_fan_path(char *fan_path, size_t fan_path_size);

static int parse_long_str(const char *src, long *value) {
    char *end = NULL;
    errno = 0;
    long v = strtol(src, &end, 10);
    if (errno != 0 || end == src || *end != '\0')
        return -1;
    *value = v;
    return 0;
}

static int is_temp_millic_sane(long temp_millic) {
    return temp_millic >= -20000 && temp_millic <= 150000;
}

static int parse_temp_input_name(const char *name, int *idx) {
    int parsed_len = 0;
    int parsed_idx = 0;
    if (sscanf(name, "temp%d_input%n", &parsed_idx, &parsed_len) != 1 ||
        parsed_idx <= 0 ||
        parsed_len != (int)strlen(name))
        return -1;

    if (idx)
        *idx = parsed_idx;
    return 0;
}

static ssize_t read_retry_eintr(int fd, void *buf, size_t count) {
    for (;;) {
        ssize_t n = read(fd, buf, count);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

static int write_all_retry_eintr(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static ssize_t read_fd_first_line(int fd, char *buf, size_t bufsize) {
    ssize_t n = read_retry_eintr(fd, buf, bufsize - 1);
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

static int read_long_from_file(const char *path, long *value) {
    char buf[32];
    if (read_file_first_line(path, buf, sizeof(buf)) <= 0)
        return -1;

    return parse_long_str(buf, value);
}

static int read_long_from_file_at(int dirfd, const char *name, long *value) {
    char buf[32];
    if (read_file_first_line_at(dirfd, name, buf, sizeof(buf)) <= 0)
        return -1;

    return parse_long_str(buf, value);
}

static void xml_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        const char *rep = NULL;
        switch (src[i]) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&apos;"; break;
            default: break;
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            if (j + rep_len >= dst_size) break;
            memcpy(dst + j, rep, rep_len);
            j += rep_len;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static const char *temp_status(long temp_c) {
    if (temp_c < 45) return "Cool";
    if (temp_c < 70) return "Normal";
    if (temp_c < 85) return "Warm";
    return "Hot";
}

static int derive_sensor_context(const char *temp_path,
                                 char *base_dir,
                                 size_t base_dir_size,
                                 char *hwmon_name,
                                 size_t hwmon_name_size) {
    if (strncmp(temp_path, HWMON_PREFIX, HWMON_PREFIX_LEN) != 0)
        return -1;

    const char *rest = temp_path + HWMON_PREFIX_LEN;
    const char *slash = strchr(rest, '/');
    if (!slash)
        return -1;

    size_t hw_len = (size_t)(slash - rest);
    if (hw_len == 0 || hw_len >= hwmon_name_size)
        return -1;

    memcpy(hwmon_name, rest, hw_len);
    hwmon_name[hw_len] = '\0';

    if (snprintf(base_dir, base_dir_size, "%s/%s", HWMON_PATH, hwmon_name) >= (int)base_dir_size)
        return -1;

    return 0;
}

static int read_first_fan_rpm(int hwfd) {
    for (int i = 1; i <= 5; i++) {
        char fan_name[16];
        snprintf(fan_name, sizeof(fan_name), "fan%d_input", i);
        long rpm = 0;
        if (read_long_from_file_at(hwfd, fan_name, &rpm) == 0 && rpm >= 0)
            return (int)rpm;
    }

    return -1;
}

static int write_cache_line(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int rc = write_all_retry_eintr(fd, value, strlen(value));
    close(fd);
    return rc;
}

static int write_no_fan_cache(const char *path) {
    char marker[64];
    long now = (long)time(NULL);
    if (snprintf(marker, sizeof(marker), "none:%ld", now) >= (int)sizeof(marker))
        return -1;
    return write_cache_line(path, marker);
}

static int should_defer_fan_rescan(const char *cached_line) {
    const char *prefix = "none:";
    size_t prefix_len = strlen(prefix);
    if (strncmp(cached_line, prefix, prefix_len) != 0)
        return 0;

    const char *ts_str = cached_line + prefix_len;
    long ts = 0;
    if (parse_long_str(ts_str, &ts) != 0)
        return 0;

    long now = (long)time(NULL);
    if (now < ts)
        return 0;

    return (now - ts) < FAN_RESCAN_BACKOFF_SEC;
}

static int score_cpu_chip_name(const char *name) {
    if (!name || !*name) return 0;

    if (strcmp(name, "coretemp") == 0) return 60;
    if (strcmp(name, "k10temp") == 0) return 60;
    if (strcmp(name, "zenpower") == 0) return 50;
    if (strcmp(name, "cpu_thermal") == 0) return 45;
    if (strcmp(name, "cputemp") == 0) return 45;
    if (strcasestr(name, "cpu") != NULL) return 30;

    return 0;
}

static int score_cpu_label(const char *label) {
    if (!label || !*label) return 0;

    if (strcasestr(label, "package id 0") != NULL) return 100;
    if (strcasestr(label, "tctl") != NULL) return 95;
    if (strcasestr(label, "tdie") != NULL) return 90;
    if (strcasestr(label, "cpu") != NULL) return 80;
    if (strcasestr(label, "physical id") != NULL) return 70;
    if (strcasestr(label, "die") != NULL) return 45;

    return 0;
}

static int find_cpu_temp_path(char *temp_path, size_t temp_path_size) {
    DIR *dir = opendir(HWMON_PATH);
    if (!dir) return -1;

    struct dirent *entry;
    int best_score = -1;
    char best_path[MAX_PATH] = {0};

    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;

        char base[MAX_PATH];
        if (snprintf(base, sizeof(base), "%s/%s", HWMON_PATH, entry->d_name) >= (int)sizeof(base))
            continue;

        int hwfd = open(base, O_RDONLY | O_DIRECTORY);
        if (hwfd < 0) continue;

        char chip_name[64] = "";
        (void)read_file_first_line_at(hwfd, "name", chip_name, sizeof(chip_name));
        int chip_score = score_cpu_chip_name(chip_name);

        DIR *hwdir = fdopendir(dup(hwfd));
        if (!hwdir) {
            close(hwfd);
            continue;
        }

        struct dirent *hwe;
        while ((hwe = readdir(hwdir))) {
            int idx = 0;
            if (parse_temp_input_name(hwe->d_name, &idx) != 0)
                continue;

            long temp_millic = 0;
            if (read_long_from_file_at(hwfd, hwe->d_name, &temp_millic) != 0)
                continue;

            if (!is_temp_millic_sane(temp_millic))
                continue;

            char label_name[32];
            snprintf(label_name, sizeof(label_name), "temp%d_label", idx);

            char label[64] = "";
            (void)read_file_first_line_at(hwfd, label_name, label, sizeof(label));

            int score = chip_score + score_cpu_label(label);
            if (idx == 1) score += 5;

            if (score > best_score) {
                if (snprintf(best_path, sizeof(best_path), "%s/%s/%s", HWMON_PATH, entry->d_name, hwe->d_name) < (int)sizeof(best_path))
                    best_score = score;
            }
        }

        closedir(hwdir);
        close(hwfd);
    }

    closedir(dir);

    if (best_score < 0)
        return -1;

    if (snprintf(temp_path, temp_path_size, "%s", best_path) >= (int)temp_path_size)
        return -1;

    return 0;
}

static int cached_temp_path_is_usable(const char *temp_path) {
    if (strncmp(temp_path, HWMON_PREFIX, HWMON_PREFIX_LEN) != 0)
        return -1;

    const char *base = strrchr(temp_path, '/');
    if (!base || *(base + 1) == '\0')
        return -1;
    base++;

    if (parse_temp_input_name(base, NULL) != 0)
        return -1;

    long temp_millic = 0;
    if (read_long_from_file(temp_path, &temp_millic) != 0)
        return -1;

    if (!is_temp_millic_sane(temp_millic))
        return -1;

    return 0;
}

static int resolve_fan_rpm(int hwfd, const char *fan_cache_path) {
    int fan_rpm = read_first_fan_rpm(hwfd);
    if (fan_rpm >= 0)
        return fan_rpm;

    int skip_scan = 0;
    char fan_path[MAX_PATH];
    if (read_file_first_line(fan_cache_path, fan_path, sizeof(fan_path)) > 0) {
        if (strncmp(fan_path, HWMON_PREFIX, HWMON_PREFIX_LEN) == 0) {
            if (read_fan_rpm_from_path(fan_path, &fan_rpm) != 0)
                unlink(fan_cache_path);
        } else if (should_defer_fan_rescan(fan_path)) {
            skip_scan = 1;
        } else {
            unlink(fan_cache_path);
        }
    }

    if (fan_rpm >= 0 || skip_scan)
        return fan_rpm;

    if (find_fan_path(fan_path, sizeof(fan_path)) == 0) {
        if (read_fan_rpm_from_path(fan_path, &fan_rpm) == 0)
            (void)write_cache_line(fan_cache_path, fan_path);
    } else {
        (void)write_no_fan_cache(fan_cache_path);
    }

    return fan_rpm;
}

static int read_fan_rpm_from_path(const char *fan_path, int *rpm) {
    if (strncmp(fan_path, HWMON_PREFIX, HWMON_PREFIX_LEN) != 0)
        return -1;

    long v = 0;
    if (read_long_from_file(fan_path, &v) != 0 || v < 0)
        return -1;

    *rpm = (int)v;
    return 0;
}

static int find_fan_path(char *fan_path, size_t fan_path_size) {
    DIR *dir = opendir(HWMON_PATH);
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;

        char base[MAX_PATH];
        if (snprintf(base, sizeof(base), "%s/%s", HWMON_PATH, entry->d_name) >= (int)sizeof(base))
            continue;

        int hwfd = open(base, O_RDONLY | O_DIRECTORY);
        if (hwfd < 0) continue;

        for (int i = 1; i <= 5; i++) {
            char fan_name[16];
            snprintf(fan_name, sizeof(fan_name), "fan%d_input", i);

            long rpm = 0;
            if (read_long_from_file_at(hwfd, fan_name, &rpm) == 0 && rpm >= 0) {
                close(hwfd);
                closedir(dir);
                if (snprintf(fan_path, fan_path_size, "%s/%s/%s", HWMON_PATH, entry->d_name, fan_name) >= (int)fan_path_size)
                    return -1;
                return 0;
            }
        }

        close(hwfd);
    }

    closedir(dir);
    return -1;
}

static int print_genmon_output(const char *temp_path, const char *fan_cache_path) {
    if (strncmp(temp_path, HWMON_PREFIX, HWMON_PREFIX_LEN) != 0)
        return -1;

    long temp_millic = 0;
    if (read_long_from_file(temp_path, &temp_millic) != 0)
        return -1;

    long temp_c = temp_millic / 1000;

    char base_dir[MAX_PATH];
    char hwmon_name[64];
    if (derive_sensor_context(temp_path, base_dir, sizeof(base_dir), hwmon_name, sizeof(hwmon_name)) != 0)
        return -1;

    int hwfd = open(base_dir, O_RDONLY | O_DIRECTORY);
    if (hwfd < 0)
        return -1;

    char sensor_name[64] = "unknown";
    (void)read_file_first_line_at(hwfd, "name", sensor_name, sizeof(sensor_name));

    int fan_rpm = resolve_fan_rpm(hwfd, fan_cache_path);
    close(hwfd);

    char esc_sensor_name[160];
    xml_escape(sensor_name, esc_sensor_name, sizeof(esc_sensor_name));

    char fan_line[64];
    if (fan_rpm >= 0)
        snprintf(fan_line, sizeof(fan_line), "%d RPM", fan_rpm);
    else
        snprintf(fan_line, sizeof(fan_line), "n/a");

    printf("<txt>%ld°C</txt>\n", temp_c);
    printf("<tool>Sensor: %s\nStatus: %s\nFan: %s</tool>\n",
           esc_sensor_name,
           temp_status(temp_c),
           fan_line);

    return 0;
}

int main(void) {

    char cache_path[MAX_PATH];
    char fan_cache_path[MAX_PATH];
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        snprintf(cache_path, sizeof(cache_path), "%s/cpu_temp_path", xdg);
        snprintf(fan_cache_path, sizeof(fan_cache_path), "%s/cpu_fan_path", xdg);
    } else {
        snprintf(cache_path, sizeof(cache_path), "/run/user/%d/cpu_temp_path", getuid());
        snprintf(fan_cache_path, sizeof(fan_cache_path), "/run/user/%d/cpu_fan_path", getuid());
    }

    char path[MAX_PATH] = "";

    // Fast path: use cache when it still points to a valid tempN_input sensor.
    // This avoids rescanning all hwmon entries on every plugin refresh.
    if (read_file_first_line(cache_path, path, sizeof(path)) > 0) {
        if (cached_temp_path_is_usable(path) == 0) {
            if (print_genmon_output(path, fan_cache_path) == 0)
                return 0;
        }
        unlink(cache_path);
    }

    // Slow path: scan and refresh cache when needed.
    if (find_cpu_temp_path(path, sizeof(path)) == 0) {
        (void)write_cache_line(cache_path, path);
        if (print_genmon_output(path, fan_cache_path) == 0)
            return 0;
        unlink(cache_path);
    }

    fprintf(stderr, "No matching sensor found\n");
    return 1;
}
