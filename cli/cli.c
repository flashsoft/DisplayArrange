/*
 * cli.c - command-line interface on top of DisplayCore.
 *
 * Build:  make cli
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "../core/DisplayCore.h"

static int cmd_list(void) {
    DCDisplayList list;
    if (!dc_collect(&list)) {
        fprintf(stderr, "displayarrange: no displays found\n");
        return 1;
    }
    for (size_t i = 0; i < list.count; i++) {
        const DCDisplay *d = &list.items[i];
        printf("unit=%u  vendor=0x%04x  model=0x%04x  serial=%u\n",
               d->unit, d->vendor, d->model, d->serial);
        printf("    origin=(%d, %d)  size=%ux%u  uuid=%s\n",
               d->origin_x, d->origin_y, d->width, d->height, d->uuid);
    }
    return 0;
}

static int cmd_save(const char *path) {
    DCDisplayList list;
    if (!dc_collect(&list)) {
        fprintf(stderr, "displayarrange: no displays found\n");
        return 1;
    }
    if (!dc_save_file(&list, path)) {
        fprintf(stderr, "displayarrange: cannot save layout\n");
        return 1;
    }
    char cfg[1024];
    if (!path) { dc_default_path(cfg, sizeof(cfg)); path = cfg; }
    printf("saved %zu display(s) to %s\n", list.count, path);
    return 0;
}

static int cmd_apply(const char *path, int only_if_different, int verbose) {
    int r = dc_restore(path, only_if_different, verbose, NULL);
    if (r == DC_RESTORE_NO_FILE) {
        fprintf(stderr, "displayarrange: cannot load saved layout\n");
        return 1;
    }
    if (r == DC_RESTORE_NO_DISPLAYS) {
        fprintf(stderr, "displayarrange: no displays found\n");
        return 1;
    }
    if (r == DC_RESTORE_ALREADY) {
        if (verbose)
            fprintf(stderr, "displayarrange: layout unchanged, skipping\n");
        return 0;
    }
    return r == DC_RESTORE_OK ? 0 : 1;
}

/* ---- watch mode -------------------------------------------------- */

static char  g_path[1024];
static int   g_debounce_ms = 800;
static int   g_verbose     = 0;

static void on_unlock(CFNotificationCenterRef center, void *observer,
                      CFStringRef name, const void *object,
                      CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)name; (void)object; (void)userInfo;
    if (g_verbose)
        fprintf(stderr, "displayarrange: unlocked, waiting %d ms...\n",
                g_debounce_ms);
    usleep((useconds_t)g_debounce_ms * 1000);

    int r = dc_restore(g_path, 1, g_verbose,
                       "displayarrange: layout drifted, restoring...\n");
    if (r == DC_RESTORE_ALREADY && g_verbose)
        fprintf(stderr, "displayarrange: layout already correct\n");
}

static int cmd_watch(const char *path, int debounce_ms, int verbose) {
    if (!path) dc_default_path(g_path, sizeof(g_path));
    else snprintf(g_path, sizeof(g_path), "%s", path);
    g_debounce_ms = debounce_ms > 0 ? debounce_ms : 800;
    g_verbose     = verbose;

    /* First run: capture the current layout as a baseline. */
    if (access(g_path, R_OK) != 0) {
        fprintf(stderr, "displayarrange: no saved layout, saving to %s\n",
                g_path);
        if (cmd_save(g_path) != 0) return 1;
    }

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDistributedCenter(),
        NULL,
        on_unlock,
        CFSTR("com.apple.screenIsUnlocked"),
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);

    fprintf(stderr, "displayarrange: watching (config: %s)\n", g_path);
    CFRunLoopRun();
    return 0;
}

/* ---- main -------------------------------------------------------- */

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

    const char *cmd  = argv[1];
    const char *path = NULL;
    int debounce = 800;
    int verbose  = 0;

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
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) { usage(argv[0]); return 0; }

    usage(argv[0]);
    return 1;
}
