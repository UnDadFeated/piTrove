#!/usr/bin/env python3
"""Add FPS getter to video_decoder.h"""
with open("src/video_decoder.h", "r") as f:
    c = f.read()

old = " double get_video_remaining() const;"
new = """ double get_video_remaining() const;
 double get_fps() const { return m_frame_duration > 0 ? 1.0 / m_frame_duration : 0; }"""
c = c.replace(old, new)

with open("src/video_decoder.h", "w") as f:
    f.write(c)
print("OK video_decoder.h FPS getter added")
