// Loads an .ofx binary the way a host does and lists the plugins it exports.
#include <dlfcn.h>
#include <stdio.h>
#include "ofxCore.h"
int main(int argc, char** argv) {
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    int (*count)(void) = (int (*)(void))dlsym(h, "OfxGetNumberOfPlugins");
    OfxPlugin* (*get)(int) = (OfxPlugin * (*)(int)) dlsym(h, "OfxGetPlugin");
    if (!count || !get) { printf("missing entry points\n"); return 1; }
    int n = count();
    printf("plugins: %d\n", n);
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = get(i);
        printf("  %s v%u.%u api=%s v%d\n", p->pluginIdentifier, p->pluginVersionMajor, p->pluginVersionMinor, p->pluginApi, p->apiVersion);
    }
    return 0;
}
