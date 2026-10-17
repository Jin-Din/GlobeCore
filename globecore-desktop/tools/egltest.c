// egltest.c — 最小 EGL + OpenGL ES 2.0 上下文自测（Linux/WSLg 可行性门）
// 目的：证明引擎硬需求（真 GLES2 上下文）在本机可满足：创建 EGL display→GLES2 config→
// ES2 context→pbuffer surface→make current→读 GL_VERSION/GL_RENDERER。
// 编译：gcc egltest.c -lEGL -lGLESv2 -o /tmp/egltest ；运行需 DISPLAY/WAYLAND_DISPLAY（WSLg 已给）。
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { printf("FAIL: no EGL display (DISPLAY/WAYLAND_DISPLAY?)\n"); return 1; }

    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { printf("FAIL: eglInitialize\n"); return 2; }
    printf("EGL initialized: %d.%d\n", maj, min);
    printf("EGL vendor=%s\n", eglQueryString(dpy, EGL_VENDOR));
    printf("EGL client APIs=%s\n", eglQueryString(dpy, EGL_CLIENT_APIS));
    printf("EGL extensions(前120)=%.120s\n", eglQueryString(dpy, EGL_EXTENSIONS));

    // 关键：必须能选到一个支持 OpenGL_ES2 的 config
    const EGLint caAttrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint nCfg = 0;
    if (!eglChooseConfig(dpy, caAttrs, &cfg, 1, &nCfg) || nCfg < 1) {
        printf("FAIL: no ES2-config available (nCfg=%d) — GLES2 后端不可用\n", nCfg);
        return 3;
    }
    printf("ES2 config found: nCfg=%d\n", nCfg);

    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL: eglCreateContext ES2 err=0x%x\n", eglGetError()); return 4; }

    const EGLint pbAttrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface sur = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (sur == EGL_NO_SURFACE) { printf("FAIL: pbuffer err=0x%x\n", eglGetError()); return 5; }

    if (!eglMakeCurrent(dpy, sur, sur, ctx)) { printf("FAIL: makeCurrent err=0x%x\n", eglGetError()); return 6; }

    printf("GL_VERSION = %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("GL_VENDOR = %s\n", glGetString(GL_VENDOR));
    GLint maxTex = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    printf("GL_MAX_TEXTURE_SIZE = %d\n", maxTex);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, sur);
    eglTerminate(dpy);
    printf("OK: GLES2 context works\n");
    return 0;
}
