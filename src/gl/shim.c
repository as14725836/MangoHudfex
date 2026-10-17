#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdio.h>
#include <stdint.h>
#include "gl.h"
#include "real_dlsym.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

/* FHUD_SHIM_TRACE -----------------------------------------------------------
 * Self-check + entry tracing.
 *
 * FHUD_SHIM_DEBUG=0            -> mute everything here
 * unset                        -> write to /sdcard/Download/fhud_shim.log (+stderr)
 * FHUD_SHIM_DEBUG=/some/path   -> write to that path instead
 *
 * Two distinct lines:
 *   "loaded"  printed from the constructor => ld.so really mapped this shim.
 *   "GL entry glXSwapBuffers" printed from inside the hook => the app's
 *            glXSwapBuffers call actually reached us (not shadowed).
 * If you see "loaded" but never a "GL entry", another LD_PRELOAD lib (e.g. a
 * Mesa libGL.so.1) is still winning symbol resolution ahead of the shim.
 */
static int fhud_trace_fd = -2; /* -2 = not resolved yet */

static void fhud_trace_open(void)
{
    if (fhud_trace_fd != -2)
        return;
    const char *dbg = getenv("FHUD_SHIM_DEBUG");
    if (dbg && dbg[0] == '0')
    {
        fhud_trace_fd = -1;
        return;
    }
    const char *path = (dbg && *dbg && dbg[0] != '0' && strcmp(dbg, "1") != 0)
                       ? dbg : "/sdcard/Download/fhud_shim.log";
    fhud_trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fhud_trace_fd < 0)
        fhud_trace_fd = -1;
}

static void fhud_trace(const char *line)
{
    const char *mute = getenv("FHUD_SHIM_DEBUG");
    if (mute && mute[0] == '0')
        return;
    fhud_trace_open();
    if (fhud_trace_fd >= 0)
    {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[pid=%d] %s\n", (int) getpid(), line);
        if (n > 0)
        {
            ssize_t w = write(fhud_trace_fd, buf, (size_t) n);
            (void) w;
        }
    }
    fprintf(stderr, "shim: %s (pid=%d)\n", line, (int) getpid());
    fflush(stderr);
}

__attribute__((constructor))
static void fhud_shim_selfcheck(void)
{
    fhud_trace("FHUD shim loaded");
}

static void* handle = NULL;
static bool mangoHudLoaded = false;

#ifdef __GLIBC__
static inline void
free_indirect(char **p)
{
    free(*p);
}

static bool load_adjacent_opengl_lib(void)
{
    __attribute__((cleanup(free_indirect))) char *location = NULL;
    __attribute__((cleanup(free_indirect))) char *lib = NULL;
    Dl_info info = {};
    void *extra_info = NULL;

    // The first argument can be any symbol in this shared library,
    // mangoHudLoaded is a convenient one
    if (!dladdr1(&mangoHudLoaded, &info, &extra_info, RTLD_DL_LINKMAP))
    {
        fprintf(stderr, "shim: Unable to find my own location: %s\n", dlerror());
        return false;
    }

    const struct link_map *map = extra_info;
    if (map == NULL)
    {
        fprintf(stderr, "shim: Unable to find my own location: NULL link_map\n");
        return false;
    }
    if (map->l_name == NULL)
    {
        fprintf(stderr, "shim: Unable to find my own location: NULL l_name\n");
        return false;
    }

    location = realpath(map->l_name, NULL);
    char *slash = strrchr(location, '/');

    if (slash == NULL)
    {
        fprintf(stderr, "shim: Unable to find my own location: no directory separator\n");
        return false;
    }

    *slash = '\0';

    if (asprintf(&lib, "%s/libFHUD_opengl.so", location) < 0)
    {
        fprintf(stderr, "shim: asprintf: %s\n", strerror(errno));
        return false;
    }

    handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);

    if (handle == NULL)
    {
        fprintf(stderr, "shim: Failed to load from \"%s\": %s\n", lib, dlerror());
        return false;
    }

    return true;
}
#endif

// Load MangoHud after EGL/GLX functions have been intercepted
static void loadMangoHud(void);
static void loadMangoHud() {
    if (mangoHudLoaded) return;

    // allow user to load custom mangohud libs (useful for testing)
    char *libs = getenv("MANGOHUD_OPENGL_LIBS");
    char *lib = NULL;

    if (libs)
    {
        lib = strtok(libs, ":");

        // when user specifies only one path
        if (!lib) lib = libs;

        while (lib != NULL)
        {
            handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);

            if (handle)
            {
                mangoHudLoaded = true;
                return;
            }
            else fprintf(stderr, "shim: Failed to load from \"%s\": %s\n", lib, dlerror());

            lib = strtok(NULL, ":");
        }
    }

#ifdef __GLIBC__
    if (load_adjacent_opengl_lib())
    {
        mangoHudLoaded = true;
        return;
    }
#endif

    if (!mangoHudLoaded)
    {
        handle = dlopen("${ORIGIN}/libFHUD_opengl.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (handle) mangoHudLoaded = true;
        else
        {
            fprintf(stderr, "shim: Failed to load from ${ORIGIN}/libFHUD_opengl.so: %s\n", dlerror());
            handle = RTLD_NEXT;
        }
    }
}

#define CREATE_FWD_VOID(name, params, ...) \
    void name params { \
        if (getenv("FHUD_SHIM_TRACE_ENTRY")) fhud_trace("GL entry " #name); \
        loadMangoHud(); \
        void (*p##name) params = real_dlsym(handle, #name); \
        if (p##name) p##name(__VA_ARGS__); \
    }
#define CREATE_FWD(ret_type, name, params, ...) \
    ret_type name params { \
        if (getenv("FHUD_SHIM_TRACE_ENTRY")) fhud_trace("GL entry " #name); \
        loadMangoHud(); \
        ret_type (*p##name) params = real_dlsym(handle, #name); \
        if (p##name) return p##name(__VA_ARGS__); \
        return (ret_type)0; \
    }

#ifdef HAVE_X11
CREATE_FWD_VOID(glXSwapBuffers, (void* dpy, void* drawable), dpy, drawable)
CREATE_FWD(int64_t, glXSwapBuffersMscOML,
           (void* dpy, void* drawable, int64_t target_msc,
            int64_t divisor, int64_t remainder),
           dpy, drawable, target_msc, divisor, remainder)
CREATE_FWD_VOID(glXDestroyContext, (void *dpy, void *ctx), dpy, ctx)
CREATE_FWD(void*, glXGetProcAddress, (const unsigned char* procName), procName)
CREATE_FWD(void*, glXGetProcAddressARB, (const unsigned char* procName), procName)
#endif
CREATE_FWD(void*, eglGetDisplay, (void* native_dpy), native_dpy)
CREATE_FWD(void*, eglGetPlatformDisplay,
    (unsigned int platform,
     void* native_display,
     const intptr_t* attrib_list),
     platform, native_display, attrib_list)
CREATE_FWD(void*, eglGetPlatformDisplayEXT,
    (unsigned int platform,
     void* native_display,
     const intptr_t* attrib_list),
     platform, native_display, attrib_list)
CREATE_FWD(unsigned int, eglSwapBuffers, (void* dpy, void* surf), dpy, surf)
CREATE_FWD(void*, eglGetProcAddress, (const char* procName), procName)
CREATE_FWD(int, eglTerminate, (void *display), display)
CREATE_FWD(unsigned int, eglDestroyContext, (void *dpy, void *ctx), dpy, ctx)

#undef CREATE_FWD
#undef CREATE_FWD_VOID

struct func_ptr {
    const char* name;
    void* ptr;
};

#define ADD_HOOK(fn) { #fn, (void*)fn }
static struct func_ptr hooks[] = {
#ifdef HAVE_X11
    ADD_HOOK(glXGetProcAddress),
    ADD_HOOK(glXGetProcAddressARB),
    ADD_HOOK(glXDestroyContext),
    ADD_HOOK(glXSwapBuffers),
    ADD_HOOK(glXSwapBuffersMscOML),
#endif
    ADD_HOOK(eglDestroyContext),
    ADD_HOOK(eglSwapBuffers),
    ADD_HOOK(eglGetPlatformDisplay),
    ADD_HOOK(eglGetPlatformDisplayEXT),
    ADD_HOOK(eglGetDisplay),
    ADD_HOOK(eglGetProcAddress),
    ADD_HOOK(eglTerminate)
};
#undef ADD_HOOK

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) sizeof(arr)/sizeof(arr[0])
#endif

#include <stddef.h>

// Glibc has nonconformance behaviour
// which is Glibc clears dlerror if dlsym succeded instead
// leaving it as it is as required by standard
// Reference: https://pubs.opengroup.org/onlinepubs/009604299/functions/dlsym.html
//
// Lets disable dlerror override to avoid risking programs
// which already relying on Glibc's behaviour so to keep
// glibc's behaviour
// while on conformance system such as Musl it fixes bugs
#ifdef __GLIBC__
# define USE_DLERROR_OVERRIDE 0
#else
# define USE_DLERROR_OVERRIDE 1
#endif

static _Thread_local char *error_override = NULL;
static _Thread_local bool activate_override = false;

char *dlerror(void)
{
    if (!USE_DLERROR_OVERRIDE)
       return real_dlerror();
    
    if (activate_override)
    {
        // Deactivate the override for next call to allow
        // real_dlerror to be used
        activate_override = false;
        return error_override;
    } else if (error_override) {
        // Free resources that was allocated for error override
        // because the override is now deactivated
        free(error_override);
        error_override = NULL;
    }

    return real_dlerror();
}

static void save_and_consume_real_dlerror()
{
    if (!USE_DLERROR_OVERRIDE)
       return;
    
    char* recent_error = real_dlerror();
    if (!recent_error)
    {
        error_override = recent_error;
        return;
    }
    
    size_t error_length = strlen(recent_error);
    error_override = malloc(error_length + 1);
    memcpy(error_override, recent_error, error_length + 1);
}

void* dlsym(void *handle, const char *name)
{
    save_and_consume_real_dlerror();
    // const char* dlsym_enabled = getenv("MANGOHUD_DLSYM");
    const char* dlsym_RTLD_DEFAULT_env = getenv("MANGOHUD_DLSYM_RTLD_DEFAULT_FIX");
    const bool enable_RTLD_DEFAULT_experimental_fix = dlsym_RTLD_DEFAULT_env ? dlsym_RTLD_DEFAULT_env[0] == '1' : false;
    const char* dlsym_RTLD_NEXT_env = getenv("MANGOHUD_DLSYM_RTLD_NEXT_BAD_FIX");
    const bool enable_RTLD_NEXT_experimental_fix = dlsym_RTLD_NEXT_env ? dlsym_RTLD_NEXT_env[0] == '1' : false;
    void* is_angle = real_dlsym(handle, "eglStreamPostD3DTextureANGLE");
    // Consume error message if there was an error
    if (!is_angle)
    {
        (void) real_dlerror();
    }

    void* fn_ptr = real_dlsym(handle, name);
    // Overriding dlsym makes it behave differently due to the change in search scope
    // this results in name resolution failure when no handle or default handles are used
    // as a mitigation we get a handle to the caller, and use it instead.
    // This makes RTLD_DEFAULT behave better, but RTLD_NEXT is still broken.
    // Note, RTLD_DEFAULT might still be broken for edge cases.

    // this causes issues with nvidia and will crash. Needs to be reworked
    if (enable_RTLD_DEFAULT_experimental_fix || enable_RTLD_NEXT_experimental_fix)
    if (!fn_ptr && (handle == RTLD_DEFAULT || (handle == RTLD_NEXT && enable_RTLD_NEXT_experimental_fix)))
    {
        void* ret_addr = __builtin_extract_return_addr(__builtin_return_address(0));
        Dl_info dl_info = {};

        if (dladdr(ret_addr, &dl_info) && dl_info.dli_fname && dl_info.dli_fname[0])
        {
            void* caller_handle = dlopen(dl_info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            if (caller_handle)
            {
                // last real_dlsym call failed so we consume the last error
                (void) real_dlerror();
                fn_ptr = real_dlsym(caller_handle, name);
                if (handle == RTLD_NEXT)
                {
                    fprintf(stderr, "MANGOHUD: WARNING dlsym called with RTLD_NEXT, if you are facing issues try to disable mangohud.\n");
                    fprintf(stderr, "MANGOHUD: or you can try MANGOHUD_DLSYM_RTLD_NEXT_BAD_FIX=0 or unset it if already set.\n");
                }
            }
        }
    }

    // Activate override if there wasn't any new error
    // from real_dlsym
    activate_override = fn_ptr != NULL;

    if (!is_angle && fn_ptr)
    {
        for (unsigned i = 0; i < ARRAY_SIZE(hooks); i++)
        {
            if (!strcmp(hooks[i].name, name))
            {
                return hooks[i].ptr;
            }
        }
    }

    return fn_ptr;
}
