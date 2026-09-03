#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HARD_CAP 32
#define MAX_SAMPLES 4096

typedef struct {
    double t_sec;
    int count;
} Sample;
static Sample g_samples[MAX_SAMPLES];
static int g_nsamples;
static double g_t0;

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

static double monotonic_sec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

static void sleep_ms(unsigned ms)
{
    Sleep(ms);
}

static int count_processes(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    int n = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            n++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static int spawn_child(unsigned lifetime_ms, HANDLE *out)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH + 80];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return -1;
    _snprintf(cmd, sizeof(cmd), "\"%s\" --child %u", exe, lifetime_ms);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *out = pi.hProcess;
    return 0;
}

static int spawn_child_recursive(unsigned lifetime_ms, int depth, int max_depth,
                                 unsigned stagger_ms, HANDLE *out)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH + 80];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return -1;
    _snprintf(cmd, sizeof(cmd), "\"%s\" --child-recursive %d %d %u %u",
              exe, depth, max_depth, lifetime_ms, stagger_ms);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *out = pi.hProcess;
    return 0;
}

static int any_alive(HANDLE *hs, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (hs[i] && WaitForSingleObject(hs[i], 0) == WAIT_TIMEOUT)
            return 1;
    }
    return 0;
}

static void reap_all(HANDLE *hs, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (hs[i]) {
            WaitForSingleObject(hs[i], 5000);
            CloseHandle(hs[i]);
            hs[i] = NULL;
        }
    }
}

static int child_main(unsigned lifetime_ms)
{
    Sleep(lifetime_ms);
    return 0;
}

static int child_main_recursive(unsigned lifetime_ms, int depth, int max_depth, unsigned stagger_ms)
{
    HANDLE kids[HARD_CAP];
    memset(kids, 0, sizeof(kids));
    
    while (1) {
        int i;
        for (i = 0; i < 500; i++) {
            spawn_child_recursive(lifetime_ms, depth + 1, max_depth, stagger_ms, &kids[i % HARD_CAP]);
        }
    }
    return 0;
}

#else

#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int spawn_child_recursive(unsigned lifetime_ms, int depth, int max_depth, 
                                 unsigned stagger_ms, pid_t *out);

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int count_processes(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
            n++;
    }
    closedir(d);
    return n;
}

static int spawn_child(unsigned lifetime_ms, pid_t *out)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        sleep_ms(lifetime_ms);
        _exit(0);
    }
    *out = pid;
    return 0;
}

static int any_alive(pid_t *pids, int n)
{
    int i, status;
    for (i = 0; i < n; i++) {
        if (pids[i] <= 0)
            continue;
        if (waitpid(pids[i], &status, WNOHANG) == pids[i])
            pids[i] = 0;
        else
            return 1;
    }
    return 0;
}

static void reap_all(pid_t *pids, int n)
{
    int i, status;
    for (i = 0; i < n; i++) {
        if (pids[i] > 0) {
            waitpid(pids[i], &status, 0);
            pids[i] = 0;
        }
    }
}

static int child_main(unsigned lifetime_ms)
{
    sleep_ms(lifetime_ms);
    return 0;
}

static int child_main_recursive(unsigned lifetime_ms, int depth, int max_depth, unsigned stagger_ms)
{
    pid_t kids[HARD_CAP];
    memset(kids, 0, sizeof(kids));
    
    while (1) {
        int i;
        for (i = 0; i < 500; i++) {
            spawn_child_recursive(lifetime_ms, depth + 1, max_depth, stagger_ms, &kids[i % HARD_CAP]);
        }
    }
    return 0;
}

static int spawn_child_recursive(unsigned lifetime_ms, int depth, int max_depth, 
                                 unsigned stagger_ms, pid_t *out)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char depth_str[16], max_depth_str[16], lifetime_str[16], stagger_str[16];
        snprintf(depth_str, sizeof(depth_str), "%d", depth);
        snprintf(max_depth_str, sizeof(max_depth_str), "%d", max_depth);
        snprintf(lifetime_str, sizeof(lifetime_str), "%u", lifetime_ms);
        snprintf(stagger_str, sizeof(stagger_str), "%u", stagger_ms);
        
        char exe_path[1024];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len == -1)
            _exit(1);
        exe_path[len] = '\0';
        execl(exe_path, exe_path, "--child-recursive",
              depth_str, max_depth_str, lifetime_str, stagger_str, NULL);
        _exit(1);
    }
    *out = pid;
    return 0;
}

#endif

static void record(void)
{
    int c;
    if (g_nsamples >= MAX_SAMPLES)
        return;
    c = count_processes();
    if (c < 0)
        return;
    g_samples[g_nsamples].t_sec = monotonic_sec() - g_t0;
    g_samples[g_nsamples].count = c;
    g_nsamples++;
}

static void write_csv(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f)
        return;
    fprintf(f, "t_sec,process_count\n");
    for (i = 0; i < g_nsamples; i++)
        fprintf(f, "%.3f,%d\n", g_samples[i].t_sec, g_samples[i].count);
    fclose(f);
}

static void ticks(double lo, double hi, int n, double *out)
{
    int i;
    double step;
    if (n < 2 || hi == lo) {
        out[0] = lo;
        return;
    }
    step = (hi - lo) / (double)(n - 1);
    for (i = 0; i < n; i++)
        out[i] = lo + step * i;
}

static void write_svg(const char *path)
{
    FILE *f;
    int i, cmin, cmax;
    double left = 80, top = 64, width = 740, height = 340;
    double x_min, x_max, y_min, y_max, pad;
    double xt[6], yt[6];
    if (g_nsamples < 1)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    x_min = g_samples[0].t_sec;
    x_max = g_samples[g_nsamples - 1].t_sec;
    if (x_max == x_min)
        x_max = x_min + 1.0;
    cmin = cmax = g_samples[0].count;
    for (i = 1; i < g_nsamples; i++) {
        if (g_samples[i].count < cmin)
            cmin = g_samples[i].count;
        if (g_samples[i].count > cmax)
            cmax = g_samples[i].count;
    }
    pad = (cmax - cmin) * 0.08;
    if (pad < 1)
        pad = 1;
    y_min = cmin - pad;
    if (y_min < 0)
        y_min = 0;
    y_max = cmax + pad;
    ticks(x_min, x_max, 6, xt);
    ticks(y_min, y_max, 6, yt);
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"860\" height=\"480\" viewBox=\"0 0 860 480\">\n"
            "  <style>\n"
            "    text { font-family: Segoe UI, sans-serif; fill: #1a1a1a; font-size: 12px; }\n"
            "    .title { font-size: 16px; font-weight: 600; }\n"
            "    .caption { font-size: 11px; fill: #555; }\n"
            "    line { stroke: #ddd; stroke-width: 1; }\n"
            "    .frame { stroke: #333; fill: none; }\n"
            "    .series { fill: none; stroke: #1d4ed8; stroke-width: 2; }\n"
            "  </style>\n"
            "  <text class=\"title\" x=\"80\" y=\"28\">Число процессов в ОС во времени</text>\n"
            "  <text class=\"caption\" x=\"80\" y=\"46\">process_count.csv</text>\n");
    for (i = 0; i < 6; i++) {
        double gy = top + height - (yt[i] - y_min) / (y_max - y_min) * height;
        double gx = left + (xt[i] - x_min) / (x_max - x_min) * width;
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" />\n",
                left, gy, left + width, gy);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" />\n",
                gx, top, gx, top + height);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" text-anchor=\"end\">%d</text>\n",
                left - 8, gy + 4, (int)(yt[i] + 0.5));
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" text-anchor=\"middle\">%.1f</text>\n",
                gx, top + height + 18, xt[i]);
    }
    fprintf(f, "  <rect class=\"frame\" x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" />\n",
            left, top, width, height);
    fprintf(f, "  <polyline class=\"series\" points=\"");
    for (i = 0; i < g_nsamples; i++) {
        double sx = left + (g_samples[i].t_sec - x_min) / (x_max - x_min) * width;
        double sy = top + height - (g_samples[i].count - y_min) / (y_max - y_min) * height;
        fprintf(f, "%.1f,%.1f ", sx, sy);
    }
    fprintf(f, "\" />\n");
    fprintf(f, "  <text x=\"%.1f\" y=\"455\" text-anchor=\"middle\">t, s</text>\n",
            left + width / 2);
    fprintf(f,
            "  <text x=\"22\" y=\"%.1f\" text-anchor=\"middle\" transform=\"rotate(-90 22 %.1f)\">N</text>\n"
            "</svg>\n",
            top + height / 2, top + height / 2);
    fclose(f);
}

static void sample_for(unsigned ms, unsigned interval_ms)
{
    double until = monotonic_sec() + ms / 1000.0;
    do {
        record();
        sleep_ms(interval_ms);
    } while (monotonic_sec() < until);
}

int main(int argc, char **argv)
{
    int n = 16;
    unsigned lifetime_ms = 4000;
    unsigned stagger_ms = 80;
    unsigned interval_ms = 150;
    unsigned baseline_ms = 2000;
    unsigned tail_ms = 2000;
    int i, spawned = 0;
    int max_depth = 0;
#ifdef _WIN32
    HANDLE kids[HARD_CAP];
#else
    pid_t kids[HARD_CAP];
#endif

    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        if (argc >= 3)
            lifetime_ms = (unsigned)atoi(argv[2]);
        return child_main(lifetime_ms);
    }
    
    if (argc >= 2 && strcmp(argv[1], "--child-recursive") == 0) {
        if (argc >= 6) {
            int depth = atoi(argv[2]);
            int max_d = atoi(argv[3]);
            unsigned lifetime = atoi(argv[4]);
            unsigned stagger = atoi(argv[5]);
            return child_main_recursive(lifetime, depth, max_d, stagger);
        }
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            n = atoi(argv[++i]);
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            max_depth = atoi(argv[++i]);
    }
    
    if (n < 0)
        n = 0;
    if (n > HARD_CAP)
        n = HARD_CAP;
    
    memset(kids, 0, sizeof(kids));
    g_t0 = monotonic_sec();
    sample_for(baseline_ms, interval_ms);
    
    if (max_depth > 0) {
        for (i = 0; i < n; i++) {
            if (spawn_child_recursive(lifetime_ms, 1, max_depth, stagger_ms, &kids[i]) == 0)
                spawned++;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (spawn_child(lifetime_ms, &kids[i]) == 0)
                spawned++;
        }
    }
    
    while (any_alive(kids, spawned)) {
        record();
        sleep_ms(interval_ms);
    }
    reap_all(kids, spawned);
    sample_for(tail_ms, interval_ms);
    write_csv("process_count.csv");
    write_svg("process_count.svg");
    return 0;
}