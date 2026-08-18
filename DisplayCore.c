/*
 * DisplayCore.c - implementation of the shared display-layout engine.
 */

#include "DisplayCore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/graphics/IOGraphicsLib.h>

/* ------------------------------------------------------------------ */
/* CF helpers                                                          */
/* ------------------------------------------------------------------ */
static int cfnumber_to_u32(CFNumberRef num, uint32_t *out) {
    if (!num) return 0;
    long v = 0;
    if (!CFNumberGetValue(num, kCFNumberLongType, &v)) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static int display_info_for_id(CGDirectDisplayID id, DCDisplay *out) {
    memset(out, 0, sizeof(*out));

    /* Display unit is unique per physical connection and stable across
       reboots / display-ID churn. Available even when the IOKit
       service lookup fails. */
    out->unit = CGDisplayUnitNumber(id);

    io_service_t service = CGDisplayIOServicePort(id);
    if (service) {
        CFDictionaryRef info =
            IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
        if (info) {
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplayVendorID)),
                            &out->vendor);
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplayProductID)),
                            &out->model);
            cfnumber_to_u32(CFDictionaryGetValue(info, CFSTR(kDisplaySerialNumber)),
                            &out->serial);
            CFRelease(info);
        }
        IOObjectRelease(service);
    }

    snprintf(out->uuid, sizeof(out->uuid), "unit-%u", out->unit);

    CGRect bounds = CGDisplayBounds(id);
    out->origin_x = (int32_t)bounds.origin.x;
    out->origin_y = (int32_t)bounds.origin.y;
    out->width    = (uint32_t)bounds.size.width;
    out->height   = (uint32_t)bounds.size.height;
    return 1;
}

#pragma clang diagnostic pop

int dc_collect(DCDisplayList *list) {
    CGDirectDisplayID ids[DC_MAX_DISPLAYS];
    uint32_t count = 0;

    memset(list, 0, sizeof(*list));

    /* Online (not just active) so sleeping displays are still seen. */
    if (CGGetOnlineDisplayList(DC_MAX_DISPLAYS, ids, &count) != kCGErrorSuccess)
        return 0;

    for (uint32_t i = 0; i < count && i < DC_MAX_DISPLAYS; i++) {
        DCDisplay d;
        if (display_info_for_id(ids[i], &d))
            list->items[list->count++] = d;
    }
    return list->count > 0;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */
void dc_to_json(const DCDisplayList *list, char *buf, size_t n) {
    size_t off = 0;
    off += snprintf(buf + off, n - off, "[\n");
    for (size_t i = 0; i < list->count; i++) {
        const DCDisplay *d = &list->items[i];
        off += snprintf(buf + off, n - off,
            "  {\"unit\": %u, \"vendor\": %u, \"model\": %u, \"serial\": %u,\n"
            "   \"origin\": [%d, %d], \"size\": [%u, %u], \"uuid\": \"%s\"}%s\n",
            d->unit, d->vendor, d->model, d->serial,
            d->origin_x, d->origin_y, d->width, d->height,
            d->uuid, (i + 1 < list->count) ? "," : "");
    }
    snprintf(buf + off, n - off, "]\n");
}

/* Minimal parser: understands only the exact shape we emit. */
int dc_from_json(const char *json, DCDisplayList *list) {
    memset(list, 0, sizeof(*list));

    const char *p = json;
    while (*p && list->count < DC_MAX_DISPLAYS) {
        while (*p && *p != '{') p++;
        if (!*p) break;
        p++;

        DCDisplay d;
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
                long a = strtol(p, (char **)&p, 10);
                while (*p == ' ' || *p == ',') p++;
                long b = strtol(p, (char **)&p, 10);
                if (strcmp(key, "origin") == 0) {
                    d.origin_x = (int32_t)a;
                    d.origin_y = (int32_t)b;
                } else {
                    d.width  = (uint32_t)a;
                    d.height = (uint32_t)b;
                }
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
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') p++;
                    if (*p == '"') p++;
                } else {
                    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
                }
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
void dc_default_path(char *buf, size_t n) {
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

static char *read_file(const char *path) {
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
    return buf;
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    fclose(f);
    return ok;
}

int dc_save_file(const DCDisplayList *list, const char *path) {
    char cfg[1024];
    if (!path) { dc_default_path(cfg, sizeof(cfg)); path = cfg; }

    size_t cap = 4096 * DC_MAX_DISPLAYS;
    char *json = malloc(cap);
    if (!json) return 0;
    dc_to_json(list, json, cap);

    ensure_parent_dirs(path);
    int ok = write_file(path, json);
    free(json);
    return ok;
}

int dc_load_file(DCDisplayList *list, const char *path) {
    char cfg[1024];
    if (!path) { dc_default_path(cfg, sizeof(cfg)); path = cfg; }

    char *json = read_file(path);
    if (!json) return 0;
    int ok = dc_from_json(json, list);
    free(json);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */
static const DCDisplay *find_match(const DCDisplayList *saved,
                                   const DCDisplay *cur) {
    for (size_t i = 0; i < saved->count; i++) {
        if (saved->items[i].unit == cur->unit)
            return &saved->items[i];
    }
    for (size_t i = 0; i < saved->count; i++) {
        const DCDisplay *s = &saved->items[i];
        if (s->vendor == cur->vendor && s->model == cur->model &&
            s->serial == cur->serial &&
            (s->vendor != 0 || s->model != 0 || s->serial != 0))
            return s;
    }
    return NULL;
}

int dc_layouts_equal(const DCDisplayList *a, const DCDisplayList *b) {
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        const DCDisplay *x = &a->items[i];
        const DCDisplay *y = find_match(b, x);
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
int dc_apply(const DCDisplayList *saved, int verbose) {
    if (saved->count == 0) {
        fprintf(stderr, "displayarrange: no displays to apply\n");
        return 1;
    }

    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        fprintf(stderr, "displayarrange: cannot begin display config\n");
        return 1;
    }

    CGDirectDisplayID ids[DC_MAX_DISPLAYS];
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(DC_MAX_DISPLAYS, ids, &count) != kCGErrorSuccess) {
        CGCancelDisplayConfiguration(config);
        return 1;
    }

    int any = 0;
    for (uint32_t i = 0; i < count; i++) {
        DCDisplay cur;
        if (!display_info_for_id(ids[i], &cur)) continue;
        const DCDisplay *want = find_match(saved, &cur);
        if (!want) {
            if (verbose)
                fprintf(stderr,
                        "displayarrange: no saved entry for unit %u\n",
                        cur.unit);
            continue;
        }
        if (cur.origin_x != want->origin_x || cur.origin_y != want->origin_y) {
            if (verbose)
                fprintf(stderr,
                        "displayarrange: move unit %u from (%d,%d) to (%d,%d)\n",
                        cur.unit, cur.origin_x, cur.origin_y,
                        want->origin_x, want->origin_y);
            CGConfigureDisplayOrigin(config, ids[i],
                                     want->origin_x, want->origin_y);
            any = 1;
        }
    }

    CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "displayarrange: failed to apply config (error %d)\n",
                err);
        return 1;
    }
    if (verbose && !any)
        fprintf(stderr, "displayarrange: layout already matches\n");
    return 0;
}
