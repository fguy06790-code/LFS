/*
 * lfs-setup: a menu-driven system configuration tool for Linux From Scratch,
 * in the spirit of Arch Linux's archinstall.
 *
 * Every setting shows its current value. Changing one only records it (marked
 * with *); "Apply changes" writes them all to the usual LFS/BLFS files:
 *
 *   hostname        /etc/hostname, /etc/hosts
 *   timezone        /etc/localtime -> /usr/share/zoneinfo/...
 *   language        LANG in /etc/profile.d/i18n.sh (or /etc/profile), localedef
 *   console keymap  KEYMAP in /etc/sysconfig/console
 *   desktop keymap  /etc/X11/xorg.conf.d/00-keyboard.conf, /etc/skel/.config/kxkbrc
 *   network         /etc/sysconfig/ifconfig.<iface> (static or dhcpcd)
 *   DNS             /etc/resolv.conf
 *   passwords/users chpasswd, useradd, userdel (admins join "wheel" for sudo)
 *   boot target     initdefault in /etc/inittab (5 = SDDM/Plasma, 3 = console)
 *   auto-login      /etc/sddm.conf.d/10-lfs-setup-autologin.conf
 *   services        S links in /etc/rc.d/rc[2-5].d (disabled = renamed _S..)
 *
 * "Install to disk" copies the running system (e.g. the live ISO) to a disk
 * and makes it bootable with GRUB.
 *
 * lfs-setup --status prints the current settings without the menu.
 *
 * MIT license. Part of https://github.com/fguy06790-code/LFS
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "1.0"
#define ZONEINFO "/usr/share/zoneinfo/"
#define AUTOLOGIN_CONF "/etc/sddm.conf.d/10-lfs-setup-autologin.conf"
#define MAX_USERS 64
#define MAX_SERVICES 128
#define MAX_OPS 64

/* ------------------------------------------------------------------ utils */

static char *xstrdup(const char *s) {
    char *d = strdup(s ? s : "");
    if (!d) { endwin(); perror("strdup"); exit(1); }
    return d;
}

static void copy(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static int is_exec(const char *path) { return access(path, X_OK) == 0; }

static const char *find_prog(const char *name) {
    static char buf[PATH_MAX];
    const char *dirs[] = {"/usr/bin", "/usr/sbin", "/bin", "/sbin", NULL};
    for (int i = 0; dirs[i]; i++) {
        snprintf(buf, sizeof buf, "%s/%s", dirs[i], name);
        if (is_exec(buf)) return buf;
    }
    return NULL;
}

/* Whole file into a malloc'd string (NULL if unreadable). */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t r;
    while (buf && (r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += r;
        if (cap - len < 2) { cap *= 2; buf = realloc(buf, cap); }
    }
    fclose(f);
    if (buf) buf[len] = 0;
    return buf;
}

/* Write a file atomically (temp file + rename), keeping mode 0644 by default. */
static int spew(const char *path, const char *text, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.lfs-setup.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(text, f);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    chmod(tmp, mode);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void mkdirs(const char *path) {
    char buf[PATH_MAX];
    copy(buf, sizeof buf, path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

/* First line of a file, or "" */
static void first_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) chomp(out);
    fclose(f);
}

/* Value of KEY=value (optionally quoted) in a shell-style file. */
static int shell_var(const char *path, const char *key, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "export ", 7)) p += 7;
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        p += kl + 1;
        chomp(p);
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            char *e = strchr(p, q);
            if (e) *e = 0;
        }
        copy(out, n, p);
        found = 1;
    }
    fclose(f);
    return found;
}

/* Replace (or append) KEY="value" in a shell-style config file. */
static int set_shell_var(const char *path, const char *key, const char *val) {
    char *old = slurp(path);
    size_t cap = (old ? strlen(old) : 0) + strlen(key) + strlen(val) + 64;
    char *out = malloc(cap);
    out[0] = 0;
    int done = 0;
    size_t kl = strlen(key);
    if (old) {
        char *p = old;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (!strncmp(q, key, kl) && q[kl] == '=') {
                if (!done) {
                    strcat(out, key); strcat(out, "=\""); strcat(out, val); strcat(out, "\"\n");
                    done = 1;
                }
            } else {
                strncat(out, p, len);
                strcat(out, "\n");
            }
            p += len + (nl ? 1 : 0);
        }
    }
    if (!done) {
        strcat(out, key); strcat(out, "=\""); strcat(out, val); strcat(out, "\"\n");
    }
    int r = spew(path, out, 0644);
    free(old);
    free(out);
    return r;
}

/* Run a program, quietly. Returns its exit status (or -1). */
static int run(char *const argv[], const char *input) {
    int pfd[2] = {-1, -1};
    if (input && pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (input) { dup2(pfd[0], 0); close(pfd[0]); close(pfd[1]); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (input) {
        close(pfd[0]);
        ssize_t w = write(pfd[1], input, strlen(input));
        (void)w;
        close(pfd[1]);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#define RUN(...) run((char *const[]){__VA_ARGS__, NULL}, NULL)

/* Run a program's output into buf (first line, trimmed). */
static int capture(char *const argv[], char *buf, size_t n) {
    int pfd[2];
    buf[0] = 0;
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    FILE *f = fdopen(pfd[0], "r");
    if (f && fgets(buf, (int)n, f)) chomp(buf);
    char sink[256];
    while (f && fgets(sink, sizeof sink, f)) {}
    if (f) fclose(f);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* --------------------------------------------------------------- string lists */

struct list { char **v; int n, cap; };

static void list_add(struct list *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = xstrdup(s);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void list_sort_uniq(struct list *l) {
    qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_str);
    int j = 0;
    for (int i = 0; i < l->n; i++) {
        if (j && !strcmp(l->v[j - 1], l->v[i])) { free(l->v[i]); continue; }
        l->v[j++] = l->v[i];
    }
    l->n = j;
}

static void list_free(struct list *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int list_find(struct list *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) return i;
    return -1;
}

/* ------------------------------------------------------------- validation */

static int valid_hostname(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 63 || s[0] == '-' || s[n - 1] == '-') return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-') return 0;
    return 1;
}

static int valid_username(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 32) return 0;
    if (!(islower((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    return 1;
}

static int valid_ipv4(const char *s) {
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static int valid_prefix(const char *s) {
    char *e;
    long v = strtol(s, &e, 10);
    return *s && !*e && v >= 1 && v <= 32;
}

static int valid_password(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (*s == ':' || *s == '\n') return 0;
    return 1;
}

/* ----------------------------------------------------------- system state */

struct user_op { char kind; /* a=add d=delete p=password */ char name[33]; char pw[128]; int admin; };

struct service { char name[64]; int now, want; };

struct config {
    char hostname[64];
    char timezone[128];
    char lang[64];
    char keymap[64];
    char xkb[64];
    char iface[32];
    char netmode[16];   /* static | dhcp */
    char ip[64], prefix[8], gateway[64];
    char dns[64];
    char bootto[16];    /* graphical | console */
    char autologin[33]; /* "" = off */
    int  winexe;        /* pe-binfmt service enabled */
};

static struct config cur, want;
static char root_pw[128];             /* pending new root password ("" = unchanged) */
static struct user_op ops[MAX_OPS];
static int nops;
static struct service svcs[MAX_SERVICES];
static int nsvcs;

static int has_desktop(void) { return exists("/etc/rc.d/init.d/sddm"); }
static int has_xkb(void) { return exists("/usr/share/X11/xkb/rules/base.lst"); }
static int has_dhcpcd(void) { return find_prog("dhcpcd") != NULL; }
static int has_winexe(void) { return exists("/etc/rc.d/init.d/pe-binfmt"); }

static const char *lang_file(void) {
    return exists("/etc/profile.d/i18n.sh") ? "/etc/profile.d/i18n.sh" : "/etc/profile";
}

/* LANG from the profile: the value that isn't the console fallback C.UTF-8. */
static void read_lang(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(lang_file(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "LANG=");
        if (!p || strstr(line, "LANG=C.UTF-8")) continue;
        if (p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue; /* LC_LANG etc. */
        p += 5;
        if (*p == '"' || *p == '\'') p++;
        char *e = p;
        while (*e && !isspace((unsigned char)*e) && *e != '"' && *e != '\'' && *e != ';') e++;
        *e = 0;
        if (*p && *p != '<') copy(out, n, p);
    }
    fclose(f);
}

static void read_network(struct config *c) {
    c->iface[0] = c->ip[0] = c->prefix[0] = c->gateway[0] = 0;
    copy(c->netmode, sizeof c->netmode, "static");
    DIR *d = opendir("/etc/sysconfig");
    struct dirent *e;
    char path[PATH_MAX], svc[64];
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "ifconfig.", 9)) continue;
        snprintf(path, sizeof path, "/etc/sysconfig/%s", e->d_name);
        copy(c->iface, sizeof c->iface, e->d_name + 9);
        shell_var(path, "SERVICE", svc, sizeof svc);
        if (strstr(svc, "dhcp")) copy(c->netmode, sizeof c->netmode, "dhcp");
        shell_var(path, "IP", c->ip, sizeof c->ip);
        shell_var(path, "PREFIX", c->prefix, sizeof c->prefix);
        shell_var(path, "GATEWAY", c->gateway, sizeof c->gateway);
        break;
    }
    if (d) closedir(d);
    if (!c->iface[0]) {
        /* No config yet: pick the first real interface. */
        d = opendir("/sys/class/net");
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
            copy(c->iface, sizeof c->iface, e->d_name);
            break;
        }
        if (d) closedir(d);
        if (!c->iface[0]) copy(c->iface, sizeof c->iface, "eth0");
        if (has_dhcpcd()) copy(c->netmode, sizeof c->netmode, "dhcp");
    }
}

static void read_dns(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char ns[128];
        if (sscanf(line, " nameserver %127s", ns) == 1) { copy(out, n, ns); break; }
    }
    fclose(f);
}

static void read_bootto(char *out, size_t n) {
    char *t = slurp("/etc/inittab");
    copy(out, n, (t && strstr(t, "id:5:initdefault:")) ? "graphical" : "console");
    free(t);
}

static void read_autologin(char *out, size_t n) {
    out[0] = 0;
    shell_var(AUTOLOGIN_CONF, "User", out, n);
}

/* ---- services */

static int rc_link(const char *dir, const char *prefix, const char *name, char *found, size_t n) {
    DIR *d = opendir(dir);
    struct dirent *e;
    size_t pl = strlen(prefix);
    int hit = 0;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, prefix, pl)) continue;
        const char *p = e->d_name + pl;
        if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) continue;
        if (strcmp(p + 2, name)) continue;
        if (found) copy(found, n, e->d_name);
        hit = 1;
        break;
    }
    if (d) closedir(d);
    return hit;
}

static int service_enabled(const char *name) {
    return rc_link("/etc/rc.d/rc3.d", "S", name, NULL, 0) || rc_link("/etc/rc.d/rc5.d", "S", name, NULL, 0);
}

static void read_services(void) {
    nsvcs = 0;
    DIR *d = opendir("/etc/rc.d/init.d");
    struct dirent *e;
    struct list names = {0};
    while (d && (e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.' || !strcmp(n, "functions") || !strcmp(n, "rc")) continue;
        if (!strcmp(n, "sddm") || !strcmp(n, "pe-binfmt")) continue; /* own menu items */
        if (rc_link("/etc/rc.d/rcS.d", "S", n, NULL, 0)) continue;  /* early boot */
        int linked = 0;
        for (int rl = 2; rl <= 5 && !linked; rl++) {
            char dir[64];
            snprintf(dir, sizeof dir, "/etc/rc.d/rc%d.d", rl);
            linked = rc_link(dir, "S", n, NULL, 0) || rc_link(dir, "K", n, NULL, 0) ||
                     rc_link(dir, "_S", n, NULL, 0);
        }
        if (linked) list_add(&names, n);
    }
    if (d) closedir(d);
    list_sort_uniq(&names);
    for (int i = 0; i < names.n && nsvcs < MAX_SERVICES; i++) {
        copy(svcs[nsvcs].name, sizeof svcs[nsvcs].name, names.v[i]);
        svcs[nsvcs].now = svcs[nsvcs].want = service_enabled(names.v[i]);
        nsvcs++;
    }
    list_free(&names);
}

static int set_service(const char *name, int on) {
    char dir[64], link[128], from[PATH_MAX], to[PATH_MAX];
    int any = 0;
    for (int rl = 2; rl <= 5; rl++) {
        snprintf(dir, sizeof dir, "/etc/rc.d/rc%d.d", rl);
        if (on) {
            while (rc_link(dir, "_S", name, link, sizeof link)) {
                snprintf(from, sizeof from, "%s/%s", dir, link);
                snprintf(to, sizeof to, "%s/%s", dir, link + 1);
                if (rename(from, to) != 0) return -1;
                any = 1;
            }
        } else {
            while (rc_link(dir, "S", name, link, sizeof link)) {
                snprintf(from, sizeof from, "%s/%s", dir, link);
                snprintf(to, sizeof to, "%s/_%s", dir, link);
                if (rename(from, to) != 0) return -1;
            }
        }
    }
    if (on && !any && !service_enabled(name)) {
        /* Never had start links: start late in the multi-user runlevels. */
        for (int rl = 3; rl <= 5; rl++) {
            snprintf(to, sizeof to, "/etc/rc.d/rc%d.d/S80%s", rl, name);
            snprintf(from, sizeof from, "../init.d/%s", name);
            if (symlink(from, to) != 0 && errno != EEXIST) return -1;
        }
    }
    return 0;
}

static void read_config(struct config *c) {
    memset(c, 0, sizeof *c);
    first_line("/etc/hostname", c->hostname, sizeof c->hostname);
    char tz[PATH_MAX];
    ssize_t n = readlink("/etc/localtime", tz, sizeof tz - 1);
    if (n > 0) {
        tz[n] = 0;
        char *p = strstr(tz, "zoneinfo/");
        copy(c->timezone, sizeof c->timezone, p ? p + 9 : tz);
    } else {
        copy(c->timezone, sizeof c->timezone, exists("/etc/localtime") ? "(custom file)" : "UTC");
    }
    read_lang(c->lang, sizeof c->lang);
    if (!shell_var("/etc/sysconfig/console", "KEYMAP", c->keymap, sizeof c->keymap))
        copy(c->keymap, sizeof c->keymap, "us");
    c->xkb[0] = 0;
    char *xk = slurp("/etc/X11/xorg.conf.d/00-keyboard.conf");
    if (xk) {
        char *p = strstr(xk, "\"XkbLayout\"");
        if (p && sscanf(p, "\"XkbLayout\" \"%63[^\"]\"", c->xkb) != 1) c->xkb[0] = 0;
        free(xk);
    }
    if (!c->xkb[0]) copy(c->xkb, sizeof c->xkb, "us");
    read_network(c);
    read_dns(c->dns, sizeof c->dns);
    read_bootto(c->bootto, sizeof c->bootto);
    read_autologin(c->autologin, sizeof c->autologin);
    c->winexe = has_winexe() && service_enabled("pe-binfmt");
}

/* Regular users: uid 1000-59999 */
static void read_users(struct list *out) {
    FILE *f = fopen("/etc/passwd", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *name = strtok(line, ":");
        strtok(NULL, ":");
        char *uid = strtok(NULL, ":");
        if (!name || !uid) continue;
        long u = atol(uid);
        if (u >= 1000 && u < 60000) list_add(out, name);
    }
    if (f) fclose(f);
}

/* Users as they will be after the pending operations. */
static void users_after_ops(struct list *out) {
    read_users(out);
    for (int i = 0; i < nops; i++) {
        int k = list_find(out, ops[i].name);
        if (ops[i].kind == 'a' && k < 0) list_add(out, ops[i].name);
        if (ops[i].kind == 'd' && k >= 0) {
            free(out->v[k]);
            out->v[k] = out->v[--out->n];
        }
    }
    list_sort_uniq(out);
}

static int user_is_admin(const char *name) {
    for (int i = nops - 1; i >= 0; i--)
        if (ops[i].kind == 'a' && !strcmp(ops[i].name, name)) return ops[i].admin;
    FILE *f = fopen("/etc/group", "r");
    char line[1024];
    int admin = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "wheel:", 6)) continue;
        chomp(line);
        char *members = strrchr(line, ':');
        if (!members) break;
        for (char *m = strtok(members + 1, ","); m; m = strtok(NULL, ","))
            if (!strcmp(m, name)) admin = 1;
    }
    if (f) fclose(f);
    return admin;
}

/* -------------------------------------------------------- choice sources */

static void load_timezones(struct list *l) {
    FILE *f = fopen(ZONEINFO "zone1970.tab", "r");
    if (!f) f = fopen(ZONEINFO "zone.tab", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *save;
        strtok_r(line, "\t", &save);
        strtok_r(NULL, "\t", &save);
        char *tz = strtok_r(NULL, "\t\n", &save);
        if (tz) list_add(l, tz);
    }
    if (f) fclose(f);
    list_add(l, "UTC");
    list_sort_uniq(l);
}

static void load_locales(struct list *l) {
    FILE *f = fopen("/usr/share/i18n/SUPPORTED", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char name[128], cs[64];
        if (sscanf(line, "%127s %63s", name, cs) != 2 || strcmp(cs, "UTF-8")) continue;
        list_add(l, name);
    }
    if (f) fclose(f);
    if (!l->n) { list_add(l, "en_US.UTF-8"); list_add(l, "C.UTF-8"); }
    list_sort_uniq(l);
}

static void walk_keymaps(const char *dir, struct list *l, int depth) {
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[PATH_MAX];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4 && strcmp(e->d_name, "include")) walk_keymaps(path, l, depth + 1);
            continue;
        }
        char *dot = strstr(e->d_name, ".map");
        if (!dot) continue;
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)(dot - e->d_name), e->d_name);
        list_add(l, name);
    }
    if (d) closedir(d);
}

static void load_keymaps(struct list *l) {
    walk_keymaps("/usr/share/keymaps", l, 0);
    if (!l->n) list_add(l, "us");
    list_sort_uniq(l);
}

/* XKB layouts from base.lst, shown as "code  description"; returns codes in parallel list. */
static void load_xkb(struct list *shown, struct list *codes) {
    FILE *f = fopen("/usr/share/X11/xkb/rules/base.lst", "r");
    char line[512];
    int in = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '!') { in = !strncmp(line, "! layout", 8); continue; }
        if (!in) continue;
        char code[64], desc[256];
        if (sscanf(line, " %63s %255[^\n]", code, desc) != 2) continue;
        char row[340];
        snprintf(row, sizeof row, "%-8s %s", code, desc);
        list_add(shown, row);
        list_add(codes, code);
    }
    if (f) fclose(f);
    if (!codes->n) { list_add(shown, "us       English (US)"); list_add(codes, "us"); }
}

/* ------------------------------------------------------------------- UI */

static int ui_active;

static void ui_start(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);   /* title bar */
        init_pair(2, COLOR_BLACK, COLOR_CYAN);   /* selection */
        init_pair(3, COLOR_YELLOW, -1);          /* changed marker */
        init_pair(4, COLOR_CYAN, -1);            /* values */
        init_pair(5, COLOR_RED, -1);             /* warnings */
        init_pair(6, COLOR_GREEN, -1);           /* ok */
    }
    ui_active = 1;
}

static void ui_stop(void) {
    if (ui_active) endwin();
    ui_active = 0;
}

/* Leave curses to run something with visible output, then come back. */
static void ui_suspend(void) { def_prog_mode(); endwin(); }

static void ui_resume(void) {
    printf("\nPress Enter to go back to lfs-setup...");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
    reset_prog_mode();
    refresh();
}

static void bar(int y, const char *left, const char *right) {
    attron(COLOR_PAIR(1) | A_BOLD);
    mvhline(y, 0, ' ', COLS);
    mvprintw(y, 1, "%s", left);
    if (right) mvprintw(y, COLS - (int)strlen(right) - 1, "%s", right);
    attroff(COLOR_PAIR(1) | A_BOLD);
}

static WINDOW *dialog(int h, int w, const char *title) {
    if (w > COLS - 2) w = COLS - 2;
    if (h > LINES - 2) h = LINES - 2;
    WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    keypad(win, TRUE);
    box(win, 0, 0);
    wattron(win, A_BOLD);
    mvwprintw(win, 0, 2, " %s ", title);
    wattroff(win, A_BOLD);
    return win;
}

static void close_dialog(WINDOW *w) {
    delwin(w);
    touchwin(stdscr);
    refresh();
}

/* Multi-line message; lines separated by \n. */
static void message(const char *title, const char *text) {
    int lines = 1, width = (int)strlen(title) + 6;
    for (const char *p = text, *s = text;; p++) {
        if (*p == '\n' || !*p) {
            if (p - s + 4 > width) width = (int)(p - s) + 4;
            if (!*p) break;
            lines++;
            s = p + 1;
        }
    }
    WINDOW *w = dialog(lines + 4, width < 30 ? 30 : width, title);
    int y = 1;
    const char *s = text;
    for (const char *p = text;; p++) {
        if (*p == '\n' || !*p) {
            mvwprintw(w, y++, 2, "%.*s", (int)(p - s), s);
            if (!*p) break;
            s = p + 1;
        }
    }
    wattron(w, A_REVERSE);
    mvwprintw(w, lines + 2, (getmaxx(w) - 6) / 2, "  OK  ");
    wattroff(w, A_REVERSE);
    wrefresh(w);
    int c;
    while ((c = wgetch(w)) != '\n' && c != ' ' && c != 27 && c != KEY_ENTER) {}
    close_dialog(w);
}

static int confirm(const char *title, const char *text) {
    int width = (int)strlen(text) + 6;
    if (width < 36) width = 36;
    WINDOW *w = dialog(7, width, title);
    int sel = 1; /* default: No */
    for (;;) {
        mvwprintw(w, 2, 3, "%s", text);
        mvwprintw(w, 4, width / 2 - 10, "%s", "");
        if (sel == 0) wattron(w, A_REVERSE);
        mvwprintw(w, 4, width / 2 - 10, "  Yes  ");
        wattroff(w, A_REVERSE);
        if (sel == 1) wattron(w, A_REVERSE);
        mvwprintw(w, 4, width / 2 + 3, "  No  ");
        wattroff(w, A_REVERSE);
        wrefresh(w);
        int c = wgetch(w);
        if (c == KEY_LEFT || c == KEY_RIGHT || c == '\t') sel ^= 1;
        else if (c == 'y' || c == 'Y') { sel = 0; break; }
        else if (c == 'n' || c == 'N' || c == 27) { sel = 1; break; }
        else if (c == '\n' || c == KEY_ENTER) break;
    }
    close_dialog(w);
    return sel == 0;
}

/* Text input; returns 1 on Enter, 0 on Esc. */
static int input(const char *title, const char *prompt, char *buf, size_t n, int hidden) {
    int width = (int)strlen(prompt) + 6;
    if (width < 50) width = 50;
    WINDOW *w = dialog(7, width, title);
    char tmp[256];
    copy(tmp, sizeof tmp, hidden ? "" : buf);
    size_t len = strlen(tmp), max = (n < sizeof tmp ? n : sizeof tmp) - 1;
    int field = width - 6, ok = 0;
    curs_set(1);
    for (;;) {
        mvwprintw(w, 1, 2, "%s", prompt);
        wattron(w, A_UNDERLINE);
        mvwhline(w, 3, 3, ' ', field);
        size_t start = len > (size_t)field - 1 ? len - (size_t)field + 1 : 0;
        for (size_t i = start; i < len; i++)
            mvwaddch(w, 3, 3 + (int)(i - start), hidden ? '*' : (unsigned char)tmp[i]);
        wattroff(w, A_UNDERLINE);
        mvwprintw(w, 5, 2, "Enter = OK   Esc = cancel");
        wmove(w, 3, 3 + (int)(len - start));
        wrefresh(w);
        int c = wgetch(w);
        if (c == '\n' || c == KEY_ENTER) { ok = 1; break; }
        if (c == 27) break;
        if ((c == KEY_BACKSPACE || c == 127 || c == 8) && len) tmp[--len] = 0;
        else if (c == 21) { len = 0; tmp[0] = 0; } /* Ctrl-U */
        else if (c >= 32 && c < 127 && len < max) { tmp[len++] = (char)c; tmp[len] = 0; }
    }
    curs_set(0);
    close_dialog(w);
    if (ok) copy(buf, n, tmp);
    memset(tmp, 0, sizeof tmp);
    return ok;
}

/* Scrollable list with type-to-filter. Returns the chosen index or -1. */
static int pick(const char *title, struct list *items, const char *current) {
    if (!items->n) { message(title, "Nothing to choose from."); return -1; }
    int h = LINES - 4, w = COLS - 8;
    if (w > 72) w = 72;
    WINDOW *win = dialog(h, w, title);
    char filter[64] = "";
    int *idx = malloc((size_t)items->n * sizeof *idx);
    int sel = 0, top = 0, result = -1, first = 1;
    int rows = h - 5;
    for (;;) {
        int m = 0;
        for (int i = 0; i < items->n; i++)
            if (!filter[0] || strcasestr(items->v[i], filter)) idx[m++] = i;
        if (first) {
            for (int i = 0; current && i < m; i++)
                if (!strcmp(items->v[idx[i]], current)) sel = i;
            first = 0;
        }
        if (sel >= m) sel = m ? m - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD);
        mvwprintw(win, 0, 2, " %s ", title);
        wattroff(win, A_BOLD);
        mvwprintw(win, 1, 2, "Search: %s_", filter);
        for (int r = 0; r < rows && top + r < m; r++) {
            const char *s = items->v[idx[top + r]];
            if (top + r == sel) wattron(win, COLOR_PAIR(2));
            mvwhline(win, 3 + r, 1, ' ', w - 2);
            mvwprintw(win, 3 + r, 2, "%.*s", w - 4, s);
            if (top + r == sel) wattroff(win, COLOR_PAIR(2));
        }
        if (!m) mvwprintw(win, 3, 2, "(no match)");
        mvwprintw(win, h - 1, 2, " type to search  Enter = choose  Esc = cancel  %d/%d ", m ? sel + 1 : 0, m);
        wrefresh(win);
        int c = wgetch(win);
        if (c == KEY_UP && sel > 0) sel--;
        else if (c == KEY_DOWN && sel < m - 1) sel++;
        else if (c == KEY_PPAGE) sel = sel > rows ? sel - rows : 0;
        else if (c == KEY_NPAGE) sel = sel + rows < m ? sel + rows : (m ? m - 1 : 0);
        else if (c == KEY_HOME) sel = 0;
        else if (c == KEY_END) sel = m ? m - 1 : 0;
        else if (c == '\n' || c == KEY_ENTER) { if (m) { result = idx[sel]; break; } }
        else if (c == 27) break;
        else if ((c == KEY_BACKSPACE || c == 127 || c == 8) && filter[0]) filter[strlen(filter) - 1] = 0;
        else if (c >= 32 && c < 127 && strlen(filter) < sizeof filter - 1) {
            size_t l = strlen(filter);
            filter[l] = (char)c; filter[l + 1] = 0;
            sel = top = 0;
        }
    }
    free(idx);
    close_dialog(win);
    return result;
}

/* Pick from a fixed set of strings. */
static int pick_of(const char *title, const char *current, int n, const char **opts) {
    struct list l = {0};
    for (int i = 0; i < n; i++) list_add(&l, opts[i]);
    int r = pick(title, &l, current);
    list_free(&l);
    return r;
}

/* Password typed twice. */
static int ask_password(const char *title, const char *who, char *out, size_t n) {
    char a[128] = "", b[128] = "", prompt[128];
    snprintf(prompt, sizeof prompt, "New password for %s:", who);
    if (!input(title, prompt, a, sizeof a, 1)) return 0;
    if (!valid_password(a)) { message(title, "The password can't be empty or contain ':'."); return 0; }
    if (!input(title, "Type it again:", b, sizeof b, 1)) { memset(a, 0, sizeof a); return 0; }
    int same = !strcmp(a, b);
    if (same) copy(out, n, a);
    else message(title, "The passwords didn't match. Nothing was changed.");
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    return same;
}

/* ------------------------------------------------------------- menu items */

static void edit_hostname(void) {
    char buf[64];
    copy(buf, sizeof buf, want.hostname);
    if (!input("Hostname", "Name of this computer (letters, digits, -):", buf, sizeof buf, 0)) return;
    if (!valid_hostname(buf)) { message("Hostname", "Use 1-63 letters, digits or '-' (not at the start or end)."); return; }
    copy(want.hostname, sizeof want.hostname, buf);
}

static void edit_timezone(void) {
    struct list l = {0};
    load_timezones(&l);
    int i = pick("Timezone", &l, want.timezone);
    if (i >= 0) copy(want.timezone, sizeof want.timezone, l.v[i]);
    list_free(&l);
}

static void edit_lang(void) {
    struct list l = {0};
    load_locales(&l);
    int i = pick("Language (locale)", &l, want.lang);
    if (i >= 0) copy(want.lang, sizeof want.lang, l.v[i]);
    list_free(&l);
}

static void edit_keymap(void) {
    struct list l = {0};
    load_keymaps(&l);
    int i = pick("Console keyboard layout", &l, want.keymap);
    if (i >= 0) copy(want.keymap, sizeof want.keymap, l.v[i]);
    list_free(&l);
}

static void edit_xkb(void) {
    struct list shown = {0}, codes = {0};
    load_xkb(&shown, &codes);
    int cur_i = list_find(&codes, want.xkb);
    int i = pick("Desktop keyboard layout", &shown, cur_i >= 0 ? shown.v[cur_i] : NULL);
    if (i >= 0) copy(want.xkb, sizeof want.xkb, codes.v[i]);
    list_free(&shown);
    list_free(&codes);
}

static void edit_network(void) {
    /* Interface */
    struct list ifs = {0};
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.' && strcmp(e->d_name, "lo")) list_add(&ifs, e->d_name);
    if (d) closedir(d);
    if (list_find(&ifs, want.iface) < 0) list_add(&ifs, want.iface);
    list_sort_uniq(&ifs);
    if (ifs.n > 1) {
        int i = pick("Network interface", &ifs, want.iface);
        if (i < 0) { list_free(&ifs); return; }
        copy(want.iface, sizeof want.iface, ifs.v[i]);
    }
    list_free(&ifs);

    /* Mode */
    const char *modes[] = {"Automatic (DHCP)", "Manual (static address)"};
    if (has_dhcpcd()) {
        int m = pick_of("How to get an address", strcmp(want.netmode, "dhcp") ? modes[1] : modes[0], 2, modes);
        if (m < 0) return;
        copy(want.netmode, sizeof want.netmode, m == 0 ? "dhcp" : "static");
    } else {
        copy(want.netmode, sizeof want.netmode, "static");
    }
    if (!strcmp(want.netmode, "dhcp")) return;

    char ip[64], pfx[8], gw[64];
    copy(ip, sizeof ip, want.ip[0] ? want.ip : "10.0.2.15");
    copy(pfx, sizeof pfx, want.prefix[0] ? want.prefix : "24");
    copy(gw, sizeof gw, want.gateway[0] ? want.gateway : "10.0.2.2");
    if (!input("Static address", "IP address (e.g. 192.168.1.50):", ip, sizeof ip, 0)) return;
    if (!valid_ipv4(ip)) { message("Static address", "That isn't an IPv4 address."); return; }
    if (!input("Static address", "Prefix length (24 = 255.255.255.0):", pfx, sizeof pfx, 0)) return;
    if (!valid_prefix(pfx)) { message("Static address", "The prefix must be a number from 1 to 32."); return; }
    if (!input("Static address", "Gateway (your router):", gw, sizeof gw, 0)) return;
    if (!valid_ipv4(gw)) { message("Static address", "That isn't an IPv4 address."); return; }
    copy(want.ip, sizeof want.ip, ip);
    copy(want.prefix, sizeof want.prefix, pfx);
    copy(want.gateway, sizeof want.gateway, gw);
}

static void edit_dns(void) {
    char buf[64];
    copy(buf, sizeof buf, want.dns[0] ? want.dns : "1.1.1.1");
    if (!input("DNS server", "DNS server address (e.g. 1.1.1.1 or your router):", buf, sizeof buf, 0)) return;
    if (!valid_ipv4(buf)) { message("DNS server", "That isn't an IPv4 address."); return; }
    copy(want.dns, sizeof want.dns, buf);
}

static void edit_root_pw(void) {
    char pw[128];
    if (ask_password("Root password", "root", pw, sizeof pw)) copy(root_pw, sizeof root_pw, pw);
    memset(pw, 0, sizeof pw);
}

static void add_op(char kind, const char *name, const char *pw, int admin) {
    if (nops >= MAX_OPS) { message("Users", "Too many pending changes; apply them first."); return; }
    struct user_op *o = &ops[nops++];
    o->kind = kind;
    copy(o->name, sizeof o->name, name);
    copy(o->pw, sizeof o->pw, pw);
    o->admin = admin;
}

static void edit_users(void) {
    for (;;) {
        struct list users = {0}, rows = {0};
        users_after_ops(&users);
        list_add(&rows, "+ Add a user");
        for (int i = 0; i < users.n; i++) {
            char row[96];
            snprintf(row, sizeof row, "  %-20s %s", users.v[i], user_is_admin(users.v[i]) ? "administrator" : "");
            list_add(&rows, row);
        }
        int r = pick("Users", &rows, NULL);
        if (r < 0) { list_free(&users); list_free(&rows); return; }
        if (r == 0) {
            char name[33] = "", pw[128];
            if (input("Add a user", "User name (lowercase, e.g. alex):", name, sizeof name, 0)) {
                if (!valid_username(name)) message("Add a user", "Use lowercase letters, digits, '-' or '_', starting with a letter.");
                else if (list_find(&users, name) >= 0) message("Add a user", "That user already exists.");
                else if (ask_password("Add a user", name, pw, sizeof pw)) {
                    int admin = confirm("Add a user", "Make this user an administrator (can use sudo)?");
                    add_op('a', name, pw, admin);
                    memset(pw, 0, sizeof pw);
                }
            }
        } else {
            const char *name = users.v[r - 1];
            const char *acts[] = {"Change password", "Delete this user (and home folder)", "Back"};
            int a = pick_of(name, NULL, 3, acts);
            char pw[128];
            if (a == 0 && ask_password("Change password", name, pw, sizeof pw)) {
                add_op('p', name, pw, 0);
                memset(pw, 0, sizeof pw);
            } else if (a == 1) {
                char q[96];
                snprintf(q, sizeof q, "Delete user %s and their files?", name);
                if (confirm("Delete user", q)) {
                    add_op('d', name, "", 0);
                    if (!strcmp(want.autologin, name)) want.autologin[0] = 0;
                }
            }
        }
        list_free(&users);
        list_free(&rows);
    }
}

static void edit_bootto(void) {
    const char *opts[] = {"Desktop (KDE Plasma login screen)", "Text console"};
    int i = pick_of("Start the computer into", strcmp(want.bootto, "graphical") ? opts[1] : opts[0], 2, opts);
    if (i >= 0) copy(want.bootto, sizeof want.bootto, i == 0 ? "graphical" : "console");
}

static void edit_autologin(void) {
    struct list users = {0}, rows = {0};
    users_after_ops(&users);
    if (!users.n) {
        message("Automatic login", "Add a user first (root can't log in to Plasma).");
        list_free(&users);
        return;
    }
    list_add(&rows, "Off (show the login screen)");
    for (int i = 0; i < users.n; i++) list_add(&rows, users.v[i]);
    int i = pick("Log in automatically as", &rows, want.autologin[0] ? want.autologin : rows.v[0]);
    if (i == 0) want.autologin[0] = 0;
    else if (i > 0) copy(want.autologin, sizeof want.autologin, users.v[i - 1]);
    list_free(&users);
    list_free(&rows);
}

static void edit_services(void) {
    if (!nsvcs) { message("Services", "No optional services found."); return; }
    int h = LINES - 4, w = 60, sel = 0, top = 0, rows = h - 4;
    WINDOW *win = dialog(h, w, "Services started at boot");
    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD);
        mvwprintw(win, 0, 2, " Services started at boot ");
        wattroff(win, A_BOLD);
        for (int r = 0; r < rows && top + r < nsvcs; r++) {
            struct service *s = &svcs[top + r];
            if (top + r == sel) wattron(win, COLOR_PAIR(2));
            mvwhline(win, 2 + r, 1, ' ', w - 2);
            mvwprintw(win, 2 + r, 2, "[%c] %-30s %s", s->want ? 'x' : ' ', s->name,
                      s->want != s->now ? "*" : "");
            if (top + r == sel) wattroff(win, COLOR_PAIR(2));
        }
        mvwprintw(win, h - 1, 2, " Space = on/off   Enter/Esc = done ");
        wrefresh(win);
        int c = wgetch(win);
        if (c == KEY_UP && sel > 0) sel--;
        else if (c == KEY_DOWN && sel < nsvcs - 1) sel++;
        else if (c == ' ') svcs[sel].want = !svcs[sel].want;
        else if (c == '\n' || c == KEY_ENTER || c == 27 || c == 'q') break;
    }
    close_dialog(win);
}

static void edit_winexe(void) {
    const char *opts[] = {"On: run .exe files directly (pe-loader)", "Off"};
    int i = pick_of("Windows programs", want.winexe ? opts[0] : opts[1], 2, opts);
    if (i >= 0) want.winexe = (i == 0);
}

/* -------------------------------------------------------------- applying */

static struct list results;

static void note(int ok, const char *fmt, ...) {
    char buf[256], line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%s %s", ok ? "[ ok ]" : "[FAIL]", buf);
    list_add(&results, line);
}

static void apply_hostname(void) {
    char hosts[1024], name[80];
    snprintf(name, sizeof name, "%s\n", want.hostname);
    snprintf(hosts, sizeof hosts,
             "# Begin /etc/hosts (written by lfs-setup)\n\n"
             "127.0.0.1 localhost.localdomain localhost\n"
             "127.0.1.1 %s.localdomain %s\n"
             "::1       localhost ip6-localhost ip6-loopback\n"
             "ff02::1   ip6-allnodes\n"
             "ff02::2   ip6-allrouters\n\n# End /etc/hosts\n",
             want.hostname, want.hostname);
    int ok = spew("/etc/hostname", name, 0644) == 0 && spew("/etc/hosts", hosts, 0644) == 0;
    if (ok) RUN("hostname", want.hostname);
    note(ok, "hostname set to %s", want.hostname);
}

static void apply_timezone(void) {
    char target[PATH_MAX];
    snprintf(target, sizeof target, ZONEINFO "%s", want.timezone);
    int ok = exists(target);
    if (ok) {
        unlink("/etc/localtime.lfs-setup");
        ok = symlink(target, "/etc/localtime.lfs-setup") == 0 &&
             rename("/etc/localtime.lfs-setup", "/etc/localtime") == 0;
    }
    note(ok, "timezone set to %s", want.timezone);
}

static int locale_available(const char *lang) {
    /* "en_US.UTF-8" is listed by locale -a as "en_US.utf8" */
    char norm[64], line[128];
    copy(norm, sizeof norm, lang);
    char *dot = strchr(norm, '.');
    if (dot) copy(dot, sizeof norm - (size_t)(dot - norm), ".utf8");
    FILE *p = popen("locale -a 2>/dev/null", "r");
    int found = 0;
    while (p && fgets(line, sizeof line, p)) {
        chomp(line);
        if (!strcasecmp(line, norm) || !strcasecmp(line, lang)) found = 1;
    }
    if (p) pclose(p);
    return found;
}

static void apply_lang(void) {
    if (!locale_available(want.lang)) {
        char base[64];
        copy(base, sizeof base, want.lang);
        char *dot = strchr(base, '.');
        if (dot) *dot = 0;
        if (RUN("localedef", "-i", base, "-f", "UTF-8", want.lang) != 0) {
            note(0, "could not create the locale %s (localedef failed)", want.lang);
            return;
        }
    }
    const char *path = lang_file();
    char *old = slurp(path);
    if (!old) { note(0, "could not read %s", path); return; }
    size_t cap = strlen(old) + 256;
    char *out = malloc(cap);
    out[0] = 0;
    int replaced = 0;
    char *p = old;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)len, p);
        char *k = strstr(line, "LANG=");
        if (k && !strstr(line, "LANG=C.UTF-8") && !(k > line && (isalnum((unsigned char)k[-1]) || k[-1] == '_'))) {
            int indent = (int)strspn(line, " \t");
            char repl[256];
            snprintf(repl, sizeof repl, "%.*sexport LANG=%s\n", indent, line, want.lang);
            strcat(out, repl);
            replaced = 1;
        } else {
            strncat(out, p, len);
            strcat(out, "\n");
        }
        p += len + (nl ? 1 : 0);
    }
    if (!replaced) {
        strcat(out, "export LANG=");
        strcat(out, want.lang);
        strcat(out, "\n");
    }
    int ok = spew(path, out, 0644) == 0;
    free(old);
    free(out);
    note(ok, "language set to %s (in %s; takes effect at next login)", want.lang, path);
}

static void apply_keymap(void) {
    int ok = set_shell_var("/etc/sysconfig/console", "KEYMAP", want.keymap) == 0;
    if (ok) RUN("loadkeys", want.keymap);
    note(ok, "console keyboard set to %s", want.keymap);
}

static void apply_xkb(void) {
    char conf[512], kx[256];
    mkdirs("/etc/X11/xorg.conf.d");
    snprintf(conf, sizeof conf,
             "# Written by lfs-setup\n"
             "Section \"InputClass\"\n"
             "    Identifier \"system-keyboard\"\n"
             "    MatchIsKeyboard \"on\"\n"
             "    Option \"XkbLayout\" \"%s\"\n"
             "EndSection\n", want.xkb);
    int ok = spew("/etc/X11/xorg.conf.d/00-keyboard.conf", conf, 0644) == 0;
    /* Plasma (also on Wayland) reads kxkbrc; new users get it from /etc/skel. */
    mkdirs("/etc/skel/.config");
    snprintf(kx, sizeof kx, "[Layout]\nLayoutList=%s\nUse=true\n", want.xkb);
    ok = ok && spew("/etc/skel/.config/kxkbrc", kx, 0644) == 0;
    note(ok, "desktop keyboard set to %s (new users; existing users: System Settings > Keyboard)", want.xkb);
}

static void apply_network(void) {
    char path[PATH_MAX], text[512];
    /* One configured interface: remove other ifconfig.* files we'd conflict with. */
    DIR *d = opendir("/etc/sysconfig");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "ifconfig.", 9) || !strcmp(e->d_name + 9, want.iface)) continue;
        snprintf(path, sizeof path, "/etc/sysconfig/%s", e->d_name);
        unlink(path);
    }
    if (d) closedir(d);
    snprintf(path, sizeof path, "/etc/sysconfig/ifconfig.%s", want.iface);
    if (!strcmp(want.netmode, "dhcp"))
        snprintf(text, sizeof text,
                 "ONBOOT=\"yes\"\nIFACE=\"%s\"\nSERVICE=\"dhcpcd\"\n"
                 "DHCP_START=\"-b -q\"\nDHCP_STOP=\"-k\"\n", want.iface);
    else
        snprintf(text, sizeof text,
                 "ONBOOT=yes\nIFACE=%s\nSERVICE=ipv4-static\nIP=%s\nGATEWAY=%s\nPREFIX=%s\n",
                 want.iface, want.ip, want.gateway, want.prefix);
    int ok = spew(path, text, 0644) == 0;
    if (!strcmp(want.netmode, "dhcp") && !exists("/lib/services/dhcpcd") && !exists("/usr/lib/services/dhcpcd"))
        note(0, "DHCP needs the dhcpcd service script from blfs-bootscripts (make install-service-dhcpcd)");
    if (!strcmp(want.netmode, "dhcp"))
        note(ok, "network: %s uses DHCP (restart networking or reboot)", want.iface);
    else
        note(ok, "network: %s = %s/%s via %s (restart networking or reboot)", want.iface, want.ip, want.prefix, want.gateway);
}

static void apply_dns(void) {
    char text[256];
    snprintf(text, sizeof text, "# Begin /etc/resolv.conf (written by lfs-setup)\n\nnameserver %s\n\n# End /etc/resolv.conf\n", want.dns);
    note(spew("/etc/resolv.conf", text, 0644) == 0, "DNS server set to %s", want.dns);
}

static void apply_bootto(void) {
    char *t = slurp("/etc/inittab");
    if (!t) { note(0, "could not read /etc/inittab"); return; }
    char *p = strstr(t, ":initdefault:");
    int ok = 0;
    if (p && p - t >= 2 && isdigit((unsigned char)p[-1]) && p[-2] == ':') {
        p[-1] = !strcmp(want.bootto, "graphical") ? '5' : '3';
        ok = spew("/etc/inittab", t, 0644) == 0;
    }
    free(t);
    note(ok, "start into: %s (from next boot)", !strcmp(want.bootto, "graphical") ? "desktop" : "text console");
}

static void apply_autologin(void) {
    int ok;
    if (!want.autologin[0]) {
        ok = unlink(AUTOLOGIN_CONF) == 0 || errno == ENOENT;
        note(ok, "automatic login turned off");
        return;
    }
    const char *session = exists("/usr/share/wayland-sessions/plasma.desktop") ? "plasma" : "plasmax11";
    char text[256];
    snprintf(text, sizeof text, "# Written by lfs-setup\n[Autologin]\nUser=%s\nSession=%s\n", want.autologin, session);
    mkdirs("/etc/sddm.conf.d");
    ok = spew(AUTOLOGIN_CONF, text, 0644) == 0;
    note(ok, "automatic login as %s", want.autologin);
}

static void apply_users(void) {
    int sudo = find_prog("sudo") != NULL;
    for (int i = 0; i < nops; i++) {
        struct user_op *o = &ops[i];
        char line[200];
        if (o->kind == 'a') {
            char groups[64];
            snprintf(groups, sizeof groups, "users,audio,video,input%s", o->admin ? ",wheel" : "");
            int ok = RUN("useradd", "-m", "-s", "/bin/bash", "-G", groups, o->name) == 0;
            snprintf(line, sizeof line, "%s:%s\n", o->name, o->pw);
            ok = ok && run((char *const[]){"chpasswd", NULL}, line) == 0;
            note(ok, "added user %s%s", o->name, o->admin ? " (administrator)" : "");
            if (ok && o->admin && sudo && !exists("/etc/sudoers.d/10-lfs-setup-wheel")) {
                mkdirs("/etc/sudoers.d");
                spew("/etc/sudoers.d/10-lfs-setup-wheel", "%wheel ALL=(ALL:ALL) ALL\n", 0440);
            }
        } else if (o->kind == 'p') {
            snprintf(line, sizeof line, "%s:%s\n", o->name, o->pw);
            note(run((char *const[]){"chpasswd", NULL}, line) == 0, "changed the password of %s", o->name);
        } else if (o->kind == 'd') {
            note(RUN("userdel", "-r", o->name) == 0, "deleted user %s", o->name);
        }
        memset(line, 0, sizeof line);
        memset(o->pw, 0, sizeof o->pw);
    }
    nops = 0;
}

static void apply_root_pw(void) {
    char line[160];
    snprintf(line, sizeof line, "root:%s\n", root_pw);
    note(run((char *const[]){"chpasswd", NULL}, line) == 0, "changed the root password");
    memset(line, 0, sizeof line);
    memset(root_pw, 0, sizeof root_pw);
}

static int pending(void) {
    if (memcmp(&cur, &want, sizeof cur) || root_pw[0] || nops) return 1;
    for (int i = 0; i < nsvcs; i++)
        if (svcs[i].now != svcs[i].want) return 1;
    return 0;
}

static void reload_state(void) {
    read_config(&cur);
    want = cur;
    read_services();
}

static void apply_all(int quiet) {
    list_free(&results);
    if (strcmp(cur.hostname, want.hostname)) apply_hostname();
    if (strcmp(cur.timezone, want.timezone)) apply_timezone();
    if (strcmp(cur.lang, want.lang)) apply_lang();
    if (strcmp(cur.keymap, want.keymap)) apply_keymap();
    if (strcmp(cur.xkb, want.xkb)) apply_xkb();
    if (strcmp(cur.iface, want.iface) || strcmp(cur.netmode, want.netmode) || strcmp(cur.ip, want.ip) ||
        strcmp(cur.prefix, want.prefix) || strcmp(cur.gateway, want.gateway))
        apply_network();
    if (strcmp(cur.dns, want.dns)) apply_dns();
    if (root_pw[0]) apply_root_pw();
    if (nops) apply_users();
    if (strcmp(cur.bootto, want.bootto)) apply_bootto();
    if (strcmp(cur.autologin, want.autologin)) apply_autologin();
    if (cur.winexe != want.winexe) {
        note(set_service("pe-binfmt", want.winexe) == 0, "Windows programs %s (from next boot)", want.winexe ? "on" : "off");
        RUN("/etc/rc.d/init.d/pe-binfmt", want.winexe ? "start" : "stop");
    }
    for (int i = 0; i < nsvcs; i++)
        if (svcs[i].now != svcs[i].want)
            note(set_service(svcs[i].name, svcs[i].want) == 0, "service %s %s at boot", svcs[i].name,
                 svcs[i].want ? "enabled" : "disabled");
    reload_state();
    if (quiet) return;
    if (!results.n) { message("Apply", "There was nothing to change."); return; }
    size_t len = 1;
    for (int i = 0; i < results.n; i++) len += strlen(results.v[i]) + 1;
    char *text = malloc(len);
    text[0] = 0;
    for (int i = 0; i < results.n; i++) {
        strcat(text, results.v[i]);
        if (i < results.n - 1) strcat(text, "\n");
    }
    message("Changes applied", text);
    free(text);
}

/* --------------------------------------------------------- install to disk */

struct disk { char name[32]; char size[16]; char model[64]; };

static void human(unsigned long long bytes, char *out, size_t n) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1000 && i < 4) { v /= 1000; i++; }
    snprintf(out, n, "%.1f %s", v, u[i]);
}

static int list_disks(struct disk *out, int max) {
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)) && n < max) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || !strncmp(nm, "loop", 4) || !strncmp(nm, "ram", 3) ||
            !strncmp(nm, "zram", 4) || !strncmp(nm, "sr", 2) || !strncmp(nm, "dm-", 3) || !strncmp(nm, "fd", 2))
            continue;
        char path[PATH_MAX], buf[128];
        snprintf(path, sizeof path, "/sys/block/%s/size", nm);
        first_line(path, buf, sizeof buf);
        unsigned long long sectors = strtoull(buf, NULL, 10);
        if (!sectors) continue;
        copy(out[n].name, sizeof out[n].name, nm);
        human(sectors * 512ULL, out[n].size, sizeof out[n].size);
        snprintf(path, sizeof path, "/sys/block/%s/device/model", nm);
        first_line(path, out[n].model, sizeof out[n].model);
        n++;
    }
    if (d) closedir(d);
    return n;
}

/* Is a partition of this disk mounted (e.g. the system we're running from)? */
static int disk_in_use(const char *name) {
    char *m = slurp("/proc/mounts");
    char needle[48];
    snprintf(needle, sizeof needle, "/dev/%s", name);
    int used = m && strstr(m, needle) != NULL;
    free(m);
    return used;
}

static int sh(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("\033[1;34m$ %s\033[0m\n", cmd);
    fflush(stdout);
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void install_to_disk(void) {
    struct disk disks[32];
    int n = list_disks(disks, 32);
    if (!n) { message("Install to disk", "No disks found."); return; }
    struct list rows = {0};
    for (int i = 0; i < n; i++) {
        char row[128];
        snprintf(row, sizeof row, "/dev/%-10s %10s  %s%s", disks[i].name, disks[i].size, disks[i].model,
                 disk_in_use(disks[i].name) ? "  (in use)" : "");
        list_add(&rows, row);
    }
    int i = pick("Install to which disk? EVERYTHING ON IT WILL BE ERASED", &rows, NULL);
    list_free(&rows);
    if (i < 0) return;
    struct disk *dk = &disks[i];
    if (disk_in_use(dk->name)) {
        message("Install to disk", "That disk is in use (mounted). Choose another disk,\n"
                                   "or boot the live ISO to install over this system.");
        return;
    }
    if (!find_prog("sfdisk") || !find_prog("mkfs.ext4") || !find_prog("grub-install")) {
        message("Install to disk", "sfdisk, mkfs.ext4 and grub-install are needed.");
        return;
    }
    char typed[32] = "", prompt[128];
    snprintf(prompt, sizeof prompt, "All data on /dev/%s (%s) will be lost. Type %s to continue:", dk->name, dk->size, dk->name);
    if (!input("Install to disk", prompt, typed, sizeof typed, 0) || strcmp(typed, dk->name)) {
        message("Install to disk", "Cancelled. Nothing was changed.");
        return;
    }
    if (pending()) apply_all(1); /* the copy carries the settings */

    char part[48];
    int digit_end = isdigit((unsigned char)dk->name[strlen(dk->name) - 1]);
    snprintf(part, sizeof part, "/dev/%s%s1", dk->name, digit_end ? "p" : "");
    const char *mnt = "/mnt/lfs-install";
    int efi = exists("/sys/firmware/efi");

    ui_suspend();
    printf("\n=== Installing Linux From Scratch to /dev/%s ===\n\n", dk->name);
    if (efi)
        printf("Note: this PC started in UEFI mode. LFS's GRUB is built for BIOS booting, so\n"
               "enable CSM / Legacy boot in the firmware settings to start the installed system.\n\n");
    int ok =
        sh("printf 'label: dos\\n,,L,*\\n' | sfdisk --wipe always --wipe-partitions always /dev/%s", dk->name) == 0 &&
        (sh("udevadm settle 2>/dev/null; sleep 1; test -b %s", part) == 0) &&
        sh("mkfs.ext4 -F -q -L LFS %s", part) == 0 &&
        sh("mkdir -p %s && mount %s %s", mnt, part, mnt) == 0;
    if (ok) {
        printf("\nCopying the system (this takes a few minutes)...\n");
        ok = sh("cp -ax / %s/", mnt) == 0;
        /* Undo the live-ISO tweaks: root on disk needs fsck and no /init. */
        sh("rm -f %s/init %s/etc/lfs-live %s/dev/console %s/dev/null", mnt, mnt, mnt, mnt);
        sh("sed -i '/running from RAM/d' %s/etc/issue", mnt);
        sh("test -e %s/etc/rc.d/init.d/checkfs && ln -sf ../init.d/checkfs %s/etc/rc.d/rcS.d/S30checkfs || true", mnt, mnt);
        sh("rm -rf %s/boot/grub && mkdir -p %s/proc %s/sys %s/dev %s/run %s/tmp %s/mnt && chmod 1777 %s/tmp",
           mnt, mnt, mnt, mnt, mnt, mnt, mnt, mnt);
    }
    char partuuid[64] = "", uuid[64] = "", kernel[256] = "";
    if (ok) {
        capture((char *const[]){"blkid", "-s", "PARTUUID", "-o", "value", part, NULL}, partuuid, sizeof partuuid);
        capture((char *const[]){"blkid", "-s", "UUID", "-o", "value", part, NULL}, uuid, sizeof uuid);
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cd %s/boot 2>/dev/null && ls vmlinuz-* | sort | tail -1", mnt);
        FILE *p = popen(cmd, "r");
        if (p && fgets(kernel, sizeof kernel, p)) chomp(kernel);
        if (p) pclose(p);
        if (!partuuid[0] || !uuid[0] || !kernel[0]) {
            printf("Could not find the partition IDs or a kernel in /boot.\n");
            ok = 0;
        }
    }
    if (ok) {
        char path[PATH_MAX], text[2048];
        snprintf(path, sizeof path, "%s/etc/fstab", mnt);
        snprintf(text, sizeof text,
                 "# Begin /etc/fstab (written by lfs-setup)\n\n"
                 "PARTUUID=%s   /              ext4     defaults            1     1\n"
                 "proc                 /proc          proc     nosuid,noexec,nodev 0     0\n"
                 "sysfs                /sys           sysfs    nosuid,noexec,nodev 0     0\n"
                 "devpts               /dev/pts       devpts   gid=5,mode=620      0     0\n"
                 "tmpfs                /run           tmpfs    defaults            0     0\n"
                 "devtmpfs             /dev           devtmpfs mode=0755,nosuid    0     0\n"
                 "tmpfs                /dev/shm       tmpfs    nosuid,nodev        0     0\n"
                 "cgroup2              /sys/fs/cgroup cgroup2  nosuid,noexec,nodev 0     0\n\n"
                 "# End /etc/fstab\n", partuuid);
        ok = spew(path, text, 0644) == 0;
        ok = ok && sh("grub-install --target=i386-pc --boot-directory=%s/boot /dev/%s", mnt, dk->name) == 0;
        if (ok) {
            char ver[64] = "";
            first_line("/etc/lfs-release", ver, sizeof ver);
            snprintf(path, sizeof path, "%s/boot/grub/grub.cfg", mnt);
            snprintf(text, sizeof text,
                     "# Begin /boot/grub/grub.cfg (written by lfs-setup)\n"
                     "set default=0\nset timeout=5\n\n"
                     "insmod part_msdos\ninsmod ext2\n"
                     "search --no-floppy --fs-uuid --set=root %s\n\n"
                     "menuentry \"Linux From Scratch %s\" {\n"
                     "        linux   /boot/%s root=PARTUUID=%s ro net.ifnames=0\n"
                     "}\n", uuid, ver, kernel, partuuid);
            ok = spew(path, text, 0644) == 0;
        }
    }
    sh("sync; umount %s 2>/dev/null || true", mnt);
    if (ok)
        printf("\n\033[1;32mDone!\033[0m Linux From Scratch is installed on /dev/%s.\n"
               "Remove the ISO/USB stick and reboot to start it.\n", dk->name);
    else
        printf("\n\033[1;31mThe installation failed.\033[0m See the messages above.\n");
    ui_resume();
}

/* ------------------------------------------------------------- main menu */

enum item {
    I_HOSTNAME, I_TIMEZONE, I_LANG, I_KEYMAP, I_XKB, I_NETWORK, I_DNS, I_ROOTPW, I_USERS,
    I_BOOTTO, I_AUTOLOGIN, I_SERVICES, I_WINEXE, I_SEP, I_INSTALL, I_APPLY, I_QUIT, I_COUNT
};

static int item_visible(int it) {
    if (it == I_XKB) return has_xkb();
    if (it == I_BOOTTO || it == I_AUTOLOGIN) return has_desktop();
    if (it == I_WINEXE) return has_winexe();
    return 1;
}

static const char *item_label(int it) {
    switch (it) {
    case I_HOSTNAME:  return "Hostname";
    case I_TIMEZONE:  return "Timezone";
    case I_LANG:      return "Language";
    case I_KEYMAP:    return "Console keyboard";
    case I_XKB:       return "Desktop keyboard";
    case I_NETWORK:   return "Network";
    case I_DNS:       return "DNS server";
    case I_ROOTPW:    return "Root password";
    case I_USERS:     return "Users";
    case I_BOOTTO:    return "Start into";
    case I_AUTOLOGIN: return "Automatic login";
    case I_SERVICES:  return "Services";
    case I_WINEXE:    return "Windows programs";
    case I_INSTALL:   return "Install to disk...";
    case I_APPLY:     return "Apply changes";
    case I_QUIT:      return "Quit";
    }
    return "";
}

/* Value text and whether it differs from what's on the system. */
static int item_value(int it, char *out, size_t n) {
    out[0] = 0;
    switch (it) {
    case I_HOSTNAME: copy(out, n, want.hostname); return strcmp(cur.hostname, want.hostname) != 0;
    case I_TIMEZONE: copy(out, n, want.timezone); return strcmp(cur.timezone, want.timezone) != 0;
    case I_LANG:     copy(out, n, want.lang[0] ? want.lang : "(not set)"); return strcmp(cur.lang, want.lang) != 0;
    case I_KEYMAP:   copy(out, n, want.keymap); return strcmp(cur.keymap, want.keymap) != 0;
    case I_XKB:      copy(out, n, want.xkb); return strcmp(cur.xkb, want.xkb) != 0;
    case I_NETWORK:
        if (!strcmp(want.netmode, "dhcp")) snprintf(out, n, "%s: automatic (DHCP)", want.iface);
        else snprintf(out, n, "%s: %s/%s via %s", want.iface, want.ip, want.prefix, want.gateway);
        return strcmp(cur.iface, want.iface) || strcmp(cur.netmode, want.netmode) || strcmp(cur.ip, want.ip) ||
               strcmp(cur.prefix, want.prefix) || strcmp(cur.gateway, want.gateway);
    case I_DNS:      copy(out, n, want.dns[0] ? want.dns : "(none)"); return strcmp(cur.dns, want.dns) != 0;
    case I_ROOTPW:   copy(out, n, root_pw[0] ? "(new password)" : "********"); return root_pw[0] != 0;
    case I_USERS: {
        struct list u = {0};
        users_after_ops(&u);
        if (!u.n) copy(out, n, "(none yet: add one to use the desktop)");
        for (int i = 0; i < u.n; i++) {
            if (i) strncat(out, ", ", n - strlen(out) - 1);
            strncat(out, u.v[i], n - strlen(out) - 1);
        }
        list_free(&u);
        return nops > 0;
    }
    case I_BOOTTO:   copy(out, n, strcmp(want.bootto, "graphical") ? "text console" : "desktop (KDE Plasma)");
                     return strcmp(cur.bootto, want.bootto) != 0;
    case I_AUTOLOGIN: copy(out, n, want.autologin[0] ? want.autologin : "off");
                     return strcmp(cur.autologin, want.autologin) != 0;
    case I_SERVICES: {
        int on = 0, changed = 0;
        for (int i = 0; i < nsvcs; i++) { on += svcs[i].want; changed |= svcs[i].want != svcs[i].now; }
        snprintf(out, n, "%d of %d enabled", on, nsvcs);
        return changed;
    }
    case I_WINEXE:   copy(out, n, want.winexe ? "on (.exe runs via pe-loader)" : "off");
                     return cur.winexe != want.winexe;
    }
    return 0;
}

static void draw_menu(int sel, int *vis, int nvis) {
    char title[128], ver[64] = "";
    first_line("/etc/lfs-release", ver, sizeof ver);
    snprintf(title, sizeof title, "Linux From Scratch %s%s", ver, exists("/etc/lfs-live") ? " (live)" : "");
    erase();
    bar(0, "lfs-setup " VERSION, title);
    int y = 2;
    for (int k = 0; k < nvis; k++) {
        int it = vis[k];
        if (it == I_SEP) { attron(A_DIM); mvhline(y++, 2, ACS_HLINE, COLS - 4 > 60 ? 60 : COLS - 4); attroff(A_DIM); continue; }
        char val[256];
        int changed = item_value(it, val, sizeof val);
        if (k == sel) attron(COLOR_PAIR(2));
        mvhline(y, 1, ' ', COLS - 2);
        mvprintw(y, 2, "%c %-20s", changed ? '*' : ' ', item_label(it));
        if (k == sel) attroff(COLOR_PAIR(2));
        if (changed && k != sel) { attron(COLOR_PAIR(3) | A_BOLD); mvaddch(y, 2, '*'); attroff(COLOR_PAIR(3) | A_BOLD); }
        if (val[0]) {
            if (k != sel) attron(COLOR_PAIR(4));
            else attron(COLOR_PAIR(2));
            mvprintw(y, 25, "%.*s", COLS - 27 > 0 ? COLS - 27 : 0, val);
            if (k != sel) attroff(COLOR_PAIR(4));
            else attroff(COLOR_PAIR(2));
        }
        if (it == I_APPLY && pending()) { attron(COLOR_PAIR(3)); mvprintw(y, 25, "(unsaved changes)"); attroff(COLOR_PAIR(3)); }
        y++;
    }
    bar(LINES - 1, "Up/Down move   Enter change   A apply   Q quit", "* = changed, not applied yet");
    refresh();
}

static int run_menu(void) {
    int vis[I_COUNT], nvis = 0;
    for (int it = 0; it < I_COUNT; it++)
        if (item_visible(it)) vis[nvis++] = it;
    int sel = 0;
    for (;;) {
        if (vis[sel] == I_SEP) sel++;
        draw_menu(sel, vis, nvis);
        int c = getch();
        int it = vis[sel];
        if (c == KEY_UP) { do sel = sel ? sel - 1 : nvis - 1; while (vis[sel] == I_SEP); continue; }
        if (c == KEY_DOWN) { do sel = (sel + 1) % nvis; while (vis[sel] == I_SEP); continue; }
        if (c == KEY_RESIZE) continue;
        if (c == 'a' || c == 'A') it = I_APPLY;
        else if (c == 'q' || c == 'Q' || c == 27) it = I_QUIT;
        else if (c != '\n' && c != KEY_ENTER && c != ' ') continue;
        switch (it) {
        case I_HOSTNAME:  edit_hostname(); break;
        case I_TIMEZONE:  edit_timezone(); break;
        case I_LANG:      edit_lang(); break;
        case I_KEYMAP:    edit_keymap(); break;
        case I_XKB:       edit_xkb(); break;
        case I_NETWORK:   edit_network(); break;
        case I_DNS:       edit_dns(); break;
        case I_ROOTPW:    edit_root_pw(); break;
        case I_USERS:     edit_users(); break;
        case I_BOOTTO:    edit_bootto(); break;
        case I_AUTOLOGIN: edit_autologin(); break;
        case I_SERVICES:  edit_services(); break;
        case I_WINEXE:    edit_winexe(); break;
        case I_INSTALL:   install_to_disk(); break;
        case I_APPLY:
            if (pending()) apply_all(0);
            else message("Apply", "There are no changes to apply.");
            break;
        case I_QUIT:
            if (!pending() || confirm("Quit", "Quit without applying your changes?")) return 0;
            break;
        }
    }
}

static void print_status(void) {
    struct list u = {0};
    read_users(&u);
    printf("hostname          %s\n", cur.hostname);
    printf("timezone          %s\n", cur.timezone);
    printf("language          %s\n", cur.lang[0] ? cur.lang : "(not set)");
    printf("console keyboard  %s\n", cur.keymap);
    if (has_xkb()) printf("desktop keyboard  %s\n", cur.xkb);
    if (!strcmp(cur.netmode, "dhcp")) printf("network           %s: DHCP\n", cur.iface);
    else printf("network           %s: %s/%s via %s\n", cur.iface, cur.ip, cur.prefix, cur.gateway);
    printf("dns               %s\n", cur.dns[0] ? cur.dns : "(none)");
    printf("users            ");
    for (int i = 0; i < u.n; i++) printf(" %s%s", u.v[i], user_is_admin(u.v[i]) ? "(admin)" : "");
    printf("%s\n", u.n ? "" : " (none)");
    if (has_desktop()) {
        printf("start into        %s\n", strcmp(cur.bootto, "graphical") ? "console" : "desktop");
        printf("automatic login   %s\n", cur.autologin[0] ? cur.autologin : "off");
    }
    if (has_winexe()) printf("windows programs  %s\n", cur.winexe ? "on" : "off");
    printf("services         ");
    for (int i = 0; i < nsvcs; i++) if (svcs[i].now) printf(" %s", svcs[i].name);
    printf("\n");
    list_free(&u);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("lfs-setup %s\n", VERSION);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("usage: lfs-setup            open the setup menu (as root)\n"
               "       lfs-setup --status   print the current settings\n");
        return 0;
    }
    reload_state();
    if (argc > 1 && !strcmp(argv[1], "--status")) { print_status(); return 0; }
    if (argc > 1) { fprintf(stderr, "lfs-setup: unknown option %s (try --help)\n", argv[1]); return 2; }
    if (geteuid() != 0) {
        fprintf(stderr, "lfs-setup changes system settings, so it needs root.\n"
                        "Run it as root, or with: sudo lfs-setup\n");
        return 1;
    }
    if (!isatty(0) || !isatty(1)) { fprintf(stderr, "lfs-setup needs a terminal.\n"); return 1; }
    ui_start();
    if (LINES < 20 || COLS < 70) {
        ui_stop();
        fprintf(stderr, "lfs-setup needs a terminal of at least 70x20.\n");
        return 1;
    }
    int r = run_menu();
    ui_stop();
    memset(root_pw, 0, sizeof root_pw);
    return r;
}
