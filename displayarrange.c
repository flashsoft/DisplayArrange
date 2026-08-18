/*
 * displayarrange - a small displayplacer-like tool for macOS.
 *
 * Commands:
 *   list                                  Print connected displays with stable IDs.
 *   save [path]                           Save current layout to JSON.
 *   apply [path]                          Apply a saved layout (match by display unit,
 *                                         fall back to vendor/model/serial).
 *   apply-if-different [path]             Apply only when current layout differs.
 *   watch [path] [--debounce N] [--verbose]
 *                                         Save layout if missing, then auto-restore
 *                                         after every screen unlock.
 *
 * Build:  cc -O2 -o displayarrange displayarrange.c -framework CoreGraphics -framework IOKit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define MAX_DISPLAYS 32
#define BUF 4096

typedef struct {
    uint32_t unit;      /* IOKit display unit: stable per physical port */
    uint32_t vendor;
    uint32_t model;
    uint32_t serial;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t width;     /* points */
    uint32_t height;
    char uuid[64];
} DisplayInfo;

typedef struct {
    DisplayInfo items[MAX_DISPLAYS];
    size_t count;
} DisplayList;

static int cfnumber_to_u32(CFNumberRef num, uint32_t *out) {
    if (!num) return 0;
    long v = 0;
    if (!CFNumberGetValue(num, kCFNumberLongType, &v)) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Display enumeration                                                 */
/* ------------------------------------------------------------------ */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static int display_info_for_id(CGDirectDisplayID id, DisplayInfo *out) {
    memset(out, 0, sizeof(*out));

    /* Display unit is unique per physical connection on this Mac and
       stable across reboots / display-ID churn. Primary match key.
       Available even when the IOKit service lookup fails. */
    out->unit = CGDisplayUnitNumber(id);

    io_service_t service = CGDisplayIOServicePort(id);
    if (service) {
        CFDictionaryRef info = IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
        if (info) {
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplayVendorID)), &out->vendor);
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplayProductID)), &out->model);
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplaySerialNumber)), &out->serial);
            CFRelease(info);
        }
        IOObjectRelease(service);
    }

    snprintf(out->uuid, sizeof(out->uuid), "unit-%u", out->unit);

    CGRect bounds = CGDisplayBounds(id);
    out->origin_x = (uint32_t)(int32_t)bounds.origin.x;
    out->origin_y = (uint32_t)(int32_t)bounds.origin.y;
    out->width    = (uint32_t)bounds.size.width;
    out->height   = (uint32_t)bounds.size.height;

    return 1;
}

#pragma clang diagnostic pop

static int collect_displays(DisplayList *list) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t count = 0;

    memset(list, 0, sizeof(*list));

    /* Online (not just active) so we still see displays that are asleep. */
    if (CGGetOnlineDisplayList(MAX_DISPLAYS, ids, &count) != kCGErrorSuccess)
        return 0;

    for (uint32_t i = 0; i < count && i < MAX_DISPLAYS; i++) {
        DisplayInfo info;
        if (display_info_for_id(ids[i], &info))
            list->items[list->count++] = info;
    }
    return list->count > 0;
}

/* ------------------------------------------------------------------ */
/* JSON serialization (hand-rolled, tiny)                              */
/* ------------------------------------------------------------------ */
static void list_to_json(const DisplayList *list, char *buf, size_t n) {
    size_t off = 0;
    off += snprintf(buf + off, n - off, "[\n");
    for (size_t i = 0; i < list->count; i++) {
        const DisplayInfo *d = &list->items[i];
        off += snprintf(buf + off, n - off,
            "  {\"unit\": %u, \"vendor\": %u, \"model\": %u, \"serial\": %u,\n"
            "   \"origin\": [%u, %u], \"size\": [%u, %u], \"uuid\": \"%s\"}%s\n",
            d->unit, d->vendor, d->model, d->serial,
            d->origin_x, d->origin_y, d->width, d->height,
            d->uuid, (i + 1 < list->count) ? "," : "");
    }
    snprintf(buf + off, n - off, "]\n");
}

/* Minimal parser: understands only the exact shape we emit. */
static int parse_json_list(const char *json, DisplayList *list) {
    memset(list, 0, sizeof(*list));

    const char *p = json;
    while (*p && list->count < MAX_DISPLAYS) {
        while (*p && *p != '{') p++;
        if (!*p) break;
        p++;

        DisplayInfo d;
        memset(&d, 0, sizeof(d));

        while (*p && *p != '}') {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
            if (*p != '"') { if (*p) p++; continue; }
            p++;
            char key[32];
            size_t ki = 0;
            while (*p && *p != '"' && ki + 1 < sizeof(key)) key[ki++] = *p++;
            key[ki] = '\0';
            if (*p == '"') p++;
            while (*p == ' ' || *p == ':') p++;

            if (strcmp(key, "unit") == 0 || strcmp(key, "vendor") == 0 ||
                strcmp(key, "model") == 0 || strcmp(key, "serial") == 0) {
                uint32_t v = (uint32_t)strtoul(p, NULL, 10);
                if (strcmp(key, "unit") == 0) d.unit = v;
                else if (strcmp(key, "vendor") == 0) d.vendor = v;
                else if (strcmp(key, "model") == 0) d.model = v;
                else d.serial = v;
                while (*p == '-' || (*p >= '0' && *p <= '9')) p++;
            } else if (strcmp(key, "origin") == 0 || strcmp(key, "size") == 0) {
                while (*p && *p != '[') p++;
                if (*p == '[') p++;
                uint32_t a = (uint32_t)strtoul(p, (char **)&p, 10);
                while (*p == ' ' || *p == ',') p++;
                uint32_t b = (uint32_t)strtoul(p, (char **)&p, 10);
                if (strcmp(key, "origin") == 0) { d.origin_x = a; d.origin_y = b; }
                else { d.width = a; d.height = b; }
            } else if (strcmp(key, "uuid") == 0) {
                if (*p == '"') {
                    p++;
                    size_t ui = 0;
                    while (*p && *p != '"' && ui + 1 < sizeof(d.uuid))
                        d.uuid[ui++] = *p++;
                    d.uuid[ui] = '\0';
                    if (*p == '"') p++;
                }
            } else {
                if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
                else while (*p && *p != ',' && *p != '}' && *p != ']') p++;
            }
        }
        if (*p == '}') p++;
        list->items[list->count++] = d;
    }
    return list->count > 0;
}

/* ------------------------------------------------------------------ */
/* File I/O                                                            */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    fclose(f);
    return ok;
}

static void default_config_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, n, "%s/.config/displayarrange/layout.json", home);
}

static void ensure_parent_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';

    char partial[1024] = "";
    char *p = tmp;
    if (*p == '/') { strcat(partial, "/"); p++; }
    char *save = NULL;
    char *token = strtok_r(p, "/", &save);
    while (token) {
        strcat(partial, token);
        mkdir(partial, 0755);
        strcat(partial, "/");
        token = strtok_r(NULL, "/", &save);
    }
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */
static const DisplayInfo *find_match(const DisplayList *saved, const DisplayInfo *cur) {
    for (size_t i = 0; i < saved->count; i++) {
        if (saved->items[i].unit == cur->unit)
            return &saved->items[i];
    }
    for (size_t i = 0; i < saved->count; i++) {
        const DisplayInfo *s = &saved->items[i];
        if (s->vendor == cur->vendor && s->model == cur->model &&
            s->serial == cur->serial &&
            (s->vendor != 0 || s->model != 0 || s->serial != 0))
            return s;
    }
    return NULL;
}

static int layouts_equal(const DisplayList *a, const DisplayList *b) {
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        const DisplayInfo *x = &a->items[i];
        const DisplayInfo *y = find_match(b, x);
        if (!y) return 0;
        if (x->origin_x != y->origin_x || x->origin_y != y->origin_y ||
            x->width != y->width || x->height != y->height)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Apply                                                               */
/* ------------------------------------------------------------------ */
static int apply_layout(const DisplayList *saved, const DisplayList *current,
                        int verbose) {
    (void)current;
    if (saved->count == 0) {
        fprintf(stderr, "displayarrange: no displays to apply\n");
        return 1;
    }

    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        fprintf(stderr, "displayarrange: cannot begin display config\n");
        return 1;
    }

    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(MAX_DISPLAYS, ids, &count) != kCGErrorSuccess) {
        CGCancelDisplayConfiguration(config);
        return 1;
    }

    int any = 0;
    for (uint32_t i = 0; i < count; i++) {
        DisplayInfo cur;
        if (!display_info_for_id(ids[i], &cur)) continue;
        const DisplayInfo *want = find_match(saved, &cur);
        if (!want) {
            if (verbose)
                fprintf(stderr, "displayarrange: no saved entry for unit %u (%s)\n",
                        cur.unit, cur.uuid);
            continue;
        }
        if (cur.origin_x != want->origin_x || cur.origin_y != want->origin_y) {
            if (verbose)
                fprintf(stderr,
                        "displayarrange: move unit %u from (%d,%d) to (%u,%u)\n",
                        cur.unit, (int32_t)cur.origin_x, (int32_t)cur.origin_y,
                        want->origin_x, want->origin_y);
            CGConfigureDisplayOrigin(config, ids[i],
                                     (int32_t)want->origin_x,
                                     (int32_t)want->origin_y);
            any = 1;
        }
    }

    CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "displayarrange: failed to apply config (error %d)\n", err);
        return 1;
    }
    if (verbose && !any)
        fprintf(stderr, "displayarrange: layout already matches, nothing to do\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */
static int cmd_list(void) {
    DisplayList list;
    if (!collect_displays(&list)) {
        fprintf(stderr, "displayarrange: no active displays found\n");
        return 1;
    }
    for (size_t i = 0; i < list.count; i++) {
        const DisplayInfo *d = &list.items[i];
        printf("unit=%u  vendor=0x%04x  model=0x%04x  serial=%u\n",
               d->unit, d->vendor, d->model, d->serial);
        printf("    origin=(%d, %d)  size=%ux%u  uuid=%s\n",
               (int32_t)d->origin_x, (int32_t)d->origin_y,
               d->width, d->height, d->uuid);
    }
    return 0;
}

static int cmd_save(const char *path) {
    char cfg[1024];
    if (!path) { default_config_path(cfg, sizeof(cfg)); path = cfg; }

    DisplayList list;
    if (!collect_displays(&list)) {
        fprintf(stderr, "displayarrange: no active displays found\n");
        return 1;
    }

    char json[BUF * MAX_DISPLAYS];
    list_to_json(&list, json, sizeof(json));

    ensure_parent_dirs(path);
    if (!write_file(path, json)) {
        fprintf(stderr, "displayarrange: cannot write %s\n", path);
        return 1;
    }
    printf("saved %zu display(s) to %s\n", list.count, path);
    return 0;
}

static int load_saved(const char *path, DisplayList *saved) {
    char cfg[1024];
    if (!path) { default_config_path(cfg, sizeof(cfg)); path = cfg; }

    size_t len = 0;
    char *json = read_file(path, &len);
    if (!json) {
        fprintf(stderr, "displayarrange: cannot read %s\n", path);
        return 0;
    }
    int ok = parse_json_list(json, saved);
    free(json);
    if (!ok)
        fprintf(stderr, "displayarrange: no display entries in %s\n", path);
    return ok;
}

static int cmd_apply(const char *path, int only_if_different, int verbose) {
    DisplayList saved, current;
    if (!load_saved(path, &saved)) return 1;
    if (!collect_displays(&current)) {
        fprintf(stderr, "displayarrange: no active displays found\n");
        return 1;
    }
    if (only_if_different && layouts_equal(&saved, &current)) {
        if (verbose)
            fprintf(stderr, "displayarrange: layout unchanged, skipping\n");
        return 0;
    }
    return apply_layout(&saved, &current, verbose);
}

/* ------------------------------------------------------------------ */
/* Watch mode: re-apply after screen unlock                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char path[1024];
    int debounce_ms;
    int verbose;
} WatchCtx;

static WatchCtx g_watch;

static void on_unlock(CFNotificationCenterRef center, void *observer,
                      CFStringRef name, const void *object,
                      CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)name; (void)object; (void)userInfo;
    if (g_watch.verbose)
        fprintf(stderr, "displayarrange: screen unlocked, waiting %d ms...\n",
                g_watch.debounce_ms);
    usleep((useconds_t)g_watch.debounce_ms * 1000);

    DisplayList saved, current;
    if (!load_saved(g_watch.path, &saved)) return;
    if (!collect_displays(&current)) return;
    if (layouts_equal(&saved, &current)) {
        if (g_watch.verbose)
            fprintf(stderr, "displayarrange: layout already correct\n");
        return;
    }
    fprintf(stderr, "displayarrange: layout drifted, restoring...\n");
    apply_layout(&saved, &current, g_watch.verbose);
}

static int cmd_watch(const char *path, int debounce_ms, int verbose) {
    if (!path) default_config_path(g_watch.path, sizeof(g_watch.path));
    else snprintf(g_watch.path, sizeof(g_watch.path), "%s", path);
    g_watch.debounce_ms = debounce_ms > 0 ? debounce_ms : 800;
    g_watch.verbose = verbose;

    /* First run: capture the current layout so there is a baseline. */
    if (access(g_watch.path, R_OK) != 0) {
        fprintf(stderr, "displayarrange: no saved layout, saving current to %s\n",
                g_watch.path);
        if (cmd_save(g_watch.path) != 0) return 1;
    }

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDistributedCenter(),
        NULL,
        on_unlock,
        CFSTR("com.apple.screenIsUnlocked"),
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);

    fprintf(stderr, "displayarrange: watching for unlock events (config: %s)\n",
            g_watch.path);
    CFRunLoopRun();
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <command> [options]\n"
        "commands:\n"
        "  list                        show connected displays\n"
        "  save [path]                 save current layout\n"
        "  apply [path]                restore saved layout\n"
        "  apply-if-different [path]   restore only if layout changed\n"
        "  watch [path]                auto-restore after screen unlock\n"
        "options for watch:\n"
        "  --debounce N                wait N ms after unlock (default 800)\n"
        "  --verbose                   log what it does\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];
    const char *path = NULL;
    int debounce = 800;
    int verbose = 0;

    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strcmp(argv[i], "--debounce") == 0 && i + 1 < argc)
            debounce = atoi(argv[++i]);
        else if (argv[i][0] != '-') path = argv[i];
        i++;
    }

    if (strcmp(cmd, "list") == 0) return cmd_list();
    if (strcmp(cmd, "save") == 0) return cmd_save(path);
    if (strcmp(cmd, "apply") == 0) return cmd_apply(path, 0, verbose);
    if (strcmp(cmd, "apply-if-different") == 0) return cmd_apply(path, 1, verbose);
    if (strcmp(cmd, "watch") == 0) return cmd_watch(path, debounce, verbose);

    usage(argv[0]);
    return 1;
}
