#ifndef RENDER_H
#define RENDER_H

#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>

/* ---- OpenGL 3.3 types & constants ---- */
typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_INFO_LOG_LENGTH        0x8B84
#define GL_ARRAY_BUFFER           0x8892
#define GL_DYNAMIC_DRAW           0x88E8
#define GL_TEXTURE0               0x84C0
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_R8                     0x8229

/* ---- GL function pointers (X-macro) ---- */
#define GL_FUNCS \
    X(GLuint,  CreateShader,            GLenum type) \
    X(void,    ShaderSource,            GLuint shader, GLsizei count, const GLchar** string, const GLint* length) \
    X(void,    CompileShader,           GLuint shader) \
    X(void,    GetShaderiv,             GLuint shader, GLenum pname, GLint* params) \
    X(void,    GetShaderInfoLog,        GLuint shader, GLsizei maxLen, GLsizei* length, GLchar* infoLog) \
    X(void,    DeleteShader,            GLuint shader) \
    X(GLuint,  CreateProgram,           void) \
    X(void,    AttachShader,            GLuint program, GLuint shader) \
    X(void,    LinkProgram,             GLuint program) \
    X(void,    GetProgramiv,            GLuint program, GLenum pname, GLint* params) \
    X(void,    GetProgramInfoLog,       GLuint program, GLsizei maxLen, GLsizei* length, GLchar* infoLog) \
    X(void,    UseProgram,              GLuint program) \
    X(void,    DeleteProgram,           GLuint program) \
    X(GLint,   GetUniformLocation,      GLuint program, const GLchar* name) \
    X(void,    Uniform1i,              GLuint loc, GLint v0) \
    X(void,    Uniform2f,              GLuint loc, GLfloat v0, GLfloat v1) \
    X(void,    GenVertexArrays,         GLsizei n, GLuint* arrays) \
    X(void,    BindVertexArray,         GLuint array) \
    X(void,    DeleteVertexArrays,      GLsizei n, const GLuint* arrays) \
    X(void,    GenBuffers,              GLsizei n, GLuint* buffers) \
    X(void,    BindBuffer,              GLenum target, GLuint buffer) \
    X(void,    BufferData,              GLenum target, GLsizeiptr size, const void* data, GLenum usage) \
    X(void,    BufferSubData,           GLenum target, GLintptr offset, GLsizeiptr size, const void* data) \
    X(void,    DeleteBuffers,           GLsizei n, const GLuint* buffers) \
    X(void,    VertexAttribPointer,     GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) \
    X(void,    EnableVertexAttribArray, GLuint index) \
    X(void,    ActiveTexture,           GLenum texture)

#define X(ret, name, ...) typedef ret (APIENTRY *PFN_gl##name)(__VA_ARGS__); extern PFN_gl##name gl##name##_;
GL_FUNCS
#undef X

#define gl_CreateShader            glCreateShader_
#define gl_ShaderSource            glShaderSource_
#define gl_CompileShader           glCompileShader_
#define gl_GetShaderiv             glGetShaderiv_
#define gl_GetShaderInfoLog        glGetShaderInfoLog_
#define gl_DeleteShader            glDeleteShader_
#define gl_CreateProgram           glCreateProgram_
#define gl_AttachShader            glAttachShader_
#define gl_LinkProgram             glLinkProgram_
#define gl_GetProgramiv            glGetProgramiv_
#define gl_GetProgramInfoLog       glGetProgramInfoLog_
#define gl_UseProgram              glUseProgram_
#define gl_DeleteProgram           glDeleteProgram_
#define gl_GetUniformLocation      glGetUniformLocation_
#define gl_Uniform1i               glUniform1i_
#define gl_Uniform2f               glUniform2f_
#define gl_GenVertexArrays         glGenVertexArrays_
#define gl_BindVertexArray         glBindVertexArray_
#define gl_DeleteVertexArrays      glDeleteVertexArrays_
#define gl_GenBuffers              glGenBuffers_
#define gl_BindBuffer              glBindBuffer_
#define gl_BufferData              glBufferData_
#define gl_BufferSubData           glBufferSubData_
#define gl_DeleteBuffers           glDeleteBuffers_
#define gl_VertexAttribPointer     glVertexAttribPointer_
#define gl_EnableVertexAttribArray glEnableVertexAttribArray_
#define gl_ActiveTexture           glActiveTexture_

/* ---- Colors (0xAARRGGBB) ---- */
#define COL_BG         0xFF1E1E2E
#define COL_SURFACE    0xFF313244
#define COL_OVERLAY    0xFF45475A
#define COL_TEXT       0xFFFFFFFF
#define COL_SUBTEXT    0xFFA6ADC8
#define COL_DIM        0xFF6C7086
#define COL_ACCENT     0xFF89B4FA
#define COL_YELLOW     0xFFF9E2AF
#define COL_GREEN      0xFFA6E3A1
#define COL_RED        0xFFF38BA8
#define COL_PEACH      0xFFFAB387
#define COL_HEADER     0xFF181825
#define COL_MANTLE     0xFF11111B
#define COL_HOVER      0xFF2A2A3C
#define COL_SELECTED   0xFF364060
#define COL_SCROLLBAR  0xFF6C7086
#define COL_BORDER     0xFF313244

/* ---- MDL2 Icons ---- */
#define ICON_CHEVRON_RIGHT  0
#define ICON_CHEVRON_DOWN   1
#define ICON_ARROW_BACK     2
#define ICON_ARROW_FWD      3
#define ICON_ARROW_UP       4
#define ICON_CLOSE          5
#define ICON_MINIMIZE       6
#define ICON_MAXIMIZE       7
#define ICON_RESTORE        8
#define ICON_ADD            9
#define ICON_RECENT        10
#define ICON_BOOKMARK      11
#define ICON_STORAGE       12
#define ICON_PLACES        13
#define ICON_HOME          14
#define ICON_REFRESH       15
#define ICON_CHEVRON_UP    16
#define ICON_CHEVRON_LEFT  17
#define ICON_LINK          18
#define ICON_COUNT         19

/* ---- Glyph / Font ---- */
typedef struct {
    uint32_t codepoint;
    int gw, gh, advance;
    float u0, v0, u1, v1;
} GlyphInfo;

#define MAX_GLYPHS       4096
#define GLYPH_HASH_SIZE  8192  /* must be power of 2 */

/* ---- Vertex ---- */
typedef struct {
    float x, y, u, v;
    uint8_t r, g, b, a;
} RVertex;

/* ---- Renderer ---- */
#define MAX_VERTICES (65536)

typedef struct {
    GLuint texture;
    GlyphInfo glyphs[MAX_GLYPHS];
    int       glyph_count;
    int       glyph_hash[GLYPH_HASH_SIZE];
    int       font_height, font_ascent;
    int       atlas_w, atlas_h;
    int       atlas_cur_x, atlas_cur_y, atlas_row_h;
    HFONT     atlas_font;
    HDC       atlas_dc;
    HBITMAP   atlas_bmp;
    void*     atlas_bits;
    unsigned char* atlas_alpha;
} FontAtlas;

typedef struct {
    GLuint program, vao, vbo;
    GLuint white_texture;
    GLint  loc_screen_size, loc_texture, loc_text_mode;

    FontAtlas fonts[2];     /* [0] = primary, [1] = small */
    /* Backwards-compatible aliases — use fonts[0] internals */
    int font_height, font_ascent;
    int atlas_w, atlas_h;
    GLuint font_texture;

    GLuint icon_texture;
    GlyphInfo icon_glyphs[ICON_COUNT];
    int icon_size;

    RVertex vertices[MAX_VERTICES];
    int vertex_count;
    int current_text_mode;
    GLuint current_tex;
    int screen_w, screen_h;
} Renderer;

int  render_init(Renderer* r);
int  render_create_font(Renderer* r, const char* font_name, int font_size);
int  render_create_font_small(Renderer* r, const char* font_name, int font_size);
int  render_create_icons(Renderer* r, int size);
void render_mdl2(Renderer* r, int icon_id, float x, float y, float sz, uint32_t color);
void render_begin(Renderer* r, int w, int h);
void render_end(Renderer* r);
void render_flush(Renderer* r);

void render_quad(Renderer* r, float x, float y, float w, float h, uint32_t color);
int  render_text(Renderer* r, const char* text, float x, float y, uint32_t color);
int  render_text_n(Renderer* r, const char* text, int maxchars, float x, float y, uint32_t color);
int  render_text_width(Renderer* r, const char* text);
int  render_text_width_n(Renderer* r, const char* text, int n);
int  render_text_small(Renderer* r, const char* text, float x, float y, uint32_t color);
int  render_text_small_n(Renderer* r, const char* text, int maxchars, float x, float y, uint32_t color);
int  render_text_width_small(Renderer* r, const char* text);
void render_folder_icon(Renderer* r, float x, float y, float size, uint32_t color);
void render_file_icon(Renderer* r, float x, float y, float size, uint32_t color);
void render_icon(Renderer* r, GLuint texture, float x, float y, float w, float h);
void render_scissor(Renderer* r, int x, int y, int w, int h);
void render_scissor_reset(Renderer* r);

int  render_load_gl_functions(void);

#endif
