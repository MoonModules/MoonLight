/// The Android app's entry: the two JNI calls the app's service makes, the counterpart of main_desktop.cpp's main().
///
/// The firmware is the desktop's, unchanged; this file gives it a data folder, a log, and a way to stop.

#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include "host_mdns.h"           // setHostMdns: Android's NsdManager as the platform's mDNS
#include "platform/platform.h"   // fsSetRoot

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

extern void mm_main(volatile std::sig_atomic_t& keepRunning, uint16_t httpPort);

namespace {

volatile std::sig_atomic_t running = 1;

// stdout and stderr into logcat, a line at a time under the tag MoonLight, since an app has no terminal: what a desktop prints there, `adb logcat -s MoonLight` shows.
void logToLogcat() {
    static int fds[2] = {-1, -1};
    if (fds[0] >= 0 || ::pipe(fds) != 0) return;   // once per process
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([] {
        char buf[512];
        std::string line;
        ssize_t n;
        while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] != '\n') { line += buf[i]; continue; }
                __android_log_write(ANDROID_LOG_INFO, "MoonLight", line.c_str());
                line.clear();
            }
        }
    }).detach();
}

// The app's mDNS, the Kotlin object Mdns over NsdManager, reached from whichever thread the firmware calls on.
JavaVM* vm_ = nullptr;
jclass mdnsClass_ = nullptr;
jmethodID mdnsBegin_ = nullptr, mdnsAdvertise_ = nullptr, mdnsWithdraw_ = nullptr, mdnsEnd_ = nullptr;

// A thread this file attached detaches as it ends, since the runtime aborts one that exits attached.
struct AttachedThread {
    bool attached = false;
    ~AttachedThread() { if (attached) vm_->DetachCurrentThread(); }
};

JNIEnv* threadEnv() {
    thread_local AttachedThread guard;
    JNIEnv* env = nullptr;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        vm_->AttachCurrentThread(&env, nullptr);
        guard.attached = true;
    }
    return env;
}

// Whether the call into Kotlin threw: the exception is logged and cleared, since one left pending aborts the process at the next JNI call.
bool threw(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

bool nsdBegin(const char* deviceName) {
    JNIEnv* env = threadEnv();
    jstring name = env->NewStringUTF(deviceName);
    env->CallStaticVoidMethod(mdnsClass_, mdnsBegin_, name);
    env->DeleteLocalRef(name);
    return !threw(env);   // false shows on the Network card, as a board's failed mDNS does
}

void nsdAdvertise(const char* type, const char* proto, uint16_t port) {
    JNIEnv* env = threadEnv();
    jstring t = env->NewStringUTF(type);
    jstring p = env->NewStringUTF(proto);
    env->CallStaticVoidMethod(mdnsClass_, mdnsAdvertise_, t, p, static_cast<jint>(port));
    env->DeleteLocalRef(t);
    env->DeleteLocalRef(p);
    threw(env);
}

void nsdWithdraw(const char* type, const char* proto) {
    JNIEnv* env = threadEnv();
    jstring t = env->NewStringUTF(type);
    jstring p = env->NewStringUTF(proto);
    env->CallStaticVoidMethod(mdnsClass_, mdnsWithdraw_, t, p);
    env->DeleteLocalRef(t);
    env->DeleteLocalRef(p);
    threw(env);
}

void nsdEnd() {
    JNIEnv* env = threadEnv();
    env->CallStaticVoidMethod(mdnsClass_, mdnsEnd_);
    threw(env);
}

// Find the Kotlin object on the app's thread, whose class loader knows it, and lend it to the platform.
void lendMdns(JNIEnv* env) {
    env->GetJavaVM(&vm_);
    jclass local = env->FindClass("org/moonmodules/moonlight/Mdns");
    if (!local) { env->ExceptionClear(); return; }
    mdnsClass_ = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    mdnsBegin_ = env->GetStaticMethodID(mdnsClass_, "begin", "(Ljava/lang/String;)V");
    mdnsAdvertise_ = env->GetStaticMethodID(mdnsClass_, "advertise", "(Ljava/lang/String;Ljava/lang/String;I)V");
    mdnsWithdraw_ = env->GetStaticMethodID(mdnsClass_, "withdraw", "(Ljava/lang/String;Ljava/lang/String;)V");
    mdnsEnd_ = env->GetStaticMethodID(mdnsClass_, "end", "()V");
    if (!mdnsBegin_ || !mdnsAdvertise_ || !mdnsWithdraw_ || !mdnsEnd_) { env->ExceptionClear(); return; }
    mm::platform::setHostMdns({nsdBegin, nsdAdvertise, nsdWithdraw, nsdEnd});
}

}  // namespace

/// Run MoonLight until stop(): the service calls it on a thread of its own, with the app's files folder as the data folder.
extern "C" JNIEXPORT void JNICALL Java_org_moonmodules_moonlight_MoonLightService_run(JNIEnv* env, jclass, jstring dataDir, jint httpPort) {
    const char* dir = env->GetStringUTFChars(dataDir, nullptr);
    // The data folder is decided when the library loads, before this call, so it is set here: MoonLight/ in the app's files, as Linux keeps it in ~/.local/share/MoonLight.
    const std::filesystem::path root = std::filesystem::path(dir) / "MoonLight";
    std::filesystem::create_directories(root);
    mm::platform::fsSetRoot(root.c_str());
    ::setenv("HOME", dir, 1);   // a home for anything else that asks
    env->ReleaseStringUTFChars(dataDir, dir);
    logToLogcat();
    lendMdns(env);
    running = 1;
    mm_main(running, static_cast<uint16_t>(httpPort));
}

/// Ask run() to return: the loop ends at its next pass, as a desktop's Ctrl+C ends it.
extern "C" JNIEXPORT void JNICALL Java_org_moonmodules_moonlight_MoonLightService_stop(JNIEnv*, jclass) {
    running = 0;
}
