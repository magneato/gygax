#ifndef GYGAX_SDK_PLUGIN_H
#define GYGAX_SDK_PLUGIN_H

/*
 * GYGAX PLUGIN ABI :: v1
 * [ C ABI | explicit ownership | no hidden approval or safety interlock ]
 *
 * Keep this interface small and language-neutral. The loader accepts only
 * the exact ABI version below; it does not negotiate compatibility.
 * This is an in-process extension point, not a sandbox.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define GYGAX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GYGAX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/** Exact plugin descriptor/callback ABI version required by the loader. */
#define GYGAX_PLUGIN_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

/** One named tool advertised by a plugin. Strings must outlive the plugin. */
typedef struct gygax_plugin_tool {
    const char* name;
    const char* description;
} gygax_plugin_tool;

/**
 * Plugin descriptor returned by gygax_plugin_entry().
 *
 * The descriptor, strings, and tool array must remain valid until shutdown.
 * call() may be invoked concurrently; implementations must synchronize their
 * own mutable state. It returns 0 on success and nonzero on failure. On every
 * call, set *output to NULL or to a NUL-terminated buffer allocated for this
 * plugin; release() is called by the host for a non-NULL buffer.
 */
typedef struct gygax_plugin {
    uint32_t abi_version;
    const char* name;
    const char* version;
    size_t tool_count;
    const gygax_plugin_tool* tools;
    int (*call)(const char* tool, const char* input, char** output);
    void (*release)(char* buffer);
    void (*shutdown)(void);
} gygax_plugin;

/**
 * Return the plugin's static descriptor.
 * Export exactly one entry point from each plugin library.
 */
GYGAX_PLUGIN_EXPORT const gygax_plugin* gygax_plugin_entry(void);

#ifdef __cplusplus
}
#endif

#endif
