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
#define MAX_PRIORITY_PROCS 10

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
  C_HEADER,
  C_BG_GREEN,
  C_ORANGE,
  C_BG_SELECTED,
  C_DIM_WHITE
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
static bool temp_available = false;

static void detect_temp_sensor(void) {
  const char *temp_paths[] = {"/sys/class/thermal/thermal_zone0/temp",
                              "/sys/class/hwmon/hwmon0/temp1_input",
                              "/sys/class/hwmon/hwmon1/temp1_input",
                              "/sys/class/hwmon/hwmon2/temp1_input", NULL};

  for (int i = 0; temp_paths[i] != NULL; i++) {
    FILE *f = fopen(temp_paths[i], "r");
    if (f) {
      fclose(f);
      strncpy(tz_path, temp_paths[i], sizeof(tz_path) - 1);
      temp_available = true;
      return;
    }
  }

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
      temp_available = true;
      return;
    }
  }

  temp_available = false;
}

static double temp_c(void) {
  if (!temp_available || !tz_path[0])
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
  bool freq_found = false;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    FILE *f = fopen(path, "r");
    if (f) {
      unsigned long khz = 0;
      if (fscanf(f, "%lu", &khz) == 1) {
        freqs[i] = khz / 1000.0;
        freq_found = true;
      }
      fclose(f);
    }
  }

  if (!freq_found && cpu_freq_ghz > 0) {
    for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
      freqs[i] = cpu_freq_ghz * 1000.0;
    }
  }

  if (!freq_found && cpu_freq_ghz == 0) {
    for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
      freqs[i] = 0;
    }
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

// ---------- Memory History ----------
static double hist_mem[HIST_W];
static int mem_hpos = 0;

static void push_mem_hist(void) {
  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  double used_pct = mt ? (double)(mt - ma) / mt : 0.0;
  hist_mem[mem_hpos] = used_pct;
  mem_hpos = (mem_hpos + 1) % HIST_W;
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
      ps[MAX_CORES + 1], pi[MAX_CORES + 1], piow[MAX_CORES + 1],
      pirq[MAX_CORES + 1], psirq[MAX_CORES + 1], psteal[MAX_CORES + 1];
  static int initialized = 0;

  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return;
  int idx = 0;
  char id[16];
  unsigned long long u = 0, n = 0, s = 0, idle = 0, iow = 0, irq = 0, sirq = 0,
                     steal = 0;

  while (idx <= MAX_CORES) {
    int items = fscanf(f, "%15s %llu %llu %llu %llu %llu %llu %llu %llu", id,
                       &u, &n, &s, &idle, &iow, &irq, &sirq, &steal);

    if (items < 9 || strncmp(id, "cpu", 3) != 0)
      break;

    unsigned long long tot = u + n + s + idle + iow + irq + sirq + steal;
    unsigned long long pt = pu[idx] + pn[idx] + ps[idx] + pi[idx] + piow[idx] +
                            pirq[idx] + psirq[idx] + psteal[idx];
    unsigned long long dt = tot - pt;
    unsigned long long di = idle - pi[idx];
    double use = (dt > 0) ? (1.0 - (double)di / (double)dt) : 0.0;

    if (initialized) {
      if (idx == 0) {
        cpu.total = use;
      } else if (idx - 1 < MAX_CORES) {
        cpu.core[idx - 1] = use;
      }
    } else {
      if (idx == 0) {
        cpu.total = 0.0;
      } else if (idx - 1 < MAX_CORES) {
        cpu.core[idx - 1] = 0.0;
      }
    }

    pu[idx] = u;
    pn[idx] = n;
    ps[idx] = s;
    pi[idx] = idle;
    piow[idx] = iow;
    pirq[idx] = irq;
    psirq[idx] = sirq;
    psteal[idx] = steal;
    idx++;
  }

  fclose(f);
  initialized = 1;
  cpu.ncores = idx - 1;
  if (cpu.ncores <= 0)
    cpu.ncores = 1;
  if (cpu.ncores > NCPU)
    cpu.ncores = NCPU;
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
  bool suspended_by_manager;
} PInfo;

static PInfo procs[MAX_PROCS];
static int nprocs = 0;
static long long gtot_prev = 0, gtot_cur = 0;

static char priority_procs[MAX_PRIORITY_PROCS][MAX_COMM];
static int num_priority_procs = 0;
static bool auto_manage_enabled = false;

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
    } else if (field == 14)
      *ut = strtoul(tok, NULL, 10);
    else if (field == 15)
      *st = strtoul(tok, NULL, 10);
    else if (field == 19) {
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
  static PInfo prev[MAX_PROCS];
  static int prevn = 0;
  static long long prev_time_ms = 0;

  long long curr_time_ms = now_ms();
  long long time_diff_ms =
      (prev_time_ms > 0) ? (curr_time_ms - prev_time_ms) : 1500;

  memset(procs, 0, sizeof(procs));
  nprocs = 0;

  DIR *d = opendir("/proc");
  if (!d)
    return;

  struct dirent *e;
  while ((e = readdir(d)) && nprocs < MAX_PROCS) {
    if (!is_pid_dir(e->d_name))
      continue;

    pid_t pid = (pid_t)atoi(e->d_name);
    PInfo *p = &procs[nprocs];

    p->pid = pid;
    p->cpu_pct = 0.0;
    p->running = true;
    p->suspended_by_manager = false;

    if (read_comm(pid, p->comm, sizeof(p->comm)) < 0)
      continue;
    if (read_stat(pid, &p->ut, &p->st, &p->nicev, &p->running) < 0)
      continue;
    read_status(pid, &p->uid, &p->rss_kb);

    for (int i = 0; i < prevn; i++) {
      if (prev[i].pid == pid) {
        unsigned long dut = (p->ut > prev[i].ut) ? (p->ut - prev[i].ut) : 0;
        unsigned long dst = (p->st > prev[i].st) ? (p->st - prev[i].st) : 0;

        long ticks_per_sec = sysconf(_SC_CLK_TCK);
        double cpu_time_ms = ((double)(dut + dst) * 1000.0) / ticks_per_sec;

        p->cpu_pct =
            (time_diff_ms > 0) ? (cpu_time_ms * 100.0 / time_diff_ms) : 0.0;
        p->suspended_by_manager = prev[i].suspended_by_manager;
        break;
      }
    }

    nprocs++;
  }
  closedir(d);

  memcpy(prev, procs, nprocs * sizeof(PInfo));
  prevn = nprocs;
  prev_time_ms = curr_time_ms;
}

static bool is_priority_proc(const char *comm) {
  for (int i = 0; i < num_priority_procs; i++) {
    if (strstr(comm, priority_procs[i]))
      return true;
  }
  return false;
}

static bool is_system_critical(const char *comm) {
  static const char *critical[] = {"systemd",
                                   "init",
                                   "kernel",
                                   "kthread",
                                   "ksoftirq",
                                   "kworker",
                                   "Xorg",
                                   "X",
                                   "wayland",
                                   "sway",
                                   "gnome-shell",
                                   "kwin",
                                   "mutter",
                                   "plasmashell",
                                   "xfwm4",
                                   "openbox",
                                   "i3",
                                   "dwm",
                                   "awesome",
                                   "gdm",
                                   "sddm",
                                   "lightdm",
                                   "login",
                                   "getty",
                                   "pulseaudio",
                                   "pipewire",
                                   "wireplumber",
                                   "alsa",
                                   "NetworkManager",
                                   "wpa_supplicant",
                                   "dhclient",
                                   "dhcpcd",
                                   "dbus",
                                   "dbus-daemon",
                                   "systemd-",
                                   "udevd",
                                   "upowerd",
                                   "polkitd",
                                   "rtkit",
                                   "accounts-daemon",
                                   "udisksd",
                                   "bluetoothd",
                                   "cupsd",
                                   "avahi",
                                   "ssh",
                                   "sshd",
                                   "cron",
                                   "crond",
                                   "atd",
                                   "rsyslogd",
                                   "syslog",
                                   "journald",
                                   "dockerd",
                                   "containerd",
                                   "kubelet",
                                   "libvirtd",
                                   "virtlogd",
                                   "qemu",
                                   "xfce4-session",
                                   "mate-session",
                                   "cinnamon-session",
                                   "lxsession",
                                   "lxqt-session",
                                   "gnome-session",
                                   "kde-session",
                                   NULL};

  for (int i = 0; critical[i] != NULL; i++) {
    if (strstr(comm, critical[i]))
      return true;
  }

  return false;
}

static void manage_resources(void) {
  if (!auto_manage_enabled)
    return;

  bool priority_running = false;
  for (int i = 0; i < nprocs; i++) {
    if (is_priority_proc(procs[i].comm) && procs[i].running) {
      priority_running = true;
      break;
    }
  }

  if (!priority_running)
    return;

  for (int i = 0; i < nprocs; i++) {
    if (is_priority_proc(procs[i].comm))
      continue;

    if (is_system_critical(procs[i].comm))
      continue;

    if (procs[i].uid == 0)
      continue;

    if ((procs[i].cpu_pct > 10.0 || procs[i].rss_kb > 500000) &&
        procs[i].running && !procs[i].suspended_by_manager) {
      if (kill(procs[i].pid, SIGSTOP) == 0) {
        procs[i].suspended_by_manager = true;
      }
    }
  }
}

static void resume_suspended(void) {
  for (int i = 0; i < nprocs; i++) {
    if (procs[i].suspended_by_manager) {
      kill(procs[i].pid, SIGCONT);
      procs[i].suspended_by_manager = false;
    }
  }
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

static int get_color(double ratio) {
  if (ratio > 0.75)
    return C_RED;
  if (ratio > 0.40)
    return C_YELLOW;
  return C_GREEN;
}

static void draw_vert_bar(int y, int x, int h, double ratio, int col) {
  if (h <= 0)
    return;
  if (ratio < 0.0)
    ratio = 0.0;
  if (ratio > 1.0)
    ratio = 1.0;

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
  if (COLS > width)
    return (COLS - width) / 2;
  return 1;
}

static int get_content_width(void) {
  if (COLS > CONTENT_WIDTH)
    return CONTENT_WIDTH;
  return COLS - 2;
}

// ---------- Pages ----------
enum {
  PAGE_MAIN = 0,
  PAGE_GRAPH,
  PAGE_SYSINFO,
  PAGE_HELP,
  PAGE_ABOUT,
  PAGE_PROCS,
  PAGE_RESOURCE_MGR
};
static int page = PAGE_MAIN, menu_sel = 0, proc_sel = 0, sort_mode = 0;

// Forward declarations
static void draw_graphs(void);
static void draw_sysinfo(void);
static void draw_help(void);
static void draw_about(void);
static void draw_procs(void);
static void draw_resource_mgr(void);

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
  const char *items[] = {"< Graph >",
                         "< System Info >",
                         "< Process Manager >",
                         "< Resource Manager >",
                         "< Help >",
                         "< About >",
                         "< Quit >"};
  mvprintw(y++, sx, "Menu");
  for (int i = 0; i < 7; i++) {
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
    if (!nl)
      break;
    mvprintw(ly++, right_col_x, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6)
      break;
  }
  attroff(COLOR_PAIR(C_MAGENTA) | A_BOLD);

  ly += 1;
  attron(COLOR_PAIR(C_CYAN));
  char *line = (char *)neofetch_info;
  while (*line && ly < H - 2) {
    char *end = strchr(line, '\n');
    if (!end)
      break;
    int len = end - line;
    if (len > right_col_w - 4)
      len = right_col_w - 4;
    mvprintw(ly++, right_col_x + 2, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));
  refresh();
}

static void draw_resource_mgr(void) {
  int W = COLS, H = LINES;
  erase();

  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "Resource Manager");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "D:Delete Last  T:Toggle Auto  R:Resume All  ESC/q:Back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  int cw = get_content_width();
  int sx = get_start_x(cw);
  int y = 2;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== RESOURCE MANAGEMENT ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);
  y++;

  mvprintw(y++, sx, "Auto Management: %s",
           auto_manage_enabled ? "[ENABLED]" : "[DISABLED]");
  if (auto_manage_enabled) {
    attron(COLOR_PAIR(C_GREEN));
    mvprintw(
        y++, sx,
        "System will suspend low-priority processes when priority apps run");
    attroff(COLOR_PAIR(C_GREEN));
  } else {
    attron(COLOR_PAIR(C_YELLOW));
    mvprintw(y++, sx, "Press 'T' to enable automatic resource management");
    attroff(COLOR_PAIR(C_YELLOW));
  }
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "Priority Processes (%d/%d):", num_priority_procs,
           MAX_PRIORITY_PROCS);
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  if (num_priority_procs == 0) {
    mvprintw(y++, sx + 2, "(No priority processes set)");
    mvprintw(y++, sx + 2, "Go to Process Manager and press 'A' on a process");
  } else {
    for (int i = 0; i < num_priority_procs; i++) {
      attron(COLOR_PAIR(C_GREEN));
      mvprintw(y++, sx + 2, "%d. %s", i + 1, priority_procs[i]);
      attroff(COLOR_PAIR(C_GREEN));
    }
  }
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "How it works:");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx + 2, "1. Go to Process Manager");
  mvprintw(y++, sx + 2, "2. Select a process and press 'A' to add to priority");
  mvprintw(y++, sx + 2, "3. Enable auto management here (T)");
  mvprintw(y++, sx + 2, "4. When priority processes run:");
  mvprintw(y++, sx + 5, "- Suspends non-root, non-system processes");
  mvprintw(y++, sx + 5, "- Only if using >10%% CPU or >500MB RAM");
  mvprintw(y++, sx + 5, "- System-critical processes are protected");
  mvprintw(y++, sx + 2, "5. Resources freed for priority processes");
  y++;

  int suspended = 0;
  for (int i = 0; i < nprocs; i++) {
    if (procs[i].suspended_by_manager)
      suspended++;
  }

  if (suspended > 0) {
    attron(COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvprintw(y++, sx, "Currently Suspended: %d processes", suspended);
    attroff(COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvprintw(y++, sx + 2, "Press 'R' to resume all suspended processes");
  }

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
    bar_h = (H - 8) / 4;
    if (bar_h < 1)
      bar_h = 1;
    box_h = bar_h + 3;
  }

  int y = start_y;
  int half_w = (max_w - 3) / 2;

  draw_box(y, start_x, box_h, half_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, start_x + 2, " Temp [C] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  if (temp_available) {
    double tc = temp_c();
    double t_ratio = tc / 100.0;
    if (t_ratio < 0)
      t_ratio = 0;
    if (t_ratio > 1)
      t_ratio = 1;

    draw_vert_bar(y + 2, start_x + 4, bar_h, t_ratio, get_color(t_ratio));
    mvprintw(y + bar_h + 2, start_x + 3, "%.1f", tc);
    char tlabel[32];
    snprintf(tlabel, sizeof(tlabel), "%s",
             tz_path[0] ? (strrchr(tz_path, '/') ? strrchr(tz_path, '/') + 1
                                                 : "Sensor")
                        : "N/A");
    mvprintw(y + 1, start_x + 3, "%-6.6s", tlabel);
  } else {
    mvprintw(y + bar_h / 2 + 1, start_x + 3, "N/A");
  }

  int mem_x = start_x + half_w + 3;
  draw_box(y, mem_x, box_h, half_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, mem_x + 2, " Memory [%%] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  double mem_used_pct = mt ? (double)(mt - ma) / mt : 0.0;

  draw_vert_bar(y + 2, mem_x + 4, bar_h, mem_used_pct, get_color(mem_used_pct));
  mvprintw(y + bar_h + 2, mem_x + 3, "%.1f%%", mem_used_pct * 100.0);
  mvprintw(y + 1, mem_x + 3, "Used");
  mvprintw(y + 1, mem_x + 12, "%.0fMB/%.0fMB", (mt - ma) / 1024.0, mt / 1024.0);

  y += box_h + 1;
  if (y > H - 10)
    goto end_draw;

  draw_box(y, start_x, box_h, max_w);
  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y, start_x + 2, " Frequency [MHz] ");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  double freqs[MAX_CORES] = {0};
  read_cpu_freq_mhz(freqs);
  double fmax = cpu_freq_ghz * 1000.0;
  if (fmax < 1000)
    fmax = 4000.0;

  int bar_width = 3, bar_spacing = 4;
  bool any_freq = false;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if (freqs[i] > 0) {
      any_freq = true;
      break;
    }
  }

  if (any_freq) {
    for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
      int x = start_x + 4 + (i * (bar_width + bar_spacing));
      if (x + bar_width + 2 > max_w + start_x)
        break;
      double f_ratio = freqs[i] / fmax;
      draw_vert_bar(y + 2, x, bar_h, f_ratio, get_color(f_ratio));
      mvprintw(y + bar_h + 2, x, "%4.0f", freqs[i]);
      char clbl[8];
      snprintf(clbl, sizeof(clbl), "Core %d", i);
      mvprintw(y + 1, x, "%-6.6s", clbl);
    }
  } else {
    mvprintw(y + bar_h / 2 + 1, start_x + max_w / 2 - 10,
             "Frequency data unavailable");
  }

end_draw:
  refresh();
}

static void draw_sysinfo(void) {
  int W = COLS, H = LINES;
  erase();

  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "System Information");
  mvhline(H - 1, 0, ' ', W);
  mvprintw(H - 1, 2, "Press ESC or q to return");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  int cw = get_content_width();
  int sx = get_start_x(cw);
  int y = 2;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== PROCESSOR ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  mvprintw(y++, sx, "Model: %s", cpu_model);
  mvprintw(y++, sx, "Cores: %d", NCPU);
  if (cpu_freq_ghz > 0) {
    mvprintw(y++, sx, "Base Frequency: %.2f GHz", cpu_freq_ghz);
  }

  double freqs[MAX_CORES] = {0};
  read_cpu_freq_mhz(freqs);
  double avg_freq = 0;
  int freq_count = 0;
  for (int i = 0; i < NCPU && i < MAX_CORES; i++) {
    if (freqs[i] > 0) {
      avg_freq += freqs[i];
      freq_count++;
    }
  }
  if (freq_count > 0) {
    avg_freq /= freq_count;
    mvprintw(y++, sx, "Current Frequency: %.0f MHz (avg)", avg_freq);
  }

  if (temp_available) {
    double tc = temp_c();
    mvprintw(y++, sx, "Temperature: %.1f°C", tc);
  }

  mvprintw(y++, sx, "Current Usage: %.1f%%", cpu.total * 100.0);
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== MEMORY ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);
  double mem_used_mb = (mt - ma) / 1024.0;
  double mem_avail_mb = ma / 1024.0;
  double mem_free_mb = mf / 1024.0;
  double mem_total_mb = mt / 1024.0;
  double used_pct = mt ? ((double)(mt - ma) / mt) * 100.0 : 0.0;

  mvprintw(y++, sx, "Total: %.0f MB (%.2f GB)", mem_total_mb,
           mem_total_mb / 1024.0);
  mvprintw(y++, sx, "Used: %.0f MB (%.1f%%)", mem_used_mb, used_pct);
  mvprintw(y++, sx, "Available: %.0f MB", mem_avail_mb);
  mvprintw(y++, sx, "Free: %.0f MB", mem_free_mb);
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== STORAGE ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  FILE *df = popen("df -h / 2>/dev/null | tail -1", "r");
  if (df) {
    char fs[64], size[32], used[32], avail[32], use_pct[32], mount[64];
    if (fscanf(df, "%63s %31s %31s %31s %31s %63s", fs, size, used, avail,
               use_pct, mount) == 6) {
      mvprintw(y++, sx, "Root Filesystem: %s", fs);
      mvprintw(y++, sx, "Total: %s", size);
      mvprintw(y++, sx, "Used: %s (%s)", used, use_pct);
      mvprintw(y++, sx, "Available: %s", avail);
    } else {
      mvprintw(y++, sx, "Storage info unavailable");
    }
    pclose(df);
  } else {
    mvprintw(y++, sx, "Storage info unavailable");
  }
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== OPERATING SYSTEM =========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  mvprintw(y++, sx, "Distribution: %s", distro);
  mvprintw(y++, sx, "Kernel: %s", kernel_rel);
  mvprintw(y++, sx, "Hostname: %s", host);

  int uh, um, us;
  read_uptime(&uh, &um, &us);
  mvprintw(y++, sx, "Uptime: %dd %02dh %02dm %02ds", uh / 24, uh % 24, um, us);
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== NETWORK ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  FILE *ip_cmd =
      popen("ip -4 addr show 2>/dev/null | grep -oP "
            "'(?<=inet\\s)\\d+(\\.\\d+){3}' | grep -v 127.0.0.1 | head -1",
            "r");
  char local_ip[64] = "Not connected";
  if (ip_cmd) {
    if (fgets(local_ip, sizeof(local_ip), ip_cmd)) {
      local_ip[strcspn(local_ip, "\n")] = 0;
    }
    pclose(ip_cmd);
  }
  mvprintw(y++, sx, "Local IP: %s", local_ip);

  FILE *iface_cmd =
      popen("ip route | grep default | awk '{print $5}' | head -1", "r");
  char iface[64] = "Unknown";
  if (iface_cmd) {
    if (fgets(iface, sizeof(iface), iface_cmd)) {
      iface[strcspn(iface, "\n")] = 0;
    }
    pclose(iface_cmd);
  }
  mvprintw(y++, sx, "Interface: %s", iface);

  FILE *gw_cmd =
      popen("ip route | grep default | awk '{print $3}' | head -1", "r");
  char gateway[64] = "Unknown";
  if (gw_cmd) {
    if (fgets(gateway, sizeof(gateway), gw_cmd)) {
      gateway[strcspn(gateway, "\n")] = 0;
    }
    pclose(gw_cmd);
  }
  mvprintw(y++, sx, "Gateway: %s", gateway);
  y++;

  attron(COLOR_PAIR(C_CYAN) | A_BOLD);
  mvprintw(y++, sx, "======== BATTERY ========");
  attroff(COLOR_PAIR(C_CYAN) | A_BOLD);

  FILE *bat_cap = fopen("/sys/class/power_supply/BAT0/capacity", "r");
  FILE *bat_stat = fopen("/sys/class/power_supply/BAT0/status", "r");
  FILE *bat_health = fopen("/sys/class/power_supply/BAT0/health", "r");

  if (bat_cap && bat_stat) {
    int capacity = 0;
    char status[32] = "Unknown";
    char health[32] = "Unknown";

    fscanf(bat_cap, "%d", &capacity);
    if (fgets(status, sizeof(status), bat_stat)) {
      status[strcspn(status, "\n")] = 0;
    }
    if (bat_health && fgets(health, sizeof(health), bat_health)) {
      health[strcspn(health, "\n")] = 0;
    }

    int bat_color = C_GREEN;
    if (capacity < 20)
      bat_color = C_RED;
    else if (capacity < 50)
      bat_color = C_YELLOW;

    attron(COLOR_PAIR(bat_color));
    mvprintw(y++, sx, "Percentage: %d%%", capacity);
    attroff(COLOR_PAIR(bat_color));

    mvprintw(y++, sx, "Status: %s", status);
    mvprintw(y++, sx, "Health: %s", health);

    int barw = (cw > 60) ? 40 : (cw - 20);
    int filled = (capacity * barw) / 100;

    attron(COLOR_PAIR(bat_color) | A_BOLD);
    mvhline(y, sx + 2, ACS_CKBOARD, filled);
    attroff(COLOR_PAIR(bat_color) | A_BOLD);
    attron(COLOR_PAIR(C_DIM_WHITE));
    mvhline(y, sx + 2 + filled, ACS_CKBOARD, barw - filled);
    attroff(COLOR_PAIR(C_DIM_WHITE));

  } else {
    mvprintw(y++, sx, "No battery detected");
  }

  if (bat_cap)
    fclose(bat_cap);
  if (bat_stat)
    fclose(bat_stat);
  if (bat_health)
    fclose(bat_health);

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
  mvprintw(y++, sx + 4, "A            - Add to priority list");
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
    if (!nl)
      break;
    mvprintw(ly++, sx, "%.*s", (int)(nl - p), p);
    p = nl + 1;
    if (ly > H - 6)
      break;
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
    if (!end)
      break;
    int len = end - line;
    if (len > info_w - 2)
      len = info_w - 2;
    mvprintw(ny++, info_x, "%.*s", len, line);
    line = end + 1;
  }
  attroff(COLOR_PAIR(C_CYAN));
  refresh();
}

static void draw_procs(void) {
  int W = COLS, H = LINES;

  if (W < 40 || H < 10) {
    return;
  }

  erase();

  int cw = get_content_width();
  int sx = get_start_x(cw);

  if (cw <= 0)
    cw = W - 4;
  if (sx < 0)
    sx = 1;
  if (cw < 60)
    cw = 60;

  attron(COLOR_PAIR(C_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', W);
  mvprintw(0, 2, "Process Manager - %d processes", nprocs);
  mvhline(H - 1, 0, ' ', W);
  mvprintw(
      H - 1, 2,
      "↑↓:Move c:CPU m:Mem A:Add Priority K:Kill S:Stop/Cont +/-:Nice q:Back");
  attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

  mvprintw(2, sx, "CPU:");

  int barw = (cw / 3) * 2;
  if (barw < 20)
    barw = 20;
  if (barw > cw - 8)
    barw = cw - 8;

  double pct = cpu.total;
  int filled = (int)(pct * barw);
  int col = get_color(pct);

  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(2, sx + 6, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(2, sx + 6 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));

  mvprintw(2, sx + 6 + barw + 2, "%3d%%", (int)(pct * 100));

  unsigned long mt = 0, mf = 0, ma = 0;
  mem_read_kb(&mt, &mf, &ma);

  double mem_used = mt - ma;
  double mem_avail = ma;
  double used_pct = mt ? (double)mem_used / mt : 0.0;
  double avail_pct = mt ? (double)mem_avail / mt : 0.0;

  mvprintw(3, sx, "MEM:");

  filled = (int)(used_pct * barw);
  col = get_color(used_pct);

  attron(COLOR_PAIR(col) | A_BOLD);
  mvhline(3, sx + 6, ACS_CKBOARD, filled);
  attroff(COLOR_PAIR(col) | A_BOLD);
  attron(COLOR_PAIR(C_DIM_WHITE));
  mvhline(3, sx + 6 + filled, ACS_CKBOARD, barw - filled);
  attroff(COLOR_PAIR(C_DIM_WHITE));

  char mem_str[100];
  snprintf(mem_str, sizeof(mem_str),
           "Used:%3d%%  Avail:%3d%%  (Used:%luMB  Avail:%luMB)",
           (int)(used_pct * 100), (int)(avail_pct * 100),
           (unsigned long)mem_used / 1024, (unsigned long)mem_avail / 1024);

  mvprintw(3, sx + 6 + barw + 2, "%s", mem_str);

  attron(A_BOLD | COLOR_PAIR(C_HEADER));
  char header_fmt[128];
  snprintf(header_fmt, sizeof(header_fmt), "%%-%ds", cw);
  mvprintw(5, sx, header_fmt,
           " PID    COMMAND              USER         CPU%%      MEM(MB)   NI "
           "STATE  PRI");
  attroff(A_BOLD | COLOR_PAIR(C_HEADER));

  if (nprocs > 0) {
    if (sort_mode == 0)
      qsort(procs, nprocs, sizeof(PInfo), cmp_cpu);
    else
      qsort(procs, nprocs, sizeof(PInfo), cmp_mem);
  }

  if (proc_sel < 0)
    proc_sel = 0;
  if (proc_sel >= nprocs && nprocs > 0)
    proc_sel = nprocs - 1;

  int rows = H - 8;
  if (rows < 1)
    rows = 1;

  int start = proc_sel - rows / 2;
  if (start < 0)
    start = 0;
  int end = start + rows;
  if (end > nprocs)
    end = nprocs;
  if (start > end - rows && end >= rows)
    start = end - rows;
  if (start < 0)
    start = 0;

  int y = 6;
  int name_w = cw - 60;
  if (name_w < 10)
    name_w = 10;

  for (int i = start; i < end && i < nprocs; i++) {
    if (y >= H - 1)
      break;

    PInfo *p = &procs[i];

    char line_buf[256];
    double ui_pct = p->cpu_pct;
    if (ui_pct < 0)
      ui_pct = 0;
    if (ui_pct > 9999)
      ui_pct = 9999;

    const char *uname = uname_from_uid(p->uid);
    if (!uname)
      uname = "unknown";

    bool is_pri = is_priority_proc(p->comm);
    const char *pri_mark = is_pri ? " *" : "";

    snprintf(line_buf, sizeof(line_buf),
             " %-6d %-20.20s %-12.12s %7.1f  %9.1f  %3d %-5s %s", p->pid,
             p->comm, uname, ui_pct, p->rss_kb / 1024.0, p->nicev,
             p->running ? "RUN" : "STOP", pri_mark);

    line_buf[sizeof(line_buf) - 1] = '\0';

    if (i == proc_sel) {
      attron(COLOR_PAIR(C_BG_SELECTED) | A_BOLD);
      mvprintw(y, sx, "%-*s", cw, line_buf);
      attroff(COLOR_PAIR(C_BG_SELECTED) | A_BOLD);
    } else {
      int proc_color = is_pri ? C_CYAN : C_GREEN;
      if (ui_pct > 50)
        proc_color = C_YELLOW;
      if (ui_pct > 75)
        proc_color = C_RED;

      attron(COLOR_PAIR(proc_color));
      mvprintw(y, sx, "%s", line_buf);
      attroff(COLOR_PAIR(proc_color));
    }

    y++;
  }

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
  push_mem_hist();
  usleep(100000);
  cpu_sample();
  push_mem_hist();
  read_cpu_totals();
  scan_processes();

  while (1) {
    long t = now_ms();

    if (t - t_cpu >= CPU_MS) {
      cpu_sample();
      push_mem_hist();
      t_cpu = t;
    }
    if (t - t_proc >= PROC_MS) {
      if (page == PAGE_PROCS || page == PAGE_MAIN ||
          page == PAGE_RESOURCE_MGR) {
        read_cpu_totals();
        scan_processes();
        if (auto_manage_enabled) {
          manage_resources();
        }
      }
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
    case PAGE_RESOURCE_MGR:
      draw_resource_mgr();
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
      break;
    }

    if (page == PAGE_MAIN) {
      if (ch == KEY_UP || ch == 'k')
        menu_sel = (menu_sel + 6) % 7;
      else if (ch == KEY_DOWN || ch == 'j')
        menu_sel = (menu_sel + 1) % 7;
      else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (menu_sel == 0)
          page = PAGE_GRAPH;
        else if (menu_sel == 1)
          page = PAGE_SYSINFO;
        else if (menu_sel == 2) {
          page = PAGE_PROCS;
          read_cpu_totals();
          scan_processes();
        } else if (menu_sel == 3)
          page = PAGE_RESOURCE_MGR;
        else if (menu_sel == 4)
          page = PAGE_HELP;
        else if (menu_sel == 5)
          page = PAGE_ABOUT;
        else if (menu_sel == 6)
          break;
      }
    } else if (page == PAGE_RESOURCE_MGR) {
      if (ch == 'D' || ch == 'd') {
        if (num_priority_procs > 0)
          num_priority_procs--;
      } else if (ch == 'T' || ch == 't') {
        auto_manage_enabled = !auto_manage_enabled;
        if (!auto_manage_enabled) {
          resume_suspended();
        }
      } else if (ch == 'R' || ch == 'r') {
        resume_suspended();
      }
    } else if (page == PAGE_PROCS) {
      if (ch == KEY_UP || ch == 'k') {
        proc_sel--;
        if (proc_sel < 0)
          proc_sel = 0;
      } else if (ch == KEY_DOWN || ch == 'j') {
        proc_sel++;
        if (nprocs > 0 && proc_sel >= nprocs)
          proc_sel = nprocs - 1;
      } else if (ch == KEY_PPAGE) {
        proc_sel -= 10;
        if (proc_sel < 0)
          proc_sel = 0;
      } else if (ch == KEY_NPAGE) {
        proc_sel += 10;
        if (nprocs > 0 && proc_sel >= nprocs)
          proc_sel = nprocs - 1;
      } else if (ch == 'c') {
        sort_mode = 0;
      } else if (ch == 'm') {
        sort_mode = 1;
      } else if (ch == 'A' || ch == 'a') {
        if (nprocs > 0 && proc_sel >= 0 && proc_sel < nprocs &&
            num_priority_procs < MAX_PRIORITY_PROCS) {
          bool already_added = false;
          for (int i = 0; i < num_priority_procs; i++) {
            if (strcmp(priority_procs[i], procs[proc_sel].comm) == 0) {
              already_added = true;
              break;
            }
          }
          if (!already_added) {
            strncpy(priority_procs[num_priority_procs], procs[proc_sel].comm,
                    MAX_COMM - 1);
            priority_procs[num_priority_procs][MAX_COMM - 1] = '\0';
            num_priority_procs++;
          }
        }
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
