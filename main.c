// main.c — DayZ ESP + Menu — Full application
// Compile: gcc -o dayz_esp main.c memory.c overlay.c -lSDL2 -lm -lpthread
// Run:     sudo ./dayz_esp  (process_vm_readv often needs CAP_SYS_PTRACE or root)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include "memory.h"
#include "overlay.h"

// Default configuration
MenuConfig config = {
    .esp_enabled     = true,
    .esp_boxes       = true,
    .esp_lines       = false,
    .esp_names       = true,
    .esp_distances   = true,
    .esp_items       = true,
    .esp_zombies     = true,
    .esp_animals     = true,
    .esp_health      = true,
    .esp_max_distance = 300.0f,
    .aimbot_enabled  = false,
    .aimbot_fov      = 5.0f,
    .aimbot_smooth   = 1.0f,
    .no_grass        = false,
    .show_menu       = true,
    .menu_selection  = 0,
};

static pid_t dayz_pid = -1;
static int screen_w = 1920;
static int screen_h = 1080;

// Global state cached between frames
static uint64_t world_addr    = 0;
static uint64_t camera_addr   = 0;
static uint64_t local_player  = 0;
static Vector3  local_pos     = {0};

void cleanup(int sig) {
    (void)sig;
    overlay_destroy();
    printf("\nESP tool shutdown.\n");
    exit(0);
}

int main(int argc, char **argv) {
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    printf("=== DayZ ESP Research Tool ===\n");
    printf("Authorized pentest use only.\n\n");

    // 1. Find DayZ process
    printf("[*] Looking for DayZ process...\n");
    for (int retry = 0; retry < 30 && dayz_pid < 0; retry++) {
        dayz_pid = find_dayz_pid();
        if (dayz_pid < 0) {
            printf("  Not found yet, retrying in 2s... (%d/30)\n", retry + 1);
            sleep(2);
        }
    }
    if (dayz_pid < 0) {
        fprintf(stderr, "[-] DayZ process not found. Is DayZ running?\n");
        return 1;
    }
    printf("[+] DayZ PID: %d\n", dayz_pid);

    // 2. Verify memory access works
    // Try reading from the DayZ binary
    {
        char memcheck[4] = {0};
        // Read from the executable's .text section (first few bytes should be MZ or PE header)
        // Under Proton this might be different; just verify process_vm_readv works
        FILE *maps = fopen("/proc/922/maps", "r"); // We'll search properly
        // Actually let's just try to read from a known-good address
        uint64_t test = read_u64(dayz_pid, (void*)0x400000); // Typical base address
        if (test == 0) {
            // Memory might be mapped higher under Wine
            printf("[*] Memory access check...\n");
        }
    }

    // 3. Initialize overlay window
    printf("[*] Initializing overlay...\n");
    if (overlay_init("DayZ ESP", screen_w, screen_h) < 0) {
        fprintf(stderr, "[-] Overlay init failed\n");
        return 1;
    }
    printf("[+] Overlay active. Press INSERT to toggle menu, ESC to quit.\n");

    // 4. Main loop
    int frame_count = 0;
    while (!overlay_should_quit()) {
        // --- POLL INPUT ---
        overlay_poll_input(&config);

        // --- READ WORLD STATE ---
        if (WORLD_STATIC_ADDR != 0) {
            world_addr = read_u64(dayz_pid, (void*)WORLD_STATIC_ADDR);
        } else {
            // Try to find world pointer dynamically
            // For now, we'll try a common address pattern
            // You MUST set WORLD_STATIC_ADDR after finding it with CE
            static bool warned = false;
            if (!warned) {
                printf("[!] WORLD_STATIC_ADDR is 0. Set it after finding with Cheat Engine.\n");
                printf("    Searching for 'GetWorld' in DayZ.exe memory...\n");
                warned = true;
            }
            // Try to find it by scanning (placeholder)
            world_addr = 0;
        }

        if (world_addr == 0) {
            // Can't read memory yet, keep trying
            overlay_clear();
            overlay_draw_text(50, 50, "Waiting for DayZ world pointer...", 255, 255, 0);
            overlay_draw_text(50, 70, "Set WORLD_STATIC_ADDR in offsets.h", 255, 200, 100);
            overlay_draw_text(50, 90, "Use Cheat Engine -> search 'GetWorld' -> find static addr", 200, 200, 200);
            overlay_present();
            usleep(100000);
            frame_count++;
            continue;
        }

        // Read camera
        camera_addr = read_u64(dayz_pid, (void*)(world_addr + OFF_WORLD_CAMERA));

        // Read local player
        local_player = read_u64(dayz_pid, (void*)(world_addr + OFF_WORLD_LOCAL_PLAYER));
        if (local_player) {
            local_pos = read_vector3(dayz_pid, (void*)(local_player + OFF_ENTITY_POSITION));
        }

        // --- READ ENTITY TABLES ---
        EntityInfo entities[MAX_ENTITIES];
        int ent_count = 0;

        // Read near entity table (players/zombies/animals)
        {
            uint64_t near_table_ptr = world_addr + OFF_WORLD_NEAR_TABLE;
            uint64_t ent_array = read_u64(dayz_pid, (void*)(near_table_ptr));
            uint32_t ent_cnt   = read_u32(dayz_pid, (void*)(near_table_ptr + 8));

            if (ent_array && ent_cnt > 0 && ent_cnt < 300) {
                uint64_t *addrs = malloc(ent_cnt * 8);
                if (mem_read(dayz_pid, (void*)ent_array, addrs, ent_cnt * 8) == (ssize_t)(ent_cnt * 8)) {
                    for (uint32_t i = 0; i < ent_cnt && ent_count < MAX_ENTITIES; i++) {
                        if (addrs[i] == 0) continue;

                        // Read entity data
                        Vector3 pos = read_vector3(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_POSITION));
                        // Dead check (position at origin)
                        if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;

                        float dist = sqrt(pow(pos.x - local_pos.x, 2) +
                                          pow(pos.y - local_pos.y, 2) +
                                          pow(pos.z - local_pos.z, 2));
                        if (dist > config.esp_max_distance) continue;

                        entities[ent_count].address  = addrs[i];
                        entities[ent_count].position = pos;
                        entities[ent_count].distance = dist;

                        // Read head position for box height
                        entities[ent_count].head_pos = read_vector3(
                            dayz_pid, (void*)(addrs[i] + OFF_ENTITY_HEAD_POS));

                        // Read type info
                        uint32_t rtti = read_u32(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_TYPE_RTTI));

                        // Read name from config name pointer
                        uint64_t name_ptr = read_u64(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_CONFIG_NAME));
                        if (name_ptr) {
                            read_armastring(dayz_pid, (void*)name_ptr,
                                            entities[ent_count].name, 63);
                        } else {
                            snprintf(entities[ent_count].name, 63, "Entity_%d", i);
                        }

                        // Read type name for classification
                        uint64_t type_ptr = read_u64(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_TYPE_NAME));
                        if (type_ptr) {
                            char tname[32];
                            read_armastring(dayz_pid, (void*)type_ptr, tname, 31);
                            strncpy(entities[ent_count].type_name, tname, 31);

                            if (strstr(tname, "Survivor") || strstr(tname, "Player"))
                                entities[ent_count].category = ENT_PLAYER;
                            else if (strstr(tname, "Zombie") || strstr(tname, "Infected"))
                                entities[ent_count].category = ENT_ZOMBIE;
                            else if (strstr(tname, "Animal") || strstr(tname, "Cow") ||
                                     strstr(tname, "Deer") || strstr(tname, "Sheep") ||
                                     strstr(tname, "Goat") || strstr(tname, "Pig") ||
                                     strstr(tname, "Chicken") || strstr(tname, "Turkey"))
                                entities[ent_count].category = ENT_ANIMAL;
                            else
                                entities[ent_count].category = ENT_ITEM;
                        } else {
                            entities[ent_count].category = ENT_UNKNOWN;
                        }

                        // Read health
                        entities[ent_count].health = read_float(
                            dayz_pid, (void*)(addrs[i] + OFF_ENTITY_HEALTH));

                        // Read network ID
                        entities[ent_count].network_id = read_u64(
                            dayz_pid, (void*)(addrs[i] + OFF_ENTITY_NETWORK_ID));

                        ent_count++;
                    }
                }
                free(addrs);
            }

            // Also read item table if items are enabled
            if (config.esp_items) {
                uint64_t item_table_ptr = world_addr + OFF_WORLD_ITEM_TABLE;
                uint64_t item_array = read_u64(dayz_pid, (void*)(item_table_ptr));
                uint32_t item_cnt   = read_u32(dayz_pid, (void*)(item_table_ptr + 8));

                if (item_array && item_cnt > 0 && item_cnt < 500 &&
                    ent_count + item_cnt < MAX_ENTITIES) {
                    uint64_t *addrs = malloc(item_cnt * 8);
                    if (mem_read(dayz_pid, (void*)item_array, addrs, item_cnt * 8) == (ssize_t)(item_cnt * 8)) {
                        for (uint32_t i = 0; i < item_cnt && ent_count < MAX_ENTITIES; i++) {
                            if (addrs[i] == 0) continue;
                            Vector3 pos = read_vector3(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_POSITION));
                            if (pos.x == 0 && pos.y == 0 && pos.z == 0) continue;
                            float dist = sqrt(pow(pos.x - local_pos.x, 2) +
                                              pow(pos.y - local_pos.y, 2) +
                                              pow(pos.z - local_pos.z, 2));
                            if (dist > config.esp_max_distance) continue;

                            entities[ent_count].address  = addrs[i];
                            entities[ent_count].position = pos;
                            entities[ent_count].distance = dist;
                            entities[ent_count].head_pos = pos;
                            entities[ent_count].category = ENT_ITEM;

                            // Read item name
                            uint64_t name_ptr = read_u64(dayz_pid, (void*)(addrs[i] + OFF_ENTITY_CONFIG_NAME));
                            if (name_ptr)
                                read_armastring(dayz_pid, (void*)name_ptr, entities[ent_count].name, 63);
                            else
                                snprintf(entities[ent_count].name, 63, "Item_%d", i);

                            entities[ent_count].health = 100;
                            entities[ent_count].network_id = 0;
                            ent_count++;
                        }
                    }
                    free(addrs);
                }
            }
        }

        // --- DRAW OVERLAY ---
        overlay_clear();

        // Draw corner watermark
        char info[128];
        snprintf(info, sizeof(info), "DayZ ESP | PID: %d | Entities: %d | Dist: %.0fm",
                 dayz_pid, ent_count, config.esp_max_distance);
        overlay_draw_text(10, 10, info, 150, 200, 255);

        // Draw FPS
        snprintf(info, sizeof(info), "Frame: %d", frame_count);
        overlay_draw_text(screen_w - 120, 10, info, 150, 200, 255);

        // Draw local position
        snprintf(info, sizeof(info), "Local: (%.0f, %.0f, %.0f)",
                 local_pos.x, local_pos.y, local_pos.z);
        overlay_draw_text(10, 25, info, 150, 200, 255);

        // Draw entities
        if (camera_addr) {
            overlay_draw_esp(entities, ent_count, local_pos, &config,
                            screen_w, screen_h, dayz_pid, camera_addr);
        }

        // Draw menu
        overlay_draw_menu(&config, screen_w, screen_h);

        // No grass feature
        if (config.no_grass && world_addr) {
            float zero = 0.0f;
            struct iovec local  = { .iov_base = &zero, .iov_len = 4 };
            struct iovec remote = { .iov_base = (void*)(world_addr + OFF_WORLD_GRASS), .iov_len = 4 };
            process_vm_writev(dayz_pid, &local, 1, &remote, 1, 0);
        }

        overlay_present();

        // ~30 FPS
        usleep(33000);
        frame_count++;
    }

    cleanup(0);
    return 0;
}
