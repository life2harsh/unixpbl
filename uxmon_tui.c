// uxhtop.c - Advanced Unix Task Manager (smooth waves + system info + free mem)
// Build: gcc -O2 -Wall -Wextra -std=c11 uxhtop.c -lncurses -lm -o uxhtop

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <ncurses.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// ---------- Tunables ----------
#define MAX_CORES 128
#define MAX_PROCS 65536
#define MAX_COMM 64
#define HIST_W 120 // wide history for maximized terminals
#define CPU_MS 120 // smoother updates (~0.7 s)
#define PROC_MS 1000
#define FRAME_MS 120
// UI framerate

// ---------- Color ----------
enum {
  C_DEFAULT = 1,
  C_GREEN,
  C_YELLOW,
  C_RED,
  C_CYAN,
  C_MAGENTA,
  C_BLUE,
  C_WHITE,
  C_BG_CYAN,
  C_BG_GREEN,
  C_ORANGE,
  C_BLACK_WHITE
};

static void init_colors(void) {
  start_color();
  use_default_colors();
  init_pair(C_DEFAULT, COLOR_WHITE, -1);
  init_pair(C_GREEN, COLOR_GREEN, -1);
  init_pair(C_YELLOW, COLOR_YELLOW, -1);
  init_pair(C_RED, COLOR_RED, -1);
  init_pair(C_CYAN, COLOR_CYAN, -1);
  init_pair(C_MAGENTA, COLOR_MAGENTA, -1);
  init_pair(C_BLUE, COLOR_BLUE, -1);
  init_pair(C_WHITE, COLOR_WHITE, -1);
  init_pair(C_BG_CYAN, COLOR_BLACK, COLOR_CYAN);
  init_pair(C_BG_GREEN, COLOR_BLACK, COLOR_GREEN);
  init_pair(C_ORANGE, COLOR_RED, -1);
  init_pair(C_BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
}

// ---------- Time ----------
static inline long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------- System Info ----------
static int NCPU = 1;
static char cpu_model[128] = "Unknown CPU";
static double cpu_freq_ghz = 0.0;
static char distro[128] = "Linux";
static char kernel_rel[128] = "";
static char host[128] = "";

static void read_cpu_info(void) {
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (!strncmp(line, "model name", 10)) {
      char *p = strchr(line, ':');
      if (p) {
        p++;
        while (*p == ' ')
          p++;
        strncpy(cpu_model, p, sizeof(cpu_model) - 1);
        cpu_model[strcspn(cpu_model, "\n")] = 0;
      }
    }
    if (!strncmp(line, "cpu MHz", 7) && cpu_freq_ghz == 0.0) {
      char *p = strchr(line, ':');
      if (p) {
        double mhz = 0;
        sscanf(p + 1, "%lf", &mhz);
        cpu_freq_ghz = mhz / 1000.0;
      }
    }
  }
  fclose(f);
}
static void read_os_release(void) {
  FILE *f = fopen("/etc/os-release", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (!strncmp(line, "PRETTY_NAME=", 12)) {
      char *v = line + 12;
      size_t n = strlen(v);
      if (n && v[0] == '"') {
        v++;
        n -= 2;
      }
      snprintf(distro, sizeof(distro), "%.*s", (int)n, v);
      distro[strcspn(distro, "\n")] = 0;
      break;
    }
  }
  fclose(f);
}
static void read_uname(void) {
  struct utsname u;
  if (uname(&u) == 0) {
    snprintf(kernel_rel, sizeof(kernel_rel), "%s %s", u.sysname, u.release);
    snprintf(host, sizeof(host), "%s", u.nodename);
  }
}
static void read_uptime(int *h, int *m, int *s) {
  double up = 0;
  FILE *f = fopen("/proc/uptime", "r");
  if (f) {
    fscanf(f, "%lf", &up);
    fclose(f);
  }
  int secs = (int)up;
  *h = secs / 3600;
  *m = (secs % 3600) / 60;
  *s = secs % 60;
}

// ---------- Temp/Freq ----------
static char tz_path[128] = "";
static double temp_smooth = 0.0;
static void detect_temp_sensor(void) {
  for (int i = 0; i < 128; i++) {
    char tpath[128];
    snprintf(tpath, sizeof(tpath), "/sys/class/thermal/thermal_zone%d/type", i);
    FILE *f = fopen(tpath, "r");
    if (!f)
      continue;
    char type[128] = "";
    fgets(type, sizeof(type), f);
    fclose(f);
    for (char *p = type; *p; ++p)
      *p = tolower(*p);
    if (strstr(type, "cpu") || strstr(type, "x86") || strstr(type, "pkg") ||
        strstr(type, "soc") || strstr(type, "core")) {
      snprintf(tz_path, sizeof(tz_path),
               "/sys/class/thermal/thermal_zone%d/temp", i);
      return;
    }
  }
}
static double temp_c(void) {
  if (!tz_path[0])
    return temp_smooth;
  FILE *f = fopen(tz_path, "r");
  if (!f)
    return temp_smooth;
  double t = 0;
  fscanf(f, "%lf", &t);
  fclose(f);
  t /= 1000.0;
  temp_smooth = (temp_smooth == 0.0) ? t : (0.7 * temp_smooth + 0.3 * t);
  return temp_smooth;
}
static void read_cpu_freq_mhz(double *freqs) {
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    FILE *f = fopen(path, "r");
    if (f) {
      unsigned long khz = 0;
      if (fscanf(f, "%lu", &khz) == 1)
        freqs[i] = khz / 1000.0;
      else
        freqs[i] = 0;
      fclose(f);
    } else
      freqs[i] = 0;
  }
}

// ---------- Memory ----------
static int mem_read_kb(unsigned long *tot, unsigned long *free_kb,
                       unsigned long *avail) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return -1;
  char k[64];
  unsigned long v;
  *tot = *free_kb = *avail = 0;
  while (fscanf(f, "%63s %lu kB", k, &v) == 2) {
    if (!strcmp(k, "MemTotal:"))
      *tot = v;
    else if (!strcmp(k, "MemFree:"))
      *free_kb = v;
    else if (!strcmp(k, "MemAvailable:"))
      *avail = v;
    int c;
    while ((c = fgetc(f)) != '\n' && c != EOF)
      ;
  }
  fclose(f);
  return 0;
}

// ---------- CPU Sampling + History ----------
typedef struct {
  double total;
  double core[MAX_CORES];
  int ncores;
} CpuSnap;
static CpuSnap cpu = {0};
static double hist_cpu[MAX_CORES][HIST_W]; // 0..1
static int hpos = 0;

static void push_hist(void) {
  for (int i = 0; i < cpu.ncores && i < MAX_CORES; i++) {
    // Normalize core utilization (0–100 → 0–1)
    double usage = cpu.core[i] / 100.0;
    if (usage < 0.0)
      usage = 0.0;
    if (usage > 1.0)
      usage = 1.0;

    hist_cpu[i][hpos] = usage;
  }
  hpos = (hpos + 1) % HIST_W;
}
static void cpu_sample(void) {
  static unsigned long long pu[MAX_CORES + 1], pn[MAX_CORES + 1],
      ps[MAX_CORES + 1], pi[MAX_CORES + 1];
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return;
  int idx = 0;
  char id[16];
  unsigned long long u, n, s, idle, iow, irq, sirq, steal;
  while (fscanf(f, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", id, &u, &n,
                &s, &idle, &iow, &irq, &sirq, &steal) == 9) {
    if (strncmp(id, "cpu", 3) != 0)
      break;
    unsigned long long tot = u + n + s + idle + iow + irq + sirq + steal;
    unsigned long long pt = pu[idx] + pn[idx] + ps[idx] + pi[idx];
    unsigned long long dt = tot - pt, di = idle - pi[idx];
    double use = dt ? (1.0 - (double)di / (double)dt) : 0.0;
    if (idx == 0)
      cpu.total = 0.8 * cpu.total + 0.2 * use;
    else
      cpu.core[idx - 1] = 0.8 * cpu.core[idx - 1] + 0.2 * use;
    pu[idx] = u;
    pn[idx] = n;
    ps[idx] = s;
    pi[idx] = idle;
    idx++;
    if (idx > MAX_CORES)
      break;
  }
  fclose(f);
  cpu.ncores = idx - 1;
  if (cpu.ncores <= 0)
    cpu.ncores = 1;
  push_hist();
}

// ---------- Neofetch ----------
static char neofetch_info[4096] = "";
static void run_neofetch_stdout(void) {
  neofetch_info[0] = 0;
  FILE *fp = popen("neofetch --stdout 2>/dev/null", "r");
  if (!fp)
    return;
  size_t pos = 0;
  char line[256];
  while (fgets(line, sizeof(line), fp) && pos < sizeof(neofetch_info) - 256) {
    size_t len = strlen(line);
    memcpy(neofetch_info + pos, line, len);
    pos += len;
  }
  neofetch_info[pos] = '\0';
  pclose(fp);
}
static const char *pick_ascii_logo(const char *pretty) {
  static const char *arch = "      /\\        \n"
                            "     /  \\       \n"
                            "    /_/\\_\\      \n"
                            "   /      \\     \n"
                            "  /  /\\    \\    \n"
                            " /__/  \\____\\   \n";
  static const char *ubuntu = "         _        \n"
                              "     ---(_)--     \n"
                              "   _/  /   \\ \\_   \n"
                              "  /_._/_____\\_._\\ \n"
                              "     \\_\\_/\\_/_/   \n";
  static const char *debian = "    ____          \n"
                              "  _/    \\_        \n"
                              " /  _  _  \\       \n"
                              " \\_/ \\/ \\_/       \n"
                              "    \\__/          \n";
  static const char *fedora = "   _______        \n"
                              "  /  __  /        \n"
                              " /  /_/ /__       \n"
                              "/______/__/       \n";
  static const char *manjaro = " _______          \n"
                               "|  ___  |         \n"
                               "| |   | |____     \n"
                               "| |   | |___ |    \n"
                               "|_|   |_|___||    \n";
  static const char *generic = "  __  __          \n"
                               " |  \\/  |         \n"
                               " | \\  / | ___     \n"
                               " | |\\/| |/ _ \\    \n"
                               " | |  | | (_) |   \n"
                               " |_|  |_|\\___/    \n";
  char low[128];
  snprintf(low, sizeof(low), "%s", pretty ? pretty : "");
  for (char *p = low; *p; ++p)
    *p = tolower(*p);
  if (strstr(low, "arch"))
    return arch;
  if (strstr(low, "ubuntu"))
    return ubuntu;
  if (strstr(low, "debian"))
    return debian;
  if (strstr(low, "fedora"))
    return fedora;
  if (strstr(low, "manjaro"))
    return manjaro;
  return generic;
}

// ---------- Processes ----------
typedef struct {
  pid_t pid;
  uid_t uid;
  char comm[MAX_COMM];
  unsigned long ut, st;
  double cpu_pct;
  unsigned long rss_kb;
  int nicev;
  bool running;
} PInfo;

static PInfo procs[MAX_PROCS];
static int nprocs = 0;
static long long gtot_prev = 0, gtot_cur = 0;

static int is_pid_dir(const char *s) {
  for (; *s; s++)
    if (!isdigit((unsigned char)*s))
      return 0;
  return 1;
}

static int read_comm(pid_t pid, char *out, size_t n) {
  char p[64];
  snprintf(p, sizeof(p), "/proc/%d/comm", pid);
  FILE *f = fopen(p, "r");
  if (!f)
    return -1;
  if (!fgets(out, n, f)) {
    fclose(f);
    return -1;
  }
  out[strcspn(out, "\n")] = 0;
  fclose(f);
  return 0;
}
static int read_stat(pid_t pid, unsigned long *ut, unsigned long *st,
                     int *nicev, bool *running) {
  char p[64];
  snprintf(p, sizeof(p), "/proc/%d/stat", pid);
  FILE *f = fopen(p, "r");
  if (!f)
    return -1;
  char line[1024];
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return -1;
  }
  fclose(f);
  char *rp = strrchr(line, ')');
  if (!rp)
    return -1;
  char *rest = rp + 2;
  int field = 3;
  char *save = rest, *tok;
  *running = true;
  while ((tok = strsep(&save, " "))) {
    if (*tok == '\0')
      continue;
    if (field == 3) {
      char st = tok[0];
      *running = (st != 'T' && st != 'Z');
    } else if (field == 14) {
      *ut = strtoul(tok, NULL, 10);
    } else if (field == 15) {
      *st = strtoul(tok, NULL, 10);
    } else if (field == 19) {
      *nicev = (int)strtol(tok, NULL, 10);
      break;
    }
    field++;
  }
  return 0;
}
static int read_status(pid_t pid, uid_t *uid, unsigned long *rss_kb) {
  char p[64];
  snprintf(p, sizeof(p), "/proc/%d/status", pid);
  FILE *f = fopen(p, "r");
  if (!f)
    return -1;
  char line[256];
  *rss_kb = 0;
  *uid = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned int u;
    unsigned long v;
    char unit[32];
    if (sscanf(line, "Uid:\t%u", &u) == 1) {
      *uid = (uid_t)u;
      continue;
    }
    if (sscanf(line, "VmRSS:\t%lu %31s", &v, unit) == 2) {
      *rss_kb = v;
    }
  }
  fclose(f);
  return 0;
}
static const char *uname_from_uid(uid_t uid) {
  struct passwd *pw = getpwuid(uid);
  return pw ? pw->pw_name : "unknown";
}
static void read_cpu_totals(void) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return;
  char buf[256];
  if (!fgets(buf, sizeof(buf), f)) {
    fclose(f);
    return;
  }
  fclose(f);
  unsigned long long u, n, s, idle, iow, irq, sirq, steal;
  char lab[8];
  sscanf(buf, "%7s %llu %llu %llu %llu %llu %llu %llu %llu", lab, &u, &n, &s,
         &idle, &iow, &irq, &sirq, &steal);
  long long tot = (long long)(u + n + s + idle + iow + irq + sirq + steal);
  gtot_prev = gtot_cur;
  gtot_cur = tot;
}
static void scan_processes(void) {
  nprocs = 0;
  static PInfo prev[65536];
  static int prevn = 0;
  DIR *d = opendir("/proc");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!is_pid_dir(e->d_name))
      continue;
    pid_t pid = (pid_t)atoi(e->d_name);
    if (nprocs >= MAX_PROCS)
      break;
    PInfo p = {0};
    p.pid = pid;
    if (read_comm(pid, p.comm, sizeof(p.comm)) < 0)
      continue;
    unsigned long ut = 0, st = 0;
    int nicev = 0;
    bool running = true;
    if (read_stat(pid, &ut, &st, &nicev, &running) < 0)
      continue;
    uid_t uid = 0;
    unsigned long rss = 0;
    read_status(pid, &uid, &rss);
    p.uid = uid;
    p.rss_kb = rss;
    p.nicev = nicev;
    p.running = running;
    int fi = -1;
    for (int i = 0; i < prevn; i++)
      if (prev[i].pid == pid) {
        fi = i;
        break;
      }
    if (fi < 0) {
      p.cpu_pct = 0.0;
      if (prevn < (int)(sizeof(prev) / sizeof(prev[0])))
        prev[prevn++] = p;
    } else {
      unsigned long dut = ut - prev[fi].ut, dst = st - prev[fi].st;
      long long dtot = (gtot_cur > gtot_prev) ? (gtot_cur - gtot_prev) : 1;
      p.cpu_pct = (double)(dut + dst) / (double)dtot * 100;
      prev[fi] = p;
    }
    procs[nprocs++] = p;
  }
  closedir(d);
}
static int cmp_cpu(const void *a, const void *b) {
  const PInfo *x = a, *y = b;
  if (y->cpu_pct > x->cpu_pct)
    return 1;
  if (y->cpu_pct < x->cpu_pct)
    return -1;
  return (int)(x->pid - y->pid);
}
static int cmp_mem(const void *a, const void *b) {
  const PInfo *x = a, *y = b;
  if (y->rss_kb > x->rss_kb)
    return 1;
  if (y->rss_kb < x->rss_kb)
    return -1;
  return (int)(x->pid - y->pid);
}

// ---------- UI helpers ----------
static void draw_box(int y, int x, int h, int w) {
  attron(COLOR_PAIR(C_WHITE));
  mvaddch(y, x, ACS_ULCORNER);
  mvhline(y, x + 1, ACS_HLINE, w - 2);
  mvaddch(y, x + w - 1, ACS_URCORNER);
  for (int i = 1; i < h - 1; i++) {
    mvaddch(y + i, x, ACS_VLINE);
    mvaddch(y + i, x + w - 1, ACS_VLINE);
  }
  mvaddch(y + h - 1, x, ACS_LLCORNER);
  mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
  mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
  attroff(COLOR_PAIR(C_WHITE));
}

// Smooth wave (line trace) drawer: connects points vertically for continuity
// Smooth wave graph renderer using historical CPU samples
static void draw_wave(int y, int x, int h, int w, int core, const char *label) {
  draw_box(y, x, h, w); // Draw the enclosing box

  // Header
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, x + 2, "%s", label);
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  // Graph bounds inside box
  int gw = w - 2;
  int gh = h - 2;
  if (gw > HIST_W)
    gw = HIST_W;

  // Draw wave based on history
  for (int i = 0; i < gw; i++) {
    // Calculate position in circular buffer
    int idx = (hpos + HIST_W - gw + i) % HIST_W;
    double v = hist_cpu[core][idx]; // CPU utilization value [0–1]

    // Smooth curve using vertical interpolation
    int height = (int)(v * gh);
    if (height < 0)
      height = 0;
    if (height > gh)
      height = gh;

    // Color based on load
    int col = (v > 0.8) ? C_RED : (v > 0.5) ? C_YELLOW : C_GREEN;

    attron(COLOR_PAIR(col));
    mvaddch(y + h - 2 - height, x + 1 + i, '*'); // Wave dot
    attroff(COLOR_PAIR(col));
  }

  // Label current utilization below
  double cur = hist_cpu[core][(hpos - 1 + HIST_W) % HIST_W];
  attron(COLOR_PAIR(C_WHITE));
  mvprintw(y + h - 1, x + w / 2 - 4, "%4.1f%%", cur * 100.0);
  attroff(COLOR_PAIR(C_WHITE));
}

// ---------- Pages ----------
enum {
  PAGE_MAIN = 0,
  PAGE_GRAPH,
  PAGE_SYSINFO,
  PAGE_HELP,
  PAGE_ABOUT,
  PAGE_PROCS
};
static int page = PAGE_MAIN, menu_sel = 0, proc_sel = 0, sort_mode = 0;

static void draw_main(void) {
  erase();
  int W = COLS, H = LINES;
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "uxhtop");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  int y = 2;
  mvprintw(y++, 2, "CPU: %s", cpu_model);
  mvprintw(y++, 2, "Base: %.2f GHz   Cores: %d", cpu_freq_ghz, NCPU);
  mvprintw(y++, 2, "OS: %s", distro);
  mvprintw(y++, 2, "Kernel: %s", kernel_rel);
  mvprintw(y++, 2, "Host: %s", host);

  y++;
  const char *items[] = {"< Graph >", "< System Info >", "< Help >",
                         "< About >", "< Quit >"};
  mvprintw(y++, 2, "Menu");
  for (int i = 0; i < 5; i++) {
    if (menu_sel == i) {
      attron(COLOR_PAIR(C_BG_GREEN) | A_BOLD);
      mvprintw(y + i, 2, "%s", items[i]);
      attroff(COLOR_PAIR(C_BG_GREEN) | A_BOLD);
    } else {
      mvprintw(y + i, 2, "%s", items[i]);
    }
  }

  // right side: ASCII logo + neofetch info
  int rx = W / 3;
  const char *logo = pick_ascii_logo(distro);
  attron(COLOR_PAIR(C_MAGENTA) | A_BOLD);
  int ly = 2;
  for (const char *p = logo; *p;) {
    const char *nl = strchr(p, '\n');
    if (!nl)
      break;
    mvprintw(ly++, rx, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6)
      break;
  }
  attroff(COLOR_PAIR(C_MAGENTA) | A_BOLD);

  attron(COLOR_PAIR(C_CYAN));
  char *line = (char *)neofetch_info;
  int ny = 2;
  int ix = rx + 28;
  if (ix > W - 30)
    ix = rx + 18;
  while (*line && ny < H - 2) {
    char *end = strchr(line, '\n');
    if (!end)
      break;
    int len = end - line;
    if (len > W - ix - 2)
      len = W - ix - 2;
    mvprintw(ny++, ix, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(
      H - 1, 2,
      "↑/↓ or j/k = select   Enter = open   TAB = Procs   q/ESC = quit/back");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  refresh();
}

// replaces your draw_graphs()
static void draw_graphs(void) {
  // throttle wave sampling + history push happens in cpu_sample()
  int W = COLS, H = LINES;

  erase();
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "CPU Activity (wave graphs)");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  // grid layout
  int cols = (cpu.ncores >= 12)  ? 4
             : (cpu.ncores >= 6) ? 3
             : (cpu.ncores >= 2) ? 2
                                 : 1;
  int rows = (cpu.ncores + cols - 1) / cols;

  int panel_w = W / cols - 3;
  if (panel_w < 22)
    panel_w = 22;
  int panel_h = (H - 4) / rows;
  if (panel_h < 7)
    panel_h = 7;

  // cap width to history so we don’t draw past available samples
  if (panel_w > HIST_W + 2)
    panel_w = HIST_W + 2;

  int idx = 0;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols && idx < cpu.ncores; c++, idx++) {
      int x = 1 + c * (panel_w + 2);
      int y = 1 + r * panel_h + 1;

      // call your wave renderer
      // If your draw_wave signature has 6th arg (label), use the first line.
      // If it has only 5 args, use the second line and remove the label.
      char lbl[8];
      snprintf(lbl, sizeof(lbl), "C%d", idx);
      draw_wave(y, x, panel_h, panel_w, idx, lbl);
      // draw_wave(y, x, panel_h, panel_w, idx);
    }
  }

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q: back");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  refresh();
}
static void draw_sysinfo(void) {
  int W = COLS, H = LINES;
  erase();

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "System Info");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  int y = 2;

  // --- CPU model / base / cores / uptime ---
  attron(COLOR_PAIR(C_WHITE));
  mvprintw(y++, 2, "CPU Model: %s", cpu_model);
  mvprintw(y++, 2, "Cores: %d   Base: %.2f GHz", NCPU, cpu_freq_ghz);
  int uh, um, us;
  read_uptime(&uh, &um, &us);
  mvprintw(y++, 2, "Uptime: %02d:%02d:%02d", uh, um, us);

  // --- Temperature + bar ---
  double tc = temp_c();
  mvprintw(y, 2, "Temp: %.1f C   Sensor: %s", tc, tz_path[0] ? tz_path : "N/A");
  double t_ratio = tc / 100.0;
  if (t_ratio < 0)
    t_ratio = 0;
  if (t_ratio > 1)
    t_ratio = 1;
  int barw = (W > 60) ? 40 : 30;
  int filled = (int)(t_ratio * barw);
  int col = (t_ratio > 0.80) ? C_RED : (t_ratio > 0.60) ? C_YELLOW : C_GREEN;
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(y + 1, 4, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  mvprintw(y + 1, 4 + barw + 2, "%3.0f%%", t_ratio * 100.0);
  y += 3;

  // --- Memory + bar (Used / Avail / Free / Total) ---
  unsigned long mt = 0, mf = 0, ma = 0; // total, free, available (kB)
  mem_read_kb(&mt, &mf, &ma);
  double mem_used = (double)(mt - ma); // kB effectively used
  double mem_avail = (double)ma;
  double used_pct = mt ? mem_used / (double)mt : 0.0;

  mvprintw(
      y++, 2, "Memory: Used %lu MB | Avail %lu MB | Free %lu MB | Total %lu MB",
      (unsigned long)(mem_used / 1024.0), (unsigned long)(mem_avail / 1024.0),
      (unsigned long)(mf / 1024.0), (unsigned long)(mt / 1024.0));

  filled = (int)(used_pct * barw);
  col = (used_pct > 0.80) ? C_RED : (used_pct > 0.60) ? C_YELLOW : C_GREEN;
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(y, 4, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  mvprintw(y, 4 + barw + 2, "%3.0f%%", used_pct * 100.0);
  y += 3;

  // --- Per-core frequency bars (MHz) ---
  double freqs[MAX_CORES] = {0};
  read_cpu_freq_mhz(freqs); // uses scaling_cur_freq; values already in MHz
  mvprintw(y++, 2, "Per-core frequency (MHz):");

  // --- Per-core frequency bars (MHz) ---
  // --- Per-core frequency bars (MHz) ---
  read_cpu_freq_mhz(freqs); // reuse the freqs[] already declared above

  // Compute dynamic range for scaling
  double fmin = 1e9, fmax = 0;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if (freqs[i] > fmax)
      fmax = freqs[i];
    if (freqs[i] < fmin && freqs[i] > 100)
      fmin = freqs[i];
  }
  if (fmax - fmin < 500)
    fmin = 0; // prevent div-by-zero when all equal

  mvprintw(y++, 2, "Per-core frequency (MHz):");

  int perrow = (W > 90) ? 8 : 6;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if ((i % perrow) == 0)
      y++;

    // Normalize based on observed min–max so bars look balanced
    double ratio = (freqs[i] - fmin) / (fmax - fmin);
    if (ratio < 0)
      ratio = 0;
    if (ratio > 1)
      ratio = 1;

    int fcol = (freqs[i] > 0.8 * fmax)   ? C_RED
               : (freqs[i] > 0.6 * fmax) ? C_YELLOW
                                         : C_GREEN;

    int startx = 4 + (i % perrow) * (barw + 12);
    mvprintw(y, startx, "C%-2d:", i);

    attron(COLOR_PAIR(fcol));
    int w = (int)(ratio * barw);
    mvhline(y, startx + 6, ACS_CKBOARD, w);
    attroff(COLOR_PAIR(fcol));

    mvprintw(y, startx + 6 + barw + 1, "%4.0f", freqs[i]);
  }
  attroff(COLOR_PAIR(C_WHITE));

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "Press ESC or q to return");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  refresh();
}

static void draw_help(void) {
  erase();
  int W = COLS, H = LINES;
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "Help");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  int y = 2;
  mvprintw(y++, 2, "Navigation:");
  mvprintw(y++, 4, "↑/↓ or j/k  - Move selection");
  mvprintw(y++, 4, "Enter        - Select");
  mvprintw(y++, 4, "ESC or q     - Back/Quit");
  mvprintw(y++, 4, "TAB          - Toggle Process Manager");
  y++;
  mvprintw(y++, 2, "Process Manager:");
  mvprintw(y++, 4, "c            - Sort by CPU     m  - Sort by Memory");
  mvprintw(y++, 4, "K            - Kill process    S  - Stop/Continue");
  mvprintw(y++, 4, "+ / -        - Increase/Decrease priority (nice)");
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q = back");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  refresh();
}

static void draw_about(void) {
  run_neofetch_stdout(); // refresh when opening
  erase();
  int W = COLS, H = LINES;
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "About");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  const char *logo = pick_ascii_logo(distro);
  attron(COLOR_PAIR(C_MAGENTA) | A_BOLD);
  int ly = 2;
  for (const char *p = logo; *p;) {
    const char *nl = strchr(p, '\n');
    if (!nl)
      break;
    mvprintw(ly++, 2, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6)
      break;
  }
  attroff(COLOR_PAIR(C_MAGENTA) | A_BOLD);

  attron(COLOR_PAIR(C_WHITE) | A_BOLD);
  mvprintw(2, 24, "uxhtop - Advanced Unix Task Manager");
  attroff(COLOR_PAIR(C_WHITE) | A_BOLD);
  mvprintw(3, 24, "System: %s", distro);
  mvprintw(4, 24, "Kernel: %s", kernel_rel);
  mvprintw(5, 24, "Host:   %s", host);

  int ix = 24, ny = 7;
  attron(COLOR_PAIR(C_CYAN));
  char *line = (char *)neofetch_info;
  while (*line && ny < H - 2) {
    char *end = strchr(line, '\n');
    if (!end)
      break;
    int len = end - line;
    if (len > W - ix - 2)
      len = W - ix - 2;
    mvprintw(ny++, ix, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q = back");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  refresh();
}

static void draw_procs(void) {
  int W = COLS, H = LINES;
  erase();
  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "Process Manager - %d processes", nprocs);
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);

  // CPU bar
  mvprintw(2, 2, "CPU:");
  int barw = W / 3;
  if (barw < 20)
    barw = 20;
  double pct = cpu.total;
  for (int i = 0; i < barw; i++) {
    int col = (pct > 0.80) ? C_RED : (pct > 0.50) ? C_YELLOW : C_GREEN;
    if (i < (int)(pct * barw)) {
      attron(COLOR_PAIR(col));
      mvaddch(2, 8 + i, ACS_CKBOARD);
      attroff(COLOR_PAIR(col));
    } else
      mvaddch(2, 8 + i, ' ');
  }
  mvprintw(2, 8 + barw + 2, "%3d%%", (int)(pct * 100));

  // Memory usage with Used/Avail/Free
  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  double mem_used = mt - ma; // effectively used memory (real)
  double mem_free = mf;      // explicitly free memory
  double mem_avail = ma;     // available memory
  double used_pct = mt ? (double)mem_used / mt : 0.0;
  double free_pct = mt ? (double)mem_avail / mt : 0.0;
  mvprintw(3, 2, "MEM:");
  for (int i = 0; i < barw; i++) {
    int col = (used_pct > 0.80)   ? C_RED
              : (used_pct > 0.50) ? C_YELLOW
                                  : C_GREEN;
    if (i < (int)(used_pct * barw)) {
      attron(COLOR_PAIR(col));
      mvaddch(3, 8 + i, ACS_CKBOARD);
      attroff(COLOR_PAIR(col));
    } else
      mvaddch(3, 8 + i, ' ');
  }
  mvprintw(3, 8 + barw + 2,
           "Used:%3d%%  Free:%3d%%  (Used:%luMB  Avail:%luMB  Free:%luMB)",
           (int)(used_pct * 100), (int)(free_pct * 100),
           (unsigned long)mem_used / 1024, (unsigned long)mem_avail / 1024,
           (unsigned long)mem_free / 1024);
  // Header
  attron(A_BOLD | COLOR_PAIR(C_CYAN));
  mvprintw(5, 2, "%-8s %-20s %-12s %8s %10s %5s %s", "PID", "COMMAND", "USER",
           "CPU%", "MEM(MB)", "NI", "STATE");
  attroff(A_BOLD | COLOR_PAIR(C_CYAN));

  if (sort_mode == 0)
    qsort(procs, nprocs, sizeof(PInfo), cmp_cpu);
  else
    qsort(procs, nprocs, sizeof(PInfo), cmp_mem);

  int rows = H - 8;
  if (rows < 1)
    rows = 1;
  if (proc_sel >= nprocs)
    proc_sel = (nprocs ? nprocs - 1 : 0);
  if (proc_sel < 0)
    proc_sel = 0;

  int start = proc_sel - rows / 2;
  if (start < 0)
    start = 0;
  int end = start + rows;
  if (end > nprocs)
    end = nprocs;

  int y = 6;
  for (int i = start; i < end; i++, y++) {
    PInfo *p = &procs[i];
    if (i == proc_sel)
      attron(COLOR_PAIR(C_BLACK_WHITE) | A_BOLD);
    else
      attron(COLOR_PAIR(C_WHITE));
    double ui_pct = p->cpu_pct * 100.0 / NCPU;
    if (ui_pct < 0)
      ui_pct = 0;
    if (ui_pct > 9999)
      ui_pct = 9999;
    mvprintw(y, 2, "%-8d %-20.20s %-12.12s %7.1f %10.1f %5d %-4s", p->pid,
             p->comm, uname_from_uid(p->uid), ui_pct, p->rss_kb / 1024.0,
             p->nicev, p->running ? "RUN" : "STOP");
    if (i == proc_sel)
      attroff(COLOR_PAIR(C_BLACK_WHITE) | A_BOLD);
    else
      attroff(COLOR_PAIR(C_WHITE));
  }

  attron(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2,
           "↑↓ j/k:Move  c:CPU  m:Mem  K:Kill  S:Stop/Cont  +/-:Nice  TAB:Main "
           " q:Quit");
  attroff(COLOR_PAIR(C_BG_CYAN) | A_BOLD);
  refresh();
}

// ---------- Actions ----------
static void act_kill(pid_t pid) {
  if (kill(pid, SIGTERM) == 0)
    return;
  usleep(200000);
  kill(pid, SIGKILL);
}
static void act_stopcont(PInfo *p) {
  if (p->running) {
    kill(p->pid, SIGSTOP);
    p->running = false;
  } else {
    kill(p->pid, SIGCONT);
    p->running = true;
  }
}
static void act_renice(pid_t pid, int delta) {
  errno = 0;
  int old = getpriority(PRIO_PROCESS, pid);
  if (errno)
    return;
  int nv = old + delta;
  if (nv < -20)
    nv = -20;
  if (nv > 19)
    nv = 19;
  setpriority(PRIO_PROCESS, pid, nv);
}

// ---------- Main ----------
int main(void) {
  NCPU = sysconf(_SC_NPROCESSORS_ONLN);
  if (NCPU <= 0)
    NCPU = 1;
  read_os_release();
  read_uname();
  read_cpu_info();
  detect_temp_sensor();
  run_neofetch_stdout();

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);
  if (has_colors())
    init_colors();

  long t_cpu = now_ms(), t_proc = now_ms();

  cpu_sample();
  read_cpu_totals();
  scan_processes();

  while (1) {
    long t = now_ms();
    if (t - t_cpu >= CPU_MS) {
      cpu_sample();
      t_cpu = t;
    }
    if (t - t_proc >= PROC_MS && page == PAGE_PROCS) {
      read_cpu_totals();
      scan_processes();
      t_proc = t;
    }

    switch (page) {
    case PAGE_MAIN:
      draw_main();
      break;
    case PAGE_GRAPH:
      draw_graphs();
      break;
    case PAGE_SYSINFO:
      draw_sysinfo();
      break;
    case PAGE_HELP:
      draw_help();
      break;
    case PAGE_ABOUT:
      draw_about();
      break;
    case PAGE_PROCS:
      draw_procs();
      break;
    }

    napms(FRAME_MS);
    int ch = getch();
    if (ch == ERR)
      continue;

    if (ch == 'q' || ch == 'Q' || ch == 27) {
      if (page != PAGE_MAIN) {
        page = PAGE_MAIN;
        continue;
      }
      endwin();
      return 0;
    }
    if (ch == '\t') {
      page = (page == PAGE_PROCS) ? PAGE_MAIN : PAGE_PROCS;
      continue;
    }

    if (page == PAGE_MAIN) {
      if (ch == KEY_UP || ch == 'k') {
        menu_sel = (menu_sel + 4) % 5;
      } else if (ch == KEY_DOWN || ch == 'j') {
        menu_sel = (menu_sel + 1) % 5;
      } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (menu_sel == 0)
          page = PAGE_GRAPH;
        else if (menu_sel == 1)
          page = PAGE_SYSINFO;
        else if (menu_sel == 2)
          page = PAGE_HELP;
        else if (menu_sel == 3)
          page = PAGE_ABOUT;
        else if (menu_sel == 4) {
          endwin();
          return 0;
        }
      }
    } else if (page == PAGE_GRAPH) {
      if (ch == 'q' || ch == 27)
        page = PAGE_MAIN;
    } else if (page == PAGE_SYSINFO) {
      if (ch == 'q' || ch == 27)
        page = PAGE_MAIN;
    } else if (page == PAGE_HELP) {
      if (ch == 'q' || ch == 27)
        page = PAGE_MAIN;
    } else if (page == PAGE_ABOUT) {
      if (ch == 'q' || ch == 27)
        page = PAGE_MAIN;
    } else if (page == PAGE_PROCS) {
      if (ch == KEY_UP || ch == 'k') {
        proc_sel--;
        if (proc_sel < 0)
          proc_sel = 0;
      } else if (ch == KEY_DOWN || ch == 'j') {
        proc_sel++;
        if (proc_sel >= nprocs)
          proc_sel = (nprocs ? nprocs - 1 : 0);
      } else if (ch == KEY_PPAGE) {
        proc_sel -= 10;
        if (proc_sel < 0)
          proc_sel = 0;
      } else if (ch == KEY_NPAGE) {
        proc_sel += 10;
        if (proc_sel >= nprocs)
          proc_sel = (nprocs ? nprocs - 1 : 0);
      } else if (ch == 'c') {
        sort_mode = 0;
      } else if (ch == 'm') {
        sort_mode = 1;
      } else if (ch == 'K' && nprocs > 0) {
        act_kill(procs[proc_sel].pid);
      } else if (ch == 'S' && nprocs > 0) {
        act_stopcont(&procs[proc_sel]);
      } else if (ch == '+') {
        if (nprocs > 0)
          act_renice(procs[proc_sel].pid, -1);
      } else if (ch == '-') {
        if (nprocs > 0)
          act_renice(procs[proc_sel].pid, +1);
      }
    }
  }
}
