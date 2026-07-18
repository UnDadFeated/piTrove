#!/usr/bin/env python3
"""Update main.cpp cache load query to include fps."""
with open("src/main.cpp", "r") as f:
    c = f.read()

# Update the SELECT query to include fps
old1 = '"SELECT path, type, w, h, duration, exif, last_shown, is_camera FROM cache WHERE bad = 0;",'
new1 = '"SELECT path, type, w, h, duration, fps, exif, last_shown, is_camera FROM cache WHERE bad = 0;",'
c = c.replace(old1, new1)

# Update the column reads after fps (col index 5)
old2 = 'mi.duration = sqlite3_column_double(stmt, 4);'
new2 = 'mi.duration = sqlite3_column_double(stmt, 4);\n                mi.fps = sqlite3_column_double(stmt, 5);'
c = c.replace(old2, new2)

with open("src/main.cpp", "w") as f:
    f.write(c)
print("OK main.cpp cache load updated for FPS")
