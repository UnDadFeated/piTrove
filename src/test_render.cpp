#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>
#include <xf86drm.h>

// Helper to find active DRM fd in the process
int find_drm_fd() {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char link[256], target[256];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name);
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (std::strstr(target, "/dev/dri/card")) {
                int fd = std::atoi(entry->d_name);
                closedir(dir);
                std::cout << "[INFO] Found DRM fd=" << fd << " (" << target << ")" << std::endl;
                return fd;
            }
        }
    }
    closedir(dir);
    return -1;
}

void render_photo(const std::string& path) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[ERROR] SDL_Init failed: " << SDL_GetError() << std::endl;
        return;
    }

    std::cout << "[INFO] Initializing fullscreen KMSDRM window..." << std::endl;
    SDL_Window* window = SDL_CreateWindow("piTrove Test Standalone",
                                          0, 0, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        std::cerr << "[ERROR] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "[ERROR] SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    std::cout << "[INFO] Screen resolution: " << w << "x" << h << std::endl;

    std::cout << "[INFO] Loading image: " << path << std::endl;
    SDL_Surface* surface = IMG_Load(path.c_str());
    if (!surface) {
        std::cerr << "[ERROR] Failed to load image: " << SDL_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        std::cerr << "[ERROR] Failed to create texture: " << SDL_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }

    // Centered aspect-ratio fitting
    float img_w, img_h;
    SDL_GetTextureSize(texture, &img_w, &img_h);
    double aspect_img = (double)img_w / img_h;
    double aspect_screen = (double)w / h;
    
    SDL_FRect dst;
    if (aspect_img > aspect_screen) {
        dst.w = (float)w;
        dst.h = (float)(w / aspect_img);
        dst.x = 0.0f;
        dst.y = (float)(h - dst.h) / 2.0f;
    } else {
        dst.h = (float)h;
        dst.w = (float)(h * aspect_img);
        dst.y = 0.0f;
        dst.x = (float)(w - dst.w) / 2.0f;
    }

    std::cout << "[INFO] Rendering photo to: x=" << dst.x << ", y=" << dst.y << ", w=" << dst.w << ", h=" << dst.h << std::endl;

    // Draw frame for 5 seconds
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < 5) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // Matte borders
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, &dst);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    std::cout << "[INFO] Cleaning up SDL3..." << std::endl;
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void play_video(const std::string& path) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "[ERROR] SDL_Init failed: " << SDL_GetError() << std::endl;
        return;
    }

    std::cout << "[INFO] Opening KMSDRM window to claim DRM master..." << std::endl;
    SDL_Window* window = SDL_CreateWindow("piTrove Video Test",
                                          0, 0, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        std::cerr << "[ERROR] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return;
    }

    int drm_fd = find_drm_fd();
    if (drm_fd >= 0) {
        int rc = drmDropMaster(drm_fd);
        std::cout << "[INFO] drmDropMaster(fd=" << drm_fd << ") = " << rc << " (Dropped master to let mpv render)" << std::endl;
    } else {
        std::cerr << "[WARNING] No active DRM fd found! mpv may fail to display." << std::endl;
    }

    std::cout << "[INFO] Spawning mpv to play: " << path << std::endl;
    pid_t pid = fork();
    if (pid == 0) {
        // Child: close files and exec mpv
        for (int i = 3; i < 1024; ++i) close(i);
        execlp("mpv", "mpv",
               "--vo=drm",
               "--drm-connector=HDMI-A-1",
               "--hwdec=auto",
               "--keepaspect=yes",
               "--no-osc",
               "--no-osd-bar",
               "--no-audio",
               path.c_str(),
               nullptr);
        _exit(1);
    } else if (pid > 0) {
        // Parent: wait 5 seconds, then stop child
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cout << "[INFO] Stopping mpv..." << std::endl;
        kill(pid, SIGTERM);
        int status;
        waitpid(pid, &status, 0);
        std::cout << "[INFO] mpv child process terminated cleanly." << std::endl;
    } else {
        std::cerr << "[ERROR] Fork failed!" << std::endl;
    }

    if (drm_fd >= 0) {
        int rc = drmSetMaster(drm_fd);
        std::cout << "[INFO] drmSetMaster(fd=" << drm_fd << ") = " << rc << " (Reclaimed DRM master for SDL3)" << std::endl;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " --photo <path_to_jpg> OR --video <path_to_video>" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    std::string path = argv[2];

    if (mode == "--photo") {
        render_photo(path);
    } else if (mode == "--video") {
        play_video(path);
    } else {
        std::cout << "Invalid mode! Use --photo or --video." << std::endl;
        return 1;
    }

    std::cout << "[SUCCESS] Standalone test completed." << std::endl;
    return 0;
}
