#include <stdlib.h>
#include <string.h>

#include <gygax/sdk/plugin.h>

static const gygax_plugin_tool kTools[] = {
    {"reverse", "Reverse the input string"},
};

static char* copy(const char* text, size_t length) {
    char* out = (char*)malloc(length + 1);
    if (out != NULL) {
        memcpy(out, text, length);
        out[length] = '\0';
    }
    return out;
}

static int call(const char* tool, const char* input, char** output) {
    if (strcmp(tool, "reverse") != 0) {
        *output = copy("unknown tool", 12);
        return 1;
    }
    const size_t n = strlen(input);
    char* out = copy(input, n);
    if (out == NULL) {
        *output = NULL;
        return 1;
    }
    for (size_t i = 0; i < n / 2; ++i) {
        const char t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    *output = out;
    return 0;
}

static void release(char* buffer) {
    free(buffer);
}

static void shutdown_plugin(void) {}

static const gygax_plugin kPlugin = {GYGAX_PLUGIN_ABI_VERSION, "cplug", "1.0.0", 1, kTools, call, release, shutdown_plugin};

const gygax_plugin* gygax_plugin_entry(void) {
    return &kPlugin;
}
