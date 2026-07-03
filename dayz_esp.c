
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/uio.h>
#include <math.h>
#include <signal.h>
#include <SDL2/SDL.h>

#define WORLD_STATIC_ADDR 0x00000000UL


#define OFF_CAMERA           0x1B8   // Camera object pointer
#define OFF_NEAR_TABLE       0xEB8   // DynamicArray of nearby entities
#define OFF_ITEM_TABLE       0x1FB8  // DynamicArray of items
#define OFF_LOCAL_PLAYER     0x15C0  // Local player entity pointer
#define OFF_GRASS            0xB80   // TerrainGrid float (0 = no grass)


#define OFF_POSITION         0x2C    // Vector3 (12 bytes: x, y, z floats)
#define OFF_HEAD_POS         0xF8    // Head/camera position vector3
#define OFF_TYPE_RTTI        0x148   // RendererEntityType (int)
#define OFF_CONFIG_NAME      0xA0    // ArmaString pointer (class config name)
#define OFF_TYPE_NAME        0x68    // ArmaString pointer (type name)
#define OFF_HEALTH           0x1A0   // Health float
#define OFF_NETWORK_ID       0x634   // Network ID (uint64)

// Camera struct offsets
#define OFF_CAM_RIGHT        0x08    // Inverted view right (Vector3)
#define OFF_CAM_UP           0x14    // Inverted view up
#define OFF_CAM_FORWARD      0x20    // Inverted view forward
#define OFF_CAM_TRANSLATE    0x2C    // Inverted view translation
#define OFF_CAM_PROJ_D1      0xD0    // Projection param 1 (float)
#define OFF_CAM_PROJ_D2      0xDC    // Projection param 2 (float)

// ============================================================
// SECTION 2: TYPES
// ============================================================

typedef struct { float x, y, z; } Vector3;

typedef enum {
    ENT_UNKNOWN, ENT_PLAYER, ENT_ZOMBIE, ENT_ANIMAL, ENT_ITEM
} EntityCategory;

typedef struct {
    uint64_t        address;
    Vector3         position;
    Vector3         head_pos;
    float           distance;
    char            name[64];
    char            type_str[32];
    EntityCategory  category;
    float           health;
} EntityInfo;

typedef struct {
    bool esp_enabled;
    bool esp_boxes;
    bool esp_lines;
    bool esp_names;
    bool esp_distances;
    bool esp_items;
    bool esp_zombies;
    bool esp_animals;
    bool esp_health;
    float esp_max_dist;
    bool no_grass;
    bool show_menu;
    int  menu_sel;
} Config;

// ============================================================
// SECTION 3: MEMORY ACCESS
// ============================================================

ssize_t mem_read(pid_t pid, const void *raddr, void *lbuf, size_t sz) {
    struct iovec lio = { .iov_base = lbuf,  .iov_len = sz };
    struct iovec rio = { .iov_base = (void*)raddr, .iov_len = sz };
    return process_vm_readv(pid, &lio, 1, &rio, 1, 0);
}

uint64_t r64(pid_t pid, const void *a) {
    uint64_t v = 0; mem_read(pid, a, &v, 8); return v;
}
uint32_t r32(pid_t pid, const void *a) {
    uint32_t v = 0; mem_read(pid, a, &v, 4); return v;
}
float rfl(pid_t pid, const void *a) {
    float v = 0; mem_read(pid, a, &v, 4); return v;
}
Vector3 rv3(pid_t pid, const void *a) {
    Vector3 v = {0}; mem_read(pid, a, &v, 12); return v;
}

int read_str(pid_t pid, const void *addr, char *out, int max) {
    // ArmaString: +0x00=vtable(8), +0x08=length(4), +0x10=data
    uint32_t len = r32(pid, addr + 8);
    if (len == 0 || len >= (uint32_t)max) len = max - 1;
    ssize_t n = mem_read(pid, addr + 0x10, out, len);
    if (n < 0) { out[0] = 0; return -1; }
    out[len] = 0;
    return len;
}

pid_t find_dayz(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    pid_t found = -1;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        pid_t p = atoi(e->d_name);
        if (p <= 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", p);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[256]; int n = fread(buf, 1, sizeof(buf)-1, f); buf[n] = 0;
        fclose(f);
        // Main DayZ process (not the BattlEye service)
        if (strstr(buf, "DayZ.exe") && !strstr(buf, "DayZ_BE.exe"))
            found = p;
    }
    closedir(d);
    return found;
}

// ============================================================
// SECTION 4: WORLDTOSCREEN MATH
// ============================================================

bool world_to_screen(Vector3 world, Vector3 *screen,
                     uint64_t cam_addr, pid_t pid,
                     int sw, int sh) {
    // Read camera view matrix components from game memory
    Vector3 right = rv3(pid, (void*)(cam_addr + OFF_CAM_RIGHT));
    Vector3 up    = rv3(pid, (void*)(cam_addr + OFF_CAM_UP));
    Vector3 fwd   = rv3(pid, (void*)(cam_addr + OFF_CAM_FORWARD));
    Vector3 trans = rv3(pid, (void*)(cam_addr + OFF_CAM_TRANSLATE));
    float   d1    = rfl(pid, (void*)(cam_addr + OFF_CAM_PROJ_D1));
    float   d2    = rfl(pid, (void*)(cam_addr + OFF_CAM_PROJ_D2));

    // Transform world position into view space
    float dx = world.x - trans.x;
    float dy = world.y - trans.y;
    float dz = world.z - trans.z;

    float vx = dx * right.x + dy * right.y + dz * right.z;
    float vy = dx * up.x    + dy * up.y    + dz * up.z;
    float vz = dx * fwd.x   + dy * fwd.y   + dz * fwd.z;

    if (vz < 0.01f) return false; // Behind camera

    // Project to 2D screen coordinates
    screen->x = (sw / 2.0f) + (vx / vz) * d1;
    screen->y = (sh / 2.0f) - (vy / vz) * d2;
    screen->z = vz;

    return (screen->x >= -500 && screen->x < sw + 500 &&
            screen->y >= -500 && screen->y < sh + 500);
}

// ============================================================
// SECTION 5: SDL2 OVERLAY RENDERING
// ============================================================

static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture  *g_tex = NULL;
static uint32_t     *g_pix = NULL;
static int g_w, g_h;
static bool g_quit = false;

// Simple 8x12 bitmap font (compact ASCII 32-126)
static const unsigned char FONT[95][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0}, // space
    {0,0,0,0,0x7E,0x5A,0x5A,0x5A,0x5A,0x7E,0,0}, // full block as fallback — real font would go here
};
// Fill with simple fallback: all chars render as a 6x10 block
// In practice, embed a real 8x12 bitmap font or use SDL2_ttf
// For now, we'll draw characters using a simple algorithm
#define FONT_W 8
#define FONT_H 12

static void put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= g_w || y < 0 || y >= g_h || !a) return;
    uint32_t *p = &g_pix[y * g_w + x];
    uint8_t *c = (uint8_t*)p;
    uint8_t sa = a;
    uint8_t da = 255 - sa;
    c[0] = (c[0] * da + b * sa) / 255;
    c[1] = (c[1] * da + g * sa) / 255;
    c[2] = (c[2] * da + r * sa) / 255;
    c[3] = 255;
}

static void draw_char(int x, int y, char ch, uint8_t r, uint8_t g, uint8_t b) {
    if (ch < 32 || ch > 126) return;
    const unsigned char *glyph = FONT[ch - 32];
    // If the glyph is all zeros (fallback), draw a generic block
    int all_zero = 1;
    for (int i = 0; i < 12; i++) if (glyph[i]) { all_zero = 0; break; }
    if (all_zero) {
        // Draw a simple 5x7 approximation
        int rows[7] = {0x7C, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C};
        for (int cy = 0; cy < 7 && cy + y < g_h; cy++)
            for (int cx = 0; cx < 7 && cx + x < g_w; cx++)
                if (rows[cy] & (1 << (6 - cx)))
                    put_px(x + cx, y + cy + 2, r, g, b, 255);
    } else {
        for (int cy = 0; cy < 12; cy++)
            for (int cx = 0; cx < 8; cx++)
                if (glyph[cy] & (0x80 >> cx))
                    put_px(x + cx, y + cy, r, g, b, 255);
    }
}

static void draw_text(int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b) {
    while (*s) {
        draw_char(x, y, *s, r, g, b);
        x += FONT_W;
        s++;
    }
}

static void draw_box(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int ix = x; ix < x + w && ix < g_w; ix++) {
        if (y >= 0 && y < g_h) put_px(ix, y, r, g, b, a);
        if (y + h - 1 >= 0 && y + h - 1 < g_h) put_px(ix, y + h - 1, r, g, b, a);
    }
    for (int iy = y; iy < y + h && iy < g_h; iy++) {
        if (x >= 0 && x < g_w) put_px(x, iy, r, g, b, a);
        if (x + w - 1 >= 0 && x + w - 1 < g_w) put_px(x + w - 1, iy, r, g, b, a);
    }
}

static void draw_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int iy = y; iy < y + h && iy < g_h; iy++)
        for (int ix = x; ix < x + w && ix < g_w; ix++)
            put_px(ix, iy, r, g, b, a);
}

static void draw_line(int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        put_px(x1, y1, r, g, b, 255);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

// ============================================================
// SECTION 6: OVERLAY INIT AND MAIN LOOP HELPERS
// ============================================================

int overlay_init(const char *title, int w, int h) {
    g_w = w; g_h = h;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1; }

    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             w, h, SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_BORDERLESS);
    if (!g_win) return -1;

    SDL_SetWindowOpacity(g_win, 0.99f);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) return -1;

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_pix = calloc(1, w * h * 4);
    return 0;
}

void overlay_clear(void) {
    memset(g_pix, 0, g_w * g_h * 4);
}

void overlay_present(void) {
    SDL_UpdateTexture(g_tex, NULL, g_pix, g_w * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

void overlay_close(void) {
    free(g_pix);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

void overlay_input(Config *cfg) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { g_quit = true; return; }
        if (e.type != SDL_KEYDOWN) continue;

        switch (e.key.keysym.sym) {
            case SDLK_INSERT:  cfg->show_menu = !cfg->show_menu; break;
            case SDLK_UP:      if (cfg->menu_sel > 0) cfg->menu_sel--; break;
            case SDLK_DOWN:    if (cfg->menu_sel < 19) cfg->menu_sel++; break;
            case SDLK_RETURN:
            case SDLK_SPACE: {
                // Toggle the option at current selection
                bool *opts[] = { &cfg->esp_enabled, &cfg->esp_boxes, &cfg->esp_lines,
                                 &cfg->esp_names, &cfg->esp_distances, &cfg->esp_items,
                                 &cfg->esp_zombies, &cfg->esp_animals, &cfg->esp_health,
                                 &cfg->no_grass };
                int n = sizeof(opts)/sizeof(opts[0]);
                if (cfg->menu_sel >= 0 && cfg->menu_sel < n)
                    *opts[cfg->menu_sel] = !*opts[cfg->menu_sel];
                break;
            }
            case SDLK_ESCAPE:  g_quit = true; break;
            default: break;
        }
    }
}

// ============================================================
// SECTION 7: DRAWING THE MENU AND ESP
// ============================================================

void draw_menu(Config *cfg, int sw, int sh) {
    if (!cfg->show_menu) return;

    const char *items[] = {
        "ESP Enabled", "ESP Boxes", "ESP Lines",
        "ESP Names", "ESP Distances", "ESP Items",
        "ESP Zombies", "ESP Animals", "ESP Health",
        "No Grass"
    };
    bool *vars[] = { &cfg->esp_enabled, &cfg->esp_boxes, &cfg->esp_lines,
                     &cfg->esp_names, &cfg->esp_distances, &cfg->esp_items,
                     &cfg->esp_zombies, &cfg->esp_animals, &cfg->esp_health,
                     &cfg->no_grass };
    int n = sizeof(items)/sizeof(items[0]);

    int mx = 30, my = 40, mw = 260, mh = n * 22 + 20;
    draw_fill(mx, my, mw, mh, 15, 15, 25, 210);
    draw_box(mx, my, mw, mh, 80, 180, 255, 220);

    for (int i = 0; i < n; i++) {
        int iy = my + 10 + i * 22;
        if (i == cfg->menu_sel)
            draw_fill(mx + 3, iy - 2, mw - 6, 20, 50, 60, 100, 180);

        char buf[64];
        snprintf(buf, sizeof(buf), "%s: [%s]", items[i], *vars[i] ? "ON" : "OFF");
        draw_text(mx + 10, iy, buf, *vars[i] ? 100 : 180, *vars[i] ? 255 : 180, *vars[i] ? 100 : 180);
    }
}

void draw_esp(EntityInfo *ents, int cnt, Vector3 lpos, Config *cfg,
              int sw, int sh, pid_t pid, uint64_t cam) {
    if (!cfg->esp_enabled || !cam) return;

    for (int i = 0; i < cnt; i++) {
        EntityInfo *e = &ents[i];
        if (e->distance > cfg->esp_max_dist) continue;
        if (e->category == ENT_ITEM && !cfg->esp_items) continue;
        if (e->category == ENT_ZOMBIE && !cfg->esp_zombies) continue;
        if (e->category == ENT_ANIMAL && !cfg->esp_animals) continue;

        Vector3 sp, shp;
        bool on = world_to_screen(e->position, &sp, cam, pid, sw, sh);
        bool hv = world_to_screen(e->head_pos, &shp, cam, pid, sw, sh);

        if (!on) continue;

        float hm = fabs(e->position.z - e->head_pos.z);
        if (hm < 0.3f) hm = 1.8f;
        float hp = (hm / e->distance) * (sw / 1.4f);
        if (hp < 8) hp = 8;
        float wp = hp * 0.35f;
        if (wp < 4) wp = 4;

        int bx = sp.x - wp/2;
        int by = sp.y - hp;

        uint8_t r=200, g=200, b=200;
        switch (e->category) {
            case ENT_PLAYER: r=255; g=60;  b=60;  break;
            case ENT_ZOMBIE: r=255; g=180; b=40;  break;
            case ENT_ANIMAL: r=40;  g=220; b=40;  break;
            case ENT_ITEM:   r=80;  g=180; b=255; break;
            default: break;
        }

        if (cfg->esp_boxes)
            draw_box(bx, by, wp, hp, r, g, b, 220);

        if (cfg->esp_lines)
            draw_line(sp.x, by + hp, sw/2, sh, r, g, b);

        if (cfg->esp_names && e->name[0]) {
            int tx = sp.x - strlen(e->name) * (FONT_W/2);
            draw_text(tx, by - FONT_H - 2, e->name, r, g, b);
        }

        if (cfg->esp_distances) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fm", e->distance);
            int tx = sp.x - strlen(buf) * (FONT_W/2);
            draw_text(tx, by + hp + 2, buf, 255, 255, 255);
        }

        // Health bar for players
        if (cfg->esp_health && e->category == ENT_PLAYER) {
            int bw = 30, bh = 4;
            int bx2 = bx + wp/2 - bw/2;
            int by2 = by + hp + 14;
            int fill = (e->health / 100.0f) * bw;
            if (fill > bw) fill = bw;
            for (int px = bx2; px < bx2 + bw && px < g_w; px++)
                for (int py = by2; py < by2 + bh && py < g_h; py++)
                    put_px(px, py, 200, 30, 30, 200);
            for (int px = bx2; px < bx2 + fill && px < g_w; px++)
                for (int py = by2; py < by2 + bh && py < g_h; py++)
                    put_px(px, py, 50, 200, 50, 200);
        }
    }
}

// ============================================================
// SECTION 8: MAIN
// ============================================================

static Config cfg = {
    .esp_enabled = true, .esp_boxes = true, .esp_lines = false,
    .esp_names = true, .esp_distances = true, .esp_items = true,
    .esp_zombies = true, .esp_animals = true, .esp_health = true,
    .esp_max_dist = 300.0f, .no_grass = false, .show_menu = true, .menu_sel = 0
};

static pid_t dayz_pid = -1;
static const int SW = 1920, SH = 1080;

void on_signal(int s) { (void)s; overlay_close(); printf("\nQuit.\n"); exit(0); }

int main() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("=== DayZ ESP Research Tool ===\n");
    printf("Authorized pentest use only.\n\n");

    // Find DayZ
    printf("[*] Looking for DayZ process...\n");
    for (int r = 0; r < 30 && dayz_pid < 0; r++) {
        dayz_pid = find_dayz();
        if (dayz_pid < 0) { printf("  Not found, retry %d/30\n", r+1); sleep(2); }
    }
    if (dayz_pid < 0) { fprintf(stderr, "[-] DayZ not running.\n"); return 1; }
    printf("[+] DayZ PID: %d\n", dayz_pid);

    // Init overlay
    if (overlay_init("DayZ ESP", SW, SH) < 0) {
        fprintf(stderr, "[-] Overlay init failed\n"); return 1;
    }
    printf("[+] Overlay active. INSERT=menu, ESC=quit.\n");

    // State
    uint64_t world = 0, cam = 0, local = 0;
    Vector3 lpos = {0};
    EntityInfo ents[512];
    int frame = 0;

    while (!g_quit) {
        overlay_input(&cfg);

        // Read world pointer
        if (WORLD_STATIC_ADDR != 0)
            world = r64(dayz_pid, (void*)WORLD_STATIC_ADDR);

        if (!world) {
            overlay_clear();
            draw_text(50, 50, "WORLD_STATIC_ADDR not set!", 255, 200, 50);
            draw_text(50, 70, "1. Start DayZ, kill DayZ_BE.exe", 200, 200, 200);
            draw_text(50, 90, "2. Open Cheat Engine under Wine, attach to DayZ.exe", 200, 200, 200);
            draw_text(50, 110, "3. Search your XYZ pos, follow ptr chain to static addr", 200, 200, 200);
            draw_text(50, 130, "4. Put that addr in WORLD_STATIC_ADDR and recompile", 200, 200, 200);
            overlay_present();
            usleep(100000);
            frame++;
            continue;
        }

        cam   = r64(dayz_pid, (void*)(world + OFF_CAMERA));
        local = r64(dayz_pid, (void*)(world + OFF_LOCAL_PLAYER));
        if (local) lpos = rv3(dayz_pid, (void*)(local + OFF_POSITION));

        // Read entities
        int ec = 0;

        // Near table (players, zombies, animals)
        uint64_t nt = world + OFF_NEAR_TABLE;
        uint64_t arr = r64(dayz_pid, (void*)nt);
        uint32_t nc  = r32(dayz_pid, (void*)(nt + 8));
        if (arr && nc > 0 && nc < 300) {
            uint64_t *addrs = malloc(nc * 8);
            if (mem_read(dayz_pid, (void*)arr, addrs, nc * 8) == (ssize_t)(nc * 8)) {
                for (uint32_t i = 0; i < nc && ec < 512; i++) {
                    if (!addrs[i]) continue;
                    Vector3 pos = rv3(dayz_pid, (void*)(addrs[i] + OFF_POSITION));
                    if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;
                    float dist = sqrt(pow(pos.x - lpos.x,2)+pow(pos.y - lpos.y,2)+pow(pos.z - lpos.z,2));
                    if (dist > cfg.esp_max_dist) continue;

                    ents[ec].address  = addrs[i];
                    ents[ec].position = pos;
                    ents[ec].head_pos = rv3(dayz_pid, (void*)(addrs[i] + OFF_HEAD_POS));
                    ents[ec].distance = dist;
                    ents[ec].health   = rfl(dayz_pid, (void*)(addrs[i] + OFF_HEALTH));

                    // Read name
                    uint64_t np = r64(dayz_pid, (void*)(addrs[i] + OFF_CONFIG_NAME));
                    if (np) read_str(dayz_pid, (void*)np, ents[ec].name, 63);
                    else    snprintf(ents[ec].name, 63, "Entity_%d", i);

                    // Read type
                    uint64_t tp = r64(dayz_pid, (void*)(addrs[i] + OFF_TYPE_NAME));
                    if (tp) {
                        char tn[32]; read_str(dayz_pid, (void*)tp, tn, 31);
                        strncpy(ents[ec].type_str, tn, 31);
                        if      (strstr(tn, "Survivor") || strstr(tn, "Player")) ents[ec].category = ENT_PLAYER;
                        else if (strstr(tn, "Zombie") || strstr(tn, "Infected")) ents[ec].category = ENT_ZOMBIE;
                        else if (strstr(tn, "Cow") || strstr(tn, "Deer") || strstr(tn, "Sheep") ||
                                 strstr(tn, "Goat") || strstr(tn, "Pig") || strstr(tn, "Chicken") ||
                                 strstr(tn, "Turkey") || strstr(tn, "Animal")) ents[ec].category = ENT_ANIMAL;
                        else ents[ec].category = ENT_ITEM;
                    } else ents[ec].category = ENT_UNKNOWN;
                    ec++;
                }
            }
            free(addrs);
        }

        // Items table
        if (cfg.esp_items) {
            uint64_t it = world + OFF_ITEM_TABLE;
            uint64_t iarr = r64(dayz_pid, (void*)it);
            uint32_t ic   = r32(dayz_pid, (void*)(it + 8));
            if (iarr && ic > 0 && ic < 500) {
                uint64_t *addrs = malloc(ic * 8);
                if (mem_read(dayz_pid, (void*)iarr, addrs, ic * 8) == (ssize_t)(ic * 8)) {
                    for (uint32_t i = 0; i < ic && ec < 512; i++) {
                        if (!addrs[i]) continue;
                        Vector3 pos = rv3(dayz_pid, (void*)(addrs[i] + OFF_POSITION));
                        if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;
                        float dist = sqrt(pow(pos.x - lpos.x,2)+pow(pos.y - lpos.y,2)+pow(pos.z - lpos.z,2));
                        if (dist > cfg.esp_max_dist) continue;
                        ents[ec].address = addrs[i];
                        ents[ec].position = pos;
                        ents[ec].head_pos = pos;
                        ents[ec].distance = dist;
                        ents[ec].category = ENT_ITEM;
                        ents[ec].health = 100;
                        uint64_t np = r64(dayz_pid, (void*)(addrs[i] + OFF_CONFIG_NAME));
                        if (np) read_str(dayz_pid, (void*)np, ents[ec].name, 63);
                        else    snprintf(ents[ec].name, 63, "Item_%d", i);
                        ec++;
                    }
                }
                free(addrs);
            }
        }

        // No grass
        if (cfg.no_grass && world) {
            float zero = 0.0f;
            struct iovec li = { .iov_base = &zero, .iov_len = 4 };
            struct iovec ri = { .iov_base = (void*)(world + OFF_GRASS), .iov_len = 4 };
            process_vm_writev(dayz_pid, &li, 1, &ri, 1, 0);
        }

        // --- RENDER ---
        overlay_clear();

        // HUD
        char info[128];
        snprintf(info, sizeof(info), "DayZ ESP | PID: %d | Entities: %d | Range: %.0fm",
                 dayz_pid, ec, cfg.esp_max_dist);
        draw_text(10, 10, info, 150, 200, 255);
        snprintf(info, sizeof(info), "Local: (%.0f, %.0f, %.0f)", lpos.x, lpos.y, lpos.z);
        draw_text(10, 25, info, 150, 200, 255);

        draw_esp(ents, ec, lpos, &cfg, SW, SH, dayz_pid, cam);
        draw_menu(&cfg, SW, SH);

        overlay_present();
        usleep(33000);
        frame++;
    }

    overlay_close();
    printf("Done.\n");
    return 0;
}
