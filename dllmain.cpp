#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <jni.h>
#include <jvmti.h>

#include "scope_guard.h"

static constexpr std::string_view MINECRAFT_NAMES[] {
    "net/minecraft/client/Minecraft", // mcp
    "net/minecraft/class_1600",       // legacy fabric
    "ave",                            // notch
};

#define JVM_VERIFY(expr, message) do { \
        if (auto jvmVerifyErr = (expr); jvmVerifyErr != 0) \
            throw std::runtime_error(std::format("{} (error {})", message, static_cast<int>(jvmVerifyErr))); \
    } while (0)

static struct InitState {
    std::atomic_flag started;
    std::atomic_flag reportedSuccess;
    std::string_view minecraftName;
} init;

static std::pair<jclass, std::string_view> findMinecraftClass(jvmtiEnv *jvmti) {
    jint i;
    jclass *classes;
    JVM_VERIFY(jvmti->GetLoadedClasses(&i, &classes), "GetLoadedClasses failed");
    ScopeGuard freeClasses([&] { jvmti->Deallocate((unsigned char *) classes); });

    char *sig;
    while (i--) {
        if (jvmti->GetClassSignature(classes[i], &sig, nullptr) != JVMTI_ERROR_NONE) continue;
        std::string_view name(sig);
        auto match = name.size() > 2 && name.front() == 'L' && name.back() == ';'
            ? std::ranges::find(MINECRAFT_NAMES, name.substr(1, name.size() - 2))
            : std::end(MINECRAFT_NAMES);
        jvmti->Deallocate((unsigned char *) sig);

        if (match != std::end(MINECRAFT_NAMES)) return {classes[i], *match};
    }

    return {nullptr, {}};
}

static const char *getVM(JavaVM **vm) {
#ifdef _WIN32
    auto handle = GetModuleHandleW(L"jvm.dll");
    if (!handle) return "jvm.dll not found";
    auto addr = reinterpret_cast<decltype(&JNI_GetCreatedJavaVMs)>(GetProcAddress(handle, "JNI_GetCreatedJavaVMs"));
#else
    auto addr = reinterpret_cast<decltype(&JNI_GetCreatedJavaVMs)>(
        dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
#endif
    if (!addr) return "JNI_GetCreatedJavaVMs not found";

    if (jsize count; addr(vm, 1, &count) != JNI_OK || count == 0)
        return "JNI_GetCreatedJavaVMs failed";

    return nullptr;
}

static void report(const char *title, const char *message, bool error) noexcept {
#ifdef _WIN32
    MessageBoxA(nullptr, message, title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
#elif defined(__APPLE__)
    auto header = CFStringCreateWithCString(nullptr, title, kCFStringEncodingUTF8);
    auto body = CFStringCreateWithCString(nullptr, message, kCFStringEncodingUTF8);
    ScopeGuard releaseStrings([&] {
        if (header) CFRelease(header);
        if (body) CFRelease(body);
    });
    if (!header || !body || CFUserNotificationDisplayAlert(
            0, error ? kCFUserNotificationStopAlertLevel : kCFUserNotificationNoteAlertLevel,
            nullptr, nullptr, nullptr,
            header, body, CFSTR("OK"), nullptr, nullptr, nullptr) != 0)
        std::fprintf(stderr, "[NoHitDelay] %s: %s\n", title, message);
#else
    std::fprintf(stderr, "[NoHitDelay] %s: %s\n", title, message);
#endif
}

static void reportException() noexcept {
    try {
        throw;
    } catch (const std::exception &ex) {
        report("Error", ex.what(), true);
    } catch (...) {
        report("Error", "unknown exception", true);
    }
}

static uint32_t applyPatch(std::span<uint8_t> data) {
    // bipush 10, putfield
    static constexpr uint8_t pattern[] { 0x10, 0x0A, 0xB5 };

    uint32_t count = 0;
    auto it = data.begin();

    while (auto match = std::ranges::search(std::ranges::subrange(it, data.end()), pattern)) {
        count++;
        // bipush 10 -> bipush 0
        match[1] = 0x00;
        it = match.end();
    }

    return count;
}

static void JNICALL classFileLoadHook(
    jvmtiEnv *jvmti_env,
    JNIEnv *jni_env,
    jclass class_being_redefined,
    jobject loader,
    const char *name,
    jobject protection_domain,
    jint class_data_len,
    const unsigned char *class_data,
    jint *new_class_data_len,
    unsigned char **new_class_data
) noexcept {
    if (!class_being_redefined || !name || name != init.minecraftName) return;

    try {
        static constexpr uint32_t EXPECTED_PATCHES = 2;

        unsigned char *patched;
        JVM_VERIFY(jvmti_env->Allocate(class_data_len, &patched), "Allocate failed");
        ScopeGuard freePatched([&] { jvmti_env->Deallocate(patched); });

        memcpy(patched, class_data, class_data_len);
        auto count = applyPatch({patched, static_cast<size_t>(class_data_len)});
        if (count != EXPECTED_PATCHES)
            throw std::runtime_error(std::format("Expected {} replacements, got {}", EXPECTED_PATCHES, count));

        auto msg = std::format("Applied patch to {} ({} replacements)", name, count);
        if (!init.reportedSuccess.test_and_set())
            report("Success", msg.c_str(), false);
        else
            std::fprintf(stderr, "[NoHitDelay] %s\n", msg.c_str());

        *new_class_data = patched;
        *new_class_data_len = class_data_len;
        freePatched.dismiss();
    } catch (...) {
        reportException();
    }
}

static void load(jvmtiEnv *jvmti) {
    auto [minecraft, name] = findMinecraftClass(jvmti);
    if (!minecraft) throw std::runtime_error("Minecraft class not found (not minecraft 1.8.9, or unsupported mapping?)");
    init.minecraftName = name;

    jvmtiCapabilities caps{.can_retransform_classes = 1, .can_retransform_any_class = 1};
    JVM_VERIFY(jvmti->AddCapabilities(&caps), "AddCapabilities failed");

    jvmtiEventCallbacks callbacks{.ClassFileLoadHook = &classFileLoadHook};
    JVM_VERIFY(jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)), "SetEventCallbacks failed");

    JVM_VERIFY(jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr), "SetEventNotificationMode(JVMTI_ENABLE) failed");
    ScopeGuard disableHook([&] { jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr); });

    JVM_VERIFY(jvmti->RetransformClasses(1, &minecraft), "RetransformClasses failed");

    disableHook.dismiss();
}

static void initialize() noexcept {
    if (init.started.test_and_set()) return;

    JavaVM *vm;
    if (auto err = getVM(&vm)) {
        report("Failed to find running JVM", err, true);
        return;
    }

    try {
        JNIEnv *jni;
        JVM_VERIFY(vm->AttachCurrentThread((void **) &jni, nullptr), "AttachCurrentThread failed");
        ScopeGuard detach([vm] { vm->DetachCurrentThread(); });

        jvmtiEnv *jvmti;
        JVM_VERIFY(vm->GetEnv((void **) &jvmti, JVMTI_VERSION_1_2), "GetEnv failed");
        load(jvmti);
    } catch (...) {
        reportException();
    }
}

#ifdef _WIN32
static DWORD WINAPI threadMain(LPVOID) {
    initialize();
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        HMODULE pinned;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR) module, &pinned))
            return FALSE;

        auto thread = CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr);
        if (!thread) return FALSE;
        if (!CloseHandle(thread)) return FALSE;
    }
    return TRUE;
}

#else
static void *threadMain(void *) {
    initialize();
    return nullptr;
}

extern "C" __attribute__((visibility("default"))) int NoHitDelay_Initialize() noexcept {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return JNI_ERR;
    ScopeGuard destroyAttributes([&] { pthread_attr_destroy(&attributes); });
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) return JNI_ERR;
    pthread_t thread;
    if (pthread_create(&thread, &attributes, threadMain, nullptr) != 0) return JNI_ERR;
    return JNI_OK;
}
#endif
