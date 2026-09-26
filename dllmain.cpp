#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
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

static constexpr const char *MINECRAFT_NAMES[] {
    "net/minecraft/client/Minecraft", // mcp
    "net/minecraft/class_1600",       // legacy fabric
    "ave",                            // notch
};

#define JVM_VERIFY(expr, message) do { \
        if (auto jvmVerifyErr = (expr); jvmVerifyErr != 0) \
            throw std::runtime_error(std::format("{} (error {})", message, static_cast<int>(jvmVerifyErr))); \
    } while (0)

static struct InitState {
    static constexpr int CLOSED = 0, OPEN = 1, CLAIMED = 2;

    std::atomic<int> state = CLOSED;
    std::exception_ptr error;
    std::string minecraftName;
} init;

static std::string describeException() {
    try {
        throw;
    } catch (const std::exception &ex) {
        return ex.what();
    } catch (...) {
        return "unknown exception";
    }
}

static std::pair<jclass, const char *> findMinecraftClass(jvmtiEnv *jvmti) {
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

    return {nullptr, nullptr};
}

static const wchar_t* getVM(JavaVM **vm) {
    auto handle = GetModuleHandleW(L"jvm.dll");
    if (!handle) return L"jvm.dll not found";
    auto addr = reinterpret_cast<decltype(&JNI_GetCreatedJavaVMs)>(GetProcAddress(handle, "JNI_GetCreatedJavaVMs"));
    if (!addr) return L"JNI_GetCreatedJavaVMs not found";

    if (jsize count; addr(vm, 1, &count) != JNI_OK || count == 0)
        return L"JNI_GetCreatedJavaVMs failed";

    return nullptr;
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

static uint32_t patchClass(jvmtiEnv *jvmti, jint len, const unsigned char *data, jint *newLen, unsigned char **newData) {
    static constexpr uint32_t EXPECTED_PATCHES = 2;

    unsigned char *out;
    JVM_VERIFY(jvmti->Allocate(len, &out), "Allocate failed");
    ScopeGuard freeOut([&] { jvmti->Deallocate(out); });

    memcpy(out, data, len);
    auto count = applyPatch({out, static_cast<size_t>(len)});
    if (count != EXPECTED_PATCHES)
        throw std::runtime_error(std::format("Expected {} replacements, got {}", EXPECTED_PATCHES, count));

    freeOut.dismiss();
    *newData = out;
    *newLen = len;
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

    char msg[256];
    if (int expected = init.OPEN; init.state.compare_exchange_strong(expected, init.CLAIMED)) {
        try {
            auto count = patchClass(jvmti_env, class_data_len, class_data, new_class_data_len, new_class_data);
            std::snprintf(msg, sizeof(msg), "Applied patch to %s (%u replacements)", name, count);
            MessageBoxA(nullptr, msg, "Success", MB_OK);
        } catch (...) {
            init.error = std::current_exception();
        }
        init.state.store(init.CLOSED);
        init.state.notify_all();
        return;
    }

    try {
        auto count = patchClass(jvmti_env, class_data_len, class_data, new_class_data_len, new_class_data);
        std::snprintf(msg, sizeof(msg), "Re-applied patch to %s (%u replacements)", name, count);
        std::fprintf(stderr, "[NoHitDelay] %s\n", msg);
    } catch (...) {
        auto error = "Failed to re-apply patch to " + init.minecraftName + ": " + describeException();
        std::fprintf(stderr, "[NoHitDelay] %s\n", msg);
        jni_env->FatalError(error.c_str());
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

    init.state.store(init.OPEN);
    auto err = jvmti->RetransformClasses(1, &minecraft);

    int expected = init.OPEN;
    bool claimed = !init.state.compare_exchange_strong(expected, init.CLOSED);
    if (claimed) init.state.wait(init.CLAIMED);

    if (init.error) std::rethrow_exception(init.error);
    JVM_VERIFY(err, "RetransformClasses failed");
    if (!claimed) throw std::runtime_error("ClassFileLoadHook was never called for " + init.minecraftName);

    disableHook.dismiss();
}

static DWORD WINAPI threadMain(LPVOID) {
    JavaVM *vm;
    if (auto err = getVM(&vm)) {
        MessageBoxW(nullptr, err, L"Failed to find running JVM", MB_OK);
        return 1;
    }

    try {
        JNIEnv *jni;
        JVM_VERIFY(vm->AttachCurrentThread((void **) &jni, nullptr), "AttachCurrentThread failed");
        ScopeGuard detach([vm] { vm->DetachCurrentThread(); });

        jvmtiEnv *jvmti;
        JVM_VERIFY(vm->GetEnv((void **) &jvmti, JVMTI_VERSION_1_2), "GetEnv failed");
        load(jvmti);
    } catch (...) {
        MessageBoxA(nullptr, describeException().c_str(), "Error", MB_OK);
        return 1;
    }

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
