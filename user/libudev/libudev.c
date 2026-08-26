#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define E __attribute__((visibility("default")))

#define INPUT_DIR "/dev/input"
#define DEV_DIR "/dev"
#define INPUT_SYSPATH "/sys/class/input/"
#define GRAPHICS_SYSPATH "/sys/class/graphics/"
#define MAX_DEVICES 32
#define MAX_PROPS 12

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define BTN_MISC 0x100
#define KEY_MAX 0x2ff
#define EVIOCGBIT(ev, len) (((2U) << 30) | ((len) << 16) | (0x45U << 8) | (0x20 + (ev)))

struct prop {
    const char *key;
    const char *value;
};

struct udev {
    int refcnt;
};

struct udev_list_entry {
    char name[288];
    char value[64];
    struct udev_list_entry *next;
};

struct udev_device {
    int refcnt;
    struct udev *udev;
    const char *subsystem;
    char syspath[64];
    char sysname[32];
    char devnode[64];
    dev_t devnum;
    struct prop props[MAX_PROPS];
    int nprops;
    struct udev_list_entry prop_entries[MAX_PROPS];
};

struct udev_enumerate {
    int refcnt;
    struct udev *udev;
    char subsystem[32];
    char sysname[32];
    struct udev_list_entry entries[MAX_DEVICES];
    int nentries;
    int scanned;
};

struct udev_monitor {
    int refcnt;
    struct udev *udev;
    int fds[2];
};

static struct udev g_udev = { .refcnt = 0 };

// ---

E struct udev *udev_new(void) {
    g_udev.refcnt++;
    return &g_udev;
}

E struct udev *udev_ref(struct udev *u) {
    if (u) u->refcnt++;
    return u;
}

E struct udev *udev_unref(struct udev *u) {
    if (u && u->refcnt > 0) u->refcnt--;
    return NULL;
}

E void *udev_get_userdata(struct udev *u) {
    (void) u;
    return NULL;
}

E void udev_set_userdata(struct udev *u, void *data) {
    (void) u;
    (void) data;
}

// ---

E struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *l) {
    return l ? l->next : NULL;
}

E const char *udev_list_entry_get_name(struct udev_list_entry *l) { return l ? l->name : NULL; }

E const char *udev_list_entry_get_value(struct udev_list_entry *l) { return l ? l->value : NULL; }

E struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *l, const char *name) {
    for (; l; l = l->next)
        if (name && strcmp(l->name, name) == 0) return l;
    return NULL;
}

// ---

static void prop_add(struct udev_device *d, const char *key, const char *value) {
    if (d->nprops >= MAX_PROPS) return;
    d->props[d->nprops].key = key;
    d->props[d->nprops].value = value;
    d->nprops++;
}

static int bit_set(const unsigned char *bits, unsigned bit) {
    return (bits[bit / 8] >> (bit % 8)) & 1;
}
static void classify(struct udev_device *d) {
    prop_add(d, "ID_INPUT", "1");

    int fd = open(d->devnode, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return;

    unsigned char types[4] = { 0 };
    unsigned char keys[(KEY_MAX + 8) / 8] = { 0 };
    unsigned char rels[4] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof(types)), types);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rels)), rels);
    close(fd);

    int has_rel = bit_set(types, EV_REL) && (bit_set(rels, 0) || bit_set(rels, 1));
    int has_keyboard_keys = 0, has_buttons = 0;
    if (bit_set(types, EV_KEY)) {
        for (unsigned k = 1; k < BTN_MISC; k++)
            if (bit_set(keys, k)) {
                has_keyboard_keys = 1;
                break;
            }
        for (unsigned k = BTN_MISC; k <= KEY_MAX; k++)
            if (bit_set(keys, k)) {
                has_buttons = 1;
                break;
            }
    }

    if (has_rel && has_buttons) prop_add(d, "ID_INPUT_MOUSE", "1");
    if (has_keyboard_keys) {
        prop_add(d, "ID_INPUT_KEYBOARD", "1");
        prop_add(d, "ID_INPUT_KEY", "1");
    }
}
// ---
static struct udev_device *device_new(struct udev *u, const char *sysname) {
    if (!sysname || strlen(sysname) >= 24) return NULL;

    int is_input = strncmp(sysname, "event", 5) == 0;
    int is_fb = strncmp(sysname, "fb", 2) == 0 && sysname[2] >= '0' && sysname[2] <= '9';
    if (!is_input && !is_fb) return NULL;

    char devnode[64];
    if (is_input)
        snprintf(devnode, sizeof(devnode), INPUT_DIR "/%s", sysname);
    else
        snprintf(devnode, sizeof(devnode), DEV_DIR "/%s", sysname);

    struct stat st;
    if (stat(devnode, &st) != 0 || !S_ISCHR(st.st_mode)) return NULL;

    struct udev_device *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->refcnt = 1;
    d->udev = u;
    d->subsystem = is_input ? "input" : "graphics";
    snprintf(d->syspath, sizeof(d->syspath), "%s%s",
             is_input ? INPUT_SYSPATH : GRAPHICS_SYSPATH, sysname);
    snprintf(d->sysname, sizeof(d->sysname), "%s", sysname);
    snprintf(d->devnode, sizeof(d->devnode), "%s", devnode);
    d->devnum = st.st_rdev;
    prop_add(d, "ID_SEAT", "seat0");
    if (is_input) classify(d);
    return d;
}

E struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *syspath) {
    if (!syspath) return NULL;
    const char *sysname = strrchr(syspath, '/');
    return device_new(u, sysname ? sysname + 1 : syspath);
}

// "event[0-9]*" and "fb[0-9]*" are the only patterns callers use: match the
// leading literal prefix, which is enough to tell the two classes apart
static int sysname_matches(const char *pattern, const char *name) {
    if (!pattern[0]) return 1;
    size_t i = 0;
    while (pattern[i] && pattern[i] != '[' && pattern[i] != '*' && pattern[i] != '?') i++;
    return strncmp(pattern, name, i) == 0;
}

E struct udev_device *udev_device_new_from_devnum(struct udev *u, char type, dev_t devnum) {
    if (type != 'c') return NULL;
    char sysname[32];
    unsigned minor_no = (unsigned) (devnum & 0xff);
    if (minor_no < 64) return NULL; // below EVDEV_MINOR_BASE
    snprintf(sysname, sizeof(sysname), "event%u", minor_no - 64);
    return device_new(u, sysname);
}

E struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *u, const char *subsystem,
                                                            const char *sysname) {
    if (!subsystem || strcmp(subsystem, "input") != 0) return NULL;
    return device_new(u, sysname);
}

E struct udev_device *udev_device_ref(struct udev_device *d) {
    if (d) d->refcnt++;
    return d;
}

E struct udev_device *udev_device_unref(struct udev_device *d) {
    if (d && --d->refcnt <= 0) free(d);
    return NULL;
}

E struct udev *udev_device_get_udev(struct udev_device *d) { return d ? d->udev : NULL; }

E const char *udev_device_get_syspath(struct udev_device *d) { return d ? d->syspath : NULL; }

E const char *udev_device_get_sysname(struct udev_device *d) { return d ? d->sysname : NULL; }

E const char *udev_device_get_sysnum(struct udev_device *d) {
    if (!d) return NULL;
    const char *p = d->sysname;
    while (*p && (*p < '0' || *p > '9')) p++;
    return *p ? p : NULL;
}

E const char *udev_device_get_devnode(struct udev_device *d) { return d ? d->devnode : NULL; }

E const char *udev_device_get_subsystem(struct udev_device *d) { return d ? d->subsystem : NULL; }

E const char *udev_device_get_devtype(struct udev_device *d) {
    (void) d;
    return NULL;
}

E const char *udev_device_get_driver(struct udev_device *d) {
    (void) d;
    return NULL;
}

E dev_t udev_device_get_devnum(struct udev_device *d) { return d ? d->devnum : 0; }

E const char *udev_device_get_action(struct udev_device *d) {
    (void) d;
    return NULL; // devices only ever come from an enumeration here
}

E int udev_device_get_is_initialized(struct udev_device *d) { return d ? 1 : 0; }

E unsigned long long udev_device_get_seqnum(struct udev_device *d) {
    (void) d;
    return 0;
}

E struct udev_device *udev_device_get_parent(struct udev_device *d) {
    (void) d;
    return NULL;
}

E struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *d,
                                                                   const char *subsystem,
                                                                   const char *devtype) {
    (void) d;
    (void) subsystem;
    (void) devtype;
    return NULL;
}

E const char *udev_device_get_property_value(struct udev_device *d, const char *key) {
    if (!d || !key) return NULL;
    for (int i = 0; i < d->nprops; i++)
        if (strcmp(d->props[i].key, key) == 0) return d->props[i].value;
    return NULL;
}

E const char *udev_device_get_sysattr_value(struct udev_device *d, const char *attr) {
    if (!d || !attr) return NULL;
    if (strcmp(attr, "name") == 0) return d->sysname;
    return NULL;
}

E int udev_device_set_sysattr_value(struct udev_device *d, const char *attr, const char *value) {
    (void) d;
    (void) attr;
    (void) value;
    return -1;
}

E struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *d) {
    if (!d || d->nprops == 0) return NULL;
    for (int i = 0; i < d->nprops; i++) {
        snprintf(d->prop_entries[i].name, sizeof(d->prop_entries[i].name), "%s", d->props[i].key);
        snprintf(d->prop_entries[i].value, sizeof(d->prop_entries[i].value), "%s",
                 d->props[i].value);
        d->prop_entries[i].next = (i + 1 < d->nprops) ? &d->prop_entries[i + 1] : NULL;
    }
    return &d->prop_entries[0];
}

E struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *d) {
    (void) d;
    return NULL;
}

E struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *d) {
    (void) d;
    return NULL;
}

E struct udev_list_entry *udev_device_get_sysattr_list_entry(struct udev_device *d) {
    (void) d;
    return NULL;
}

// ---

E struct udev_enumerate *udev_enumerate_new(struct udev *u) {
    struct udev_enumerate *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->refcnt = 1;
    e->udev = u;
    return e;
}

E struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e) {
    if (e) e->refcnt++;
    return e;
}

E struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e) {
    if (e && --e->refcnt <= 0) free(e);
    return NULL;
}

E struct udev *udev_enumerate_get_udev(struct udev_enumerate *e) { return e ? e->udev : NULL; }

E int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem) {
    if (!e) return -1;
    if (subsystem) snprintf(e->subsystem, sizeof(e->subsystem), "%s", subsystem);
    return 0;
}

E int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *e, const char *subsystem) {
    (void) subsystem;
    return e ? 0 : -1;
}

E int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname) {
    if (!e) return -1;
    if (sysname) snprintf(e->sysname, sizeof(e->sysname), "%s", sysname);
    return 0;
}

E int udev_enumerate_add_match_property(struct udev_enumerate *e, const char *k, const char *v) {
    (void) k;
    (void) v;
    return e ? 0 : -1;
}

E int udev_enumerate_add_match_sysattr(struct udev_enumerate *e, const char *k, const char *v) {
    (void) k;
    (void) v;
    return e ? 0 : -1;
}

E int udev_enumerate_add_match_tag(struct udev_enumerate *e, const char *tag) {
    (void) tag;
    return e ? 0 : -1;
}

E int udev_enumerate_add_match_is_initialized(struct udev_enumerate *e) { return e ? 0 : -1; }

static void enumerate_dir(struct udev_enumerate *e, const char *dir_path, const char *prefix,
                          const char *syspath_prefix) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) && e->nentries < MAX_DEVICES) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) continue;
        if (!sysname_matches(e->sysname, ent->d_name)) continue;
        struct udev_list_entry *le = &e->entries[e->nentries];
        snprintf(le->name, sizeof(le->name), "%s%s", syspath_prefix, ent->d_name);
        le->value[0] = '\0';
        le->next = NULL;
        if (e->nentries > 0) e->entries[e->nentries - 1].next = le;
        e->nentries++;
    }
    closedir(dir);
}

E int udev_enumerate_scan_devices(struct udev_enumerate *e) {
    if (!e) return -1;
    e->nentries = 0;
    int want_input = !e->subsystem[0] || strcmp(e->subsystem, "input") == 0;
    int want_graphics = !e->subsystem[0] || strcmp(e->subsystem, "graphics") == 0;
    if (want_input) enumerate_dir(e, INPUT_DIR, "event", INPUT_SYSPATH);
    if (want_graphics) enumerate_dir(e, DEV_DIR, "fb", GRAPHICS_SYSPATH);
    e->scanned = 1;
    return 0;
}

E int udev_enumerate_scan_subsystems(struct udev_enumerate *e) {
    if (!e) return -1;
    e->nentries = 0;
    e->scanned = 1;
    return 0;
}

E struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e) {
    if (!e || !e->scanned || e->nentries == 0) return NULL;
    return &e->entries[0];
}

// ---

E struct udev_monitor *udev_monitor_new_from_netlink(struct udev *u, const char *name) {
    (void) name;
    struct udev_monitor *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->refcnt = 1;
    m->udev = u;
    m->fds[0] = m->fds[1] = -1;
    if (pipe(m->fds) < 0) {
        free(m);
        return NULL;
    }
    fcntl(m->fds[0], F_SETFL, O_NONBLOCK);
    fcntl(m->fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(m->fds[1], F_SETFD, FD_CLOEXEC);
    return m;
}

E struct udev_monitor *udev_monitor_ref(struct udev_monitor *m) {
    if (m) m->refcnt++;
    return m;
}

E struct udev_monitor *udev_monitor_unref(struct udev_monitor *m) {
    if (!m || --m->refcnt > 0) return NULL;
    if (m->fds[0] >= 0) close(m->fds[0]);
    if (m->fds[1] >= 0) close(m->fds[1]);
    free(m);
    return NULL;
}

E struct udev *udev_monitor_get_udev(struct udev_monitor *m) { return m ? m->udev : NULL; }

E int udev_monitor_get_fd(struct udev_monitor *m) { return m ? m->fds[0] : -1; }

E int udev_monitor_enable_receiving(struct udev_monitor *m) { return m ? 0 : -1; }

E int udev_monitor_set_receive_buffer_size(struct udev_monitor *m, int size) {
    (void) size;
    return m ? 0 : -1;
}

E int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
                                                     const char *devtype) {
    (void) subsystem;
    (void) devtype;
    return m ? 0 : -1;
}

E int udev_monitor_filter_add_match_tag(struct udev_monitor *m, const char *tag) {
    (void) tag;
    return m ? 0 : -1;
}

E int udev_monitor_filter_update(struct udev_monitor *m) { return m ? 0 : -1; }

E int udev_monitor_filter_remove(struct udev_monitor *m) { return m ? 0 : -1; }

// no hotplug now
E struct udev_device *udev_monitor_receive_device(struct udev_monitor *m) {
    (void) m;
    return NULL;
}
