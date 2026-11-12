"""The Duke's Archives library-hall sun-off boxes as rectangles in the 45-degree frame (x' = (x+z)/sqrt2, z' = (z-x)/sqrt2),
after the independent review of 8c6d86c: hall 1 grown 0.5 m toward hall 2 (no strip between them on the walkways) and its
south-east face moved in to leave the open-sky balcony (collision h0049, z' <= 153.1) out; hall 2 split at x' 657.3 so its
east end stays off the corridor junction (n0091/n0092, z' <= 159.4); the narrow box ends above the exit corridor n0090.
Prints the area_overrides lines; verify_hall_boxes.py checks them."""
import numpy as np
Y0, Y1 = 276.0, 357.0
RECTS = {  # name: (x'0, x'1, z'0, z'1)
    "library hall 1": (550.60, 607.30, 153.86, 191.36),
    "library hall 2": (606.76, 657.30, 156.28, 188.28),
    "library hall 2 apse": (657.30, 669.60, 159.60, 188.28),
    "library hall 2 east": (643.75, 650.75, 153.50, 194.27),
}
def world_box(r):
    x0, x1, z0, z1 = r
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    hx, hz = (x1 - x0) / 2, (z1 - z0) / 2
    s = np.sqrt(0.5)
    wx, wz = (cx - cz) * s, (cx + cz) * s   # inverse of x' = (x+z)s, z' = (z-x)s
    return (wx - hx, Y0, wz - hz, wx + hx, Y1, wz + hz)
if __name__ == "__main__":
    for name, r in RECTS.items():
        b = world_box(r)
        print(f"[{name}] box = {b[0]:.2f} {b[1]:.1f} {b[2]:.2f} {b[3]:.2f} {b[4]:.1f} {b[5]:.2f}   yaw = 45")
