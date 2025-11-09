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
#define MAX_PROCS 2048
#define MAX_COMM 64
#define HIST_W 120
#define CPU_MS 250
#define PROC_MS 1500
#define FRAME_MS 166
#define CONTENT_WIDTH 100

// ---------- Color ----------
enum {
  C_DEFAULT = 1, C_GREEN, C_YELLOW, C_RED, C_CYAN, C_MAGENTA, C_BLUE,
  C_WHITE, C_HEADER, C_BG_GREEN, C_ORANGE, C_BG_SELECTED, C_DIM_WHITE
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
  init_pair(C_HEADER, COLOR_BLACK, COLOR_CYAN);
  init_pair(C_BG_GREEN, COLOR_BLACK, COLOR_GREEN);
  init_pair(C_ORANGE, COLOR_RED, -1);
  init_pair(C_BG_SELECTED, COLOR_WHITE, COLOR_BLUE);
  init_pair(C_DIM_WHITE, 8, -1);
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
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (!strncmp(line, "model name", 10)) {
      char *p = strchr(line, ':');
      if (p) { p++; while (*p == ' ') p++;
        strncpy(cpu_model, p, sizeof(cpu_model) - 1);
        cpu_model[strcspn(cpu_model, "\n")] = 0;
      }
    }
    if (!strncmp(line, "cpu MHz", 7) && cpu_freq_ghz == 0.0) {
      char *p = strchr(line, ':');
      if (p) { double mhz = 0; sscanf(p + 1, "%lf", &mhz);
        cpu_freq_ghz = mhz / 1000.0;
      }
    }
  }
  fclose(f);
}

static void read_os_release(void) {
  FILE *f = fopen("/etc/os-release", "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (!strncmp(line, "PRETTY_NAME=", 12)) {
      char *v = line + 12;
      size_t n = strlen(v);
      if (n && v[0] == '"') { v++; n -= 2; }
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
  if (f) { fscanf(f, "%lf", &up); fclose(f); }
  int secs = (int)up;
  *h = secs / 3600; *m = (secs % 3600) / 60; *s = secs % 60;
}

// ---------- Temp/Freq ----------
static char tz_path[128] = "";
static double temp_smooth = 0.0;

static void detect_temp_sensor(void) {
  for (int i = 0; i < 128; i++) {
    char tpath[128];
    snprintf(tpath, sizeof(tpath), "/sys/class/thermal/thermal_zone%d/type", i);
    FILE *f = fopen(tpath, "r");
    if (!f) continue;
    char type[128] = ""; fgets(type, sizeof(type), f); fclose(f);
    for (char *p = type; *p; ++p) *p = tolower(*p);
    if (strstr(type, "cpu") || strstr(type, "x86") || strstr(type, "pkg") ||
        strstr(type, "soc") || strstr(type, "core")) {
      snprintf(tz_path, sizeof(tz_path), "/sys/class/thermal/thermal_zone%d/temp", i);
      return;
    }
  }
}

static double temp_c(void) {
  if (!tz_path[0]) return temp_smooth;
  FILE *f = fopen(tz_path, "r");
  if (!f) return temp_smooth;
  double t = 0; fscanf(f, "%lf", &t); fclose(f);
  t /= 1000.0;
  temp_smooth = (temp_smooth == 0.0) ? t : (0.7 * temp_smooth + 0.3 * t);
  return temp_smooth;
}

static void read_cpu_freq_mhz(double *freqs) {
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    FILE *f = fopen(path, "r");
    if (f) { unsigned long khz = 0;
      if (fscanf(f, "%lu", &khz) == 1) freqs[i] = khz / 1000.0;
      else freqs[i] = 0;
      fclose(f);
    } else freqs[i] = 0;
  }
}

// ---------- Memory ----------
static int mem_read_kb(unsigned long *tot, unsigned long *free_kb, unsigned long *avail) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  char k[64]; unsigned long v;
  *tot = *free_kb = *avail = 0;
  while (fscanf(f, "%63s %lu kB", k, &v) == 2) {
    if (!strcmp(k, "MemTotal:")) *tot = v;
    else if (!strcmp(k, "MemFree:")) *free_kb = v;
    else if (!strcmp(k, "MemAvailable:")) *avail = v;
    int c; while ((c = fgetc(f)) != '\n' && c != EOF);
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
static double hist_cpu[MAX_CORES][HIST_W];
static int hpos = 0;

static void push_hist(void) {
  for (int i = 0; i < cpu.ncores && i < MAX_CORES; i++) {
    double usage = cpu.core[i];
    if (usage < 0.0) usage = 0.0; if (usage > 1.0) usage = 1.0;
    hist_cpu[i][hpos] = usage;
  }
  hpos = (hpos + 1) % HIST_W;
}

static void cpu_sample(void) {
  static unsigned long long pu[MAX_CORES + 1], pn[MAX_CORES + 1],
      ps[MAX_CORES + 1], pi[MAX_CORES + 1];
  FILE *f = fopen("/proc/stat", "r");
  if (!f) return;
  int idx = 0; char id[16];
  unsigned long long u, n, s, idle, iow, irq, sirq, steal;
  while (fscanf(f, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", id, &u, &n,
                &s, &idle, &iow, &irq, &sirq, &steal) == 9) {
    if (strncmp(id, "cpu", 3) != 0) break;
    unsigned long long tot = u + n + s + idle + iow + irq + sirq + steal;
    unsigned long long pt = pu[idx] + pn[idx] + ps[idx] + pi[idx];
    unsigned long long dt = tot - pt, di = idle - pi[idx];
    double use = dt ? (1.0 - (double)di / (double)dt) : 0.0;
    if (idx == 0) cpu.total = 0.3 * cpu.total + 0.7 * use;
    else if (idx - 1 < MAX_CORES) cpu.core[idx - 1] = 0.8 * cpu.core[idx - 1] + 0.2 * use;
    pu[idx] = u; pn[idx] = n; ps[idx] = s; pi[idx] = idle;
    idx++;
    if (idx > MAX_CORES) break;
  }
  fclose(f);
  cpu.ncores = idx - 1;
  if (cpu.ncores <= 0) cpu.ncores = 1;
  push_hist();
}

// ---------- Neofetch ----------
static char neofetch_info[4096] = "";

static void run_neofetch_stdout(void) {
  neofetch_info[0] = 0;
  FILE *fp = popen("neofetch --stdout 2>/dev/null", "r");
  if (!fp) return;
  size_t pos = 0; char line[256];
  while (fgets(line, sizeof(line), fp) && pos < sizeof(neofetch_info) - 256) {
    size_t len = strlen(line);
    memcpy(neofetch_info + pos, line, len);
    pos += len;
  }
  neofetch_info[pos] = '\0';
  pclose(fp);
}

static const char *pick_ascii_logo(const char *pretty) {
  static const char *arch = "       /\\        \n"
                            "      /  \\       \n"
                            "     /_/\\_\\      \n"
                            "    /      \\     \n"
                            "   /  /\\    \\    \n"
                            "  /__/  \\____\\   \n";
  static const char *ubuntu = "         _        \n"
                             "     ---(_)---      \n"
                             "   _/  /   \\ \\_   \n"
                             "  /_._/_____\\_._\\  \n"
                             "     \\_\\_/\\_/_/    \n";
  static const char *debian = "    ____         \n"
                             "   /    \\_       \n"
                             "  /  _ _  \\      \n"
                             "  \\_/ \\/ \\_/      \n"
                             "     \\__/         \n";
  static const char *fedora = "    _______      \n"
                             "   /  __  /      \n"
                             "  /  /_/ /__     \n"
                             " /______/__/     \n";
  static const char *manjaro = " _______         \n"
                              "|  ___  |         \n"
                              "| |   | |____     \n"
                              "| |   | |___ |    \n"
                              "|_|   |_|___||    \n";
  static const char *generic = "  __  __         \n"
                              " |  \\/  |         \n"
                              " | \\  / | ___     \n"
                              " | |\\/| |/ _ \\    \n"
                              " | |  | | (_) |   \n"
                              " |_|  |_|\\___/    \n";
  char low[128];
  snprintf(low, sizeof(low), "%s", pretty ? pretty : "");
  for (char *p = low; *p; ++p) *p = tolower(*p);
  if (strstr(low, "arch")) return arch;
  if (strstr(low, "ubuntu")) return ubuntu;
  if (strstr(low, "debian")) return debian;
  if (strstr(low, "fedora")) return fedora;
  if (strstr(low, "manjaro")) return manjaro;
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
  for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
  return 1;
}

static int read_comm(pid_t pid, char *out, size_t n) {
  char p[64]; snprintf(p, sizeof(p), "/proc/%d/comm", pid);
  FILE *f = fopen(p, "r");
  if (!f) return -1;
  if (!fgets(out, n, f)) { fclose(f); return -1; }
  out[strcspn(out, "\n")] = 0;
  fclose(f); return 0;
}

static int read_stat(pid_t pid, unsigned long *ut, unsigned long *st, int *nicev, bool *running) {
  char p[64]; snprintf(p, sizeof(p), "/proc/%d/stat", pid);
  FILE *f = fopen(p, "r");
  if (!f) return -1;
  char line[1024];
  if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
  fclose(f);
  char *rp = strrchr(line, ')');
  if (!rp) return -1;
  char *rest = rp + 2;
  int field = 3; char *save = rest, *tok;
  *running = true;
  while ((tok = strsep(&save, " "))) {
    if (*tok == '\0') continue;
    if (field == 3) { char st = tok[0]; *running = (st != 'T' && st != 'Z'); }
    else if (field == 14) *ut = strtoul(tok, NULL, 10);
    else if (field == 15) *st = strtoul(tok, NULL, 10);
    else if (field == 19) { *nicev = (int)strtol(tok, NULL, 10); break; }
    field++;
  }
  return 0;
}

static int read_status(pid_t pid, uid_t *uid, unsigned long *rss_kb) {
  char p[64]; snprintf(p, sizeof(p), "/proc/%d/status", pid);
  FILE *f = fopen(p, "r");
  if (!f) return -1;
  char line[256]; *rss_kb = 0; *uid = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned int u; unsigned long v; char unit[32];
    if (sscanf(line, "Uid:\t%u", &u) == 1) { *uid = (uid_t)u; continue; }
    if (sscanf(line, "VmRSS:\t%lu %31s", &v, unit) == 2) { *rss_kb = v; }
  }
  fclose(f); return 0;
}

static const char *uname_from_uid(uid_t uid) {
  struct passwd *pw = getpwuid(uid);
  return pw ? pw->pw_name : "unknown";
}

static void read_cpu_totals(void) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f) return;
  char buf[256];
  if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
  fclose(f);
  unsigned long long u, n, s, idle, iow, irq, sirq, steal; char lab[8];
  sscanf(buf, "%7s %llu %llu %llu %llu %llu %llu %llu %llu", lab, &u, &n, &s,
         &idle, &iow, &irq, &sirq, &steal);
  long long tot = (long long)(u + n + s + idle + iow + irq + sirq + steal);
  gtot_prev = gtot_cur; gtot_cur = tot;
}

static void scan_processes(void) {
  static PInfo prev[MAX_PROCS];
  static int prevn = 0;

  memset(procs, 0, sizeof(procs));
  nprocs = 0;

  DIR *d = opendir("/proc");
  if (!d) return;

  struct dirent *e;
  while ((e = readdir(d)) && nprocs < MAX_PROCS) {
    if (!is_pid_dir(e->d_name)) continue;

    pid_t pid = (pid_t)atoi(e->d_name);
    PInfo *p = &procs[nprocs];
    
    p->pid = pid;
    p->cpu_pct = 0.0;
    p->running = true;
    
    if (read_comm(pid, p->comm, sizeof(p->comm)) < 0) continue;
    if (read_stat(pid, &p->ut, &p->st, &p->nicev, &p->running) < 0) continue;
    read_status(pid, &p->uid, &p->rss_kb);

    for (int i = 0; i < prevn; i++) {
      if (prev[i].pid == pid) {
        unsigned long dut = (p->ut > prev[i].ut) ? (p->ut - prev[i].ut) : 0;
        unsigned long dst = (p->st > prev[i].st) ? (p->st - prev[i].st) : 0;
        long long dtot = (gtot_cur > gtot_prev) ? (gtot_cur - gtot_prev) : 1;
        p->cpu_pct = (dtot > 0) ? ((double)(dut + dst) / (double)dtot) : 0.0;
        break;
      }
    }
    
    nprocs++;
  }
  closedir(d);

  memcpy(prev, procs, nprocs * sizeof(PInfo));
  prevn = nprocs;
}

static int cmp_cpu(const void *a, const void *b) {
  const PInfo *x = a, *y = b;
  if (y->cpu_pct > x->cpu_pct) return 1;
  if (y->cpu_pct < x->cpu_pct) return -1;
  return (int)(x->pid - y->pid);
}

static int cmp_mem(const void *a, const void *b) {
  const PInfo *x = a, *y = b;
  if (y->rss_kb > x->rss_kb) return 1;
  if (y->rss_kb < x->rss_kb) return -1;
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

static int get_color(double ratio) {
  if (ratio > 0.75) return C_RED;
  if (ratio > 0.40) return C_YELLOW;
  return C_GREEN;
}

static void draw_vert_bar(int y, int x, int h, double ratio, int col) {
    if (h <= 0) return;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    
    double exact_fill = ratio * h;
    int full_blocks = (int)exact_fill;
    double fractional = exact_fill - full_blocks;
    
    attron(COLOR_PAIR(C_DIM_WHITE));
    for (int i = 0; i < h; i++) {
        mvaddch(y + h - 1 - i, x, ACS_CKBOARD);
    }
    attroff(COLOR_PAIR(C_DIM_WHITE));
    
    attron(COLOR_PAIR(col) | A_BOLD);
    for (int i = 0; i < full_blocks; i++) {
        mvaddch(y + h - 1 - i, x, ACS_CKBOARD);
    }
    attroff(COLOR_PAIR(col) | A_BOLD);
    
    if (full_blocks < h && fractional > 0.25) {
        attron(COLOR_PAIR(col));
        mvaddch(y + h - 1 - full_blocks, x, ACS_CKBOARD);
        attroff(COLOR_PAIR(col));
    }
}

static int get_start_x(int width) {
    if (COLS > width) return (COLS - width) / 2;
    return 1;
}

static int get_content_width(void) {
    if (COLS > CONTENT_WIDTH) return CONTENT_WIDTH;
    return COLS - 2;
}

// ---------- Pages ----------
enum {
  PAGE_MAIN = 0, PAGE_GRAPH, PAGE_SYSINFO, PAGE_HELP, PAGE_ABOUT, PAGE_PROCS
};
static int page = PAGE_MAIN, menu_sel = 0, proc_sel = 0, sort_mode = 0;

static void draw_main(void) {
  erase();
  int W = COLS, H = LINES;
  
  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "uxhtop");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "↑/↓ j/k: Select  Enter: Open  q: Quit/Back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  int cw = get_content_width();
  int sx = get_start_x(cw);
  int left_col_w = cw / 3;
  int right_col_x = sx + left_col_w + 2;
  int right_col_w = cw - left_col_w - 2;
  int y = 2;
  mvprintw(y++, sx, "CPU: %s", cpu_model);
  mvprintw(y++, sx, "Base: %.2f GHz    Cores: %d", cpu_freq_ghz, NCPU);
  mvprintw(y++, sx, "OS: %s", distro);
  mvprintw(y++, sx, "Kernel: %s", kernel_rel);
  mvprintw(y++, sx, "Host: %s", host);

  y++;
  const char *items[] = {"< Graph >", "< System Info >", "< Process Manager >",
                         "< Help >", "< About >", "< Quit >"};
  mvprintw(y++, sx, "Menu");
  for (int i = 0; i < 6; i++) {
    if (menu_sel == i) {
      attron(COLOR_PAIR(C_BG_GREEN) | A_BOLD);
      mvprintw(y + i, sx, "%s", items[i]);
      attroff(COLOR_PAIR(C_BG_GREEN) | A_BOLD);
    } else {
      mvprintw(y + i, sx, "%s", items[i]);
    }
  }

  const char *logo = pick_ascii_logo(distro);
  attron(COLOR_PAIR(C_MAGENTA) | A_BOLD);
  int ly = 2;
  for (const char *p = logo; *p;) {
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    mvprintw(ly++, right_col_x, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6) break;
  }
  attroff(COLOR_PAIR(C_MAGENTA) | A_BOLD);

  ly += 1;
  attron(COLOR_PAIR(C_CYAN));
  char *line = (char *)neofetch_info;
  while (*line && ly < H - 2) {
    char *end = strchr(line, '\n');
    if (!end) break;
    int len = end - line;
    if (len > right_col_w - 4) len = right_col_w - 4;
    mvprintw(ly++, right_col_x + 2, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));
  refresh();
}

static void draw_graphs(void) {
  int W = COLS, H = LINES;
  erase();

  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "System Monitor (Graphs)");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q: back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  int start_y = 2;
  int bar_h = 7;
  int box_h = bar_h + 3;
  int max_w = get_content_width();
  int start_x = get_start_x(max_w);
  
  if (H < 30) {
    bar_h = (H - 8) / 3;
    if (bar_h < 1) bar_h = 1;
    box_h = bar_h + 3;
  }

  int y = start_y;
  draw_box(y, start_x, box_h, max_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, start_x + 2, " Temp [C] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  double tc = temp_c();
  double t_ratio = tc / 100.0;
  if (t_ratio < 0) t_ratio = 0; if (t_ratio > 1) t_ratio = 1;

  draw_vert_bar(y + 2, start_x + 4, bar_h, t_ratio, get_color(t_ratio));
  mvprintw(y + bar_h + 2, start_x + 3, "%.1f", tc);
  char tlabel[32];
  snprintf(tlabel, sizeof(tlabel), "%s", tz_path[0] ? (strrchr(tz_path, '/') ? strrchr(tz_path, '/') + 1 : "Sensor") : "N/A");
  mvprintw(y + 1, start_x + 3, "%-6.6s", tlabel);

  y += box_h + 1;
  if (y > H - 10) goto end_draw;

  draw_box(y, start_x, box_h, max_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, start_x + 2, " Frequency [MHz] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  double freqs[MAX_CORES] = {0};
  read_cpu_freq_mhz(freqs);
  double fmax = cpu_freq_ghz * 1000.0;
  if (fmax < 1000) fmax = 4000.0;

  int bar_width = 3, bar_spacing = 4;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    int x = start_x + 4 + (i * (bar_width + bar_spacing));
    if (x + bar_width + 2 > max_w + start_x) break;
    double f_ratio = freqs[i] / fmax;
    draw_vert_bar(y + 2, x, bar_h, f_ratio, get_color(f_ratio));
    mvprintw(y + bar_h + 2, x, "%4.0f", freqs[i]);
    char clbl[8]; snprintf(clbl, sizeof(clbl), "Core %d", i);
    mvprintw(y + 1, x, "%-6.6s", clbl);
  }

  y += box_h + 1;
  if (y > H - 10) goto end_draw;

  draw_box(y, start_x, box_h, max_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, start_x + 2, " Util [%%] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    int x = start_x + 4 + (i * (bar_width + bar_spacing));
    if (x + bar_width + 2 > max_w + start_x) break;
    double u_ratio = hist_cpu[i][(hpos - 1 + HIST_W) % HIST_W];
    draw_vert_bar(y + 2, x, bar_h, u_ratio, get_color(u_ratio));
    mvprintw(y + bar_h + 2, x, "%5.1f", u_ratio * 100.0);
    char clbl[8]; snprintf(clbl, sizeof(clbl), "Core %d", i);
    mvprintw(y + 1, x, "%-6.6s", clbl);
  }

end_draw:
  refresh();
}

static void draw_sysinfo(void) {
  int W = COLS, H = LINES;
  erase();

  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "System Info");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "Press ESC or q to return");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);
  
  int cw = get_content_width();
  int sx = get_start_x(cw);
  int y = 2;

  attron(COLOR_PAIR(C_WHITE));
  mvprintw(y++, sx, "CPU Model: %s", cpu_model);
  mvprintw(y++, sx, "Cores: %d    Base: %.2f GHz", NCPU, cpu_freq_ghz);
  int uh, um, us; read_uptime(&uh, &um, &us);
  mvprintw(y++, sx, "Uptime: %02d:%02d:%02d", uh, um, us);

  double tc = temp_c();
  mvprintw(y, sx, "Temp: %.1f C    Sensor: %s", tc, tz_path[0] ? tz_path : "N/A");
  double t_ratio = tc / 100.0;
  if (t_ratio < 0) t_ratio = 0; if (t_ratio > 1) t_ratio = 1;
  
  int barw = (cw > 60) ? 40 : (cw - 10);
  int filled = (int)(t_ratio * barw);
  int col = get_color(t_ratio);
  
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(y + 1, sx + 4, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(y + 1, sx + 4 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));
  
  mvprintw(y + 1, sx + 4 + barw + 2, "%3.0f%%", t_ratio * 100.0);
  y += 3;

  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  double mem_used = (double)(mt - ma);
  double mem_avail = (double)ma;
  double used_pct = mt ? mem_used / (double)mt : 0.0;

  mvprintw(
      y++, sx, "Memory: Used %lu MB | Avail %lu MB | Free %lu MB | Total %lu MB",
      (unsigned long)(mem_used / 1024.0), (unsigned long)(mem_avail / 1024.0),
      (unsigned long)(mf / 1024.0), (unsigned long)(mt / 1024.0));

  filled = (int)(used_pct * barw);
  col = get_color(used_pct);
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(y, sx + 4, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(y, sx + 4 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));
  
  mvprintw(y, sx + 4 + barw + 2, "%3.0f%%", used_pct * 100.0);
  y += 3;

  double freqs[MAX_CORES] = {0};
  read_cpu_freq_mhz(freqs); 
  double fmin = 1e9, fmax = 0;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if (freqs[i] > fmax) fmax = freqs[i];
    if (freqs[i] < fmin && freqs[i] > 100) fmin = freqs[i];
  }
  if (fmax - fmin < 500) fmin = 0;

  mvprintw(y++, sx, "Per-core frequency (MHz):");
  int freq_bar_w = (barw / 2) - 8;
  if (freq_bar_w < 10) freq_bar_w = 10;
  int perrow = cw / (freq_bar_w + 12);
  if (perrow == 0) perrow = 1;

  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if ((i % perrow) == 0) y++;
    if (y >= H - 2) break;
    double ratio = (fmax - fmin > 1) ? (freqs[i] - fmin) / (fmax - fmin) : 1.0;
    if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
    int fcol = get_color(ratio);
    int startx_col = sx + 4 + (i % perrow) * (freq_bar_w + 14);
    if (startx_col + freq_bar_w + 12 > sx + cw) break;
    
    mvprintw(y, startx_col, "C%-2d:", i);
    attron(COLOR_PAIR(fcol));
    int w = (int)(ratio * freq_bar_w);
    mvhline(y, startx_col + 6, ACS_CKBOARD, w);
    attroff(COLOR_PAIR(fcol));
    attron(COLOR_PAIR(C_DIM_WHITE));
    mvhline(y, startx_col + 6 + w, ACS_CKBOARD, freq_bar_w - w);
    attroff(COLOR_PAIR(C_DIM_WHITE));
    mvprintw(y, startx_col + 6 + freq_bar_w + 1, "%4.0f", freqs[i]);
  }
  attroff(COLOR_PAIR(C_WHITE));
  refresh();
}

static void draw_help(void) {
  erase();
  int W = COLS, H = LINES;
  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "Help");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q = back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);
  
  int sx = get_start_x(80);
  int y = 2;
  mvprintw(y++, sx, "Navigation:");
  mvprintw(y++, sx + 4, "↑/↓ or j/k  - Move selection");
  mvprintw(y++, sx + 4, "Enter        - Select");
  mvprintw(y++, sx + 4, "ESC or q     - Back/Quit");
  y++;
  mvprintw(y++, sx, "Process Manager:");
  mvprintw(y++, sx + 4, "c            - Sort by CPU    m  - Sort by Memory");
  mvprintw(y++, sx + 4, "K            - Kill process   S  - Stop/Continue");
  mvprintw(y++, sx + 4, "+ / -        - Increase/Decrease priority (nice)");
  refresh();
}

static void draw_about(void) {
  run_neofetch_stdout();
  erase();
  int W = COLS, H = LINES;
  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "About");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "ESC/q = back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  int cw = get_content_width();
  int sx = get_start_x(cw);
  int logo_w = 24;
  int info_x = sx + logo_w;
  int info_w = cw - logo_w;

  const char *logo = pick_ascii_logo(distro);
  attron(COLOR_PAIR(C_MAGENTA) | A_BOLD);
  int ly = 2;
  for (const char *p = logo; *p;) {
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    mvprintw(ly++, sx, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6) break;
  }
  attroff(COLOR_PAIR(C_MAGENTA) | A_BOLD);

  attron(COLOR_PAIR(C_WHITE) | A_BOLD);
  mvprintw(2, info_x, "uxhtop - Advanced Unix Task Manager");
  attroff(COLOR_PAIR(C_WHITE) | A_BOLD);
  mvprintw(3, info_x, "System: %s", distro);
  mvprintw(4, info_x, "Kernel: %s", kernel_rel);
  mvprintw(5, info_x, "Host:   %s", host);

  int ny = 7;
  attron(COLOR_PAIR(C_CYAN));
  char *line = (char *)neofetch_info;
  while (*line && ny < H - 2) {
    char *end = strchr(line, '\n');
    if (!end) break;
    int len = end - line;
    if (len > info_w - 2) len = info_w - 2;
    mvprintw(ny++, info_x, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));
  refresh();
}

static void draw_procs(void) {
  int W = COLS, H = LINES;
  fprintf(stderr, "DEBUG: draw_procs, W=%d H=%d\n", W, H);
  fflush(stderr);
  
  if (W < 40 || H < 10) {
    fprintf(stderr, "ERROR: Terminal too small\n");
    return;
  }
  
  erase();
  fprintf(stderr, "DEBUG: After erase()\n");
  fflush(stderr);
  
  int cw = get_content_width();
  int sx = get_start_x(cw);
  
  fprintf(stderr, "DEBUG: cw=%d sx=%d\n", cw, sx);
  fflush(stderr);
  
  if (cw <= 0) cw = W - 4;
  if (sx < 0) sx = 1;
  if (cw < 60) cw = 60;
  
  fprintf(stderr, "DEBUG: After safety, cw=%d sx=%d nprocs=%d\n", cw, sx, nprocs);
  fflush(stderr);
  
  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  fprintf(stderr, "DEBUG: After attron\n");
  fflush(stderr);
  
  mvhline(0, 0, ' ', W);
  fprintf(stderr, "DEBUG: After first mvhline\n");
  fflush(stderr);
  
  mvprintw(0, 2, "Process Manager - %d processes", nprocs);
  fprintf(stderr, "DEBUG: After first mvprintw\n");
  fflush(stderr);
  
  mvhline(H - 1, 0, ' ', W);
  fprintf(stderr, "DEBUG: After second mvhline\n");
  fflush(stderr);
  
  mvprintw(H - 1, 2, "↑↓ j/k:Move  c:CPU  m:Mem  K:Kill  S:Stop/Cont  +/-:Nice  q:Back");
  fprintf(stderr, "DEBUG: After footer\n");
  fflush(stderr);
  
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);
  fprintf(stderr, "DEBUG: After attroff\n");
  fflush(stderr);

  mvprintw(2, sx, "CPU:");
  fprintf(stderr, "DEBUG: After CPU label\n");
  fflush(stderr);
  
  int barw = (cw / 3) * 2;
  if (barw < 20) barw = 20;
  if (barw > cw - 8) barw = cw - 8;
  
  fprintf(stderr, "DEBUG: barw=%d\n", barw);
  fflush(stderr);
  
  double pct = cpu.total;
  int filled = (int)(pct * barw);
  int col = get_color(pct);
  
  fprintf(stderr, "DEBUG: CPU pct=%.2f filled=%d col=%d\n", pct, filled, col);
  fflush(stderr);
  
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(2, sx + 6, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(2, sx + 6 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));
  
  fprintf(stderr, "DEBUG: After CPU bars\n");
  fflush(stderr);
  
  mvprintw(2, sx + 6 + barw + 2, "%3d%%", (int)(pct * 100));
  fprintf(stderr, "DEBUG: After CPU percent\n");
  fflush(stderr);

  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  fprintf(stderr, "DEBUG: Memory: mt=%lu mf=%lu ma=%lu\n", mt, mf, ma);
  fflush(stderr);
  
  double mem_used = mt - ma; 
  double mem_avail = ma;    
  double used_pct = mt ? (double)mem_used / mt : 0.0;
  double avail_pct = mt ? (double)mem_avail / mt : 0.0;
  
  fprintf(stderr, "DEBUG: used_pct=%.2f avail_pct=%.2f\n", used_pct, avail_pct);
  fflush(stderr);
  
  mvprintw(3, sx, "MEM:");
  fprintf(stderr, "DEBUG: After MEM label\n");
  fflush(stderr);
  
  filled = (int)(used_pct * barw);
  col = get_color(used_pct);
  
  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(3, sx + 6, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(3, sx + 6 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));
  
  fprintf(stderr, "DEBUG: After MEM bars\n");
  fflush(stderr);

  char mem_str[100];
  snprintf(mem_str, sizeof(mem_str), 
           "Used:%3d%%  Avail:%3d%%  (Used:%luMB  Avail:%luMB)",
           (int)(used_pct * 100), (int)(avail_pct * 100),
           (unsigned long)mem_used / 1024, (unsigned long)mem_avail / 1024);
  fprintf(stderr, "DEBUG: mem_str='%s'\n", mem_str);
  fflush(stderr);
  
  mvprintw(3, sx + 6 + barw + 2, "%s", mem_str);
  fprintf(stderr, "DEBUG: After MEM string\n");
  fflush(stderr);

  attron(A_BOLD | COLOR_PAIR(C_HEADER));
  fprintf(stderr, "DEBUG: Before header format, cw=%d\n", cw);
  fflush(stderr);
  
  char header_fmt[128];
  snprintf(header_fmt, sizeof(header_fmt), "%%-%ds", cw);
  fprintf(stderr, "DEBUG: header_fmt='%s'\n", header_fmt);
  fflush(stderr);
  
  mvprintw(5, sx, header_fmt, " PID    COMMAND              USER         CPU%%     MEM(MB)   NI STATE");
  fprintf(stderr, "DEBUG: After header mvprintw\n");
  fflush(stderr);
  
  attroff(A_BOLD | COLOR_PAIR(C_HEADER));

  fprintf(stderr, "DEBUG: Before qsort, nprocs=%d sort_mode=%d\n", nprocs, sort_mode);
  fflush(stderr);
  
  if (nprocs > 0) {
    if (sort_mode == 0)
      qsort(procs, nprocs, sizeof(PInfo), cmp_cpu);
    else
      qsort(procs, nprocs, sizeof(PInfo), cmp_mem);
  }
  
  fprintf(stderr, "DEBUG: After qsort\n");
  fflush(stderr);

  if (proc_sel < 0) proc_sel = 0;
  if (proc_sel >= nprocs && nprocs > 0) proc_sel = nprocs - 1;

  int rows = H - 8;
  if (rows < 1) rows = 1;

  int start = proc_sel - rows / 2;
  if (start < 0) start = 0;
  int end = start + rows;
  if (end > nprocs) end = nprocs;
  if (start > end - rows && end >= rows) start = end - rows;
  if (start < 0) start = 0;

  fprintf(stderr, "DEBUG: Loop range: start=%d end=%d rows=%d proc_sel=%d\n", start, end, rows, proc_sel);
  fflush(stderr);

  int y = 6;
  int name_w = cw - 56; 
  if (name_w < 10) name_w = 10;
  
  fprintf(stderr, "DEBUG: name_w=%d, starting process loop\n", name_w);
  fflush(stderr);
  
  for (int i = start; i < end && i < nprocs; i++) {
    fprintf(stderr, "DEBUG: Loop iteration i=%d y=%d\n", i, y);
    fflush(stderr);
    
    if (y >= H - 1) break;
    
    PInfo *p = &procs[i];
    fprintf(stderr, "DEBUG: Got process pointer, pid=%d\n", p->pid);
    fflush(stderr);
    
    char line_fmt[256];
    snprintf(line_fmt, sizeof(line_fmt),
             " %%-6d %%-%d.%ds %%-12.12s %%7.1f %%10.1f %%5d %%-4s",
             name_w, name_w);
    
    fprintf(stderr, "DEBUG: line_fmt='%s'\n", line_fmt);
    fflush(stderr);

    char line_buf[256];
    double ui_pct = p->cpu_pct * 100.0;
    if (ui_pct < 0) ui_pct = 0; 
    if (ui_pct > 9999) ui_pct = 9999;
    
    fprintf(stderr, "DEBUG: About to call uname_from_uid for uid=%u\n", p->uid);
    fflush(stderr);
    
    const char *uname = uname_from_uid(p->uid);
    fprintf(stderr, "DEBUG: Got username='%s'\n", uname ? uname : "NULL");
    fflush(stderr);
    
    snprintf(line_buf, sizeof(line_buf), line_fmt, 
             p->pid, p->comm, uname, 
             ui_pct, p->rss_kb / 1024.0, p->nicev, 
             p->running ? "RUN" : "STOP");
    
    fprintf(stderr, "DEBUG: line_buf created\n");
    fflush(stderr);
             
    line_buf[sizeof(line_buf) - 1] = '\0';

    if (i == proc_sel) {
  attron(COLOR_PAIR(C_BG_SELECTED) | A_BOLD);
  mvprintw(y, sx, "%-*s", cw, line_buf);
  attroff(COLOR_PAIR(C_BG_SELECTED) | A_BOLD);
} else {
  int proc_color = C_GREEN;
  if (ui_pct > 50) proc_color = C_YELLOW;
  if (ui_pct > 75) proc_color = C_RED;
  
  attron(COLOR_PAIR(proc_color));
  mvprintw(y, sx, "%s", line_buf);
  attroff(COLOR_PAIR(proc_color));
}
    
    fprintf(stderr, "DEBUG: Process line drawn\n");
    fflush(stderr);
    
    y++;
  }
  
  fprintf(stderr, "DEBUG: Before refresh()\n");
  fflush(stderr);
  
  refresh();
  
  fprintf(stderr, "DEBUG: draw_procs completed successfully\n");
  fflush(stderr);
}

// ---------- Actions ----------
static void act_kill(pid_t pid) {
  if (kill(pid, SIGTERM) == 0) return;
  usleep(200000);
  kill(pid, SIGKILL);
}

static void act_stopcont(PInfo *p) {
  if (p->running) { kill(p->pid, SIGSTOP); p->running = false; }
  else { kill(p->pid, SIGCONT); p->running = true; }
}

static void act_renice(pid_t pid, int delta) {
  errno = 0;
  int old = getpriority(PRIO_PROCESS, pid);
  if (errno) return;
  int nv = old + delta;
  if (nv < -20) nv = -20; if (nv > 19) nv = 19;
  setpriority(PRIO_PROCESS, pid, nv);
}

// ---------- Main ----------
int main(void) {
  NCPU = sysconf(_SC_NPROCESSORS_ONLN);
  if (NCPU <= 0) NCPU = 1;
  read_os_release(); read_uname(); read_cpu_info();
  detect_temp_sensor(); run_neofetch_stdout();

  initscr(); cbreak(); noecho();
  keypad(stdscr, TRUE); nodelay(stdscr, TRUE);
  curs_set(0);
  if (has_colors()) init_colors();

  long t_cpu = now_ms(), t_proc = now_ms();

  cpu_sample();
  cpu.total = 0.0;
  for(int i = 0; i < MAX_CORES; i++) cpu.core[i] = 0.0;
  cpu_sample();
  read_cpu_totals();
  scan_processes();

  while (1) {
    long t = now_ms();
    
    if (t - t_cpu >= CPU_MS) {
      cpu_sample();
      t_cpu = t;
    }
    if (t - t_proc >= PROC_MS) {
      if(page == PAGE_PROCS || page == PAGE_MAIN) {
          read_cpu_totals();
          scan_processes();
      }
      t_proc = t;
    }

    switch (page) {
    case PAGE_MAIN:    draw_main();    break;
    case PAGE_GRAPH:   draw_graphs();  break;
    case PAGE_SYSINFO: draw_sysinfo(); break;
    case PAGE_HELP:    draw_help();    break;
    case PAGE_ABOUT:   draw_about();   break;
    case PAGE_PROCS:   draw_procs();   break;
    }

    napms(FRAME_MS);
    int ch = getch();
    if (ch == ERR) continue;

    if (ch == 'q' || ch == 'Q' || ch == 27) {
      if (page != PAGE_MAIN) { 
          page = PAGE_MAIN;
          continue; 
      }
      break;
    }

    if (page == PAGE_MAIN) {
      if (ch == KEY_UP || ch == 'k') menu_sel = (menu_sel + 5) % 6;
      else if (ch == KEY_DOWN || ch == 'j') menu_sel = (menu_sel + 1) % 6;
      else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (menu_sel == 0)      page = PAGE_GRAPH;
        else if (menu_sel == 1) page = PAGE_SYSINFO;
        else if (menu_sel == 2) { 
          page = PAGE_PROCS;
          read_cpu_totals();
          scan_processes();
        }
        else if (menu_sel == 3) page = PAGE_HELP;
        else if (menu_sel == 4) page = PAGE_ABOUT;
        else if (menu_sel == 5) break;
      }
    } else if (page == PAGE_PROCS) {
      if (ch == KEY_UP || ch == 'k') {
        proc_sel--;
        if (proc_sel < 0) proc_sel = 0;
      } else if (ch == KEY_DOWN || ch == 'j') {
        proc_sel++;
        if (nprocs > 0 && proc_sel >= nprocs) proc_sel = nprocs - 1;
      } else if (ch == KEY_PPAGE) {
        proc_sel -= 10;
        if (proc_sel < 0) proc_sel = 0;
      } else if (ch == KEY_NPAGE) {
        proc_sel += 10;
        if (nprocs > 0 && proc_sel >= nprocs) proc_sel = nprocs - 1;
      } else if (ch == 'c') {
        sort_mode = 0;
      } else if (ch == 'm') {
        sort_mode = 1;
      } else if (ch == 'K') {
        if (nprocs > 0 && proc_sel >= 0 && proc_sel < nprocs) {
          act_kill(procs[proc_sel].pid);
        }
      } else if (ch == 'S') {
        if (nprocs > 0 && proc_sel >= 0 && proc_sel < nprocs) {
          act_stopcont(&procs[proc_sel]);
        }
      } else if (ch == '+') {
        if (nprocs > 0 && proc_sel >= 0 && proc_sel < nprocs) {
          act_renice(procs[proc_sel].pid, -1);
        }
      } else if (ch == '-') {
        if (nprocs > 0 && proc_sel >= 0 && proc_sel < nprocs) {
          act_renice(procs[proc_sel].pid, +1);
        }
      }
    }
  }

  endwin();
  return 0;
}
