#ifndef PITROVE_TRANSITION_H
#define PITROVE_TRANSITION_H

#include <SDL.h>
#include <GLES3/gl3.h>
#include <map>
#include <string>
#include <vector>

struct ShaderProgram {
    GLuint program = 0;
    GLint loc_projection = -1;
    GLint loc_model = -1;
    GLint loc_texture = -1;
    GLint loc_time = -1;
    GLint loc_resolution = -1;
    GLint loc_direction = -1;
    GLint loc_pixel_size = -1;
    GLint loc_bias_color = -1;
    GLint loc_scanline_intensity = -1;

    // Shader-specific custom uniforms
    GLint loc_zoom_start = -1;
    GLint loc_zoom_end = -1;
    GLint loc_pan_x = -1;
    GLint loc_pan_y = -1;
    GLint loc_resolution_x = -1;
    GLint loc_resolution_y = -1;
    GLint loc_bias_light = -1;
    GLint loc_bias_width = -1;
    GLint loc_vignette = -1;
    GLint loc_progress = -1;
    GLint loc_tex_prev = -1;
    GLint loc_tex_next = -1;
};

class TransitionEngine {
private:
    SDL_Renderer* renderer;
    std::map<std::string, ShaderProgram> shaders;
    GLuint vao{0}, vbo{0}, ebo{0};

    // Fullscreen quad vertex data (x, y, u, v)
    float quad_vertices[16] = {
        // Positions   // TexCoords
        -1.0f,  1.0f,  0.0f, 0.0f, // Top-Left
        -1.0f, -1.0f,  0.0f, 1.0f, // Bottom-Left
         1.0f, -1.0f,  1.0f, 1.0f, // Bottom-Right
         1.0f,  1.0f,  1.0f, 0.0f  // Top-Right
    };

    unsigned int quad_indices[6] = {
        0, 1, 2,
        2, 3, 0
    };

public:
    TransitionEngine(SDL_Renderer* renderer);
    ~TransitionEngine();

    void init();
    void cleanup();

    // Shader loading
    GLuint load_shader(const std::string& name, const std::string& vs_path, const std::string& fs_path);
    GLuint load_shader_from_memory(const std::string& name, const char* vs_src, const char* fs_src);

    // Uniform setters
    void set_uniform_float(GLuint program, const char* name, float value);
    void set_uniform_vec2(GLuint program, const char* name, float x, float y);
    void set_uniform_vec3(GLuint program, const char* name, float x, float y, float z);
    void set_uniform_vec4(GLuint program, const char* name, float x, float y, float z, float w);
    void set_uniform_mat4(GLuint program, const char* name, const float* value);
    void set_uniform_texture(GLuint program, const char* name, GLuint tex, GLint unit = 0);

    // Get specific shader
    ShaderProgram* get_shader(const std::string& name);

    // Draw quad
    void draw_quad();

    // Transition render calls (direct EGL texture operations)
    void render_ken_burns(SDL_Texture* tex, float time, float zoom_start, float zoom_end, float pan_x, float pan_y, int screen_w, int screen_h);
    void render_wipe(SDL_Texture* prev_tex, SDL_Texture* next_tex, float progress, int direction);
    void render_pixelate(SDL_Texture* prev_tex, SDL_Texture* next_tex, float progress, float pixel_size, int screen_w, int screen_h);
    void render_post_process(SDL_Texture* tex, float time, float bias_r, float bias_g, float bias_b, float scanline_intensity, int screen_w, int screen_h);

private:
    GLuint compile_shader(GLenum type, const char* source);
    GLuint load_shader_pair(const char* vs_source, const char* fs_source, ShaderProgram& out_prog);
};

#endif // PITROVE_TRANSITION_H
