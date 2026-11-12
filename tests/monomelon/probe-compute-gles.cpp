// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>
#include "GPU3D_Compute_shaders.h"
using namespace melonDS::ComputeRendererShaders;

static bool program(const char* name, const std::string& body, std::initializer_list<const char*> defines, bool textureBuffer)
{
    std::string source = "#version 310 es\n#extension GL_EXT_shader_implicit_conversions : require\n";
    if (textureBuffer) source += "#extension GL_EXT_texture_buffer : require\nprecision highp uimageBuffer;\n";
    source += "precision highp float;\nprecision highp int;\nprecision highp image2D;\nprecision highp uimage2D;\nprecision highp usampler2D;\nprecision highp usampler2DArray;\nprecision highp sampler2DArray;\n";
    for (const char* define : defines) source += std::string("#define ") + define + "\n";
    source += "#define ScreenWidth 256\n#define ScreenHeight 192\n#define MaxWorkTiles 12288\n#define TileSize 8\nconst int CoarseTileCountY=4;\n#define CoarseTileArea 32\n#define ClearCoarseBinMaskLocalSize 64\n";
    source += Common;
    source += body;
    for (size_t pos = 0; (pos = source.find("return z0;", pos)) != std::string::npos;)
    {
        source.replace(pos, 10, "return uint(z0);");
        pos += 16;
    }
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    char log[16384] = {};
    if (!ok)
    {
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::printf("FAIL COMPILE %s\n%s\n", name, log);
        glDeleteShader(shader);
        return false;
    }
    GLuint linked = glCreateProgram();
    glAttachShader(linked, shader);
    glLinkProgram(linked);
    glGetProgramiv(linked, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        glGetProgramInfoLog(linked, sizeof(log), nullptr, log);
        std::printf("FAIL LINK %s\n%s\n", name, log);
    }
    else std::printf("PASS %s\n", name);
    glDeleteProgram(linked);
    glDeleteShader(shader);
    return ok;
}

int main()
{
    auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!getDisplay) return 1;
    EGLDisplay display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) { std::printf("EGL initialize failed %x\n", eglGetError()); return 1; }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return 1;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) return 1;
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return 1;
    std::printf("Renderer: %s\nVersion: %s\nGLSL: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    bool textureBuffer = false;
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (int i = 0; i < extensions; i++)
    {
        std::string name(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)));
        if (name == "GL_EXT_texture_buffer") textureBuffer = true;
        if (name.find("shader") != std::string::npos || name.find("texture_buffer") != std::string::npos) std::printf("Extension: %s\n", name.c_str());
    }
    for (auto limit : {GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, GL_MAX_SHADER_STORAGE_BLOCK_SIZE, GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS})
    {
        GLint value = 0;
        glGetIntegerv(limit, &value);
        std::printf("Limit %x: %d\n", limit, value);
    }
    int failures = 0;
    failures += !program("InterpSpans/Z", InterpSpans, {"InterpSpans", "ZBuffer"}, textureBuffer);
    failures += !program("InterpSpans/W", InterpSpans, {"InterpSpans", "WBuffer"}, textureBuffer);
    failures += !program("BinCombined", BinCombined, {"BinCombined"}, textureBuffer);
    failures += !program("DepthBlend/Z", DepthBlend, {"DepthBlend", "ZBuffer"}, textureBuffer);
    failures += !program("Rasterise/Modulate", Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Modulate"}, textureBuffer);
    failures += !program("FinalPass", FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking", "Fog"}, textureBuffer);
    failures += !program("ClearCoarseBinMask", ClearCoarseBinMask, {"ClearCoarseBinMask"}, textureBuffer);
    std::printf("Shader failures: %d\n", failures);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return failures ? 2 : 0;
}
