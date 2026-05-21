#include "transition.h"
#include "util.h"
#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>

static std::string read_file_content(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string get_shader_path(const std::string& filename) {
    std::string exe_dir = get_exe_dir();
    // Try exe_dir/src/shaders/
    std::string path1 = exe_dir + "/src/shaders/" + filename;
    if (file_exists(path1)) return path1;
    // Try exe_dir/shaders/
    std::string path2 = exe_dir + "/shaders/" + filename;
    if (file_exists(path2)) return path2;
    // Try absolute default
    std::string path3 = "/home/pi/piTrove/src/shaders/" + filename;
    if (file_exists(path3)) return path3;
    // Fallback relative
    return "src/shaders/" + filename;
}

TransitionEngine::TransitionEngine(SDL_Renderer* renderer) : renderer(renderer) {}

TransitionEngine::~TransitionEngine() {
    cleanup();
}

void TransitionEngine::init() {
    // Generate and bind VAO/VBO/EBO
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

    // Position attribute (layout location 0)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // TexCoord attribute (layout location 1)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    // Compile and register shaders
    load_shader("ken_burns", get_shader_path("ken_burns.vs"), get_shader_path("ken_burns.fs"));
    load_shader("wipe", get_shader_path("wipe.vs"), get_shader_path("wipe.fs"));
    load_shader("pixelate", get_shader_path("pixelate.vs"), get_shader_path("pixelate.fs"));
    load_shader("post_process", get_shader_path("post_process.vs"), get_shader_path("post_process.fs"));
}

void TransitionEngine::cleanup() {
    if (vao) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vbo) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (ebo) {
        glDeleteBuffers(1, &ebo);
        ebo = 0;
    }

    for (auto& pair : shaders) {
        if (pair.second.program) {
            glDeleteProgram(pair.second.program);
        }
    }
    shaders.clear();
}

GLuint TransitionEngine::compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        g_logger.error("Shader compilation failed: %s", infoLog);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint TransitionEngine::load_shader_pair(const char* vs_source, const char* fs_source, ShaderProgram& out_prog) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);

    // Bind standard layout attribute locations
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");

    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        g_logger.error("Program linking failed: %s", infoLog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    out_prog.program = program;

    // Standard uniform locations
    out_prog.loc_projection = glGetUniformLocation(program, "uProjection");
    out_prog.loc_model = glGetUniformLocation(program, "uModel");
    out_prog.loc_texture = glGetUniformLocation(program, "uTexture");
    out_prog.loc_time = glGetUniformLocation(program, "uTime");
    out_prog.loc_resolution = glGetUniformLocation(program, "uResolution");
    out_prog.loc_direction = glGetUniformLocation(program, "uDirection");
    out_prog.loc_pixel_size = glGetUniformLocation(program, "uPixelSize");
    out_prog.loc_bias_color = glGetUniformLocation(program, "uBiasColor");
    out_prog.loc_scanline_intensity = glGetUniformLocation(program, "uScanlineIntensity");

    // Custom shader-specific uniform locations
    out_prog.loc_zoom_start = glGetUniformLocation(program, "uZoomStart");
    out_prog.loc_zoom_end = glGetUniformLocation(program, "uZoomEnd");
    out_prog.loc_pan_x = glGetUniformLocation(program, "uPanX");
    out_prog.loc_pan_y = glGetUniformLocation(program, "uPanY");
    out_prog.loc_resolution_x = glGetUniformLocation(program, "uResolutionX");
    out_prog.loc_resolution_y = glGetUniformLocation(program, "uResolutionY");
    out_prog.loc_bias_light = glGetUniformLocation(program, "uBiasLight");
    out_prog.loc_bias_width = glGetUniformLocation(program, "uBiasWidth");
    out_prog.loc_vignette = glGetUniformLocation(program, "uVignette");
    out_prog.loc_progress = glGetUniformLocation(program, "uProgress");
    out_prog.loc_tex_prev = glGetUniformLocation(program, "uTexPrev");
    out_prog.loc_tex_next = glGetUniformLocation(program, "uTexNext");

    return program;
}

GLuint TransitionEngine::load_shader(const std::string& name, const std::string& vs_path, const std::string& fs_path) {
    std::string vs_src = read_file_content(vs_path);
    std::string fs_src = read_file_content(fs_path);
    if (vs_src.empty() || fs_src.empty()) {
        g_logger.error("Failed to read shader files for %s (VS: %s, FS: %s)", name.c_str(), vs_path.c_str(), fs_path.c_str());
        return 0;
    }
    return load_shader_from_memory(name, vs_src.c_str(), fs_src.c_str());
}

GLuint TransitionEngine::load_shader_from_memory(const std::string& name, const char* vs_src, const char* fs_src) {
    ShaderProgram prog;
    GLuint program = load_shader_pair(vs_src, fs_src, prog);
    if (program) {
        shaders[name] = prog;
        g_logger.info("Successfully loaded shader program: %s (id: %u)", name.c_str(), program);
    }
    return program;
}

void TransitionEngine::set_uniform_float(GLuint program, const char* name, float value) {
    glUseProgram(program);
    glUniform1f(glGetUniformLocation(program, name), value);
}

void TransitionEngine::set_uniform_vec2(GLuint program, const char* name, float x, float y) {
    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, name), x, y);
}

void TransitionEngine::set_uniform_vec3(GLuint program, const char* name, float x, float y, float z) {
    glUseProgram(program);
    glUniform3f(glGetUniformLocation(program, name), x, y, z);
}

void TransitionEngine::set_uniform_vec4(GLuint program, const char* name, float x, float y, float z, float w) {
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, name), x, y, z, w);
}

void TransitionEngine::set_uniform_mat4(GLuint program, const char* name, const float* value) {
    glUseProgram(program);
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, value);
}

void TransitionEngine::set_uniform_texture(GLuint program, const char* name, GLuint tex, GLint unit) {
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(program, name), unit);
}

ShaderProgram* TransitionEngine::get_shader(const std::string& name) {
    auto it = shaders.find(name);
    if (it != shaders.end()) {
        return &it->second;
    }
    return nullptr;
}

void TransitionEngine::draw_quad() {
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void TransitionEngine::render_ken_burns(SDL_Texture* tex, float time, float zoom_start, float zoom_end, float pan_x, float pan_y, int screen_w, int screen_h) {
    ShaderProgram* prog = get_shader("ken_burns");
    if (!prog || !prog->program) return;

    glUseProgram(prog->program);

    float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    if (prog->loc_projection != -1) glUniformMatrix4fv(prog->loc_projection, 1, GL_FALSE, identity);
    if (prog->loc_model != -1) glUniformMatrix4fv(prog->loc_model, 1, GL_FALSE, identity);

    glActiveTexture(GL_TEXTURE0);
    SDL_GL_BindTexture(tex, nullptr, nullptr);
    if (prog->loc_texture != -1) glUniform1i(prog->loc_texture, 0);

    if (prog->loc_time != -1) glUniform1f(prog->loc_time, time);
    if (prog->loc_zoom_start != -1) glUniform1f(prog->loc_zoom_start, zoom_start);
    if (prog->loc_zoom_end != -1) glUniform1f(prog->loc_zoom_end, zoom_end);
    if (prog->loc_pan_x != -1) glUniform1f(prog->loc_pan_x, pan_x);
    if (prog->loc_pan_y != -1) glUniform1f(prog->loc_pan_y, pan_y);
    if (prog->loc_resolution_x != -1) glUniform1f(prog->loc_resolution_x, (float)screen_w);
    if (prog->loc_resolution_y != -1) glUniform1f(prog->loc_resolution_y, (float)screen_h);

    draw_quad();

    SDL_GL_UnbindTexture(tex);
}

void TransitionEngine::render_wipe(SDL_Texture* prev_tex, SDL_Texture* next_tex, float progress, int direction) {
    // First, draw the previous texture fully opaque as background
    render_post_process(prev_tex, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0);

    // Now, render the next texture on top using the wipe shader with blend enabled
    ShaderProgram* prog = get_shader("wipe");
    if (!prog || !prog->program) return;

    glUseProgram(prog->program);

    float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    if (prog->loc_projection != -1) glUniformMatrix4fv(prog->loc_projection, 1, GL_FALSE, identity);
    if (prog->loc_model != -1) glUniformMatrix4fv(prog->loc_model, 1, GL_FALSE, identity);

    glActiveTexture(GL_TEXTURE0);
    SDL_GL_BindTexture(next_tex, nullptr, nullptr);
    if (prog->loc_texture != -1) glUniform1i(prog->loc_texture, 0);

    float dir_x = 1.0f, dir_y = 0.0f;
    float edge = 0.0f;

    // directions: 0 = Left-to-Right, 1 = Right-to-Left, 2 = Top-to-Bottom, 3 = Bottom-to-Top
    if (direction == 0) { // Left-to-Right
        dir_x = 1.0f; dir_y = 0.0f;
        edge = (1.0f - progress) * 1.1f - 0.05f;
    } else if (direction == 1) { // Right-to-Left
        dir_x = -1.0f; dir_y = 0.0f;
        edge = -progress * 1.1f + 0.05f;
    } else if (direction == 2) { // Top-to-Bottom
        dir_x = 0.0f; dir_y = 1.0f;
        edge = (1.0f - progress) * 1.1f - 0.05f;
    } else { // Bottom-to-Top
        dir_x = 0.0f; dir_y = -1.0f;
        edge = -progress * 1.1f + 0.05f;
    }

    if (prog->loc_progress != -1) glUniform1f(prog->loc_progress, edge);
    if (prog->loc_direction != -1) glUniform2f(prog->loc_direction, dir_x, dir_y);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_quad();

    glDisable(GL_BLEND);

    SDL_GL_UnbindTexture(next_tex);
}

void TransitionEngine::render_pixelate(SDL_Texture* prev_tex, SDL_Texture* next_tex, float progress, float pixel_size, int screen_w, int screen_h) {
    ShaderProgram* prog = get_shader("pixelate");
    if (!prog || !prog->program) return;

    glUseProgram(prog->program);

    float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    if (prog->loc_projection != -1) glUniformMatrix4fv(prog->loc_projection, 1, GL_FALSE, identity);
    if (prog->loc_model != -1) glUniformMatrix4fv(prog->loc_model, 1, GL_FALSE, identity);

    if (prog->loc_resolution != -1) glUniform2f(prog->loc_resolution, (float)screen_w, (float)screen_h);

    if (progress < 0.5f) {
        float p = progress / 0.5f; // 0.0 to 1.0
        float size = 1.0f + p * (pixel_size - 1.0f);

        if (prog->loc_pixel_size != -1) glUniform1f(prog->loc_pixel_size, size);

        glActiveTexture(GL_TEXTURE0);
        SDL_GL_BindTexture(prev_tex, nullptr, nullptr);
        if (prog->loc_texture != -1) glUniform1i(prog->loc_texture, 0);

        draw_quad();

        SDL_GL_UnbindTexture(prev_tex);
    } else {
        float p = (progress - 0.5f) / 0.5f; // 0.0 to 1.0
        float size = pixel_size - p * (pixel_size - 1.0f);

        if (prog->loc_pixel_size != -1) glUniform1f(prog->loc_pixel_size, size);

        glActiveTexture(GL_TEXTURE0);
        SDL_GL_BindTexture(next_tex, nullptr, nullptr);
        if (prog->loc_texture != -1) glUniform1i(prog->loc_texture, 0);

        draw_quad();

        SDL_GL_UnbindTexture(next_tex);
    }
}

void TransitionEngine::render_post_process(SDL_Texture* tex, float time, float bias_r, float bias_g, float bias_b, float scanline_intensity, int screen_w, int screen_h) {
    ShaderProgram* prog = get_shader("post_process");
    if (!prog || !prog->program) return;

    glUseProgram(prog->program);

    float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    if (prog->loc_projection != -1) glUniformMatrix4fv(prog->loc_projection, 1, GL_FALSE, identity);
    if (prog->loc_model != -1) glUniformMatrix4fv(prog->loc_model, 1, GL_FALSE, identity);

    glActiveTexture(GL_TEXTURE0);
    SDL_GL_BindTexture(tex, nullptr, nullptr);
    if (prog->loc_texture != -1) glUniform1i(prog->loc_texture, 0);

    if (prog->loc_time != -1) glUniform1f(prog->loc_time, time);
    if (prog->loc_bias_light != -1) glUniform3f(prog->loc_bias_light, bias_r, bias_g, bias_b);
    if (prog->loc_bias_width != -1) glUniform1f(prog->loc_bias_width, 0.15f);

    // Read vignette from global config
    float vig = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        vig = g_cfg.vignette_enabled ? 0.3f : 0.0f;
    }
    if (prog->loc_vignette != -1) glUniform1f(prog->loc_vignette, vig);
    
    if (prog->loc_scanline_intensity != -1) glUniform1f(prog->loc_scanline_intensity, scanline_intensity);
    if (prog->loc_resolution != -1) glUniform2f(prog->loc_resolution, (float)screen_w, (float)screen_h);

    draw_quad();

    SDL_GL_UnbindTexture(tex);
}
