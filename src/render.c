#include "render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- GL function pointer storage ---- */
#define X(ret, name, ...) PFN_gl##name gl##name##_ = NULL;
GL_FUNCS
#undef X

int render_load_gl_functions(void) {
    #define X(ret, name, ...) \
        gl##name##_ = (PFN_gl##name)wglGetProcAddress("gl" #name); \
        if (!gl##name##_) return 0;
    GL_FUNCS
    #undef X
    return 1;
}

/* ---- Shader helpers ---- */
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = gl_CreateShader(type);
    gl_ShaderSource(s, 1, &src, NULL);
    gl_CompileShader(s);
    GLint ok;
    gl_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        gl_GetShaderInfoLog(s, 512, NULL, log);
        OutputDebugStringA(log);
        gl_DeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint create_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint prog = gl_CreateProgram();
    gl_AttachShader(prog, vs);
    gl_AttachShader(prog, fs);
    gl_LinkProgram(prog);
    GLint ok;
    gl_GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { gl_DeleteProgram(prog); prog = 0; }
    gl_DeleteShader(vs);
    gl_DeleteShader(fs);
    return prog;
}

static const char* VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec4 aColor;\n"
    "uniform vec2 uScreenSize;\n"
    "out vec2 vUV; out vec4 vColor;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(aPos.x/uScreenSize.x*2.0-1.0, 1.0-aPos.y/uScreenSize.y*2.0);\n"
    "  gl_Position = vec4(ndc,0.0,1.0); vUV=aUV; vColor=aColor;\n"
    "}\n";

static const char* FS_SRC =
    "#version 330 core\n"
    "in vec2 vUV; in vec4 vColor;\n"
    "uniform sampler2D uTexture; uniform int uTextMode;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "  if(uTextMode==1) { float a=texture(uTexture,vUV).r; FragColor=vec4(vColor.rgb,vColor.a*a); }\n"
    "  else if(uTextMode==2) { FragColor=texture(uTexture,vUV); }\n"
    "  else { FragColor=vColor; }\n"
    "}\n";

int render_init(Renderer* r) {
    memset(r, 0, sizeof(*r));
    r->program = create_program(VS_SRC, FS_SRC);
    if (!r->program) return 0;
    r->loc_screen_size = gl_GetUniformLocation(r->program, "uScreenSize");
    r->loc_texture     = gl_GetUniformLocation(r->program, "uTexture");
    r->loc_text_mode   = gl_GetUniformLocation(r->program, "uTextMode");

    gl_GenVertexArrays(1, &r->vao);
    gl_BindVertexArray(r->vao);
    gl_GenBuffers(1, &r->vbo);
    gl_BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl_BufferData(GL_ARRAY_BUFFER, sizeof(r->vertices), NULL, GL_DYNAMIC_DRAW);
    gl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)0);
    gl_EnableVertexAttribArray(0);
    gl_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RVertex), (void*)(2*sizeof(float)));
    gl_EnableVertexAttribArray(1);
    gl_VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RVertex), (void*)(4*sizeof(float)));
    gl_EnableVertexAttribArray(2);
    gl_BindVertexArray(0);

    glGenTextures(1, &r->white_texture);
    glBindTexture(GL_TEXTURE_2D, r->white_texture);
    uint8_t white[] = {255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    r->current_tex = r->white_texture;
    return 1;
}

/* ---- Font atlas via GDI (dynamic, on-demand glyph caching) ---- */
static int atlas_create_glyph(FontAtlas* a, Renderer* r, unsigned int cp);
static int atlas_lookup_or_create(FontAtlas* a, Renderer* r, unsigned int cp);

static int atlas_init(FontAtlas* a, Renderer* r, const char* font_name, int font_size) {
    a->atlas_w = 1024; a->atlas_h = 1024;

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = a->atlas_w;
    bmi.bmiHeader.biHeight = -(int)a->atlas_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    a->atlas_dc = CreateCompatibleDC(NULL);
    a->atlas_bmp = CreateDIBSection(a->atlas_dc, &bmi, DIB_RGB_COLORS, &a->atlas_bits, NULL, 0);
    SelectObject(a->atlas_dc, a->atlas_bmp);
    memset(a->atlas_bits, 0, a->atlas_w * a->atlas_h * 4);

    WCHAR wfont[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, font_name, -1, wfont, 64);
    a->atlas_font = CreateFontW(-font_size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_DONTCARE, wfont);
    SelectObject(a->atlas_dc, a->atlas_font);
    SetBkMode(a->atlas_dc, OPAQUE);
    SetBkColor(a->atlas_dc, RGB(0,0,0));
    SetTextColor(a->atlas_dc, RGB(255,255,255));

    TEXTMETRICA tm;
    GetTextMetricsA(a->atlas_dc, &tm);
    a->font_height = tm.tmHeight;
    a->font_ascent = tm.tmAscent;
    a->atlas_row_h = tm.tmHeight + 2;
    a->atlas_cur_x = 1;
    a->atlas_cur_y = 1;

    a->atlas_alpha = (unsigned char*)calloc((size_t)a->atlas_w * a->atlas_h, 1);

    glGenTextures(1, &a->texture);
    glBindTexture(GL_TEXTURE_2D, a->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, a->atlas_w, a->atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, a->atlas_alpha);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    memset(a->glyph_hash, 0, sizeof(a->glyph_hash));
    a->glyph_count = 0;
    for (unsigned int c = 32; c < 127; c++) atlas_lookup_or_create(a, r, c);
    return 1;
}

int render_create_font(Renderer* r, const char* font_name, int font_size) {
    int ok = atlas_init(&r->fonts[0], r, font_name, font_size);
    r->font_height = r->fonts[0].font_height;
    r->font_ascent = r->fonts[0].font_ascent;
    r->atlas_w = r->fonts[0].atlas_w;
    r->atlas_h = r->fonts[0].atlas_h;
    r->font_texture = r->fonts[0].texture;
    return ok;
}

int render_create_font_small(Renderer* r, const char* font_name, int font_size) {
    return atlas_init(&r->fonts[1], r, font_name, font_size);
}

static int atlas_create_glyph(FontAtlas* a, Renderer* r, unsigned int cp) {
    if (a->glyph_count >= MAX_GLYPHS) return -1;

    WCHAR w[2]; int wlen;
    if (cp < 0x10000) { w[0] = (WCHAR)cp; wlen = 1; }
    else {
        unsigned int c2 = cp - 0x10000;
        w[0] = (WCHAR)(0xD800 | (c2 >> 10));
        w[1] = (WCHAR)(0xDC00 | (c2 & 0x3FF));
        wlen = 2;
    }

    SIZE sz = {0};
    GetTextExtentPoint32W(a->atlas_dc, w, wlen, &sz);
    int gw = sz.cx, gh = sz.cy;
    if (gw <= 0) gw = (cp == ' ') ? a->font_height / 3 : 1;
    if (gh <= 0) gh = a->font_height;

    if (a->atlas_cur_x + gw + 1 > a->atlas_w) {
        a->atlas_cur_x = 1;
        a->atlas_cur_y += a->atlas_row_h + 1;
    }
    if (a->atlas_cur_y + gh > a->atlas_h) return -1;

    int gx = a->atlas_cur_x, gy = a->atlas_cur_y;

    ExtTextOutW(a->atlas_dc, gx, gy, 0, NULL, w, wlen, NULL);
    GdiFlush();

    unsigned char* src = (unsigned char*)a->atlas_bits;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            int si = ((gy + y) * a->atlas_w + (gx + x)) * 4;
            unsigned char b = src[si+0], gc = src[si+1], rv = src[si+2];
            unsigned char av = b > gc ? (b > rv ? b : rv) : (gc > rv ? gc : rv);
            a->atlas_alpha[(gy + y) * a->atlas_w + (gx + x)] = av;
        }
    }

    GLuint prev_tex = r->current_tex;
    glBindTexture(GL_TEXTURE_2D, a->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, a->atlas_w);
    glTexSubImage2D(GL_TEXTURE_2D, 0, gx, gy, gw, gh, GL_RED, GL_UNSIGNED_BYTE,
                    a->atlas_alpha + gy * a->atlas_w + gx);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (prev_tex) glBindTexture(GL_TEXTURE_2D, prev_tex);

    int idx = a->glyph_count++;
    GlyphInfo* gi = &a->glyphs[idx];
    gi->codepoint = cp;
    gi->gw = gw; gi->gh = gh;
    gi->advance = gw;
    gi->u0 = (float)gx / a->atlas_w;
    gi->v0 = (float)gy / a->atlas_h;
    gi->u1 = (float)(gx + gw) / a->atlas_w;
    gi->v1 = (float)(gy + gh) / a->atlas_h;

    a->atlas_cur_x += gw + 1;
    return idx;
}

static int atlas_lookup_or_create(FontAtlas* a, Renderer* r, unsigned int cp) {
    unsigned int h = (cp * 2654435761u) & (GLYPH_HASH_SIZE - 1);
    while (1) {
        int slot = a->glyph_hash[h];
        if (slot == 0) {
            int idx = atlas_create_glyph(a, r, cp);
            if (idx < 0) return -1;
            a->glyph_hash[h] = idx + 1;
            return idx;
        }
        if (a->glyphs[slot - 1].codepoint == cp) return slot - 1;
        h = (h + 1) & (GLYPH_HASH_SIZE - 1);
    }
}

/* UTF-8 decoder. Advances *ps past the consumed byte(s). Returns -1 at end. */
static int utf8_decode(const char** ps) {
    const unsigned char* s = (const unsigned char*)*ps;
    if (s[0] == 0) return -1;
    int cp = 0, len = 0;
    if (s[0] < 0x80) { cp = s[0]; len = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; len = 4; }
    else { *ps += 1; return 0xFFFD; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *ps += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *ps += len;
    return cp;
}

/* ---- Batch rendering ---- */
static void set_mode(Renderer* r, int text_mode, GLuint tex) {
    if (r->current_text_mode != text_mode || r->current_tex != tex) {
        render_flush(r);
        r->current_text_mode = text_mode;
        r->current_tex = tex;
    }
}

static void push_vtx(Renderer* r, float x, float y, float u, float v, uint32_t c) {
    if (r->vertex_count >= MAX_VERTICES) render_flush(r);
    RVertex* vtx = &r->vertices[r->vertex_count++];
    vtx->x=x; vtx->y=y; vtx->u=u; vtx->v=v;
    vtx->r=(c>>16)&0xFF; vtx->g=(c>>8)&0xFF; vtx->b=c&0xFF; vtx->a=(c>>24)&0xFF;
}

void render_quad(Renderer* r, float x, float y, float w, float h, uint32_t color) {
    set_mode(r, 0, r->white_texture);
    float x1=x+w, y1=y+h;
    push_vtx(r,x,y,0,0,color);   push_vtx(r,x1,y,0,0,color);  push_vtx(r,x,y1,0,0,color);
    push_vtx(r,x1,y,0,0,color);  push_vtx(r,x,y1,0,0,color);  push_vtx(r,x1,y1,0,0,color);
}

static int render_text_atlas(Renderer* r, FontAtlas* a, const char* text, int max_bytes, float x, float y, uint32_t color) {
    set_mode(r, 1, a->texture);
    float sx = x;
    const char* p = text;
    const char* end = (max_bytes < 0) ? NULL : text + max_bytes;
    while (*p && (!end || p < end)) {
        int cp = utf8_decode(&p);
        if (cp < 0) break;
        if (end && p > end) break;
        if (cp < 32) continue;
        int idx = atlas_lookup_or_create(a, r, (unsigned int)cp);
        if (idx < 0) continue;
        GlyphInfo* g = &a->glyphs[idx];
        if (g->gw <= 0) continue;
        float gw = (float)g->gw, gh = (float)g->gh;
        push_vtx(r, x,    y,      g->u0, g->v0, color); push_vtx(r, x+gw, y,      g->u1, g->v0, color);
        push_vtx(r, x,    y+gh,   g->u0, g->v1, color); push_vtx(r, x+gw, y,      g->u1, g->v0, color);
        push_vtx(r, x,    y+gh,   g->u0, g->v1, color); push_vtx(r, x+gw, y+gh,   g->u1, g->v1, color);
        x += g->advance;
    }
    return (int)(x - sx);
}

static int width_atlas(Renderer* r, FontAtlas* a, const char* text, int max_bytes) {
    int w = 0;
    const char* p = text;
    const char* end = (max_bytes < 0) ? NULL : text + max_bytes;
    while (*p && (!end || p < end)) {
        int cp = utf8_decode(&p);
        if (cp < 0) break;
        if (end && p > end) break;
        if (cp < 32) continue;
        int idx = atlas_lookup_or_create(a, r, (unsigned int)cp);
        if (idx >= 0) w += a->glyphs[idx].advance;
    }
    return w;
}

int render_text(Renderer* r, const char* text, float x, float y, uint32_t color) {
    return render_text_atlas(r, &r->fonts[0], text, -1, x, y, color);
}

int render_text_n(Renderer* r, const char* text, int max_bytes, float x, float y, uint32_t color) {
    return render_text_atlas(r, &r->fonts[0], text, max_bytes, x, y, color);
}

int render_text_width(Renderer* r, const char* text) {
    return width_atlas(r, &r->fonts[0], text, -1);
}

int render_text_width_n(Renderer* r, const char* text, int max_bytes) {
    return width_atlas(r, &r->fonts[0], text, max_bytes);
}

int render_text_small(Renderer* r, const char* text, float x, float y, uint32_t color) {
    return render_text_atlas(r, &r->fonts[1], text, -1, x, y, color);
}

int render_text_small_n(Renderer* r, const char* text, int max_bytes, float x, float y, uint32_t color) {
    return render_text_atlas(r, &r->fonts[1], text, max_bytes, x, y, color);
}

int render_text_width_small(Renderer* r, const char* text) {
    return width_atlas(r, &r->fonts[1], text, -1);
}

void render_folder_icon(Renderer* r, float x, float y, float sz, uint32_t color) {
    float tab_w = sz * 0.5f, tab_h = sz * 0.25f;
    float body_y = y + tab_h * 0.6f;
    render_quad(r, x, y, tab_w, tab_h + 1, color);
    render_quad(r, x, body_y, sz, sz - tab_h * 0.6f, color);
}

void render_file_icon(Renderer* r, float x, float y, float sz, uint32_t color) {
    render_quad(r, x + 1, y, sz - 2, sz, color);
    /* small fold at top-right */
    render_quad(r, x + sz - 4, y, 3, 3, COL_BG);
}

void render_icon(Renderer* r, GLuint texture, float x, float y, float w, float h) {
    if (!texture) return;
    set_mode(r, 2, texture);
    float x1 = x + w, y1 = y + h;
    uint32_t white = 0xFFFFFFFF;
    push_vtx(r, x, y, 0, 0, white);   push_vtx(r, x1, y, 1, 0, white);  push_vtx(r, x, y1, 0, 1, white);
    push_vtx(r, x1, y, 1, 0, white);  push_vtx(r, x, y1, 0, 1, white);  push_vtx(r, x1, y1, 1, 1, white);
}

/* ---- MDL2 icon atlas ---- */
int render_create_icons(Renderer* r, int size) {
    int atlas_w = ICON_COUNT * size, atlas_h = size;
    BITMAPINFO bmi2 = {0};
    bmi2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi2.bmiHeader.biWidth = atlas_w;
    bmi2.bmiHeader.biHeight = -atlas_h;
    bmi2.bmiHeader.biPlanes = 1;
    bmi2.bmiHeader.biBitCount = 32;
    bmi2.bmiHeader.biCompression = BI_RGB;
    void* bits2 = NULL;
    HDC dc2 = CreateCompatibleDC(NULL);
    HBITMAP bmp2 = CreateDIBSection(dc2, &bmi2, DIB_RGB_COLORS, &bits2, NULL, 0);
    SelectObject(dc2, bmp2);
    memset(bits2, 0, atlas_w * atlas_h * 4);
    HFONT hf = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    SelectObject(dc2, hf);
    SetBkMode(dc2, OPAQUE);
    SetBkColor(dc2, RGB(0,0,0));
    SetTextColor(dc2, RGB(255,255,255));
    WCHAR codes[ICON_COUNT] = {
        0xE76C, 0xE70D, 0xE72B, 0xE72A, 0xE74A, 0xE711,
        0xE921, 0xE922, 0xE923, 0xE710, 0xE81C, 0xE8A4,
        0xEDA2, 0xE707, 0xE80F, 0xE72C, 0xE70E, 0xE76B,
        0xE71B
    };
    for (int i = 0; i < ICON_COUNT; i++) {
        RECT rc2 = { i * size, 0, i * size + size, size };
        DrawTextW(dc2, &codes[i], 1, &rc2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        r->icon_glyphs[i].u0 = (float)(i * size) / atlas_w;
        r->icon_glyphs[i].v0 = 0;
        r->icon_glyphs[i].u1 = (float)(i * size + size) / atlas_w;
        r->icon_glyphs[i].v1 = 1.0f;
        r->icon_glyphs[i].gw = size;
        r->icon_glyphs[i].gh = size;
    }
    GdiFlush();
    uint8_t* iatlas = (uint8_t*)malloc(atlas_w * atlas_h);
    uint8_t* isrc = (uint8_t*)bits2;
    for (int i = 0; i < atlas_w * atlas_h; i++) {
        uint8_t b=isrc[i*4], g2=isrc[i*4+1], rv=isrc[i*4+2];
        iatlas[i] = b>g2 ? (b>rv?b:rv) : (g2>rv?g2:rv);
    }
    glGenTextures(1, &r->icon_texture);
    glBindTexture(GL_TEXTURE_2D, r->icon_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, iatlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(iatlas);
    DeleteObject(bmp2); DeleteObject(hf); DeleteDC(dc2);
    r->icon_size = size;
    return 1;
}

void render_mdl2(Renderer* r, int icon_id, float x, float y, float sz, uint32_t color) {
    if (icon_id < 0 || icon_id >= ICON_COUNT || !r->icon_texture) return;
    set_mode(r, 1, r->icon_texture);
    GlyphInfo* ig = &r->icon_glyphs[icon_id];
    float x1 = x + sz, y1 = y + sz;
    push_vtx(r, x, y, ig->u0, ig->v0, color);   push_vtx(r, x1, y, ig->u1, ig->v0, color);
    push_vtx(r, x, y1, ig->u0, ig->v1, color);  push_vtx(r, x1, y, ig->u1, ig->v0, color);
    push_vtx(r, x, y1, ig->u0, ig->v1, color);  push_vtx(r, x1, y1, ig->u1, ig->v1, color);
}

void render_flush(Renderer* r) {
    if (r->vertex_count == 0) return;
    gl_BindVertexArray(r->vao);
    gl_BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl_BufferSubData(GL_ARRAY_BUFFER, 0, r->vertex_count * sizeof(RVertex), r->vertices);
    gl_Uniform1i(r->loc_text_mode, r->current_text_mode);
    gl_ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->current_tex);
    gl_Uniform1i(r->loc_texture, 0);
    glDrawArrays(GL_TRIANGLES, 0, r->vertex_count);
    r->vertex_count = 0;
}

void render_begin(Renderer* r, int w, int h) {
    r->screen_w = w; r->screen_h = h;
    r->vertex_count = 0;
    r->current_text_mode = 0;
    r->current_tex = r->white_texture;
    glViewport(0, 0, w, h);
    float br=((COL_MANTLE>>16)&0xFF)/255.0f, bg=((COL_MANTLE>>8)&0xFF)/255.0f, bb=(COL_MANTLE&0xFF)/255.0f;
    glClearColor(br, bg, bb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    gl_UseProgram(r->program);
    gl_Uniform2f(r->loc_screen_size, (float)w, (float)h);
}

void render_end(Renderer* r) {
    render_flush(r);
    glDisable(GL_SCISSOR_TEST);
}

void render_scissor(Renderer* r, int x, int y, int w, int h) {
    render_flush(r);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, r->screen_h - y - h, w, h);
}

void render_scissor_reset(Renderer* r) {
    render_flush(r);
    glDisable(GL_SCISSOR_TEST);
}
