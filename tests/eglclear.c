/* Regression test for clearing EGL pbuffers across draw-surface switches. */
#include <pspuser.h>
#include <pspiofilemgr.h>
#include <GLES/egl.h>
#include <GLES/gl.h>
#include "pspgl_internal.h"
#include "pspgl_buffers.h"
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

PSP_HEAP_SIZE_KB(192);

PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define WIDTH 512
#define HEIGHT 512
static char logbuf[4096];
static unsigned loglen;
static unsigned failed;
static unsigned tests;
static void log_msg(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(line) && loglen + n < sizeof(logbuf)) {
        memcpy(logbuf + loglen, line, n);
        loglen += n;
    }
}
static void check_pixels(const char *name, EGLSurface hsurf,
                         uint16_t fill, uint16_t outside,
                         int rect_x, int rect_y, int rect_w, int rect_h)
{
    struct pspgl_surface *s = (struct pspgl_surface *)hsurf;
    struct pspgl_buffer *buf = *(s->draw);
    unsigned wrong = 0;
    int firstx = -1, firsty = -1;
    unsigned firstval = 0, firstexpect = 0;
    unsigned short *pixels;

    glFinish();
    pixels = (unsigned short *)__pspgl_buffer_map(buf, GL_READ_ONLY);
    if (!pixels) {
        log_msg("%s ERROR MAP_FAILED\n", name);
        ++failed;
        return;
    }
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            uint16_t want = (x >= rect_x && y >= rect_y &&
                             x < rect_x + rect_w && y < rect_y + rect_h) ? fill : outside;
            uint16_t got = pixels[y * s->pixelperline + x];
            if (got != want) {
                if (!wrong) { firstx = x; firsty = y; firstval = got; firstexpect = want; }
                ++wrong;
            }
        }
    }
    __pspgl_buffer_unmap(buf, GL_READ_ONLY);
    ++tests;
    failed += (wrong != 0);
    log_msg("%s %s mismatched=%u first=(%d,%d) got=%04x want=%04x\n",
        name, wrong ? "FAIL" : "PASS", wrong, firstx, firsty, firstval, firstexpect);
}

static void record_clipping(const char *stage)
{
    struct pspgl_context *c = pspgl_curctx;
    log_msg("%s region2=%06x scissor2=%06x drawable=%ux%u\n", stage,
         c->hw.ge_reg[CMD_REGION2] & 0x00ffffff,
         c->hw.ge_reg[CMD_SCISSOR2] & 0x00ffffff,
         c->draw->width, c->draw->height);
}

static int audit_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface win, pbuf;
    EGLint ncfg;
    SceUID fd;
    const EGLint attr[] = {
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_ALPHA_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_NONE
    };
    const EGLint pbattr[] = {EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE};

    dpy = eglGetDisplay(0);
    if (!dpy || !eglInitialize(dpy, 0, 0)) { log_msg("EGL display error %x\n", eglGetError()); failed++; goto finish; }
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &ncfg) || !ncfg) { log_msg("EGL choose config error %x\n", eglGetError()); failed++; goto finish; }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    win = eglCreateWindowSurface(dpy, cfg, 0, NULL);
    pbuf = eglCreatePbufferSurface(dpy, cfg, pbattr);
    if (!ctx || !win || !pbuf) { log_msg("EGL create error %x ctx=%p win=%p pbuf=%p\n", eglGetError(), ctx, win, pbuf); failed++; goto finish; }

    eglMakeCurrent(dpy, win, win, ctx);
    record_clipping("window initial");
    eglMakeCurrent(dpy, pbuf, pbuf, ctx);
    record_clipping("pbuffer after switch");

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, WIDTH, HEIGHT);
    glScissor(0, 0, WIDTH, HEIGHT);
    record_clipping("pbuffer explicit setup");
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("CONTROL 512 full clear", pbuf, 0x001f, 0x001f, 0, 0, WIDTH, HEIGHT);

    eglMakeCurrent(dpy, win, win, ctx);
    glViewport(0, 0, 480, 272);
    glScissor(0, 0, 480, 272);
    glDisable(GL_SCISSOR_TEST);
    record_clipping("window setup 480x272");
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    eglMakeCurrent(dpy, pbuf, pbuf, ctx);
    record_clipping("pbuffer after 2nd switch");
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("SWITCH 512 full clear", pbuf, 0xf800, 0xf800, 0, 0, WIDTH, HEIGHT);

    glViewport(0, 0, WIDTH, HEIGHT);
    glScissor(0, 0, WIDTH, HEIGHT);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("WORKAROUND explicit setup", pbuf, 0xf800, 0xf800, 0, 0, WIDTH, HEIGHT);

    glEnable(GL_SCISSOR_TEST);
    glScissor(64, 32, 128, 96);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("SCISSOR partial", pbuf, 0x001f, 0xf800, 64, HEIGHT - (32 + 96), 128, 96);

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, 128, 128);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("VIEWPORT no scissor", pbuf, 0xf800, 0xf800, 0, 0, WIDTH, HEIGHT);

    eglMakeCurrent(dpy, win, win, ctx);
    glEnable(GL_SCISSOR_TEST);
    glScissor(64, 32, 128, 96);
    record_clipping("window scissor enabled");
    eglMakeCurrent(dpy, pbuf, pbuf, ctx);
    record_clipping("pbuffer inherited scissor");
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    check_pixels("SWITCH enabled scissor", pbuf, 0x001f, 0xf800,
                 64, HEIGHT - (32 + 96), 128, 96);

finish:
    log_msg("RESULT %s (%u/%u passed)\n", failed ? "FAIL" : "PASS",
            tests >= failed ? tests-failed : 0, tests);
    fd = sceIoOpen("ms0:/pspgl-eglclear-results.txt", PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, logbuf, loglen);
        sceIoClose(fd);
    }
    sceKernelSleepThread();
    return 0;
}

int main(void)
{
    SceUID thread = sceKernelCreateThread("PSPGL audit", audit_thread, 32, 0x10000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
    sceKernelSleepThread();
    return 0;
}
