// overlay.c
#include "overlay.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>

static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture  = NULL;
static TTF_Font     *g_font     = NULL;
static uint32_t     *g_pixels   = NULL;
static int           g_w, g_h;
static bool          g_quit = false;

// Simple bitmap font rendering (built-in, no TTF dependency)
// 8x12 monochrome bitmap font for basic ASCII
static const uint8_t FONT8x12[95][12] = {
    // Space through tilde — you'd embed the full font array here
    // I'll include a minimal subset; in practice use SDL2_ttf or load a .fnt
    [0]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    [33] = {0x00,0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x00,0x00,0x00}, // '!'
    // ... full 95-character font would go here (too large for inline)
    // For now we'll use SDL2_ttf if available, or fall back to basic rendering
};

int overlay_init(const char *title, int width, int height) {
    g_w = width;
    g_h = height;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return -1;
    }

    // Create transparent, topmost, click-through window
    g_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        width, height,
        SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SKIP_TASKBAR |
        SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS
    );
    if (!g_window) {
        fprintf(stderr, "Window create failed: %s\n", SDL_GetError());
        return -1;
    }

    // Make window click-through (X11 method)
    // You'll need to set _NET_WM_WINDOW_TYPE_DOCK and input shape
    SDL_SetWindowOpacity(g_window, 0.99f);

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        fprintf(stderr, "Renderer create failed: %s\n", SDL_GetError());
        return -1;
    }

    // Set transparent clear color
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        width, height
    );

    g_pixels = calloc(1, width * height * 4);
    return 0;
}

void overlay_destroy(void) {
    free(g_pixels);
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void overlay_clear(void) {
    // Clear to fully transparent
    memset(g_pixels, 0, g_w * g_h * 4);
}

static void put_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= g_w || y < 0 || y >= g_h || a == 0) return;
    uint32_t *p = &g_pixels[y * g_w + x];
    uint8_t *c = (uint8_t*)p;
    // Alpha blend over existing pixel
    uint8_t src_a = a;
    uint8_t dst_a = 255 - src_a;
    c[0] = (c[0] * dst_a + b * src_a) / 255;
    c[1] = (c[1] * dst_a + g * src_a) / 255;
    c[2] = (c[2] * dst_a + r * src_a) / 255;
    c[3] = 255;
}

void overlay_draw_text(int x, int y, const char *text,
                       uint8_t r, uint8_t g, uint8_t b) {
    // Simple 8x12 bitmap text rendering
    // Each character is 8 pixels wide, 12 pixels tall
    while (*text) {
        int ch = *text - 32; // ASCII to font index
        if (ch < 0 || ch > 94) { text++; x += 8; continue; }

        // Draw character column by column
        for (int cy = 0; cy < 12; cy++) {
            uint8_t row = FONT8x12[ch][cy];
            for (int cx = 0; cx < 8; cx++) {
                if (row & (0x80 >> cx)) {
                    put_pixel(x + cx, y + cy, r, g, b, 255);
                }
            }
        }
        x += 8;
        text++;
    }
}

void overlay_draw_box(int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Draw rectangle outline
    for (int ix = x; ix < x + w; ix++) {
        put_pixel(ix, y, r, g, b, a);
        put_pixel(ix, y + h - 1, r, g, b, a);
    }
    for (int iy = y; iy < y + h; iy++) {
        put_pixel(x, iy, r, g, b, a);
        put_pixel(x + w - 1, iy, r, g, b, a);
    }
}

void overlay_draw_line(int x1, int y1, int x2, int y2,
                       uint8_t r, uint8_t g, uint8_t b) {
    // Bresenham's line algorithm
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        put_pixel(x1, y1, r, g, b, 255);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void overlay_present(void) {
    // Upload pixels to texture and render
    SDL_UpdateTexture(g_texture, NULL, g_pixels, g_w * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

// WorldToScreen implementation
bool world_to_screen(Vector3 world, Vector3 *screen,
                     uint64_t cam_addr, pid_t pid,
                     int sw, int sh) {
    // Read camera view matrix components
    Vector3 right = read_vector3(pid, (void*)(cam_addr + OFF_CAM_INV_VIEW_RIGHT));
    Vector3 up    = read_vector3(pid, (void*)(cam_addr + OFF_CAM_INV_VIEW_UP));
    Vector3 fwd   = read_vector3(pid, (void*)(cam_addr + OFF_CAM_INV_VIEW_FORWARD));
    Vector3 trans = read_vector3(pid, (void*)(cam_addr + OFF_CAM_INV_VIEW_TRANSLATE));
    float   d1    = read_float(pid, (void*)(cam_addr + OFF_CAM_PROJECTION_D1));
    float   d2    = read_float(pid, (void*)(cam_addr + OFF_CAM_PROJECTION_D2));

    // Transform to view space
    Vector3 delta = { world.x - trans.x, world.y - trans.y, world.z - trans.z };

    float view_x = delta.x * right.x + delta.y * right.y + delta.z * right.z;
    float view_y = delta.x * up.x    + delta.y * up.y    + delta.z * up.z;
    float view_z = delta.x * fwd.x   + delta.y * fwd.y   + delta.z * fwd.z;

    // Behind camera check
    if (view_z < 0.01f) return false;

    // Project to screen (simplified — you'll need the actual projection matrix)
    float fov_x = d1; // Usually screen_width / (2 * tan(FOV/2))
    float fov_y = d2; // Usually screen_height / (2 * tan(FOV/2))

    screen->x = (sw / 2.0f) + (view_x / view_z) * fov_x;
    screen->y = (sh / 2.0f) - (view_y / view_z) * fov_y;
    screen->z = view_z; // Depth for sorting

    return (screen->x >= -500 && screen->x < sw + 500 &&
            screen->y >= -500 && screen->y < sh + 500);
}

void overlay_poll_input(MenuConfig *cfg) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                g_quit = true;
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                    case SDLK_INSERT:
                        cfg->show_menu = !cfg->show_menu;
                        break;
                    case SDLK_UP:
                        if (cfg->show_menu && cfg->menu_selection > 0)
                            cfg->menu_selection--;
                        break;
                    case SDLK_DOWN:
                        if (cfg->show_menu && cfg->menu_selection < MAX_MENU_ITEMS - 1)
                            cfg->menu_selection++;
                        break;
                    case SDLK_RETURN:
                    case SDLK_SPACE:
                        // Toggle the selected menu item
                        if (cfg->show_menu) {
                            int *opts[] = {
                                &cfg->esp_enabled, &cfg->esp_boxes, &cfg->esp_lines,
                                &cfg->esp_names, &cfg->esp_distances, &cfg->esp_items,
                                &cfg->esp_zombies, &cfg->esp_animals, &cfg->esp_health,
                                &cfg->aimbot_enabled, &cfg->no_grass
                            };
                            int idx = cfg->menu_selection;
                            if (idx >= 0 && idx < (int)(sizeof(opts)/sizeof(opts[0])))
                                *opts[idx] = !*opts[idx];
                        }
                        break;
                    case SDLK_ESCAPE:
                        g_quit = true;
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }
}

bool overlay_should_quit(void) {
    return g_quit;
}

void overlay_draw_menu(MenuConfig *cfg, int sw, int sh) {
    if (!cfg->show_menu) return;

    // Semi-transparent background
    int mx = 20, my = 40, mw = 280, mh = 320;
    for (int y = my; y < my + mh; y++)
        for (int x = mx; x < mx + mw; x++)
            put_pixel(x, y, 20, 20, 30, 200);

    overlay_draw_box(mx, my, mw, mh, 100, 200, 255, 255);

    char line[64];
    const char *items[] = {
        "ESP Enabled", "ESP Boxes", "ESP Lines",
        "ESP Names", "ESP Distances", "ESP Items",
        "ESP Zombies", "ESP Animals", "ESP Health",
        "Aimbot", "No Grass"
    };
    int enabled[] = {
        cfg->esp_enabled, cfg->esp_boxes, cfg->esp_lines,
        cfg->esp_names, cfg->esp_distances, cfg->esp_items,
        cfg->esp_zombies, cfg->esp_animals, cfg->esp_health,
        cfg->aimbot_enabled, cfg->no_grass
    };
    int num_items = sizeof(items) / sizeof(items[0]);

    for (int i = 0; i < num_items; i++) {
        int iy = my + 10 + i * 25;
        // Highlight selection
        if (i == cfg->menu_selection) {
            for (int x = mx + 2; x < mx + mw - 2; x++)
                for (int y = iy; y < iy + 20; y++)
                    put_pixel(x, y, 60, 60, 80, 128);
        }
        snprintf(line, sizeof(line), "%s: [%s]", items[i],
                 enabled[i] ? "ON" : "OFF");
        overlay_draw_text(mx + 10, iy, line,
                          enabled[i] ? 100 : 200,
                          enabled[i] ? 255 : 200,
                          enabled[i] ? 100 : 200);
    }
}

void overlay_draw_esp(EntityInfo *entities, int count, Vector3 local_pos,
                      MenuConfig *cfg, int sw, int sh,
                      pid_t pid, uint64_t camera_addr) {
    if (!cfg->esp_enabled) return;

    for (int i = 0; i < count; i++) {
        EntityInfo *e = &entities[i];

        // Skip based on filters
        if (e->distance > cfg->esp_max_distance) continue;
        if (e->category == ENT_ITEM && !cfg->esp_items) continue;
        if (e->category == ENT_ZOMBIE && !cfg->esp_zombies) continue;
        if (e->category == ENT_ANIMAL && !cfg->esp_animals) continue;

        // WorldToScreen
        Vector3 screen_pos, screen_head;
        bool on_screen = world_to_screen(e->position, &screen_pos,
                                          camera_addr, pid, sw, sh);
        bool head_visible = world_to_screen(e->head_pos, &screen_head,
                                             camera_addr, pid, sw, sh);
        if (!on_screen) continue;

        // Calculate box height based on player height (about 1.8m)
        float height_m = fabs(e->position.z - e->head_pos.z);
        if (height_m < 0.5f) height_m = 1.8f; // Fallback

        // Box height in pixels (inverse proportional to distance)
        float height_px = (height_m / e->distance) * (sw / 1.2f);
        if (height_px < 5) height_px = 5;
        float width_px = height_px * 0.4f;

        int box_x = screen_pos.x - width_px / 2;
        int box_y = screen_pos.y - height_px;

        // Choose color based on category
        uint8_t r=255, g=255, b=255;
        switch (e->category) {
            case ENT_PLAYER: r=255; g=50;  b=50;  break;  // Red
            case ENT_ZOMBIE: r=255; g=200; b=50;  break;  // Orange
            case ENT_ANIMAL: r=50;  g=200; b=50;  break;  // Green
            case ENT_ITEM:   r=100; g=200; b=255; break;  // Blue
            default:         r=200; g=200; b=200; break;  // Grey
        }

        // ESP Box
        if (cfg->esp_boxes) {
            overlay_draw_box(box_x, box_y, width_px, height_px, r, g, b, 200);
        }

        // ESP Line (from bottom of box to center-bottom of screen)
        if (cfg->esp_lines) {
            overlay_draw_line(screen_pos.x, box_y + height_px,
                              sw / 2, sh, r, g, b);
        }

        // ESP Name
        if (cfg->esp_names && e->name[0]) {
            int tx = screen_pos.x - strlen(e->name) * 4;
            overlay_draw_text(tx, box_y - 14, e->name, r, g, b);
        }

        // ESP Distance
        if (cfg->esp_distances) {
            char dist_str[32];
            snprintf(dist_str, sizeof(dist_str), "%.0fm", e->distance);
            int tx = screen_pos.x - strlen(dist_str) * 4;
            overlay_draw_text(tx, box_y - 2, dist_str, 255, 255, 255);
        }

        // ESP Health bar
        if (cfg->esp_health && e->category == ENT_PLAYER) {
            int bar_w = 30, bar_h = 4;
            int bx = box_x + width_px / 2 - bar_w / 2;
            int by = box_y + height_px + 2;
            // Background (red)
            for (int lx = bx; lx < bx + bar_w; lx++)
                for (int ly = by; ly < by + bar_h; ly++)
                    put_pixel(lx, ly, 200, 30, 30, 200);
            // Health (green portion)
            int fill = (e->health / 100.0f) * bar_w;
            if (fill > bar_w) fill = bar_w;
            for (int lx = bx; lx < bx + fill; lx++)
                for (int ly = by; ly < by + bar_h; ly++)
                    put_pixel(lx, ly, 50, 200, 50, 200);
        }
    }
}
