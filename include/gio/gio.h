#pragma once
// Stub for gio/gio.h
static inline void g_settings_set_int(void* settings, const char* key, int value) {}
static inline void g_settings_set_string(void* settings, const char* key, const char* value) {}
static inline void* g_settings_new(const char* schema) { return nullptr; }
static inline void* g_settings_schema_source_get_default() { return nullptr; }
static inline void* g_settings_schema_source_lookup(void*, const char*, bool) { return nullptr; }
static inline bool g_settings_schema_has_key(void*, const char*) { return false; }
static inline void g_settings_schema_unref(void*) {}
static inline void g_settings_sync() {}
static inline void g_object_unref(void*) {}
