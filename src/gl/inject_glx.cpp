#include <X11/Xlib.h>
#include <iostream>
#include <array>
#include <thread>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <spdlog/spdlog.h>
#include "real_dlsym.h"
#include "loaders/loader_glx.h"
#include "loaders/loader_x11.h"
#include "mesa/util/macros.h"
#include "mesa/util/os_time.h"
#include "blacklist.h"

#include <chrono>
#include <iomanip>

#include <glad/glad.h>
#include "gl_hud.h"
#include "../config.h"
#include "../fps_limiter.h"

using namespace MangoHud::GL;

#ifndef GLX_WIDTH
#define GLX_WIDTH   0x801D
#define GLX_HEIGHT  0x801E
#endif

static glx_loader glx;

// single global lock, for simplicity
static std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;
static std::unordered_map<void *, gl_context *> gl_contexts;

static void* get_glx_proc_address(const char* name) {
    glx.Load();

    void *func = nullptr;
    if (glx.GetProcAddress)
        func = glx.GetProcAddress( (const unsigned char*) name );

    if (!func && glx.GetProcAddressARB)
        func = glx.GetProcAddressARB( (const unsigned char*) name );

    if (!func)
        func = get_proc_address( name );

    if (!func) {
        SPDLOG_ERROR("Failed to get function '{}'", name);
    }

    return func;
}

bool glx_mesa_queryInteger(int attrib, unsigned int *value);
bool glx_mesa_queryInteger(int attrib, unsigned int *value)
{
    static int (*pfn_queryInteger)(int attribute, unsigned int *value) =
        reinterpret_cast<decltype(pfn_queryInteger)>(get_glx_proc_address(
                    "glXQueryCurrentRendererIntegerMESA"));
    if (pfn_queryInteger)
        return !!pfn_queryInteger(attrib, value);
    return false;
}

static gl_context *create_gl_context(void *ctx)
{
    gl_context *gl_ctx;

    gl_ctx = (gl_context *)calloc(1, sizeof(*gl_ctx));
    gl_ctx->ctx = ctx;
    gl_contexts[ctx] = gl_ctx;
    //SPDLOG_DEBUG("created gl_context {} for GLX context {}", (void *)gl_ctx, ctx);
    return gl_ctx;
}

static void destroy_gl_context(gl_context *gl_ctx)
{
    //SPDLOG_DEBUG("destroying gl_context {} for GLX context {}", (void *)gl_ctx, gl_ctx->ctx);
    gl_contexts.erase(gl_ctx->ctx);
    free(gl_ctx);
}

EXPORT_C_(void) glXDestroyContext(void *dpy, void *ctx)
{
    ::scoped_lock lk(global_lock);
    gl_context *gl_ctx = gl_contexts[ctx];
    void *current_ctx, *draw, *read = nullptr;
    int r;

    SPDLOG_DEBUG("{}: {}", __func__, ctx);
    glx.Load();

    if (gl_ctx)
    {
        current_ctx = glx.GetCurrentContext();
        draw = glx.GetCurrentDrawable();
        if (glx.GetCurrentReadDrawable)
            read = glx.GetCurrentReadDrawable();
        //SPDLOG_DEBUG("gl_context {}, current_ctx {}, draw {}, read {}", (void *)gl_ctx, current_ctx, draw, read);
        if (glx.MakeContextCurrent)
            r = glx.MakeContextCurrent(dpy, draw, read, ctx);
        else
            r = glx.MakeCurrent(dpy, draw, ctx);
        if (r)
        {
            imgui_shutdown(gl_ctx, gl_contexts.size() == 1);
            if (glx.MakeContextCurrent)
                glx.MakeContextCurrent(dpy, draw, read, current_ctx);
            else
                glx.MakeCurrent(dpy, draw, current_ctx);
        }
        destroy_gl_context(gl_ctx);
    }

    glx.DestroyContext(dpy, ctx);
}

#ifndef GLX_SWAP_INTERVAL_EXT
#define GLX_SWAP_INTERVAL_EXT 0x20F1
#endif

static void do_imgui_swap(void *dpy, void *drawable)
{
    static auto last_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();

    // if bufferSize is 0 then glXQueryDrawable is probably not working
    // this is the case with llvmpipe
    unsigned int bufferSize;
    glx.QueryDrawable(dpy, drawable, GL_BUFFER_SIZE, &bufferSize);

    std::chrono::duration<double> elapsed_seconds = current_time - last_time;
    if (bufferSize != 0 && (HUDElements.vsync == 10 || elapsed_seconds.count() > 5.0))
        glx.QueryDrawable(dpy, drawable, GLX_SWAP_INTERVAL_EXT, &HUDElements.vsync);

    GLint vp[4];
    if (!is_blacklisted()) {
        ::scoped_lock lk(global_lock);
        void *ctx = glx.GetCurrentContext();
        gl_context *gl_ctx = gl_contexts[ctx];

        //SPDLOG_TRACE("ctx {}, gl_ctx {}", ctx, (void *)gl_ctx);
        if (!gl_ctx)
            gl_ctx = create_gl_context(ctx);
        imgui_create(gl_ctx, gl_wsi::GL_WSI_GLX);

        unsigned int width = -1, height = -1;

        switch (get_params()->gl_size_query)
        {
            case GL_SIZE_VIEWPORT:
                glGetIntegerv (GL_VIEWPORT, vp);
                width = vp[2];
                height = vp[3];
                break;
            case GL_SIZE_SCISSORBOX:
                glGetIntegerv (GL_SCISSOR_BOX, vp);
                width = vp[2];
                height = vp[3];
                break;
            default:
                glx.QueryDrawable(dpy, drawable, GLX_WIDTH, &width);
                glx.QueryDrawable(dpy, drawable, GLX_HEIGHT, &height);
                break;
        }

        // 尺寸兜底：某些 drawable（pixmap 等）查不到 GLX_WIDTH/HEIGHT，会返回 0。
        // 用 0 去画会把 ImGui 的 DisplaySize 设成 0 → HUD 畸形/闪烁。
        // 这里沿用上一次的有效尺寸；若仍无效则跳过本帧，不画坏的。
        static unsigned int last_w = 0, last_h = 0;
        if (width == 0 || height == 0) {
            width = last_w;
            height = last_h;
        } else {
            last_w = width;
            last_h = height;
        }
        if (width == 0 || height == 0)
            return;

        // 诊断开关：MANGOHUD_GL_DEBUG=1（配合 MANGOHUD_LOG_LEVEL=info 即可看到）
        static int gl_dbg = -1;
        if (gl_dbg < 0) {
            const char* e = getenv("MANGOHUD_GL_DEBUG");
            gl_dbg = (e && *e && *e != '0') ? 1 : 0;
        }
        if (gl_dbg) {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            SPDLOG_INFO("GL swap: drawable={} size={}x{} fbo={} ctx={}",
                        drawable, width, height, fbo, ctx);
            // 同时写文件，手机端好取：
            //   MANGOHUD_GL_DEBUG=/sdcard/Download/mangohud_gl.log
            //   MANGOHUD_GL_DEBUG=1 则用默认路径 /sdcard/Download/mangohud_gl.log
            static FILE* dbg_fp = nullptr;
            static bool dbg_opened = false;
            if (!dbg_opened) {
                dbg_opened = true;
                const char* p = getenv("MANGOHUD_GL_DEBUG");
                const char* path =
                    (p && *p && strcmp(p, "1") != 0) ? p : "/sdcard/Download/mangohud_gl.log";
                dbg_fp = fopen(path, "w");
            }
            if (dbg_fp) {
                fprintf(dbg_fp, "drawable=%p size=%ux%u fbo=%d ctx=%p\n",
                        drawable, width, height, fbo, ctx);
                fflush(dbg_fp);
            }
        }
        SPDLOG_TRACE("swap buffers: {}x{}", width, height);
        imgui_render(gl_ctx, width, height);
    }
}

static void set_swap_interval(void* dpy, void* drawable, int interval)
{
    ::scoped_lock lk(global_lock);
    void* ctx = glx.GetCurrentContext();
    gl_context *gl_ctx = gl_contexts[ctx];

    if (!is_blacklisted() || interval >= 0)
    {
        std::shared_ptr<overlay_params> real_params;

        if (gl_ctx)
        {
            if (gl_ctx->swap_interval_set && interval < -1)
                return;

            if (!is_blacklisted())
            {
                imgui_create(gl_ctx, gl_wsi::GL_WSI_GLX);

                real_params = get_params();
            }
        }

        // Disabled: -1 is outside of the GLX_SWAP_INTERVAL_EXT spec and will crash with BadValue in Zink.
        // Original code kept for reference:
        // if (glx.SwapIntervalEXT && ((real_params && real_params->gl_vsync >= -1) || (interval >= -1 && dpy && drawable)))
        // {
        //     glx.SwapIntervalEXT(dpy, drawable, real_params && real_params->gl_vsync >= -1 ? real_params->gl_vsync : interval);
        // }
        if ((real_params && real_params->gl_vsync >= 0) || interval >= 0)
        {
            if (glx.SwapIntervalSGI)
                glx.SwapIntervalSGI(real_params && real_params->gl_vsync >= 0 ? real_params->gl_vsync : interval);
            if (glx.SwapIntervalMESA)
                glx.SwapIntervalMESA(real_params && real_params->gl_vsync >= 0 ? real_params->gl_vsync : interval);
        }
        if (gl_ctx)
            gl_ctx->swap_interval_set = true;
    }
}

EXPORT_C_(void) glXSwapBuffers(void* dpy, void* drawable) {
    glx.Load();
    SPDLOG_TRACE("{}: {}", __func__, drawable);

    do_imgui_swap(dpy, drawable);
    set_swap_interval(dpy, drawable, -2);
    if (fps_limiter)
        fps_limiter->limit(true);

    glx.SwapBuffers(dpy, drawable);
    if (!is_blacklisted())
        if (fps_limiter)
            fps_limiter->limit(false);
}

EXPORT_C_(int64_t) glXSwapBuffersMscOML(void* dpy, void* drawable, int64_t target_msc, int64_t divisor, int64_t remainder)
{
    glx.Load();
    SPDLOG_DEBUG("{}: {}, {}, {}, {}", __func__, drawable, target_msc, divisor, remainder);
    if (!glx.SwapBuffersMscOML)
        return -1;

    do_imgui_swap(dpy, drawable);
    set_swap_interval(dpy, drawable, -2);
    if (fps_limiter)
        fps_limiter->limit(true);

    int64_t ret = glx.SwapBuffersMscOML(dpy, drawable, target_msc, divisor, remainder);

    if (!is_blacklisted())
        if (fps_limiter)
            fps_limiter->limit(false);   // 交换后必须解除限帧（原先误写成 true，状态会错乱）

    return ret;
}

EXPORT_C_(void) glXSwapIntervalEXT(void *dpy, void *draw, int interval) {
    SPDLOG_DEBUG("{}: {}, {}", __func__, draw, interval);
    glx.Load();
    if (!glx.SwapIntervalEXT)
        return;

    set_swap_interval(dpy, draw, interval);
}

EXPORT_C_(int) glXSwapIntervalSGI(int interval) {
    SPDLOG_DEBUG("{}: {}", __func__, interval);
    glx.Load();
    if (!glx.SwapIntervalSGI)
        return -1;

    set_swap_interval(nullptr, nullptr, interval);
    return 0;
}

EXPORT_C_(int) glXSwapIntervalMESA(unsigned int interval) {
    SPDLOG_DEBUG("{}: {}", __func__, interval);
    glx.Load();
    if (!glx.SwapIntervalMESA)
        return -1;

    set_swap_interval(nullptr, nullptr, interval);
    return 0;
}

EXPORT_C_(int) glXGetSwapIntervalMESA() {
    glx.Load();
    if (!glx.GetSwapIntervalMESA)
        return 0;

    int interval = glx.GetSwapIntervalMESA();

    if (!is_blacklisted()) {
        static bool first_call = true;

        if (first_call) {
            first_call = false;
            if (get_params()->gl_vsync >= 0) {
                interval = get_params()->gl_vsync;
                glx.SwapIntervalMESA(interval);
            }
        }
    }

    SPDLOG_DEBUG("{}: {}", __func__, interval);
    return interval;
}

struct func_ptr {
   const char *name;
   void *ptr;
};

static std::array<const func_ptr, 9> name_to_funcptr_map = {{
#define ADD_HOOK(fn) { #fn, (void *) fn }
   ADD_HOOK(glXGetProcAddress),
   ADD_HOOK(glXGetProcAddressARB),
   ADD_HOOK(glXDestroyContext),
   ADD_HOOK(glXSwapBuffers),
   ADD_HOOK(glXSwapBuffersMscOML),

   ADD_HOOK(glXSwapIntervalEXT),
   ADD_HOOK(glXSwapIntervalSGI),
   ADD_HOOK(glXSwapIntervalMESA),
   ADD_HOOK(glXGetSwapIntervalMESA),
#undef ADD_HOOK
}};

EXPORT_C_(void *) mangohud_find_glx_ptr(const char *name);
EXPORT_C_(void *) mangohud_find_glx_ptr(const char *name)
{
  if (is_blacklisted())
      return nullptr;

   for (auto& func : name_to_funcptr_map) {
      if (strcmp(name, func.name) == 0)
         return func.ptr;
   }

   return nullptr;
}

EXPORT_C_(void *) glXGetProcAddress(const unsigned char* procName) {
    void *real_func = get_glx_proc_address((const char*)procName);
    void *func = mangohud_find_glx_ptr( (const char*)procName );
    SPDLOG_TRACE("{}: '{}', real: {}, fun: {}", __func__, reinterpret_cast<const char*>(procName), real_func, func);

    if (func && real_func)
        return func;

    return real_func;
}

EXPORT_C_(void *) glXGetProcAddressARB(const unsigned char* procName) {
    void *real_func = get_glx_proc_address((const char*)procName);
    void *func = mangohud_find_glx_ptr( (const char*)procName );
    SPDLOG_TRACE("{}: '{}', real: {}, fun: {}", __func__, reinterpret_cast<const char*>(procName), real_func, func);
    if (func && real_func)
        return func;

    return real_func;
}
