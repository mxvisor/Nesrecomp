#include "fm2_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── single-file playback state ── */
static uint8_t *s_buf   = NULL;
static int      s_total = 0;
static int      s_frame = 0;

/* ── directory queue ── */
static char **s_queue       = NULL;
static int    s_queue_total = 0;
static int    s_queue_idx   = 0;

/* =========================================================================
   Internal helpers
   ========================================================================= */
static uint8_t parse_buttons(const char *s) {
    uint8_t v = 0;
    if (strlen(s) >= 8) {
        if (s[0] != '.') v |= 0x01;  /* Right  */
        if (s[1] != '.') v |= 0x02;  /* Left   */
        if (s[2] != '.') v |= 0x04;  /* Down   */
        if (s[3] != '.') v |= 0x08;  /* Up     */
        if (s[4] != '.') v |= 0x10;  /* Start  */
        if (s[5] != '.') v |= 0x20;  /* Select */
        if (s[6] != '.') v |= 0x40;  /* B      */
        if (s[7] != '.') v |= 0x80;  /* A      */
    }
    return v;
}

static int load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[fm2] cannot open %s\n", path); return 0; }

    char line[1024];
    int raw = 0;
    while (fgets(line, sizeof(line), f))
        if (line[0] == '|') raw++;
    if (raw == 0) { fclose(f); return 0; }

    free(s_buf);
    s_buf = malloc(raw * 2);
    if (!s_buf) { fclose(f); return 0; }

    rewind(f);
    int idx = 0;
    while (fgets(line, sizeof(line), f) && idx < raw) {
        if (line[0] != '|') continue;
        int skip;
        char p0[16] = "", p1[16] = "";
        int n = sscanf(line, "|%d|%8[^|]|%8[^|]", &skip, p0, p1);
        if (n < 2) continue;
        uint8_t c0 = parse_buttons(p0);
        uint8_t c1 = parse_buttons(p1);
        int repeat = skip + 1;
        for (int r = 0; r < repeat && idx < raw; r++) {
            s_buf[idx * 2 + 0] = c0;
            s_buf[idx * 2 + 1] = c1;
            idx++;
        }
    }

    s_total = idx;
    s_frame = 0;
    fprintf(stderr, "[fm2] loaded %d frames from %s\n", s_total, path);
    fclose(f);
    return 1;
}

static int queue_next(void) {
    if (s_queue_idx >= s_queue_total) return 0;
    return load_file(s_queue[s_queue_idx++]);
}

static int build_queue(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "[fm2] cannot open dir %s\n", dir); return 0; }

    int cap = 16;
    s_queue = malloc(cap * sizeof(char *));
    s_queue_total = 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 4 || strcmp(n + len - 4, ".fm2") != 0) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, n);

        if (s_queue_total >= cap) {
            cap *= 2;
            s_queue = realloc(s_queue, cap * sizeof(char *));
        }
        s_queue[s_queue_total++] = strdup(path);
    }
    closedir(d);

    if (s_queue_total == 0) {
        fprintf(stderr, "[fm2] no .fm2 files in %s\n", dir);
        free(s_queue); s_queue = NULL;
        return 0;
    }

    /* Sort alphabetically */
    for (int i = 0; i < s_queue_total - 1; i++)
        for (int j = i + 1; j < s_queue_total; j++)
            if (strcmp(s_queue[i], s_queue[j]) > 0) {
                char *tmp = s_queue[i]; s_queue[i] = s_queue[j]; s_queue[j] = tmp;
            }

    fprintf(stderr, "[fm2] queued %d files from %s\n", s_queue_total, dir);
    return 1;
}

/* =========================================================================
   Public API
   ========================================================================= */
int fm2_open(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!build_queue(path)) return 0;
        return queue_next();
    }
    return load_file(path);
}

int fm2_tick(uint8_t *c0, uint8_t *c1) {
    if (!s_buf || s_frame >= s_total) return 0;
    *c0 = s_buf[s_frame * 2 + 0];
    *c1 = s_buf[s_frame * 2 + 1];
    if (++s_frame >= s_total) {
        fprintf(stderr, "[fm2] playback complete (%d frames)\n", s_total);
        if (!queue_next()) return 0;  /* advance to next file in queue */
    }
    return 1;
}

int fm2_active(void) {
    return s_buf != NULL;
}

void fm2_free(void) {
    free(s_buf); s_buf = NULL;
    for (int i = 0; i < s_queue_total; i++) free(s_queue[i]);
    free(s_queue); s_queue = NULL;
    s_total = s_frame = s_queue_total = s_queue_idx = 0;
}
