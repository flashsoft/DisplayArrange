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

#endif /* DISPLAY_CORE_H */
