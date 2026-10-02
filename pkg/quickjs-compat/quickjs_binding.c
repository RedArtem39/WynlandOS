#include "quickjs.h"
#include "compat.h"
#include <wynland/types.h>
#include <wynland/heap.h>

/* Browser state variables */
static int js_video_playing = 0;
static int js_video_views = 128;
static int js_video_likes = 14;
static int js_comments_count = 1;

static JSRuntime *js_rt = NULL;
static JSContext *js_ctx = NULL;

/* playVideo() javascript callback */
static JSValue js_play_video(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    js_video_playing = !js_video_playing;
    if (js_video_playing) {
        js_video_views++;
    }
    return JS_UNDEFINED;
}

/* likeVideo() javascript callback */
static JSValue js_like_video(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    js_video_likes++;
    return JS_UNDEFINED;
}

/* addComment() javascript callback */
static JSValue js_add_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (js_comments_count < 4) {
        js_comments_count++;
    }
    return JS_UNDEFINED;
}

/* Custom memory allocation functions for JSRuntime */
static void *js_malloc_wrapper(JSMallocState *s, size_t size)
{
    (void)s;
    return kmalloc(size);
}

static void js_free_wrapper(JSMallocState *s, void *ptr)
{
    (void)s;
    kfree(ptr);
}

static void *js_realloc_wrapper(JSMallocState *s, void *ptr, size_t size)
{
    (void)s;
    return krealloc(ptr, size);
}

static size_t js_malloc_usable_size_wrapper(const void *ptr)
{
    return malloc_usable_size((void *)ptr);
}

static const JSMallocFunctions js_malloc_funcs = {
    js_malloc_wrapper,
    js_free_wrapper,
    js_realloc_wrapper,
    js_malloc_usable_size_wrapper
};

/* API: Initialize the JS engine context and register functions */
void js_engine_init(void)
{
    if (js_rt) {
        JS_FreeContext(js_ctx);
        JS_FreeRuntime(js_rt);
    }

    js_rt = JS_NewRuntime2(&js_malloc_funcs, NULL);
    if (!js_rt) {
        serial_write_string("QuickJS: Failed to create runtime!\r\n");
        return;
    }

    js_ctx = JS_NewContext(js_rt);
    if (!js_ctx) {
        serial_write_string("QuickJS: Failed to create context!\r\n");
        JS_FreeRuntime(js_rt);
        js_rt = NULL;
        return;
    }

    /* Register global browser functions */
    JSValue global_obj = JS_GetGlobalObject(js_ctx);
    JS_SetPropertyStr(js_ctx, global_obj, "playVideo", JS_NewCFunction(js_ctx, js_play_video, "playVideo", 0));
    JS_SetPropertyStr(js_ctx, global_obj, "likeVideo", JS_NewCFunction(js_ctx, js_like_video, "likeVideo", 0));
    JS_SetPropertyStr(js_ctx, global_obj, "addComment", JS_NewCFunction(js_ctx, js_add_comment, "addComment", 0));
    JS_FreeValue(js_ctx, global_obj);

    /* Reset state variables */
    js_video_playing = 0;
    js_video_views = 128;
    js_video_likes = 14;
    js_comments_count = 1;

    serial_write_string("QuickJS: Engine successfully initialized.\r\n");
}

/* API: Destroy current JS engine */
void js_engine_destroy(void)
{
    if (js_ctx) {
        JS_FreeContext(js_ctx);
        js_ctx = NULL;
    }
    if (js_rt) {
        JS_FreeRuntime(js_rt);
        js_rt = NULL;
    }
    serial_write_string("QuickJS: Engine shut down.\r\n");
}

/* API: Evaluate JavaScript string and return result */
const char *js_engine_eval(const char *code)
{
    if (!js_ctx) {
        js_engine_init();
        if (!js_ctx) return "Error: QuickJS engine not active";
    }

    JSValue val = JS_Eval(js_ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        JSValue exception = JS_GetException(js_ctx);
        const char *err = JS_ToCString(js_ctx, exception);
        JS_FreeValue(js_ctx, exception);
        JS_FreeValue(js_ctx, val);

        static char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "JS Exception: %s", err ? err : "unknown error");
        if (err) JS_FreeCString(js_ctx, err);
        return err_buf;
    }

    const char *str = JS_ToCString(js_ctx, val);
    JS_FreeValue(js_ctx, val);

    if (str) {
        static char res_buf[2048];
        snprintf(res_buf, sizeof(res_buf), "%s", str);
        JS_FreeCString(js_ctx, str);
        return res_buf;
    }

    return "";
}

/* API: Get JS State variable */
int js_engine_get_state(const char *name)
{
    if (strcmp(name, "video_playing") == 0) return js_video_playing;
    if (strcmp(name, "video_views") == 0)   return js_video_views;
    if (strcmp(name, "video_likes") == 0)   return js_video_likes;
    if (strcmp(name, "comments_count") == 0) return js_comments_count;
    return -1;
}
