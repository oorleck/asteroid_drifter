// gl.h -- minimal self-contained OpenGL 3.3 core loader (no GLEW/GLAD needed).
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>
#include <cstdint>

typedef char        GLchar;
typedef ptrdiff_t   GLsizeiptr;
typedef ptrdiff_t   GLintptr;
#else
// On Linux libGL exports every entry point up to 4.6, so there is nothing to load:
// the prototypes in glext.h are the real functions and the loader below is skipped.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "keys.h"
#include <cstddef>
#include <cstdint>
#endif

// ---- enums not present in the ancient Windows gl.h -------------------------
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                   0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW                    0x88E0
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW                    0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW                   0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER                0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER                  0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS                 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS                    0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH                0x8B84
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                       0x84C0
#endif
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER                 0x8C2A
#endif
#ifndef GL_MAX_TEXTURE_BUFFER_SIZE
#define GL_MAX_TEXTURE_BUFFER_SIZE        0x8C2B
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F                        0x8814
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F                        0x881A
#endif
#ifndef GL_RGB16F
#define GL_RGB16F                         0x881B
#endif
#ifndef GL_R11F_G11F_B10F
#define GL_R11F_G11F_B10F                 0x8C3A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT                     0x140B
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER               0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0              0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                  0x812F
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD                       0x8006
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE                    0x809D
#endif
#ifndef GL_VERTEX_PROGRAM_POINT_SIZE
#define GL_VERTEX_PROGRAM_POINT_SIZE      0x8642
#endif
#ifndef GL_POINT_SPRITE
#define GL_POINT_SPRITE                   0x8861
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL              0x813D
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION                  0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION                  0x821C
#endif

// ---- WGL extension enums --------------------------------------------------
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB         0x00000001
#define WGL_DRAW_TO_WINDOW_ARB            0x2001
#define WGL_ACCELERATION_ARB              0x2003
#define WGL_SUPPORT_OPENGL_ARB            0x2010
#define WGL_DOUBLE_BUFFER_ARB             0x2011
#define WGL_PIXEL_TYPE_ARB                0x2013
#define WGL_COLOR_BITS_ARB                0x2014
#define WGL_DEPTH_BITS_ARB                0x2022
#define WGL_STENCIL_BITS_ARB              0x2023
#define WGL_FULL_ACCELERATION_ARB         0x2027
#define WGL_TYPE_RGBA_ARB                 0x202B
#define WGL_SAMPLE_BUFFERS_ARB            0x2041
#define WGL_SAMPLES_ARB                   0x2042

// ---- the function pointers we actually use --------------------------------
// X(return, name, params)
#define GL_FUNCTION_LIST(X) \
X(GLuint,  glCreateShader,      (GLenum)) \
X(void,    glDeleteShader,      (GLuint)) \
X(void,    glShaderSource,      (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
X(void,    glCompileShader,     (GLuint)) \
X(void,    glGetShaderiv,       (GLuint, GLenum, GLint*)) \
X(void,    glGetShaderInfoLog,  (GLuint, GLsizei, GLsizei*, GLchar*)) \
X(GLuint,  glCreateProgram,     (void)) \
X(void,    glDeleteProgram,     (GLuint)) \
X(void,    glAttachShader,      (GLuint, GLuint)) \
X(void,    glLinkProgram,       (GLuint)) \
X(void,    glGetProgramiv,      (GLuint, GLenum, GLint*)) \
X(void,    glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
X(void,    glUseProgram,        (GLuint)) \
X(GLint,   glGetUniformLocation,(GLuint, const GLchar*)) \
X(void,    glUniform1i,         (GLint, GLint)) \
X(void,    glUniform1f,         (GLint, GLfloat)) \
X(void,    glUniform2f,         (GLint, GLfloat, GLfloat)) \
X(void,    glUniform3f,         (GLint, GLfloat, GLfloat, GLfloat)) \
X(void,    glUniform4f,         (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
X(void,    glBindAttribLocation,(GLuint, GLuint, const GLchar*)) \
X(void,    glBindFragDataLocation,(GLuint, GLuint, const GLchar*)) \
X(void,    glGenBuffers,        (GLsizei, GLuint*)) \
X(void,    glDeleteBuffers,     (GLsizei, const GLuint*)) \
X(void,    glBindBuffer,        (GLenum, GLuint)) \
X(void,    glBufferData,        (GLenum, GLsizeiptr, const void*, GLenum)) \
X(void,    glBufferSubData,     (GLenum, GLintptr, GLsizeiptr, const void*)) \
X(void,    glGenVertexArrays,   (GLsizei, GLuint*)) \
X(void,    glDeleteVertexArrays,(GLsizei, const GLuint*)) \
X(void,    glBindVertexArray,   (GLuint)) \
X(void,    glEnableVertexAttribArray, (GLuint)) \
X(void,    glDisableVertexAttribArray,(GLuint)) \
X(void,    glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
X(void,    glVertexAttribIPointer,(GLuint, GLint, GLenum, GLsizei, const void*)) \
X(void,    glActiveTexture,     (GLenum)) \
X(void,    glTexBuffer,         (GLenum, GLenum, GLuint)) \
X(void,    glGenFramebuffers,   (GLsizei, GLuint*)) \
X(void,    glDeleteFramebuffers,(GLsizei, const GLuint*)) \
X(void,    glBindFramebuffer,   (GLenum, GLuint)) \
X(void,    glFramebufferTexture2D,(GLenum, GLenum, GLenum, GLuint, GLint)) \
X(GLenum,  glCheckFramebufferStatus,(GLenum)) \
X(void,    glDrawBuffers,       (GLsizei, const GLenum*)) \
X(void,    glMultiDrawArrays,   (GLenum, const GLint*, const GLsizei*, GLsizei)) \
X(void,    glBlendEquation,     (GLenum)) \
X(void,    glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))

#ifdef _WIN32
#define GL_DECLARE(ret, name, params) typedef ret (APIENTRY *PFN_##name) params; extern PFN_##name name;
GL_FUNCTION_LIST(GL_DECLARE)
#undef GL_DECLARE

// WGL extensions
typedef HGLRC (APIENTRY *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL  (APIENTRY *PFN_wglChoosePixelFormatARB)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
typedef BOOL  (APIENTRY *PFN_wglSwapIntervalEXT)(int);
extern PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB;
extern PFN_wglChoosePixelFormatARB    wglChoosePixelFormatARB;
extern PFN_wglSwapIntervalEXT         wglSwapIntervalEXT;

bool glLoadCoreFunctions();      // after a context is current
bool wglLoadExtensions();        // needs a dummy context current
#else
inline bool glLoadCoreFunctions() { return true; }   // linked, not loaded
#endif
