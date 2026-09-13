// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal OpenGL 4.1 core-profile loader for Sweep++.
//
// This replaces a generated glad blob with the subset the renderer actually
// calls -- roughly 70 entry points instead of 40k lines -- so the surface we
// depend on stays reviewable. It is committed source; nothing is fetched.
//
// Entry points are resolved through a caller-supplied getProcAddress (in
// practice glfwGetProcAddress, which on Windows also covers the GL 1.x
// functions that wglGetProcAddress refuses to return).
//
// Deliberately NOT included by imgui_impl_opengl3.cpp -- that backend keeps
// its own bundled loader. The two never meet in one translation unit.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLclampx;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned int GLhandleARB;
typedef struct __GLsync* GLsync;

/* -------------------------------------------------------------- constants */

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_NONE 0

#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FUNC_ADD 0x8006
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_MULTISAMPLE 0x809D
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406

#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872

#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2

#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLAMP_TO_BORDER 0x812D

#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_R8 0x8229
#define GL_R32F 0x822E
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_RGBA32F 0x8814

#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ALIGNMENT 0x0D05

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

/* ----------------------------------------------------------- entry points */

/* One line per function: SWEEPPP_GL_FN(return, name, (params)) */
#define SWEEPPP_GL_FUNCTIONS(X)                                                                    \
    X(void, glClear, (GLbitfield mask))                                                            \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                            \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                  \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                   \
    X(void, glEnable, (GLenum cap))                                                                \
    X(void, glDisable, (GLenum cap))                                                               \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))                                         \
    X(void, glBlendEquation, (GLenum mode))                                                        \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                            \
    X(GLenum, glGetError, (void))                                                                  \
    X(const GLubyte*, glGetString, (GLenum name))                                                  \
    X(void, glGetIntegerv, (GLenum pname, GLint * data))                                           \
    X(void, glFinish, (void))                                                                      \
    X(void, glFlush, (void))                                                                       \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                               \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))        \
    X(void, glReadPixels,                                                                          \
      (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels))          \
    /* textures */                                                                                 \
    X(void, glGenTextures, (GLsizei n, GLuint * textures))                                         \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                                 \
    X(void, glBindTexture, (GLenum target, GLuint texture))                                        \
    X(void, glActiveTexture, (GLenum texture))                                                     \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                           \
    X(void, glTexParameterfv, (GLenum target, GLenum pname, const GLfloat* params))                \
    X(void, glTexImage1D,                                                                          \
      (GLenum target, GLint level, GLint internalFormat, GLsizei w, GLint border, GLenum format,   \
       GLenum type, const void* pixels))                                                           \
    X(void, glTexImage2D,                                                                          \
      (GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint border,       \
       GLenum format, GLenum type, const void* pixels))                                            \
    X(void, glTexSubImage1D,                                                                       \
      (GLenum target, GLint level, GLint xoffset, GLsizei w, GLenum format, GLenum type,           \
       const void* pixels))                                                                        \
    X(void, glTexSubImage2D,                                                                       \
      (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei w, GLsizei h,             \
       GLenum format, GLenum type, const void* pixels))                                            \
    X(void, glGetTexImage, (GLenum target, GLint level, GLenum format, GLenum type, void* pixels)) \
    /* buffers and vertex arrays */                                                                \
    X(void, glGenBuffers, (GLsizei n, GLuint * buffers))                                           \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                                   \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                          \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))        \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))  \
    X(void, glGenVertexArrays, (GLsizei n, GLuint * arrays))                                       \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                               \
    X(void, glBindVertexArray, (GLuint array))                                                     \
    X(void, glEnableVertexAttribArray, (GLuint index))                                             \
    X(void, glDisableVertexAttribArray, (GLuint index))                                            \
    X(void, glVertexAttribPointer,                                                                 \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,                \
       const void* pointer))                                                                       \
    /* shaders */                                                                                  \
    X(GLuint, glCreateShader, (GLenum type))                                                       \
    X(void, glDeleteShader, (GLuint shader))                                                       \
    X(void, glShaderSource,                                                                        \
      (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))            \
    X(void, glCompileShader, (GLuint shader))                                                      \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint * params))                          \
    X(void, glGetShaderInfoLog,                                                                    \
      (GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog))                        \
    X(GLuint, glCreateProgram, (void))                                                             \
    X(void, glDeleteProgram, (GLuint program))                                                     \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                       \
    X(void, glDetachShader, (GLuint program, GLuint shader))                                       \
    X(void, glLinkProgram, (GLuint program))                                                       \
    X(void, glUseProgram, (GLuint program))                                                        \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint * params))                        \
    X(void, glGetProgramInfoLog,                                                                   \
      (GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog))                       \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                           \
    X(GLint, glGetAttribLocation, (GLuint program, const GLchar* name))                            \
    X(void, glUniform1i, (GLint location, GLint v0))                                               \
    X(void, glUniform1f, (GLint location, GLfloat v0))                                             \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                 \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                     \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))         \
    X(void, glUniformMatrix4fv,                                                                    \
      (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))                  \
    /* framebuffers (waterfall readback / snapshot export) */                                      \
    X(void, glGenFramebuffers, (GLsizei n, GLuint * framebuffers))                                 \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                         \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                \
    X(void, glFramebufferTexture2D,                                                                \
      (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))           \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))

#define SWEEPPP_GL_DECLARE(ret, name, params)                                                      \
    typedef ret(*PFN_sweeppp_##name) params;                                                       \
    extern PFN_sweeppp_##name sweeppp_##name;
SWEEPPP_GL_FUNCTIONS(SWEEPPP_GL_DECLARE)
#undef SWEEPPP_GL_DECLARE

/* Call the loaded pointers by their normal GL names. These macros are local to
 * whichever translation unit includes this header, so they cannot collide with
 * the loader bundled inside imgui_impl_opengl3.cpp. */
#define glClear sweeppp_glClear
#define glClearColor sweeppp_glClearColor
#define glViewport sweeppp_glViewport
#define glScissor sweeppp_glScissor
#define glEnable sweeppp_glEnable
#define glDisable sweeppp_glDisable
#define glBlendFunc sweeppp_glBlendFunc
#define glBlendEquation sweeppp_glBlendEquation
#define glPixelStorei sweeppp_glPixelStorei
#define glGetError sweeppp_glGetError
#define glGetString sweeppp_glGetString
#define glGetIntegerv sweeppp_glGetIntegerv
#define glFinish sweeppp_glFinish
#define glFlush sweeppp_glFlush
#define glDrawArrays sweeppp_glDrawArrays
#define glDrawElements sweeppp_glDrawElements
#define glReadPixels sweeppp_glReadPixels
#define glGenTextures sweeppp_glGenTextures
#define glDeleteTextures sweeppp_glDeleteTextures
#define glBindTexture sweeppp_glBindTexture
#define glActiveTexture sweeppp_glActiveTexture
#define glTexParameteri sweeppp_glTexParameteri
#define glTexParameterfv sweeppp_glTexParameterfv
#define glTexImage1D sweeppp_glTexImage1D
#define glTexImage2D sweeppp_glTexImage2D
#define glTexSubImage1D sweeppp_glTexSubImage1D
#define glTexSubImage2D sweeppp_glTexSubImage2D
#define glGetTexImage sweeppp_glGetTexImage
#define glGenBuffers sweeppp_glGenBuffers
#define glDeleteBuffers sweeppp_glDeleteBuffers
#define glBindBuffer sweeppp_glBindBuffer
#define glBufferData sweeppp_glBufferData
#define glBufferSubData sweeppp_glBufferSubData
#define glGenVertexArrays sweeppp_glGenVertexArrays
#define glDeleteVertexArrays sweeppp_glDeleteVertexArrays
#define glBindVertexArray sweeppp_glBindVertexArray
#define glEnableVertexAttribArray sweeppp_glEnableVertexAttribArray
#define glDisableVertexAttribArray sweeppp_glDisableVertexAttribArray
#define glVertexAttribPointer sweeppp_glVertexAttribPointer
#define glCreateShader sweeppp_glCreateShader
#define glDeleteShader sweeppp_glDeleteShader
#define glShaderSource sweeppp_glShaderSource
#define glCompileShader sweeppp_glCompileShader
#define glGetShaderiv sweeppp_glGetShaderiv
#define glGetShaderInfoLog sweeppp_glGetShaderInfoLog
#define glCreateProgram sweeppp_glCreateProgram
#define glDeleteProgram sweeppp_glDeleteProgram
#define glAttachShader sweeppp_glAttachShader
#define glDetachShader sweeppp_glDetachShader
#define glLinkProgram sweeppp_glLinkProgram
#define glUseProgram sweeppp_glUseProgram
#define glGetProgramiv sweeppp_glGetProgramiv
#define glGetProgramInfoLog sweeppp_glGetProgramInfoLog
#define glGetUniformLocation sweeppp_glGetUniformLocation
#define glGetAttribLocation sweeppp_glGetAttribLocation
#define glUniform1i sweeppp_glUniform1i
#define glUniform1f sweeppp_glUniform1f
#define glUniform2f sweeppp_glUniform2f
#define glUniform3f sweeppp_glUniform3f
#define glUniform4f sweeppp_glUniform4f
#define glUniformMatrix4fv sweeppp_glUniformMatrix4fv
#define glGenFramebuffers sweeppp_glGenFramebuffers
#define glDeleteFramebuffers sweeppp_glDeleteFramebuffers
#define glBindFramebuffer sweeppp_glBindFramebuffer
#define glFramebufferTexture2D sweeppp_glFramebufferTexture2D
#define glCheckFramebufferStatus sweeppp_glCheckFramebufferStatus

typedef void* (*SweepppGlGetProcAddress)(const char* name);

/* Resolves every entry point above. Returns the number that failed to
 * resolve; 0 means a usable GL 4.1 core context. Must be called with the
 * context current, once per context. */
int sweeppp_gl_load(SweepppGlGetProcAddress getProcAddress);

/* Name of the first unresolved entry point, or NULL. Useful in the error
 * message when a driver turns out to be pre-4.1. */
const char* sweeppp_gl_first_missing(void);

#ifdef __cplusplus
}
#endif
