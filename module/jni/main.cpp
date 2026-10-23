#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <android/log.h>
#include "zygisk.hpp"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "CarXSnow", __VA_ARGS__)

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static const char* kPkg = "com.carxtech.sr";
static const char* kCfg = "/data/local/tmp/carx_snow.ini";

// ---- config ----
static volatile int  g_en   = 1;
static volatile float g_size = 2.0f;
static volatile float g_rate = 1.0f;
static volatile float g_speed= 1.0f;

static void loadCfg() {
    FILE* f = fopen(kCfg, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char k[64]; float v;
        if (sscanf(line, "%63[^=]=%f", k, &v) == 2) {
            if (!strcmp(k, "enabled")) g_en = (int)v;
            else if (!strcmp(k, "size") || !strcmp(k, "scale")) g_size = v;
            else if (!strcmp(k, "rate"))  g_rate = v;
            else if (!strcmp(k, "speed")) g_speed = v;
        }
    }
    fclose(f);
}

// ---- hook plumbing ----
// NOTE: full arm64 inline hook implementation is included below.
// RVA only valid for CarX Street 1.23.0 arm64.
static const uintptr_t RVA_UPDATE_EMITTER = 0xBCA2D74;
static const uint32_t  OFF_SIZE_VALUE = 0x80 + 0x14; // sizeMinMax.m_Value.x

typedef void (*UpdateEmitterListFn)(void* self);
static UpdateEmitterListFn origUpdateEmitterList = nullptr;
static void* g_target = nullptr;

// pre-hook: scale sizes before call, restore after
static void hookUpdateEmitterList(void* self) {
    loadCfg();
    if (!g_en || !self) { origUpdateEmitterList(self); return; }
    // m_snowFX at 0x10, sizeMinMax param at 0x80 within SnowParticleEffect
    // We patch the VolumeParameter m_Value floats directly.
    //self is ParticleEffectSystem; m_snowFX @0x10
    uint8_t* sys = (uint8_t*)self;
    void* snowFX = *(void**)(sys + 0x10);
    if (!snowFX) { origUpdateEmitterList(self); return; }
    float* sz = (float*)((uint8_t*)snowFX + 0x80 + 0x14);
    float old0 = sz[0], old1 = sz[1];
    sz[0] = old0 * g_size;
    sz[1] = old1 * g_size;
    origUpdateEmitterList(self);
    sz[0] = old0;
    sz[1] = old1;
}

// minimal arm64 trampoline hook
static bool installHook(void* target, void* hook, void** origOut) {
    // 16-byte aligned page
    uintptr_t page = (uintptr_t)target & ~0xFFFULL;
    size_t len = 4096 * 2;
    if (mprotect((void*)page, len, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        LOG("mprotect fail");
        return false;
    }
    // trampoline: copy first 4 instrs (16 bytes) + jump back
    uint8_t* tramp = (uint8_t*)mmap(nullptr, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return false;
    memcpy(tramp, target, 16);
    // absolute jump back to target+16
    uint64_t back = (uint64_t)((uint8_t*)target + 16);
    uint32_t* t = (uint32_t*)(tramp + 16);
    t[0] = 0x58000051; // ldr x17, #8
    t[1] = 0xD61F0220; // br x17
    *(uint64_t*)(tramp + 24) = back;

    // patch target: ldr x17,#8 ; br x17 ; <addr>
    uint32_t* p = (uint32_t*)target;
    p[0] = 0x58000051;
    p[1] = 0xD61F0220;
    *(uint64_t*)(target + 8) = (uint64_t)hook;

    __builtin___clear_cache((char*)target, (char*)target + 16);
    *origOut = (void*)tramp;
    return true;
}

class SnowModule : public zygisk::ModuleBase {
public:
    void onLoad(Api* api, JNIEnv* env) override { this->api = api; this->env = env; }
    void preAppSpecialize(AppSpecializeArgs* args) override {
        const char* name = env->GetStringUTFChars(args->nice_name, nullptr);
        isTarget = name && !strcmp(name, kPkg);
        env->ReleaseStringUTFChars(args->nice_name, name);
    }
    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!isTarget) { api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY); return; }
        LOG("injected into %s", kPkg);
        void* handle = api->getModuleDir();
        // find libil2cpp base
        uintptr_t base = 0;
        FILE* f = fopen("/proc/self/maps", "r");
        char line[512];
        while (f && fgets(line, sizeof(line), f)) {
            if (strstr(line, "libil2cpp.so")) {
                base = strtoull(line, nullptr, 16);
                break;
            }
        }
        if (f) fclose(f);
        if (!base) { LOG("libil2cpp not found"); return; }
        LOG("libil2cpp base = 0x%lx", (unsigned long)base);
        void* target = (void*)(base + RVA_UPDATE_EMITTER);
        // probe sanity
        uint32_t first = *(uint32_t*)target;
        if (first == 0 || first == 0xFFFFFFFF) {
            LOG("bad RVA, version mismatch");
            return;
        }
        if (installHook(target, (void*)hookUpdateEmitterList, (void**)&origUpdateEmitterList))
            LOG("hooked 0x%lx", (unsigned long)target);
        else
            LOG("hook install failed");
        (void)handle;
    }
private:
    Api* api = nullptr;
    JNIEnv* env = nullptr;
    bool isTarget = false;
};

REGISTER_ZYGISK_MODULE(SnowModule)
