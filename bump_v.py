path_util = '/home/pi/piTrove/src/util.h'
with open(path_util, 'r') as f:
    c = f.read()
c = c.replace('#define VERSION "14.7.5"', '#define VERSION "14.7.7"')
c = c.replace('#define VERSION "14.7.6"', '#define VERSION "14.7.7"')
with open(path_util, 'w') as f:
    f.write(c)
print('Updated util.h to 14.7.7')

path_cl = '/home/pi/piTrove/CHANGELOG.md'
with open(path_cl, 'r') as f:
    cl = f.read()

new_entry = '''## v14.7.7 — Video Decoder EOF Flush, Framerate Pacing & Transition Fix (July 18, 2026)

### Fixed
- **Video Decoder EOF Infinite Flush Loop & Docker Crash** — Resolved infinite packet sending loop during EOF decoder flush when avcodec_receive_frame returned AVERROR_EOF (-541478725), preventing log spam and Docker container crashes.
- **Video Framerate Pacing** — Synchronized video frame rendering with presentation timestamps (frame.pts) relative to wall-clock time (SDL_GetTicks()) and ensured pacing variables are reset when switching videos, fixing fast playback.
- **Frozen Last Frame & Video Lifecycle** — Corrected decoder completion check in main.cpp so slideshow transitions immediately when all frames are rendered, and refactored VideoDecoder lifecycle (start/stop) to ensure threads and SDL audio streams are always joined and shut down cleanly.

'''

marker = '# Changelog\n\n'
if marker in cl:
    cl = cl.replace(marker, marker + new_entry)
else:
    cl = new_entry + cl

with open(path_cl, 'w') as f:
    f.write(cl)
print('Updated CHANGELOG.md')
