#!/usr/bin/env python3
"""After decoder starts, cache FPS to SQLite."""
with open("src/main.cpp", "r") as f:
    c = f.read()

old = """                }
                playlist_lock.unlock();
                SDL_Delay(100);
                continue;
            }
        }
        } catch (const std::exception& e) {
            g_logger.error("VIDEO_DEC: Exception in main loop: %s", e.what());"""

new = """                }
                // Cache FPS from decoder to SQLite
                double video_fps = g_video_decoder.get_fps();
                if (video_fps > 0) {
                    g_logger.info("Caching FPS=%.2f for video: %s", video_fps, g_eligible[current_idx].path.c_str());
                    g_eligible[current_idx].fps = video_fps;
                    if (fast_cache) {
                        fast_cache->upsert(g_eligible[current_idx], 0, 0, 0);
                    }
                }
                playlist_lock.unlock();
                SDL_Delay(100);
                continue;
            }
        }
        } catch (const std::exception& e) {
            g_logger.error("VIDEO_DEC: Exception in main loop: %s", e.what());"""

c = c.replace(old, new)

with open("src/main.cpp", "w") as f:
    f.write(c)
print("OK main.cpp FPS caching added")
