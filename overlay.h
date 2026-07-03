// overlay.h
#pragma once
#include <stdbool.h>
#include "memory.h"

#define MAX_ENTITIES 512
#define MAX_MENU_ITEMS 32

typedef enum {
    ENT_UNKNOWN,
    ENT_PLAYER,
    ENT_ZOMBIE,
    ENT_ANIMAL,
    ENT_ITEM,
    ENT_BULLET,
} EntityCategory;

typedef struct {
    uint64_t        address;
    Vector3         position;
    Vector3         head_pos;
    float           distance;
    char            name[64];
    char            type_name[32];
    EntityCategory  category;
    uint64_t        network_id;
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
    float esp_max_distance;
    bool aimbot_enabled;
    float aimbot_fov;
    float aimbot_smooth;
    bool no_grass;
    bool show_menu;
    int  menu_selection;
} MenuConfig;

// Overlay functions
int  overlay_init(const char *window_title, int width, int height);
void overlay_destroy(void);
void overlay_clear(void);
void overlay_present(void);
void overlay_draw_text(int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b);
void overlay_draw_box(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void overlay_draw_line(int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b);
void overlay_draw_menu(MenuConfig *cfg, int screen_w, int screen_h);
void overlay_draw_esp(EntityInfo *entities, int count, Vector3 local_pos,
                      MenuConfig *cfg, int screen_w, int screen_h,
                      pid_t pid, uint64_t camera_addr);

// WorldToScreen
bool world_to_screen(Vector3 world_pos, Vector3 *screen_out,
                     uint64_t camera_addr, pid_t pid,
                     int screen_w, int screen_h);

// Input
void overlay_poll_input(MenuConfig *cfg);
bool overlay_should_quit(void);
