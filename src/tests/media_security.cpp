#include "image_loader.h"
#include "blur.h"
#include "util.h"
#include "cache.h"
#include "renderer.h"

bool CacheManager::get_error_details(const std::string&, std::string&, std::string&, std::string&) { return false; }
Renderer::Renderer() = default;
Renderer::~Renderer() = default;
#include <SDL3/SDL.h>
#include <jpeglib.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

static void check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

static std::vector<uint8_t> make_jpeg(int w, int h) {
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* out_buf = nullptr;
    unsigned long out_size = 0;
    jpeg_mem_dest(&cinfo, &out_buf, &out_size);

    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 50, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> row(static_cast<size_t>(w) * 3, 128);
    for (int x = 0; x < w; ++x) {
        row[x * 3 + 0] = static_cast<uint8_t>((x * 255) / std::max(1, w));
        row[x * 3 + 1] = 80;
        row[x * 3 + 2] = 200;
    }
    JSAMPROW ptr = row.data();
    while (cinfo.next_scanline < cinfo.image_height) {
        jpeg_write_scanlines(&cinfo, &ptr, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    std::vector<uint8_t> result(out_buf, out_buf + out_size);
    std::free(out_buf);
    return result;
}

static void test_high_mp_jpeg_and_bomb() {
    // 10000x8000 = 80MP (>64MP legacy limit, <256MP smartphone ceiling)
    auto large_jpg = make_jpeg(10000, 8000);
    int w = 0, h = 0;
    check(ImageLoader::image_dimensions(large_jpg, w, h) && w == 10000 && h == 8000,
          "80MP JPEG header dimensions rejected");

    RawImage scaled = ImageLoader::decode_rgba(large_jpg, 1920);
    check(scaled.valid && scaled.pixels != nullptr, "80MP JPEG DCT downscale decode failed");
    check(scaled.width == 2500 && scaled.height == 2000, "80MP JPEG expected 1/4 DCT scale (2500x2000)");

    // Default max_dim=0 on >64MP JPEG should still auto-downscale safely
    RawImage auto_scaled = ImageLoader::decode_rgba(large_jpg);
    check(auto_scaled.valid && auto_scaled.width == 5000 && auto_scaled.height == 4000,
          "80MP JPEG default auto-downscale failed");

    // Small JPEG keeps exact 1:1 dimensions
    auto small_jpg = make_jpeg(64, 48);
    RawImage exact = ImageLoader::decode_rgba(small_jpg, 1920);
    check(exact.valid && exact.width == 64 && exact.height == 48, "Small JPEG did not keep 1:1 dimensions");

    // Patch SOF0 dimensions in small_jpg to simulate a >256MP bomb (32769x10000)
    auto bomb_jpg = small_jpg;
    bool patched = false;
    for (size_t i = 0; i + 9 < bomb_jpg.size(); ++i) {
        if (bomb_jpg[i] == 0xFF && bomb_jpg[i + 1] == 0xC0) {
            // SOF0: length(2), precision(1), height(2), width(2)
            bomb_jpg[i + 5] = 0x4E; bomb_jpg[i + 6] = 0x20; // 20000
            bomb_jpg[i + 7] = 0x4E; bomb_jpg[i + 8] = 0x20; // 20000 -> 400MP > 256MP
            patched = true;
            break;
        }
    }
    check(patched, "failed to patch SOF0 in bomb fixture");
    check(!ImageLoader::image_dimensions(bomb_jpg, w, h), "400MP JPEG bomb dimensions accepted");
    check(!ImageLoader::decode_rgba(bomb_jpg, 1920).valid, "400MP JPEG bomb decoded");
}

static void test_symlink_rejection() {
    const auto dir = std::filesystem::temp_directory_path() / ("pitrove-imgsec-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto real_file = dir / "real.jpg";
    const auto link_file = dir / "link.jpg";
    {
        std::ofstream out(real_file, std::ios::binary);
        out << "fixture-bytes";
    }
    std::filesystem::create_symlink(real_file, link_file);
    check(!ImageLoader::read_file_to_buffer(real_file.string()).empty(), "regular file read failed");
    check(ImageLoader::read_file_to_buffer(link_file.string()).empty(), "symlink file was not rejected");
    std::filesystem::remove_all(dir);
}

static void test_exif_rotation_case4_and_blur() {
    SDL_Surface* surf = SDL_CreateSurface(13, 7, SDL_PIXELFORMAT_RGBA32);
    check(surf != nullptr, "SDL_CreateSurface failed");
    auto* px = static_cast<uint8_t*>(surf->pixels);
    for (int y = 0; y < surf->h; ++y) {
        for (int x = 0; x < surf->w; ++x) {
            uint8_t* p = px + y * surf->pitch + x * 4;
            p[0] = static_cast<uint8_t>(y * 30);
            p[1] = static_cast<uint8_t>(x * 15);
            p[2] = 99;
            p[3] = 255;
        }
    }
    SDL_Surface* flipped = ImageLoader::apply_exif_rotation(surf, 4);
    check(flipped != nullptr && flipped->w == 13 && flipped->h == 7, "EXIF case 4 flip failed");
    const uint8_t* top = static_cast<const uint8_t*>(flipped->pixels);
    const uint8_t* bot = static_cast<const uint8_t*>(flipped->pixels) + 6 * flipped->pitch;
    check(top[0] == 180 && bot[0] == 0, "EXIF case 4 vertical flip pixels wrong");
    SDL_DestroySurface(flipped);
    SDL_DestroySurface(surf);

    // Verify sliding-window box_blur produces valid non-zero output and preserves constant image exactly
    RawImage uniform;
    uniform.width = 40;
    uniform.height = 30;
    uniform.channels = 4;
    uniform.valid = true;
    uniform.pixels = static_cast<uint8_t*>(std::malloc(40 * 30 * 4));
    for (int i = 0; i < 40 * 30; ++i) {
        uniform.pixels[i * 4 + 0] = 42;
        uniform.pixels[i * 4 + 1] = 100;
        uniform.pixels[i * 4 + 2] = 210;
        uniform.pixels[i * 4 + 3] = 255;
    }
    RawImage blurred = box_blur(uniform, 8);
    check(blurred.valid && blurred.pixels != nullptr && blurred.width == 40 && blurred.height == 30,
          "box_blur failed on uniform image");
    for (int i = 0; i < 40 * 30; ++i) {
        check(blurred.pixels[i * 4 + 0] == 42 && blurred.pixels[i * 4 + 1] == 100 &&
              blurred.pixels[i * 4 + 2] == 210 && blurred.pixels[i * 4 + 3] == 255,
              "box_blur drifted on uniform image");
    }
}

static void test_host_and_time_helpers() {
    check(is_valid_host_or_ip("192.168.4.1"), "valid IPv4 rejected");
    check(is_valid_host_or_ip("2001:db8::1"), "valid IPv6 rejected");
    check(is_valid_host_or_ip("router.local"), "valid hostname rejected");
    check(!is_valid_host_or_ip(""), "empty host accepted");
    check(!is_valid_host_or_ip("-c"), "leading dash accepted");
    check(!is_valid_host_or_ip("192.168.4.1;id"), "shell metachar accepted");
    check(!is_valid_host_or_ip("bad host"), "whitespace host accepted");
    check(!is_valid_host_or_ip("bad..host"), "empty label accepted");

    int h = -1, m = -1;
    check(display_local_hm(86400 + 3600 * 14 + 60 * 35, "UTC", h, m) && h == 14 && m == 35,
          "display_local_hm UTC mismatch");
}

int main() {
    try {
        check(SDL_Init(0), SDL_GetError());
        test_high_mp_jpeg_and_bomb();
        test_symlink_rejection();
        test_exif_rotation_case4_and_blur();
        test_host_and_time_helpers();
        SDL_Quit();
        std::cout << "Media scaling, symlink O_NOFOLLOW, EXIF pitch, sliding blur, and host validation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
