/*
 * DisplayCore.h - shared display-layout engine.
 *
 * Pure C, no AppKit/SwiftUI dependencies, so the same code drives
 * both the CLI and the menu-bar app.
 */

#ifndef DISPLAY_CORE_H
#define DISPLAY_CORE_H

#include <stdint.h>
#include <stddef.h>

#define DC_MAX_DISPLAYS 32

typedef struct {
    uint32_t unit;      /* stable per physical port, primary match key */
    uint32_t vendor;
    uint32_t model;
    uint32_t serial;
    int32_t  origin_x;
    int32_t  origin_y;
    uint32_t width;     /* points */
    uint32_t height;
    char     uuid[64];
} DCDisplay;

typedef struct {
    DCDisplay items[DC_MAX_DISPLAYS];
    size_t    count;
} DCDisplayList;

/* Enumerate every online display (including sleeping ones).
   Returns 1 on success, 0 on failure. */
int  dc_collect(DCDisplayList *list);

/* Serialize / deserialize the layout as JSON. */
void dc_to_json(const DCDisplayList *list, char *buf, size_t n);
int  dc_from_json(const char *json, DCDisplayList *list);

/* File helpers. path == NULL uses ~/.config/displayarrange/layout.json */
int  dc_save_file(const DCDisplayList *list, const char *path);
int  dc_load_file(DCDisplayList *list, const char *path);
void dc_default_path(char *buf, size_t n);

/* Compare two layouts (match by unit, then vendor/model/serial). */
int  dc_layouts_equal(const DCDisplayList *a, const DCDisplayList *b);

/* Apply `saved` to the currently connected displays.
   Only displays that have drifted are moved.
   Returns 0 on success, non-zero on error. */
int  dc_apply(const DCDisplayList *saved, int verbose);

/* Result codes for dc_restore. */
#define DC_RESTORE_OK           0  /* applied successfully */
#define DC_RESTORE_ALREADY      1  /* already matches, nothing applied */
#define DC_RESTORE_NO_FILE      2  /* saved layout could not be loaded */
#define DC_RESTORE_NO_DISPLAYS  3  /* no online displays found */
#define DC_RESTORE_APPLY_FAILED 4  /* dc_apply failed */

/* Load the saved layout from path (NULL = default file), compare it with
   the currently connected displays and restore it. When only_if_different
   is set and the layout already matches, nothing is applied and
   DC_RESTORE_ALREADY is returned. pre_apply_msg, when non-NULL, is printed
   to stderr just before applying. */
int  dc_restore(const char *path, int only_if_different, int verbose,
                const char *pre_apply_msg);

/* Hold or release a power assertion preventing idle system sleep.
   The assertion is process-local: it is released automatically when the
   process exits. It cannot stop lid-close or manual sleep.
   Returns 1 on success, 0 on failure. */
int  dc_set_prevent_sleep(int enable);

/* Same as dc_set_prevent_sleep, but prevents the display from sleeping
   on idle instead of the whole system. Independent of the system-sleep
   assertion. */
int  dc_set_prevent_display_sleep(int enable);

#endif /* DISPLAY_CORE_H */
