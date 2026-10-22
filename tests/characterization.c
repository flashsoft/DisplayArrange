/*
 * characterization.c - lock in the current observable behavior of the
 * pure parts of DisplayCore (JSON emit/parse, layout matching, file I/O).
 *
 * These tests describe the implementation AS IT IS, including quirks of
 * the minimal JSON parser; they are a refactor safety net, not a spec.
 * They never touch the real display configuration: dc_collect/dc_apply
 * are intentionally not exercised here.
 *
 * Build & run:  make test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../core/DisplayCore.h"

static int failures = 0;

#define CHECK(cond) do {                                            \
    if (cond) {                                                     \
        printf("PASS  %s\n", #cond);                                \
    } else {                                                        \
        printf("FAIL  %s  (%s:%d)\n", #cond, __FILE__, __LINE__);   \
        failures++;                                                 \
    }                                                               \
} while (0)

static DCDisplay make_display(uint32_t unit, uint32_t vendor,
                              uint32_t model, uint32_t serial,
                              int32_t x, int32_t y,
                              uint32_t w, uint32_t h) {
    DCDisplay d;
    memset(&d, 0, sizeof(d));
    d.unit = unit;
    d.vendor = vendor;
    d.model = model;
    d.serial = serial;
    d.origin_x = x;
    d.origin_y = y;
    d.width = w;
    d.height = h;
    snprintf(d.uuid, sizeof(d.uuid), "unit-%u", unit);
    return d;
}

static int same_display(const DCDisplay *a, const DCDisplay *b) {
    return a->unit == b->unit && a->vendor == b->vendor &&
           a->model == b->model && a->serial == b->serial &&
           a->origin_x == b->origin_x && a->origin_y == b->origin_y &&
           a->width == b->width && a->height == b->height &&
           strcmp(a->uuid, b->uuid) == 0;
}

/* ---- JSON emit: exact byte format -------------------------------- */

static void test_emit_golden(void) {
    DCDisplayList list;
    memset(&list, 0, sizeof(list));
    list.items[0] = make_display(7, 1, 2, 3, 3, 4, 800, 600);
    list.count = 1;

    char buf[4096];
    dc_to_json(&list, buf, sizeof(buf));

    const char *golden =
        "[\n"
        "  {\"unit\": 7, \"vendor\": 1, \"model\": 2, \"serial\": 3,\n"
        "   \"origin\": [3, 4], \"size\": [800, 600], \"uuid\": \"unit-7\"}\n"
        "]\n";
    CHECK(strcmp(buf, golden) == 0);

    /* Two items: comma after the first, none after the last. */
    list.items[1] = make_display(9, 0, 0, 0, -1440, -10, 2560, 1440);
    list.count = 2;
    dc_to_json(&list, buf, sizeof(buf));
    const char *golden2 =
        "[\n"
        "  {\"unit\": 7, \"vendor\": 1, \"model\": 2, \"serial\": 3,\n"
        "   \"origin\": [3, 4], \"size\": [800, 600], \"uuid\": \"unit-7\"},\n"
        "  {\"unit\": 9, \"vendor\": 0, \"model\": 0, \"serial\": 0,\n"
        "   \"origin\": [-1440, -10], \"size\": [2560, 1440], \"uuid\": \"unit-9\"}\n"
        "]\n";
    CHECK(strcmp(buf, golden2) == 0);
}

/* ---- JSON round trip --------------------------------------------- */

static void test_round_trip(void) {
    DCDisplayList a, b;
    memset(&a, 0, sizeof(a));
    a.items[0] = make_display(1, 0x10, 0x20, 0x30, -1440, 0, 2560, 1440);
    a.items[1] = make_display(0, 0, 0, 0, 0, 1440, 1920, 1080);
    a.count = 2;

    char buf[8192];
    dc_to_json(&a, buf, sizeof(buf));
    CHECK(dc_from_json(buf, &b) == 1);
    CHECK(b.count == a.count);
    if (b.count == a.count) {
        CHECK(same_display(&a.items[0], &b.items[0]));
        CHECK(same_display(&a.items[1], &b.items[1]));
    }
}

/* ---- Minimal parser: current edge behavior ----------------------- */

static void test_parser_edges(void) {
    DCDisplayList l;

    /* Empty input and an empty array both fail (count 0 -> return 0). */
    CHECK(dc_from_json("", &l) == 0);
    CHECK(dc_from_json("[]", &l) == 0);

    /* Truncated input fails the same way. */
    CHECK(dc_from_json("[", &l) == 0);
    CHECK(dc_from_json("[ {\"unit\": 5", &l) == 1); /* missing '}' still counts */

    /* Unknown keys are skipped, known keys still parsed. */
    const char *json =
        "[ {\"unit\": 7, \"unknown\": \"x\", \"origin\": [3, 4],"
        "   \"size\": [800, 600], \"uuid\": \"unit-7\"} ]";
    CHECK(dc_from_json(json, &l) == 1);
    CHECK(l.count == 1);
    if (l.count == 1) {
        CHECK(l.items[0].unit == 7);
        CHECK(l.items[0].origin_x == 3 && l.items[0].origin_y == 4);
        CHECK(l.items[0].width == 800 && l.items[0].height == 600);
        CHECK(strcmp(l.items[0].uuid, "unit-7") == 0);
    }

    /* Fields absent from the JSON stay zero. */
    CHECK(dc_from_json("[ {\"unit\": 42} ]", &l) == 1);
    CHECK(l.items[0].unit == 42);
    CHECK(l.items[0].vendor == 0 && l.items[0].serial == 0);
    CHECK(l.items[0].origin_x == 0 && l.items[0].width == 0);
}

/* ---- Layout matching --------------------------------------------- */

static void test_layouts_equal(void) {
    DCDisplayList a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    a.items[0] = make_display(1, 0x10, 0x20, 0x30, 0, 0, 2560, 1440);
    a.items[1] = make_display(0, 0, 0, 0, 2560, 0, 1920, 1080);
    a.count = 2;

    /* Identical copy, and order-swapped copy, are both equal. */
    b = a;
    CHECK(dc_layouts_equal(&a, &b) == 1);
    b.items[0] = a.items[1];
    b.items[1] = a.items[0];
    CHECK(dc_layouts_equal(&a, &b) == 1);

    /* Origin or size drift breaks equality. */
    b = a;
    b.items[1].origin_x += 1;
    CHECK(dc_layouts_equal(&a, &b) == 0);
    b = a;
    b.items[0].height += 1;
    CHECK(dc_layouts_equal(&a, &b) == 0);

    /* Count mismatch breaks equality. */
    b = a;
    b.count = 1;
    CHECK(dc_layouts_equal(&a, &b) == 0);

    /* Fallback: same vendor/model/serial matches across different units. */
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.items[0] = make_display(99, 1, 2, 3, 0, 0, 1920, 1080);
    a.count = 1;
    b.items[0] = make_display(55, 1, 2, 3, 0, 0, 1920, 1080);
    b.count = 1;
    CHECK(dc_layouts_equal(&a, &b) == 1);

    /* ... but an all-zero vendor/model/serial tuple never falls back. */
    b.items[0] = make_display(55, 0, 0, 0, 0, 0, 1920, 1080);
    a.items[0] = make_display(99, 0, 0, 0, 0, 0, 1920, 1080);
    CHECK(dc_layouts_equal(&a, &b) == 0);
}

/* ---- File I/O under a temporary HOME ------------------------------ */

static void test_file_io(void) {
    char home[128];
    snprintf(home, sizeof(home), "/tmp/da_char_%ld", (long)getpid());
    CHECK(mkdir(home, 0755) == 0);
    CHECK(setenv("HOME", home, 1) == 0);

    char expect[1200];
    snprintf(expect, sizeof(expect),
             "%s/.config/displayarrange/layout.json", home);

    char path[1200];
    dc_default_path(path, sizeof(path));
    CHECK(strcmp(path, expect) == 0);

    DCDisplayList a, b;
    memset(&a, 0, sizeof(a));
    a.items[0] = make_display(1, 0x10, 0x20, 0x30, 0, 0, 2560, 1440);
    a.items[1] = make_display(0, 0, 0, 0, 2560, 0, 1920, 1080);
    a.count = 2;

    /* NULL path resolves to the default; parent dirs are created. */
    CHECK(dc_save_file(&a, NULL) == 1);
    FILE *f = fopen(expect, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);

    CHECK(dc_load_file(&b, NULL) == 1);
    CHECK(b.count == a.count);
    if (b.count == a.count) {
        CHECK(same_display(&a.items[0], &b.items[0]));
        CHECK(same_display(&a.items[1], &b.items[1]));
    }

    /* Loading a missing file fails. */
    CHECK(dc_load_file(&b, "/tmp/da_char_no_such_file.json") == 0);
}

/* ---- dc_restore orchestration ------------------------------------- */

static void test_restore(void) {
    /* Missing layout file. */
    CHECK(dc_restore("/tmp/da_char_no_such_file.json", 1, 0, NULL)
          == DC_RESTORE_NO_FILE);

    /* Everything below needs at least one online display. */
    DCDisplayList cur;
    if (!dc_collect(&cur)) {
        printf("SKIP  dc_restore live-display checks (no displays)\n");
        return;
    }

    /* Saving the current layout and restoring it must be a no-op:
       ALREADY, without touching the display configuration. */
    char path[128];
    snprintf(path, sizeof(path), "/tmp/da_char_%ld_restore.json",
             (long)getpid());
    CHECK(dc_save_file(&cur, path) == 1);
    CHECK(dc_restore(path, 1, 0, NULL) == DC_RESTORE_ALREADY);
}

int main(void) {
    test_emit_golden();
    test_round_trip();
    test_parser_edges();
    test_layouts_equal();
    test_file_io();
    test_restore();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
