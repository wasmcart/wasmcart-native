// gl_track.c — the GL state a libretro core saves and restores around each
// cart frame, kept as it is set instead of read back.
//
// RetroArch changes GL state between retro_run calls (its menu, overlays), so
// the core saves the cart's state after each frame and restores it before the
// next. Reading it back took about 20 glGetIntegerv / glIsEnabled calls a
// frame, and each one is a full sync with Mesa's GL thread (glthread). Every
// GL call this process makes goes through gl_procs.h, so the calls that set
// those states update a copy here as they pass (gl_procs.h maps them to the
// wc_trk_ functions below), and wc_gl_track_get reads the copy. The copy is
// exact while nothing else changes GL state: from wc_gl_track_reset (one read
// of the real state, when a context is new) to a frame's save, and from each
// restore (which sets every tracked state through these functions) on.
#include <GLES3/gl3.h>
#include <stdlib.h>
#include <string.h>
#include "gl_procs.h"

#define MAX_UNITS 192

static struct {
    GLint program, vao, draw_fbo, active_tex, array_buf, viewport[4];
    GLint tex2d[MAX_UNITS];  // GL_TEXTURE_BINDING_2D per texture unit
    GLint* elem;             // GL_ELEMENT_ARRAY_BUFFER_BINDING per VAO id (VAO state)
    GLuint elem_cap;
    GLboolean blend, depth_test, scissor_test, stencil_test, cull_face, dither;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    GLboolean depth_mask, color_mask[4];
    GLint stencil_mask;
    GLfloat clear_color[4];
} T;

static GLint* elem_slot(GLuint vao) {
    if (vao >= T.elem_cap) {
        GLuint cap = T.elem_cap ? T.elem_cap : 64;
        while (cap <= vao) cap *= 2;
        GLint* e = (GLint*)realloc(T.elem, cap * sizeof *e);
        if (!e) return NULL;
        memset(e + T.elem_cap, 0, (cap - T.elem_cap) * sizeof *e);
        T.elem = e;
        T.elem_cap = cap;
    }
    return &T.elem[vao];
}

static GLboolean* cap_slot(GLenum cap) {
    switch (cap) {
    case GL_BLEND: return &T.blend;
    case GL_DEPTH_TEST: return &T.depth_test;
    case GL_SCISSOR_TEST: return &T.scissor_test;
    case GL_STENCIL_TEST: return &T.stencil_test;
    case GL_CULL_FACE: return &T.cull_face;
    case GL_DITHER: return &T.dither;
    default: return NULL;
    }
}

void wc_gl_track_reset(void) {
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &T.program);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &T.vao);
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &T.draw_fbo);
    p_glGetIntegerv(GL_ACTIVE_TEXTURE, &T.active_tex);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &T.array_buf);
    p_glGetIntegerv(GL_VIEWPORT, T.viewport);
    // the units' 2D bindings: each unit read once, then the active one put back
    GLint units = 0;
    p_glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    if (units > MAX_UNITS) units = MAX_UNITS;
    memset(T.tex2d, 0, sizeof T.tex2d);
    for (GLint u = 0; u < units; u++) {
        p_glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &T.tex2d[u]);
    }
    p_glActiveTexture((GLenum)T.active_tex);
    if (T.elem) memset(T.elem, 0, T.elem_cap * sizeof *T.elem);
    GLint* e = elem_slot((GLuint)T.vao);
    if (e) p_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, e);
    T.blend = p_glIsEnabled(GL_BLEND);
    T.depth_test = p_glIsEnabled(GL_DEPTH_TEST);
    T.scissor_test = p_glIsEnabled(GL_SCISSOR_TEST);
    T.stencil_test = p_glIsEnabled(GL_STENCIL_TEST);
    T.cull_face = p_glIsEnabled(GL_CULL_FACE);
    T.dither = p_glIsEnabled(GL_DITHER);
    p_glGetIntegerv(GL_BLEND_SRC_RGB, &T.blend_src_rgb);
    p_glGetIntegerv(GL_BLEND_DST_RGB, &T.blend_dst_rgb);
    p_glGetIntegerv(GL_BLEND_SRC_ALPHA, &T.blend_src_a);
    p_glGetIntegerv(GL_BLEND_DST_ALPHA, &T.blend_dst_a);
    p_glGetIntegerv(GL_BLEND_EQUATION_RGB, &T.blend_eq_rgb);
    p_glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &T.blend_eq_a);
    p_glGetBooleanv(GL_DEPTH_WRITEMASK, &T.depth_mask);
    p_glGetBooleanv(GL_COLOR_WRITEMASK, T.color_mask);
    p_glGetIntegerv(GL_STENCIL_WRITEMASK, &T.stencil_mask);
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, T.clear_color);
}

void wc_gl_track_get(wc_gl_tracked_state* s) {
    s->program = T.program;
    s->vao = T.vao;
    s->fbo = T.draw_fbo;
    s->active_tex = T.active_tex;
    GLint unit = T.active_tex - (GLint)GL_TEXTURE0;
    s->tex_2d = unit >= 0 && unit < MAX_UNITS ? T.tex2d[unit] : 0;
    s->array_buf = T.array_buf;
    s->elem_buf = (GLuint)T.vao < T.elem_cap ? T.elem[T.vao] : 0;
    memcpy(s->viewport, T.viewport, sizeof s->viewport);
    s->blend = T.blend;
    s->depth_test = T.depth_test;
    s->scissor_test = T.scissor_test;
    s->stencil_test = T.stencil_test;
    s->cull_face = T.cull_face;
    s->dither = T.dither;
    s->blend_src_rgb = T.blend_src_rgb;
    s->blend_dst_rgb = T.blend_dst_rgb;
    s->blend_src_a = T.blend_src_a;
    s->blend_dst_a = T.blend_dst_a;
    s->blend_eq_rgb = T.blend_eq_rgb;
    s->blend_eq_a = T.blend_eq_a;
    s->depth_mask = T.depth_mask;
    memcpy(s->color_mask, T.color_mask, sizeof s->color_mask);
    s->stencil_mask = T.stencil_mask;
    memcpy(s->clear_color, T.clear_color, sizeof s->clear_color);
}

// ── the calls that set them ───────────────────────────────────────────────

void wc_trk_glUseProgram(GLuint program) { T.program = (GLint)program; p_glUseProgram(program); }
void wc_trk_glBindVertexArray(GLuint vao) { T.vao = (GLint)vao; elem_slot(vao); p_glBindVertexArray(vao); }
void wc_trk_glBindFramebuffer(GLenum target, GLuint fb) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) T.draw_fbo = (GLint)fb;
    p_glBindFramebuffer(target, fb);
}
void wc_trk_glActiveTexture(GLenum unit) { T.active_tex = (GLint)unit; p_glActiveTexture(unit); }
void wc_trk_glBindTexture(GLenum target, GLuint tex) {
    GLint unit = T.active_tex - (GLint)GL_TEXTURE0;
    if (target == GL_TEXTURE_2D && unit >= 0 && unit < MAX_UNITS) T.tex2d[unit] = (GLint)tex;
    p_glBindTexture(target, tex);
}
void wc_trk_glBindBuffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) T.array_buf = (GLint)buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) { GLint* e = elem_slot((GLuint)T.vao); if (e) *e = (GLint)buf; }
    p_glBindBuffer(target, buf);
}
void wc_trk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    T.viewport[0] = x; T.viewport[1] = y; T.viewport[2] = w; T.viewport[3] = h;
    p_glViewport(x, y, w, h);
}
void wc_trk_glEnable(GLenum cap) { GLboolean* c = cap_slot(cap); if (c) *c = GL_TRUE; p_glEnable(cap); }
void wc_trk_glDisable(GLenum cap) { GLboolean* c = cap_slot(cap); if (c) *c = GL_FALSE; p_glDisable(cap); }
void wc_trk_glBlendFunc(GLenum s, GLenum d) {
    T.blend_src_rgb = T.blend_src_a = (GLint)s;
    T.blend_dst_rgb = T.blend_dst_a = (GLint)d;
    p_glBlendFunc(s, d);
}
void wc_trk_glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    T.blend_src_rgb = (GLint)sr; T.blend_dst_rgb = (GLint)dr; T.blend_src_a = (GLint)sa; T.blend_dst_a = (GLint)da;
    p_glBlendFuncSeparate(sr, dr, sa, da);
}
void wc_trk_glBlendEquation(GLenum m) { T.blend_eq_rgb = T.blend_eq_a = (GLint)m; p_glBlendEquation(m); }
void wc_trk_glBlendEquationSeparate(GLenum rgb, GLenum a) { T.blend_eq_rgb = (GLint)rgb; T.blend_eq_a = (GLint)a; p_glBlendEquationSeparate(rgb, a); }
void wc_trk_glDepthMask(GLboolean m) { T.depth_mask = m; p_glDepthMask(m); }
void wc_trk_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    T.color_mask[0] = r; T.color_mask[1] = g; T.color_mask[2] = b; T.color_mask[3] = a;
    p_glColorMask(r, g, b, a);
}
void wc_trk_glStencilMask(GLuint m) { T.stencil_mask = (GLint)m; p_glStencilMask(m); }
void wc_trk_glStencilMaskSeparate(GLenum face, GLuint m) {
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) T.stencil_mask = (GLint)m;
    p_glStencilMaskSeparate(face, m);
}
void wc_trk_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    T.clear_color[0] = r; T.clear_color[1] = g; T.clear_color[2] = b; T.clear_color[3] = a;
    p_glClearColor(r, g, b, a);
}
// deleting a bound object unbinds it (in the current VAO for its element buffer)
void wc_trk_glDeleteBuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; ids && i < n; i++) {
        if (!ids[i]) continue;
        if (T.array_buf == (GLint)ids[i]) T.array_buf = 0;
        if ((GLuint)T.vao < T.elem_cap && T.elem[T.vao] == (GLint)ids[i]) T.elem[T.vao] = 0;
    }
    p_glDeleteBuffers(n, ids);
}
void wc_trk_glDeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; ids && i < n; i++)
        if (ids[i]) for (int u = 0; u < MAX_UNITS; u++) if (T.tex2d[u] == (GLint)ids[i]) T.tex2d[u] = 0;
    p_glDeleteTextures(n, ids);
}
void wc_trk_glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; ids && i < n; i++) if (ids[i] && T.draw_fbo == (GLint)ids[i]) T.draw_fbo = 0;
    p_glDeleteFramebuffers(n, ids);
}
void wc_trk_glDeleteVertexArrays(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; ids && i < n; i++) {
        if (!ids[i]) continue;
        if (T.vao == (GLint)ids[i]) T.vao = 0;
        if (ids[i] < T.elem_cap) T.elem[ids[i]] = 0;
    }
    p_glDeleteVertexArrays(n, ids);
}
