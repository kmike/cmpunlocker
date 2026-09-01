/*
 * cmp-gen2-watch — advertisement-triggered PCIe Gen2 retrain for the
 * CMP 170HX (10de:20c2 / 10de:2082), replacing the blind timer-based
 * hammer.
 *
 * Mechanism (measured on GA100 CMP 170HX, driver 610.43.02, Sep 2026):
 * the driver patch (driver/patches/pcie-gen2.patch) flips the endpoint's
 * LnkCap/LnkCtl2 to Gen2 during GSP bootstrap; that flipped state — the "advertisement window"
 * — lasts only ~430 ms on some driver/platform combinations before
 * firmware reverts it. Any retrain fired outside the window hits a
 * Gen1-advertising endpoint and lands Gen1. Additionally, a Retrain
 * Link write whose effective target speed equals the current speed is
 * a silent no-op. Therefore: poll LnkCap at 1 kHz; the moment a card
 * advertises >= Gen2, raise the parent port's LnkCtl2 target and fire
 * Retrain Link bursts (50 ms apart) for as long as the flip is live.
 * A link that trains Gen2 inside the window stays Gen2 after it closes.
 *
 * Deployment stages (both installed by tools/watch-setup.sh):
 *   - initramfs (dracut module / initramfs-tools hook): spawned before
 *     udev coldplug and — crucially — survives the pivot to the real
 *     root, so it covers every window from initramfs start onward;
 *   - systemd userspace unit as the second net, pulled in at
 *     sysinit.target with DefaultDependencies=no.
 *
 * Auto-discovers every supported GPU and its nearest downstream parent
 * port (root port or switch downstream port). No configuration needed.
 *
 * Output: human-readable log lines on stdout (stdio-unbuffered so
 * logs survive abrupt termination, e.g. the initramfs switch_root kill).
 *
 * Build:  cc -O2 -Wall -o cmp-gen2-watch cmp-gen2-watch.c
 * Run as: root (needs config-space R/W). No other dependencies.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <limits.h>

#define SYS_PCI "/sys/bus/pci/devices"
#define NVIDIA_VENDOR "0x10de"
#define LINK_RETRAIN_INTERVAL_MS 50
#define MAX_CARDS 16

typedef struct {
    char bdf[32];
    char parent[32];
    int cfgfd;                 /* GPU config fd */
    int pfd;                   /* parent config fd */
    int pcap, gcap;            /* PCIe cap offsets */
    uint32_t last_lcap;
    long last_burst;
    int announced;
} card_t;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int kmsg_fd = -1;
static void kmsg_try_open(void) {
    if (kmsg_fd < 0) kmsg_fd = open("/dev/kmsg", O_WRONLY);
}
static void logline(const char *fmt, ...) {
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    fputs(buf, stdout);
    fputs("\n", stdout);
    if (kmsg_fd >= 0) {
        ssize_t r = write(kmsg_fd, buf, (size_t)n);   /* one line per kmsg record */
        (void)r;
    }
}

static char *slurp(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t r = read(fd, buf, len - 1);
    close(fd);
    if (r <= 0) return NULL;
    buf[r] = 0;
    while (r > 0 && (buf[r-1] == '\n' || buf[r-1] == ' ')) buf[--r] = 0;
    return buf;
}

static int cfg_open(const char *bdf) {
    char path[256];
    snprintf(path, sizeof path, SYS_PCI "/%s/config", bdf);
    return open(path, O_RDWR);
}

static int cfg_read(int fd, off_t off, void *buf, size_t len) {
    return pread(fd, buf, len, off) == (ssize_t)len ? 0 : -1;
}
static int cfg_write(int fd, off_t off, const void *buf, size_t len) {
    return pwrite(fd, buf, len, off) == (ssize_t)len ? 0 : -1;
}

static uint16_t r16(int fd, off_t off) {
    uint16_t v = 0xFFFF;
    if (cfg_read(fd, off, &v, 2)) return 0xFFFF;
    return v;
}
static uint32_t r32(int fd, off_t off) {
    uint32_t v = 0xFFFFFFFF;
    if (cfg_read(fd, off, &v, 4)) return 0xFFFFFFFF;
    return v;
}
static int w16(int fd, off_t off, uint16_t v) {
    return cfg_write(fd, off, &v, 2);
}

/* standard capability walk to find PCIe (0x10) */
static int find_pcie_cap(int fd) {
    unsigned char ptr = 0;
    if (cfg_read(fd, 0x34, &ptr, 1)) return 0;
    for (int i = 0; i < 48 && (ptr & 0xFC); i++) {
        unsigned char hdr[2];
        if (cfg_read(fd, ptr, hdr, 2)) return 0;
        if (hdr[0] == 0x10) return ptr;
        ptr = hdr[1] & 0xFC;
    }
    return 0;
}

/* PCIECAP register is at cap+0x02 (byte 0 = cap id, byte 1 = next
 * ptr); bits 7:4 port type. Downstream-capable = root port (4) or
 * switch downstream (6). */
static int port_type(int fd, int cap) {
    uint16_t p = r16(fd, cap + 0x02);
    if (p == 0xFFFF) return -1;
    return (p >> 4) & 0xF;
}

/* sysfs parent of a device: resolve the symlink, take the dirname */
static int sysfs_parent(const char *bdf, char *out, size_t len) {
    char path[256], target[PATH_MAX];
    snprintf(path, sizeof path, SYS_PCI "/%s", bdf);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n < 0) return -1;
    target[n] = 0;
    char *slash = strrchr(target, '/');
    if (!slash || slash == target) return -1;
    *slash = 0;
    char *parent = strrchr(target, '/');
    if (!parent) return -1;
    snprintf(out, len, "%s", parent + 1);
    return 0;
}

static int is_supported_gpu(const struct dirent *e) {
    char buf[64], path[512];
    if (snprintf(path, sizeof path, SYS_PCI "/%s/vendor", e->d_name) >= (int)sizeof path) return 0;
    if (!slurp(path, buf, sizeof buf) || strcmp(buf, NVIDIA_VENDOR)) return 0;
    if (snprintf(path, sizeof path, SYS_PCI "/%s/device", e->d_name) >= (int)sizeof path) return 0;
    if (!slurp(path, buf, sizeof buf)) return 0;
    return !strcmp(buf, "0x20c2") || !strcmp(buf, "0x2082");
}

/* walk up the topology to the nearest downstream port able to retrain */
static int find_retrain_parent(const char *bdf, char *out, size_t len) {
    char cur[32];
    snprintf(cur, sizeof cur, "%s", bdf);
    for (int hop = 0; hop < 8; hop++) {
        char parent[32];
        if (sysfs_parent(cur, parent, sizeof parent)) return -1;
        int fd = cfg_open(parent);
        if (fd < 0) return -1;
        int cap = find_pcie_cap(fd);
        int t = cap ? port_type(fd, cap) : -1;
        close(fd);
        if (t == 4 || t == 6) {           /* root port or switch downstream */
            snprintf(out, len, "%s", parent);
            return 0;
        }
        snprintf(cur, sizeof cur, "%s", parent);
    }
    return -1;
}

static int discover(card_t *cards, int max) {
    DIR *d = opendir(SYS_PCI);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        if (!is_supported_gpu(e)) continue;
        card_t *c = &cards[n];
        memset(c, 0, sizeof *c);
        snprintf(c->bdf, sizeof c->bdf, "%.31s", e->d_name);  /* bounded: dirent names */
        if (find_retrain_parent(c->bdf, c->parent, sizeof c->parent)) {
            logline("# %s: no downstream parent port found, skipping", c->bdf);
            continue;
        }
        c->cfgfd = cfg_open(c->bdf);
        c->pfd = cfg_open(c->parent);
        c->gcap = c->cfgfd >= 0 ? find_pcie_cap(c->cfgfd) : 0;
        c->pcap = c->pfd >= 0 ? find_pcie_cap(c->pfd) : 0;
        if (c->cfgfd < 0 || c->pfd < 0 || !c->gcap || !c->pcap) {
            logline("# %s: config/parent not ready yet, skipping", c->bdf);
            if (c->cfgfd >= 0) close(c->cfgfd);
            if (c->pfd >= 0) close(c->pfd);
            continue;
        }
        c->last_lcap = 0xFFFFFFFF;
        c->last_burst = -1000000;
        logline("# %s via parent %s: watching", c->bdf, c->parent);
        n++;
    }
    closedir(d);
    return n;
}

/* Raise the parent's target link speed. Left in place on exit:
 * a parent port with TLS already at target is the correct persistent
 * state — it is what makes any later retrain (ours or anyone else's)
 * able to train up. Idempotent. */
static void parent_set_tls(card_t *c, int target) {
    uint16_t v = r16(c->pfd, c->pcap + 0x30);
    if (v == 0xFFFF) return;
    uint16_t nv = (v & ~0x000F) | (target & 0xF);
    if (nv != v) w16(c->pfd, c->pcap + 0x30, nv);
}

static void parent_retrain(card_t *c) {
    uint16_t ctl = r16(c->pfd, c->pcap + 0x10);
    if (ctl == 0xFFFF) return;
    w16(c->pfd, c->pcap + 0x10, ctl | 0x0020);   /* Retrain Link, bit 5 */
}

static volatile sig_atomic_t stop = 0;
static void on_alrm(int s) { (void)s; stop = 1; }

int main(int argc, char **argv) {
    long interval_us = 1000;
    long duration_ms = 120000;
    int target = 2;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interval-us") && i+1 < argc) interval_us = atol(argv[++i]);
        else if (!strcmp(argv[i], "--duration-ms") && i+1 < argc) duration_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--target") && i+1 < argc) target = atoi(argv[++i]);
        else { fprintf(stderr, "usage: cmp-gen2-watch [--interval-us N] [--duration-ms N] [--target 2]\n"
                              "Auto-discovers all CMP 170HX GPUs (10de:20c2/2082) and retrains each\n"
                              "from its parent port the moment its LnkCap advertises >= target.\n"); return 2; }
    }
    setvbuf(stdout, NULL, _IONBF, 0);   /* survive switch_root kills */
    kmsg_try_open();                    /* best effort; retried below */

    card_t cards[MAX_CARDS];
    memset(cards, 0, sizeof cards);
    int n = discover(cards, MAX_CARDS);
    if (!n) { logline("# no supported GPUs found; nothing to do"); return 0; }

    /* fast path: if every card is already trained at/above target
     * (e.g. the initramfs stage already caught the window), exit at
     * once so the userspace unit never delays boot. */
    int all_trained = 1;
    for (int i = 0; i < n; i++) {
        uint16_t sta = r16(cards[i].cfgfd, cards[i].gcap + 0x12);
        logline("# %s initial LnkSta=%04x", cards[i].bdf, sta);
        if (sta == 0xFFFF || (sta & 0xF) < (uint16_t)target) all_trained = 0;
    }
    if (all_trained) { logline("# all cards already at/above Gen%d; exiting", target); return 0; }

    signal(SIGALRM, on_alrm);
    alarm((duration_ms / 1000) + 5);

    long t0 = now_ms();
    for (long t = 0; !stop; t = now_ms() - t0) {
        if (kmsg_fd < 0 && (t & 0x3FF) == 0) kmsg_try_open();  /* ~1s cadence */
        for (int i = 0; i < n; i++) {
            card_t *c = &cards[i];
            uint32_t lcap = r32(c->cfgfd, c->gcap + 0x0c);
            if (lcap == 0xFFFFFFFF) continue;          /* transient */
            if (lcap != c->last_lcap) {
                logline("T+%ld.%03ld %s LnkCap %08x -> %08x",
                        t/1000, t%1000, c->bdf, c->last_lcap, lcap);
                c->last_lcap = lcap;
            }
            int speed = lcap & 0xF;
            if (speed >= target) {
                if (!c->announced) {
                    logline("T+%ld.%03ld %s flip detected (speed nibble %d) -> retrain via %s",
                            t/1000, t%1000, c->bdf, speed, c->parent);
                    c->announced = 1;
                }
                if (t - c->last_burst >= LINK_RETRAIN_INTERVAL_MS) {
                    parent_set_tls(c, target);         /* no-op retrain guard */
                    parent_retrain(c);
                    c->last_burst = t;
                }
            }
        }
        if (t >= duration_ms) break;
        struct timespec ts = { interval_us / 1000000, (interval_us % 1000000) * 1000 };
        nanosleep(&ts, NULL);
    }

    for (int i = 0; i < n; i++) {
        card_t *c = &cards[i];
        uint16_t sta = r16(c->cfgfd, c->gcap + 0x12);
        logline("# %s final LnkSta=%04x (%s)", c->bdf, sta,
                (sta != 0xFFFF && (sta & 0xF) >= (uint16_t)target) ? "GEN2+" : "gen1");
    }
    return 0;
}
