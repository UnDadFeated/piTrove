import sys

with open("/home/pi/piTrove/src/main.cpp", "r") as f:
    lines = f.readlines()

replacement = r'''                if (g_video_decoder.is_running()) {
                    VideoFrame frame;
                    bool got_frame = g_video_decoder.get_frame(frame);
                    
                    // BUGFIX: Drain queue even if decoder thread is finishing
                    // When queue is empty but decoder is still winding down, skip this cycle
                    if (!got_frame) {
                        g_logger.debug("VIDEO_DEC: Queue empty, decoder still draining");
                        playlist_lock.unlock();
                        SDL_Delay(5);
                        continue;
                    }
                    
                    if (current_tex) { SDL_DestroyTexture(current_tex); current_tex = nullptr; }
                    current_tex = SDL_CreateTexture(g_renderer.sdl_renderer, SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
                    if (current_tex) {
                        SDL_UpdateTexture(current_tex, nullptr, frame.data, frame.width * 4);
                        g_renderer.clear(0, 0, 0, 255);
                        // BUGFIX: Calculate letterbox/pillarbox for correct aspect ratio
                        SDL_FRect dst_rect;
                        double video_ar = (double)frame.width / (double)frame.height;
                        double screen_ar = (double)g_renderer.screen_w / (double)g_renderer.screen_h;
                        if (video_ar > screen_ar) {
                            // Wider than screen: letterbox (black bars top/bottom)
                            dst_rect.w = (float)g_renderer.screen_w;
                            dst_rect.h = (float)(g_renderer.screen_w / video_ar);
                            dst_rect.x = 0;
                            dst_rect.y = (float)((g_renderer.screen_h - dst_rect.h) / 2.0);
                        } else {
                            // Taller than screen: pillarbox (black bars left/right)
                            dst_rect.h = (float)g_renderer.screen_h;
                            dst_rect.w = (float)(g_renderer.screen_h * video_ar);
                            dst_rect.x = (float)((g_renderer.screen_w - dst_rect.w) / 2.0);
                            dst_rect.y = 0;
                        }
                        SDL_RenderTexture(g_renderer.sdl_renderer, current_tex, nullptr, &dst_rect);
                        
                        // BUGFIX: Draw overlays ON TOP of video frame BEFORE presenting
                        if (g_overlay) {
                            double remaining = g_video_decoder.get_video_remaining();
                            std::string remaining_str = "";
                            if (remaining > 0.0) {
                                int mins = (int)(remaining / 60.0);
                                int secs = (int)(remaining - mins * 60.0);
                                remaining_str = std::to_string(mins) + ":" + (secs < 10 ? "0" : "") + std::to_string(secs);
                            }
                            g_overlay->draw_all(current_idx, (int)g_eligible.size(),
                                &g_eligible[current_idx],
                                &g_eligible[(current_idx + 1) % (int)g_eligible.size()],
                                0.0, true, 0.0, nullptr, nullptr, remaining_str);
                        }
                        
                        g_renderer.present();
                    }
                    // Frame pacing: track when next frame should display
                    static uint64_t video_frame_target = 0;
                    static double video_last_frame_dur = -1;
                    double frame_dur_ms = 0;
                    { double cfps = g_eligible[current_idx].fps; if (cfps > 0) frame_dur_ms = 1000.0 / cfps; else frame_dur_ms = 33.333; }
                    uint64_t now = SDL_GetTicks();
                    // Reset pacing on new video (frame duration changes)
                    if (std::abs(video_last_frame_dur - frame_dur_ms) > 0.1) {
                        video_frame_target = now;
                        video_last_frame_dur = frame_dur_ms;
                    }
                    if (now < video_frame_target) {
                        uint32_t sleep = (uint32_t)(video_frame_target - now);
                        if (sleep > 2) SDL_Delay(sleep);
                    }
                    video_frame_target += (uint32_t)frame_dur_ms;
                    
                    playlist_lock.unlock();
                    SDL_Delay(5);
                    continue;
                }'''

start_marker = "if (g_video_decoder.is_running()) {"
end_marker = "// Video decoder not running - start it"

start_idx = None
end_idx = None
for i, line in enumerate(lines):
    stripped = line.strip()
    if stripped == start_marker and start_idx is None:
        start_idx = i
    if stripped == end_marker and start_idx is not None and end_idx is None:
        end_idx = i
        break

if start_idx is None or end_idx is None:
    print(f"ERROR: Could not find block. start={start_idx} end={end_idx}")
    sys.exit(1)

new_lines = lines[:start_idx] + [replacement + "\n"] + lines[end_idx:]
with open("/home/pi/piTrove/src/main.cpp", "w") as f:
    f.writelines(new_lines)

print(f"OK: Replaced lines {start_idx+1}-{end_idx} in main.cpp")
