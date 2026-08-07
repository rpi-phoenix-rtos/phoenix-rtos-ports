/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PHOENIX

#include "../SDL_sysvideo.h"

#include "SDL_phoenixvideo.h"
#include "SDL_phoenixopengl.h"

/* Pull in real extern prototypes for the GL 1.5+/2.0+/FBO entry points so the
 * GetProcAddress table below can take their addresses. They resolve at the
 * game/test final link from libGL-phoenix.a. Must precede SDL_opengl.h. */
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"

#include <string.h>
#include <unistd.h>

int PHOENIX_GL_LoadLibrary(_THIS, const char *path)
{
    /* Mesa is statically linked into the program; nothing to load. SDL's
     * SDL_GL_LoadLibrary() maintains gl_config.driver_loaded itself, so we only
     * record a path and succeed. */
    (void)path;
    _this->gl_config.dll_handle = NULL;
    SDL_strlcpy(_this->gl_config.driver_path, "(builtin mesa-v3d)",
                SDL_arraysize(_this->gl_config.driver_path));
    return 0;
}

void PHOENIX_GL_UnloadLibrary(_THIS)
{
    (void)_this;
}

/*
 * Static name -> function-pointer table. Phoenix has no dlopen/dlsym, so
 * SDL_GL_GetProcAddress is served from this hand-maintained table of the Mesa
 * GL entry points the SDL renderer and the ported applications use. It mirrors
 * the set the in-scope GL applications proved present in libGL-phoenix.a; extend
 * it as new applications need more of the GL surface.
 */
typedef struct
{
    const char *name;
    void *addr;
} PHOENIX_GLProc;

/* Winsys: bind the current scanout-FB-backed FBO (defined in the GL-context glue). */
extern void phxgl_bind_fbo(void);

/* The surfaceless V3D context has no usable default framebuffer (FB 0) — the winsys scanout FBO is
 * the screen. GL1-class renderers bind FB 0 to "return to the backbuffer",
 * which would dereference a NULL default framebuffer in Mesa (_mesa_bind_framebuffers). Redirect a
 * bind of 0 to the scanout FBO via the winsys; pass real (nonzero) FBO ids through unchanged. */
static void PHOENIX_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (framebuffer == 0) {
        phxgl_bind_fbo();
    }
    else {
        glBindFramebuffer(target, framebuffer);
    }
}

static const PHOENIX_GLProc phoenix_gl_procs[] = {
    /* --- GL 1.1 core --- */
    { "glClear", (void *)glClear },
    { "glClearColor", (void *)glClearColor },
    { "glClearDepth", (void *)glClearDepth },
    { "glViewport", (void *)glViewport },
    { "glScissor", (void *)glScissor },
    { "glGetString", (void *)glGetString },
    { "glGetError", (void *)glGetError },
    { "glGetIntegerv", (void *)glGetIntegerv },
    { "glGetFloatv", (void *)glGetFloatv },
    { "glEnable", (void *)glEnable },
    { "glDisable", (void *)glDisable },
    { "glFinish", (void *)glFinish },
    { "glFlush", (void *)glFlush },
    { "glReadPixels", (void *)glReadPixels },
    { "glReadBuffer", (void *)glReadBuffer },
    { "glPixelStorei", (void *)glPixelStorei },
    { "glDrawArrays", (void *)glDrawArrays },
    { "glDrawElements", (void *)glDrawElements },
    { "glBindTexture", (void *)glBindTexture },
    { "glGenTextures", (void *)glGenTextures },
    { "glDeleteTextures", (void *)glDeleteTextures },
    { "glTexImage2D", (void *)glTexImage2D },
    { "glTexSubImage2D", (void *)glTexSubImage2D },
    { "glTexParameteri", (void *)glTexParameteri },
    { "glTexParameterf", (void *)glTexParameterf },
    { "glTexEnvi", (void *)glTexEnvi },
    { "glTexEnvf", (void *)glTexEnvf },
    { "glDepthFunc", (void *)glDepthFunc },
    { "glDepthMask", (void *)glDepthMask },
    { "glDepthRange", (void *)glDepthRange },
    { "glBlendFunc", (void *)glBlendFunc },
    /* core GL 1.x entrypoints required by GL1-class renderers (QGL_Core_PROCS) */
    { "glDrawBuffer", (void *)glDrawBuffer },
    { "glGetBooleanv", (void *)glGetBooleanv },
    { "glLineWidth", (void *)glLineWidth },
    { "glNormalPointer", (void *)glNormalPointer },
    { "glPolygonMode", (void *)glPolygonMode },
    { "glPolygonOffset", (void *)glPolygonOffset },
    { "glStencilFunc", (void *)glStencilFunc },
    { "glStencilOp", (void *)glStencilOp },
    { "glAlphaFunc", (void *)glAlphaFunc },
    { "glCullFace", (void *)glCullFace },
    { "glFrontFace", (void *)glFrontFace },
    { "glShadeModel", (void *)glShadeModel },
    { "glColorMask", (void *)glColorMask },
    { "glColor4f", (void *)glColor4f },
    { "glColor3f", (void *)glColor3f },
    { "glTexCoord2f", (void *)glTexCoord2f },
    { "glVertex2f", (void *)glVertex2f },
    { "glVertex3f", (void *)glVertex3f },
    { "glBegin", (void *)glBegin },
    { "glEnd", (void *)glEnd },
    { "glMatrixMode", (void *)glMatrixMode },
    { "glLoadIdentity", (void *)glLoadIdentity },
    { "glLoadMatrixf", (void *)glLoadMatrixf },
    { "glPushMatrix", (void *)glPushMatrix },
    { "glPopMatrix", (void *)glPopMatrix },
    { "glOrtho", (void *)glOrtho },
    { "glFrustum", (void *)glFrustum },
    { "glTranslatef", (void *)glTranslatef },
    { "glRotatef", (void *)glRotatef },
    { "glScalef", (void *)glScalef },
    /* --- GL 1.3/1.5 multitexture + VBO --- */
    { "glActiveTexture", (void *)glActiveTexture },
    { "glClientActiveTexture", (void *)glClientActiveTexture },
    { "glMultiTexCoord2f", (void *)glMultiTexCoord2f },
    { "glGenBuffers", (void *)glGenBuffers },
    { "glBindBuffer", (void *)glBindBuffer },
    { "glBufferData", (void *)glBufferData },
    { "glBufferSubData", (void *)glBufferSubData },
    { "glDeleteBuffers", (void *)glDeleteBuffers },
    { "glEnableClientState", (void *)glEnableClientState },
    { "glDisableClientState", (void *)glDisableClientState },
    { "glVertexPointer", (void *)glVertexPointer },
    { "glTexCoordPointer", (void *)glTexCoordPointer },
    { "glColorPointer", (void *)glColorPointer },
    /* --- GL 2.0 shaders --- */
    { "glCreateShader", (void *)glCreateShader },
    { "glDeleteShader", (void *)glDeleteShader },
    { "glShaderSource", (void *)glShaderSource },
    { "glCompileShader", (void *)glCompileShader },
    { "glGetShaderiv", (void *)glGetShaderiv },
    { "glGetShaderInfoLog", (void *)glGetShaderInfoLog },
    { "glCreateProgram", (void *)glCreateProgram },
    { "glDeleteProgram", (void *)glDeleteProgram },
    { "glAttachShader", (void *)glAttachShader },
    { "glLinkProgram", (void *)glLinkProgram },
    { "glUseProgram", (void *)glUseProgram },
    { "glGetProgramiv", (void *)glGetProgramiv },
    { "glGetProgramInfoLog", (void *)glGetProgramInfoLog },
    { "glBindAttribLocation", (void *)glBindAttribLocation },
    { "glGetAttribLocation", (void *)glGetAttribLocation },
    { "glVertexAttribPointer", (void *)glVertexAttribPointer },
    { "glEnableVertexAttribArray", (void *)glEnableVertexAttribArray },
    { "glDisableVertexAttribArray", (void *)glDisableVertexAttribArray },
    { "glGetUniformLocation", (void *)glGetUniformLocation },
    { "glUniform1i", (void *)glUniform1i },
    { "glUniform1f", (void *)glUniform1f },
    { "glUniform2f", (void *)glUniform2f },
    { "glUniform3f", (void *)glUniform3f },
    { "glUniform4f", (void *)glUniform4f },
    { "glUniformMatrix4fv", (void *)glUniformMatrix4fv },
    /* --- ARB framebuffer object (FBO) --- */
    { "glGenFramebuffers", (void *)glGenFramebuffers },
    { "glBindFramebuffer", (void *)PHOENIX_glBindFramebuffer }, /* maps FB 0 -> scanout FBO */
    { "glDeleteFramebuffers", (void *)glDeleteFramebuffers },
    { "glGenRenderbuffers", (void *)glGenRenderbuffers },
    { "glBindRenderbuffer", (void *)glBindRenderbuffer },
    { "glDeleteRenderbuffers", (void *)glDeleteRenderbuffers },
    { "glRenderbufferStorage", (void *)glRenderbufferStorage },
    { "glFramebufferRenderbuffer", (void *)glFramebufferRenderbuffer },
    { "glFramebufferTexture2D", (void *)glFramebufferTexture2D },
    { "glCheckFramebufferStatus", (void *)glCheckFramebufferStatus },
    { "glBlitFramebuffer", (void *)glBlitFramebuffer },
    { "glGenerateMipmap", (void *)glGenerateMipmap },
    { "glGetFramebufferAttachmentParameteriv", (void *)glGetFramebufferAttachmentParameteriv },
    { "glGetRenderbufferParameteriv", (void *)glGetRenderbufferParameteriv },
    { "glIsFramebuffer", (void *)glIsFramebuffer },
    { "glRenderbufferStorageMultisample", (void *)glRenderbufferStorageMultisample },
    /* --- ARB-suffixed entrypoints resolved by GL1-class renderers (QGL_Ext/ARB/VBO_PROCS).
     * These engines look up the ARB/EXT names explicitly (not the core aliases), so provide them. --- */
    { "glActiveTextureARB", (void *)glActiveTextureARB },
    { "glClientActiveTextureARB", (void *)glClientActiveTextureARB },
    { "glMultiTexCoord2fARB", (void *)glMultiTexCoord2fARB },
    { "glLockArraysEXT", (void *)glLockArraysEXT },
    { "glUnlockArraysEXT", (void *)glUnlockArraysEXT },
    { "glBindBufferARB", (void *)glBindBufferARB },
    { "glBufferDataARB", (void *)glBufferDataARB },
    { "glDeleteBuffersARB", (void *)glDeleteBuffersARB },
    { "glGenBuffersARB", (void *)glGenBuffersARB },
    { "glBindProgramARB", (void *)glBindProgramARB },
    { "glDeleteProgramsARB", (void *)glDeleteProgramsARB },
    { "glGenProgramsARB", (void *)glGenProgramsARB },
    { "glProgramStringARB", (void *)glProgramStringARB },
    { "glProgramLocalParameter4fARB", (void *)glProgramLocalParameter4fARB },
    { "glProgramLocalParameter4fvARB", (void *)glProgramLocalParameter4fvARB },
};

void *PHOENIX_GL_GetProcAddress(_THIS, const char *proc)
{
    size_t i;
    (void)_this;
    if (!proc) {
        return NULL;
    }
    for (i = 0; i < SDL_arraysize(phoenix_gl_procs); ++i) {
        if (SDL_strcmp(proc, phoenix_gl_procs[i].name) == 0) {
            return phoenix_gl_procs[i].addr;
        }
    }
    SDL_SetError("PHOENIX: GL entry point '%s' not in the static proc table", proc);
    return NULL;
}

SDL_GLContext PHOENIX_GL_CreateContext(_THIS, SDL_Window *window)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    int *ctx;

    if (data->gl_created) {
        SDL_SetError("PHOENIX: a GL context already exists (single-window driver)");
        return NULL;
    }

    /* Create the V3D screen + Mesa state-tracker context + scanout FBO(s) at the
     * window's (native) size. Provided by the separately-linked GL glue TU. */
    if (phxgl_init(window->w, window->h) != 0) {
        SDL_SetError("PHOENIX: phxgl_init (V3D GL context create) failed");
        return NULL;
    }
    phxgl_make_current();
    phxgl_bind_fbo(); /* the surfaceless context has no usable default FB 0 */

    data->gl_created = 1;
    data->scanout = v3d_phoenix_scanout_active();

    /* SDL only needs an opaque non-NULL handle; store a token we can free. */
    ctx = (int *)SDL_malloc(sizeof(int));
    if (!ctx) {
        SDL_OutOfMemory();
        return NULL;
    }
    *ctx = 1;
    return (SDL_GLContext)ctx;
}

int PHOENIX_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
    (void)_this;
    (void)window;
    if (context) {
        phxgl_make_current();
        phxgl_bind_fbo();
    }
    return 0;
}

int PHOENIX_GL_SetSwapInterval(_THIS, int interval)
{
    /* The page-flip present always latches at vsync; treat as vsync-on. */
    (void)_this;
    (void)interval;
    return 0;
}

int PHOENIX_GL_GetSwapInterval(_THIS)
{
    (void)_this;
    return 1;
}

int PHOENIX_GL_SwapWindow(_THIS, SDL_Window *window)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    (void)window;

    glFinish(); /* submit + synchronously finish the frame before the flip */

    if (phxgl_resolve()) {
        /* Scanout path: the just-rendered back buffer was page-flipped to the
         * display; bind the next back buffer for the following frame. */
        phxgl_bind_fbo();
        return 0;
    }

    /* Degraded fallback (V3D scanout not claimed, e.g. headless bring-up): copy
     * the rendered FBO out and blit it to /dev/fb0. Not pipelined and not
     * Y-flipped — the scanout path above is the real presentation path. */
    if (data->fb_fd >= 0 && data->width && data->height) {
        static void *cpubuf = NULL;
        static size_t cpubuf_len = 0;
        size_t need = (size_t)data->width * data->height * 4;
        if (cpubuf_len < need) {
            void *nb = SDL_realloc(cpubuf, need);
            if (nb) {
                cpubuf = nb;
                cpubuf_len = need;
            }
        }
        if (cpubuf && cpubuf_len >= need) {
            glReadPixels(0, 0, (int)data->width, (int)data->height,
                         GL_RGBA, GL_UNSIGNED_BYTE, cpubuf);
            (void)lseek(data->fb_fd, 0, SEEK_SET);
            (void)write(data->fb_fd, cpubuf, need);
        }
    }
    return 0;
}

void PHOENIX_GL_DeleteContext(_THIS, SDL_GLContext context)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    /* The Mesa context lives for the process; just drop our token. */
    if (context) {
        SDL_free(context);
    }
    data->gl_created = 0;
}

#endif /* SDL_VIDEO_DRIVER_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
