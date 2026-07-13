#!/usr/bin/env python3
"""
DayZ Offset Dumper for Linux/Proton
Saves current offsets to a config file.
"""

import os
import sys
import struct
import subprocess
import json

SIGNATURES = {
    "OFF_WORLD": {
        # 48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 8B 01 48 8B 40 ? 48 85 C0
        "pattern": b"\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x88\x00\x00\x00\x00\x48\x8B\x01\x48\x8B\x40\x00\x48\x85\xC0",
        "mask": "x??xxx??xxx??xxxxxx?xxx",
    },
    "OFF_WORLD_LOCAL_PLAYER": {
        # 48 8B 05 ? ? ? ? 48 8B 80 ? ? ? ? 48 8B 40 ? 48 85 C0 74 ? 48 8B 40
        "pattern": b"\x48\x8B\x05\x00\x00\x00\x00\x48\x8B\x80\x00\x00\x00\x00\x48\x8B\x40\x00\x48\x85\xC0\x74\x00\x48\x8B\x40",
        "mask": "x??xxx??xxx??xxxxx?xxx",
    },
}


def find_pattern(data, pattern, mask):
    """Simple pattern scan in memory."""
    pattern_len = len(pattern)
    data_len = len(data)
    
    for i in range(data_len - pattern_len):
        match = True
        for j in range(pattern_len):
            if mask[j] == 'x' and data[i + j] != pattern[j]:
                match = False
                break
        if match:
            return i
    return -1


def dump_offsets():
    pid = None
    try:
        pid = int(subprocess.check_output(["pgrep", "-f", "DayZ_x64.exe"]).decode().strip())
    except:
        print("[!] DayZ not running.")
        return
    
    print(f"[+] DayZ PID: {pid}")
    
    # Read the executable from /proc/pid/exe
    maps = open(f"/proc/{pid}/maps").read()
    
    # Find the .text section of DayZ_x64.exe
    base = None
    text_start = None
    text_end = None
    
    for line in maps.split("\n"):
        if "DayZ_x64.exe" in line:
            parts = line.split()
            addr_start = int(parts[0].split("-")[0], 16)
            addr_end = int(parts[0].split("-")[1], 16)
            perms = parts[1]
            
            if "r-xp" in perms and base is None:
                base = addr_start
                text_start = addr_start
                text_end = addr_end
            
            if "r--p" in perms and base is not None:
                # Found the .text section (first r-xp region)
                break
    
    if not base:
        print("[!] Could not find DayZ base in maps")
        return
    
    print(f"[+] Base: 0x{base:x}")
    print(f"[+] Text: 0x{text_start:x} - 0x{text_end:x}")
    
    # Read the .text section
    text_size = text_end - text_start
    mem = open(f"/proc/{pid}/mem", "rb")
    
    offsets = {}
    
    for name, sig in SIGNATURES.items():
        mem.seek(text_start)
        data = mem.read(text_size)
        
        pos = find_pattern(data, sig["pattern"], sig["mask"])
        if pos >= 0:
            # The offset is encoded in the RIP-relative addressing
            # Usually at byte 3 of the pattern
            rel_offset = struct.unpack("<i", data[pos + 3:pos + 7])[0]
            # RIP = address of next instruction = pattern_addr + 7
            rip = text_start + pos + 7
            target = rip + rel_offset
            offset = target - base
            offsets[name] = offset
            print(f"  {name} = 0x{offset:x}")
        else:
            print(f"  {name} = NOT FOUND")
    
    mem.close()
    
    # Now read the world to verify
    mem = open(f"/proc/{pid}/mem", "rb")
    if "OFF_WORLD" in offsets:
        mem.seek(base + offsets["OFF_WORLD"])
        world = struct.unpack("<Q", mem.read(8))[0]
        print(f"\n[+] World pointer value: 0x{world:x}")
        
        # Try reading entity tables
        offsets_to_try = {
            "O_NEAR_TABLE": 0x0ED0,
            "O_NEAR_SIZE": 0x0ED8,
            "O_FAR_TABLE": 0x1FD0,
            "O_FAR_SIZE": 0x1FD8,
        }
        
        for name, off in offsets_to_try.items():
            mem.seek(world + off)
            val = struct.unpack("<Q" if "SIZE" not in name else "<I", mem.read(8 if "SIZE" not in name else 4))[0]
            print(f"  {name} = 0x{val:x}" if "SIZE" not in name else f"  {name} = {val}")
    
    mem.close()
    
    # Save to file
    with open("dayz_offsets.json", "w") as f:
        json.dump(offsets, f, indent=2)
    print(f"\n[+] Offsets saved to dayz_offsets.json")


if __name__ == "__main__":
    dump_offsets()
