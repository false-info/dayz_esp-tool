// offsets.h — DayZ memory layout offsets (version 1.25+ compatible, update as needed)
#pragma once
#include <stdint.h>

// Static address for the World pointer — FIND THIS YOURSELF with Cheat Engine
// This changes every patch. Search for "GetWorld" in IDA or scan with CE.
// Example: 0x40B7D50 for some older versions
#define WORLD_STATIC_ADDR 0x00000000UL  // <-- YOU MUST FIND THIS

// Offsets from the World object
enum WorldOffsets {
    OFF_WORLD_CAMERA           = 0x1B8,    // Camera object
    OFF_WORLD_CAMERA_ON        = 0x28B8,   // Camera enabled flag
    OFF_WORLD_NEAR_TABLE       = 0xEB8,    // Near entity table (players/zombies close)
    OFF_WORLD_FAR_TABLE        = 0x1000,   // Far entity table
    OFF_WORLD_ITEM_TABLE       = 0x1FB8,   // Items/loot table
    OFF_WORLD_BULLET_TABLE     = 0xD70,    // Bullet table
    OFF_WORLD_LOCAL_PLAYER     = 0x15C0,   // Local player pointer
    OFF_WORLD_GRASS            = 0xB80,    // Terrain grid (set to 0 = no grass)
};

// Offsets from each Entity object
enum EntityOffsets {
    OFF_ENTITY_POSITION        = 0x2C,     // Vector3 (12 bytes) — X, Y, Z floats
    OFF_ENTITY_HEAD_POS        = 0xF8,     // Head position for aiming
    OFF_ENTITY_TYPE_RTTI       = 0x148,    // RendererEntityType (int)
    OFF_ENTITY_CONFIG_NAME     = 0xA0,     // Pointer to ArmaString (config class name)
    OFF_ENTITY_TYPE_NAME       = 0x68,     // Pointer to ArmaString (type name like "SurvivorMale")
    OFF_ENTITY_MODEL_NAME      = 0x80,     // Pointer to ArmaString (model name)
    OFF_ENTITY_CLEAN_NAME      = 0x4E0,    // Pointer to ArmaString (display name)
    OFF_ENTITY_NETWORK_ID      = 0x634,    // Unique network ID
    OFF_ENTITY_HEALTH          = 0x1A0,    // Health float (estimated)
    OFF_ENTITY_SKELETON_PLAYER = 0x760,    // Bone structure
    OFF_ENTITY_SKELETON_ZOMBIE = 0x5D0,    // Bone structure for zombies
    OFF_ENTITY_INVENTORY       = 0x5B0,    // Inventory pointer
    OFF_ENTITY_FLAG_DAMAGED    = 0x15D,    // IsDamagedOrDestroyed byte
};

// Offsets from Camera object
enum CameraOffsets {
    OFF_CAM_INV_VIEW_RIGHT     = 0x08,     // 4 floats
    OFF_CAM_INV_VIEW_UP        = 0x14,
    OFF_CAM_INV_VIEW_FORWARD   = 0x20,
    OFF_CAM_INV_VIEW_TRANSLATE = 0x2C,
    OFF_CAM_VIEWPORT_SIZE      = 0x58,
    OFF_CAM_PROJECTION_D1      = 0xD0,
    OFF_CAM_PROJECTION_D2      = 0xDC,
};

// ArmaString: a custom string type used by the Enfusion engine
// Offset 0x00: vtable? (8 bytes)
// Offset 0x08: length (uint32)
// Offset 0x10: data pointer (or inline data if short)
struct ArmaString {
    uint64_t vtable;     // +0x00
    uint32_t length;     // +0x08
    char     data[1];    // +0x10 (variable length, or pointer to external buffer)
};

// Dynamic array (like std::vector) used for entity tables
// First 8 bytes: pointer to array of 8-byte entity references
// Next 4 bytes: count of valid entries
// Next 4 bytes: capacity
