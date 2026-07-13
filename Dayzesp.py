"""
DayZ ESP Overlay — Linux/Proton version
Run directly on the gaming PC where DayZ runs under Proton.
Uses /proc/pid/mem for reading and GTK/Cairo for overlay.
"""

import os
import sys
import struct
import time
import threading
import subprocess
import re
import math

# === CONFIGURATION ===
# Update these after dumping fresh offsets!
OFF_WORLD              = 0x40B7D50
OFF_WORLD_CAMERA       = 0x1B8
OFF_WORLD_LOCAL_PLAYER = 0x28A8
OFF_NEAR_TABLE         = 0x0ED0
OFF_NEAR_TABLE_SIZE    = 0x0ED8
OFF_FAR_TABLE          = 0x1FD0
OFF_FAR_TABLE_SIZE     = 0x1FD8
OFF_ITEM_TABLE         = 0x1FD0
OFF_ITEM_TABLE_SIZE    = 0x1FD8

OFF_ENTITY_VISUAL_STATE = 0x198
OFF_ENTITY_TYPE         = 0x148
OFF_ENTITY_TYPE_NAME    = 0xA0
OFF_ENTITY_CLEAN_NAME   = 0x4E0
OFF_ENTITY_IS_DEAD      = 0x15D
OFF_ENTITY_INVENTORY    = 0x5F8
OFF_INVENTORY_HANDS     = 0x658

OFF_VISUAL_STATE_POS    = 0x2C

OFF_CAM_VIEW_RIGHT      = 0x8
OFF_CAM_VIEW_UP         = 0x14
OFF_CAM_VIEW_FORWARD    = 0x20
OFF_CAM_VIEW_TRANSLATE  = 0x2C
OFF_CAM_VIEWPORT_SIZE   = 0x58
OFF_CAM_PROJ_D1         = 0xD0
OFF_CAM_PROJ_D2         = 0xDC

# RTTI type IDs — UPDATE THESE for your DayZ version
TYPE_DAYZ_PLAYER = 0x7265
TYPE_INFECTED    = 0x1234ABCD  # FIND THIS
TYPE_ITEM        = 0xD7DD68DB


def find_dayz_pid():
    """Find the DayZ_x64.exe PID running under Proton."""
    # Method 1: pgrep (most reliable on most distros)
    try:
        result = subprocess.run(
            ["pgrep", "-f", "DayZ_x64.exe"],
            capture_output=True, text=True
        )
        if result.stdout.strip():
            return int(result.stdout.strip().split('\n')[0])  # first PID
    except:
        pass

    # Method 2: ps aux with grep (fallback)
    try:
        result = subprocess.run(
            ["ps", "aux"], capture_output=True, text=True
        )
        for line in result.stdout.split("\n"):
            if "DayZ_x64.exe" in line and "defunct" not in line:
                parts = line.split()
                pid = int(parts[1])
                return pid
    except:
        pass

    return None


def get_base_address(pid):
    """Get DayZ_x64.exe base address from /proc/pid/maps.
    Handles various Proton/Wine path formats."""
    try:
        with open(f"/proc/{pid}/maps", "r") as f:
            for line in f:
                # Check for multiple possible path patterns
                line_lower = line.lower()
                if "dayz_x64.exe" in line_lower or "dayz" in line_lower:
                    # Only consider executable sections (first mapping usually)
                    if "r-xp" in line:
                        return int(line.split("-")[0], 16)
    except:
        pass

    # If not found, try a broader search for any executable with 'dayz' in name
    try:
        with open(f"/proc/{pid}/maps", "r") as f:
            for line in f:
                if "dayz" in line.lower() and "r-xp" in line:
                    return int(line.split("-")[0], 16)
    except:
        pass

    return None


class MemoryReader:
    def __init__(self, pid):
        self.pid = pid
        self.mem_fd = None
        
    def open(self):
        self.mem_fd = os.open(f"/proc/{self.pid}/mem", os.O_RDONLY)
        
    def close(self):
        if self.mem_fd:
            os.close(self.mem_fd)
            
    def read(self, address, size):
        if not self.mem_fd:
            return None
        try:
            os.lseek(self.mem_fd, address, os.SEEK_SET)
            return os.read(self.mem_fd, size)
        except:
            return None
    
    def read_u64(self, address):
        data = self.read(address, 8)
        if data and len(data) == 8:
            return struct.unpack("<Q", data)[0]
        return 0
    
    def read_u32(self, address):
        data = self.read(address, 4)
        if data and len(data) == 4:
            return struct.unpack("<I", data)[0]
        return 0
    
    def read_u8(self, address):
        data = self.read(address, 1)
        if data and len(data) == 1:
            return data[0]
        return 0
    
    def read_float(self, address):
        data = self.read(address, 4)
        if data and len(data) == 4:
            return struct.unpack("<f", data)[0]
        return 0.0
    
    def read_vec3(self, address):
        x = self.read_float(address)
        y = self.read_float(address + 4)
        z = self.read_float(address + 8)
        return (x, y, z)
    
    def read_wstring(self, address, max_len=128):
        """Read Enfusion-style wide string: [ptr][len][capacity]"""
        data_ptr = self.read_u64(address)
        data_len = self.read_u32(address + 8)
        if data_len == 0 or data_len > max_len:
            return ""
        raw = self.read(data_ptr, data_len * 2)
        if raw:
            try:
                return raw.decode("utf-16-le")
            except:
                pass
        return ""
    
    def read_ptr_chain(self, base, offsets):
        addr = base
        for i, off in enumerate(offsets):
            if i == len(offsets) - 1:
                addr = self.read_u64(addr + off) if off != 0 else addr
            else:
                addr = self.read_u64(addr + off)
            if not addr:
                return 0
        return addr


class Vector3:
    def __init__(self, x=0, y=0, z=0):
        self.x, self.y, self.z = x, y, z
    
    def __sub__(self, other):
        return Vector3(self.x - other.x, self.y - other.y, self.z - other.z)
    
    def dot(self, other):
        return self.x * other.x + self.y * other.y + self.z * other.z
    
    def length(self):
        return math.sqrt(self.x**2 + self.y**2 + self.z**2)
    
    def distance(self, other):
        return (self - other).length()


class DayZESP:
    def __init__(self, reader, base_addr):
        self.reader = reader
        self.base = base_addr
        self.world_addr = 0
        self.local_player = 0
        self.local_pos = Vector3()
        
        # Camera
        self.view_right = Vector3()
        self.view_up = Vector3()
        self.view_forward = Vector3()
        self.view_trans = Vector3()
        self.viewport_w = 0
        self.viewport_h = 0
        self.proj_d1 = 0
        self.proj_d2 = 0
        
        self.entities = []
    
    def update(self):
        self.entities.clear()
        
        self.world_addr = self.reader.read_u64(self.base + OFF_WORLD)
        if not self.world_addr:
            return
        
        # Get local player
        local_ptr = self.reader.read_u64(self.world_addr + OFF_WORLD_LOCAL_PLAYER)
        if local_ptr:
            self.local_player = self.reader.read_u64(local_ptr + 0x8) - 0xA8
        
        # Get local position
        if self.local_player:
            vis_state = self.reader.read_u64(self.local_player + OFF_ENTITY_VISUAL_STATE)
            if vis_state:
                pos = self.reader.read_vec3(vis_state + OFF_VISUAL_STATE_POS)
                self.local_pos = Vector3(*pos)
        
        # Get camera
        camera = self.reader.read_u64(self.world_addr + OFF_WORLD_CAMERA)
        if camera:
            self.view_right = Vector3(*self.reader.read_vec3(camera + OFF_CAM_VIEW_RIGHT))
            self.view_up = Vector3(*self.reader.read_vec3(camera + OFF_CAM_VIEW_UP))
            self.view_forward = Vector3(*self.reader.read_vec3(camera + OFF_CAM_VIEW_FORWARD))
            self.view_trans = Vector3(*self.reader.read_vec3(camera + OFF_CAM_VIEW_TRANSLATE))
            self.viewport_w = self.reader.read_float(camera + OFF_CAM_VIEWPORT_SIZE)
            self.viewport_h = self.reader.read_float(camera + OFF_CAM_VIEWPORT_SIZE + 4)
            self.proj_d1 = self.reader.read_float(camera + OFF_CAM_PROJ_D1)
            self.proj_d2 = self.reader.read_float(camera + OFF_CAM_PROJ_D2)
        
        # Enumerate entities
        self._enumerate_near()
        self._enumerate_far()
    
    def world_to_screen(self, world_pos):
        diff = world_pos - self.view_trans
        x = diff.dot(self.view_right)
        y = diff.dot(self.view_up)
        z = diff.dot(self.view_forward)
        
        if z <= 0.01:
            return None
        
        sx = (self.viewport_w / 2) * (1.0 + x / (z * self.proj_d1))
        sy = (self.viewport_h / 2) * (1.0 - y / (z * self.proj_d2))
        
        return (sx, sy)
    
    def _enumerate_near(self):
        table = self.reader.read_u64(self.world_addr + OFF_NEAR_TABLE)
        count = self.reader.read_u32(self.world_addr + OFF_NEAR_TABLE_SIZE)
        if count > 512:
            count = 512
        
        for i in range(count):
            ent = self.reader.read_u64(table + i * 8)
            if ent and ent != self.local_player:
                self._read_entity(ent)
    
    def _enumerate_far(self):
        table = self.reader.read_u64(self.world_addr + OFF_FAR_TABLE)
        count = self.reader.read_u32(self.world_addr + OFF_FAR_TABLE_SIZE)
        if count > 2048:
            count = 2048
        
        for i in range(count):
            ent = self.reader.read_u64(table + i * 8)
            if ent and ent != self.local_player:
                self._read_entity(ent)
    
    def _read_entity(self, addr):
        vis_state = self.reader.read_u64(addr + OFF_ENTITY_VISUAL_STATE)
        if not vis_state:
            return
        
        world_pos_vec = self.reader.read_vec3(vis_state + OFF_VISUAL_STATE_POS)
        world_pos = Vector3(*world_pos_vec)
        
        # Get type info
        type_addr = self.reader.read_u64(addr + OFF_ENTITY_TYPE)
        type_id = 0
        name = ""
        clean_name = ""
        
        if type_addr:
            type_id = self.reader.read_u32(type_addr)
            name = self.reader.read_wstring(type_addr + OFF_ENTITY_TYPE_NAME)
        
        clean_name = self.reader.read_wstring(addr + OFF_ENTITY_CLEAN_NAME)
        is_dead = self.reader.read_u8(addr + OFF_ENTITY_IS_DEAD) != 0
        
        dist = world_pos.distance(self.local_pos)
        screen = self.world_to_screen(world_pos)
        
        # Classify
        is_player = (type_id == TYPE_DAYZ_PLAYER)
        is_zombie = (type_id == TYPE_INFECTED or "infected" in name.lower() 
                     or "zombie" in name.lower())
        if not is_player and not is_zombie:
            if "survivor" in name.lower() or "player" in name.lower():
                is_player = True
        
        item_in_hands = ""
        if is_player:
            inv = self.reader.read_u64(addr + OFF_ENTITY_INVENTORY)
            if inv:
                hands_item = self.reader.read_u64(inv + OFF_INVENTORY_HANDS)
                if hands_item:
                    item_in_hands = self.reader.read_wstring(
                        hands_item + OFF_ENTITY_CLEAN_NAME
                    )
        
        display_name = clean_name if clean_name else name
        
        self.entities.append({
            "addr": addr,
            "name": display_name,
            "world_pos": world_pos,
            "screen": screen,
            "distance": dist,
            "is_player": is_player,
            "is_zombie": is_zombie,
            "is_dead": is_dead,
            "item_in_hands": item_in_hands,
            "type_id": type_id,
        })


# === OVERLAY using GTK/Cairo ===
try:
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    from gi.repository import Gtk, Gdk, GLib, Pango, cairo
    HAS_GTK = True
except:
    HAS_GTK = False
    print("[!] GTK3 not available. Install with: sudo pacman -S gtk3 python-gobject cairo")


class ESPOverlay(Gtk.Window):
    def __init__(self, esp):
        super().__init__(type=Gtk.WindowType.POPUP)
        self.esp = esp
        self.set_title("DayZ ESP")
        
        # Get screen geometry
        display = Gdk.Display.get_default()
        monitor = display.get_primary_monitor()
        geo = monitor.get_geometry()
        self.screen_w = geo.width
        self.screen_h = geo.height
        
        self.set_default_size(self.screen_w, self.screen_h)
        self.set_position(Gtk.WindowPosition.CENTER)
        self.set_app_paintable(True)
        self.set_keep_above(True)
        
        # Transparent background
        screen = self.get_screen()
        visual = screen.get_rgba_visual()
        if visual:
            self.set_visual(visual)
        
        self.set_decorated(False)
        
        # Click-through setup (X11)
        self._set_clickthrough()
        
        self.connect("draw", self.on_draw)
        self.connect("key-press-event", self.on_key)
        
        # Colors
        self.col_player = (1.0, 0.2, 0.2)    # Red
        self.col_zombie = (1.0, 0.8, 0.0)    # Yellow
        self.col_item   = (0.2, 1.0, 0.2)    # Green
        
        self.show_all()
    
    def _set_clickthrough(self):
        """Make window click-through so you can play through it."""
        try:
            gdk_win = self.get_window()
            if gdk_win:
                xid = gdk_win.get_xid()
                subprocess.run([
                    "xprop", "-id", str(xid),
                    "-f", "_NET_WM_WINDOW_TYPE", "32a",
                    "-set", "_NET_WM_WINDOW_TYPE",
                    "_NET_WM_WINDOW_TYPE_DOCK"
                ])
        except:
            pass
    
    def on_key(self, widget, event):
        if event.keyval == Gdk.KEY_End:
            Gtk.main_quit()
        return False
    
    def on_draw(self, widget, ctx):
        ctx.set_operator(cairo.OPERATOR_CLEAR)
        ctx.paint()
        ctx.set_operator(cairo.OPERATOR_OVER)
        
        for ent in self.esp.entities:
            if not ent["screen"]:
                continue
            
            sx, sy = ent["screen"]
            dist = ent["distance"]
            
            if sx < 0 or sx > self.screen_w or sy < 0 or sy > self.screen_h:
                continue
            
            if dist > 500:
                continue
            
            if ent["is_player"]:
                r, g, b = self.col_player
            elif ent["is_zombie"]:
                r, g, b = self.col_zombie
            else:
                r, g, b = self.col_item
            
            box_h = max(20, 1800 / dist)
            box_w = box_h * 0.6
            
            ctx.set_source_rgb(r, g, b)
            ctx.set_line_width(1.5)
            ctx.rectangle(sx - box_w/2, sy - box_h, box_w, box_h)
            ctx.stroke()
            
            name = ent["name"][:20] if ent["name"] else "Unknown"
            ctx.set_source_rgb(r, g, b)
            ctx.select_font_face("Consolas", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
            ctx.set_font_size(11)
            
            ctx.move_to(sx - 50, sy - box_h - 5)
            ctx.show_text(name)
            
            ctx.move_to(sx - 30, sy - box_h - 18)
            ctx.show_text(f"{dist:.0f}m")
            
            if ent["item_in_hands"]:
                ctx.move_to(sx - 50, sy + 5)
                ctx.show_text(f"[{ent['item_in_hands'][:15]}]")
        
        return False


def main():
    print("=== DayZ ESP for Linux/Proton ===")
    print("Make sure DayZ is running.\n")
    
    pid = find_dayz_pid()
    if not pid:
        print("[!] Could not find DayZ_x64.exe process.")
        print("    Is DayZ running via Steam/Proton?")
        sys.exit(1)
    
    print(f"[+] Found DayZ PID: {pid}")
    
    base = get_base_address(pid)
    if not base:
        print("[!] Could not find DayZ base address.")
        # Print maps for debugging
        print("    Dumping /proc/pid/maps for lines containing 'dayz':")
        try:
            with open(f"/proc/{pid}/maps", "r") as f:
                for line in f:
                    if "dayz" in line.lower():
                        print(f"      {line.strip()}")
        except:
            pass
        sys.exit(1)
    
    print(f"[+] DayZ base: 0x{base:x}")
    
    reader = MemoryReader(pid)
    reader.open()
    
    esp = DayZESP(reader, base)
    
    world_check = reader.read_u64(base + OFF_WORLD)
    if not world_check:
        print(f"[!] World pointer at 0x{base+OFF_WORLD:x} is null.")
        print("    Offsets are wrong — update them with a fresh dump.")
        reader.close()
        sys.exit(1)
    
    print(f"[+] World address: 0x{world_check:x}")
    
    if not HAS_GTK:
        print("\n[!] No GTK available. Running in console mode.")
        print("    Install: sudo pacman -S gtk3 python-gobject cairo\n")
        try:
            while True:
                esp.update()
                os.system("clear")
                print(f"Local: ({esp.local_pos.x:.1f}, {esp.local_pos.y:.1f}, {esp.local_pos.z:.1f})")
                print(f"Entities: {len(esp.entities)}\n")
                for ent in sorted(esp.entities, key=lambda e: e["distance"]):
                    tag = "P" if ent["is_player"] else ("Z" if ent["is_zombie"] else "?")
                    screen = ent["screen"]
                    sx_str = f"({screen[0]:.0f},{screen[1]:.0f})" if screen else "OFF"
                    print(f"  [{tag}] {ent['name'][:20]:20s} dist={ent['distance']:5.0f}m  screen={sx_str}")
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
    else:
        overlay = ESPOverlay(esp)
        
        def update_loop():
            while True:
                esp.update()
                overlay.queue_draw()
                time.sleep(0.033)
        
        thread = threading.Thread(target=update_loop, daemon=True)
        thread.start()
        
        Gtk.main()
    
    reader.close()


if __name__ == "__main__":
    main()