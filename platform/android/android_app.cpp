#include "android_app.h"

#include <jni.h>
#include <SDL.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <thread>
#include <android/log.h>

namespace {
JavaVM* vm = nullptr;
jclass bridge = nullptr;
std::mutex stateMutex;
std::string filesPath, libraryPath;
cane::android::TouchState touch;
std::atomic<int> thermalStatus{0};
std::chrono::steady_clock::time_point lastTouchInput{};

class Environment {
    bool attached = false;
public:
    JNIEnv* env = nullptr;
    Environment() {
        if (!vm) throw std::runtime_error("Android application is not initialized");
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw std::runtime_error("Cannot attach Android worker thread");
            attached = true;
        }
    }
    ~Environment() { if (attached) vm->DetachCurrentThread(); }
    void check() {
        if (env->ExceptionCheck()) {
            auto failure = env->ExceptionOccurred();
            env->ExceptionClear();
            std::string message = "Android application operation failed";
            auto type = env->GetObjectClass(failure);
            auto method = env->GetMethodID(type, "getMessage", "()Ljava/lang/String;");
            if (method) {
                auto detail = static_cast<jstring>(env->CallObjectMethod(failure, method));
                if (detail) {
                    const char* bytes = env->GetStringUTFChars(detail, nullptr);
                    if (bytes) { message += ": "; message += bytes; env->ReleaseStringUTFChars(detail, bytes); }
                    env->DeleteLocalRef(detail);
                }
            }
            env->ExceptionClear(); env->DeleteLocalRef(type); env->DeleteLocalRef(failure);
            throw std::runtime_error(message);
        }
    }
    jmethodID method(const char* name, const char* signature) {
        if (!bridge) throw std::runtime_error("Android activity is unavailable");
        auto result = env->GetStaticMethodID(bridge, name, signature);
        check();
        if (!result) throw std::runtime_error("Missing Android application interface");
        return result;
    }
};
std::string string(JNIEnv* env, jstring value) {
    if (!value) return {};
    const jchar* units = env->GetStringChars(value, nullptr);
    if (!units) return {};
    std::string result;
    const auto size = env->GetStringLength(value);
    for (jsize i = 0; i < size; ++i) {
        uint32_t point = units[i];
        if (point >= 0xd800 && point <= 0xdbff && i + 1 < size &&
            units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
            point = 0x10000 + ((point - 0xd800) << 10) + units[++i] - 0xdc00;
        } else if (point >= 0xd800 && point <= 0xdfff) point = 0xfffd;
        if (point < 0x80) result += static_cast<char>(point);
        else if (point < 0x800) {
            result += static_cast<char>(0xc0 | (point >> 6));
            result += static_cast<char>(0x80 | (point & 0x3f));
        } else if (point < 0x10000) {
            result += static_cast<char>(0xe0 | (point >> 12));
            result += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (point & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (point >> 18));
            result += static_cast<char>(0x80 | ((point >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (point & 0x3f));
        }
    }
    env->ReleaseStringChars(value, units);
    return result;
}
jstring javaString(JNIEnv* env, const std::string& value) {
    // JNI's NewStringUTF uses modified UTF-8; ordinary UTF-8 paths and player
    // names can contain supplementary codepoints, so convert explicitly.
    std::vector<jchar> units;
    for (size_t i = 0; i < value.size();) {
        const uint8_t first = static_cast<uint8_t>(value[i++]);
        uint32_t point = first; unsigned count = 0;
        if (first >= 0xc2 && first <= 0xdf) { point = first & 0x1f; count = 1; }
        else if (first >= 0xe0 && first <= 0xef) { point = first & 0x0f; count = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { point = first & 7; count = 3; }
        else if (first >= 0x80) point = 0xfffd;
        bool valid = value.size() - i >= count;
        for (unsigned j = 0; valid && j < count; ++j) {
            const uint8_t next = static_cast<uint8_t>(value[i + j]);
            if ((next & 0xc0) != 0x80) valid = false;
            else point = (point << 6) | (next & 0x3f);
        }
        if (valid) i += count; else point = 0xfffd;
        if (point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
            (count == 1 && point < 0x80) || (count == 2 && point < 0x800) || (count == 3 && point < 0x10000)) point = 0xfffd;
        if (point < 0x10000) units.push_back(static_cast<jchar>(point));
        else {
            units.push_back(static_cast<jchar>(0xd800 + ((point - 0x10000) >> 10)));
            units.push_back(static_cast<jchar>(0xdc00 + ((point - 0x10000) & 0x3ff)));
        }
    }
    return env->NewString(units.data(), static_cast<jsize>(units.size()));
}
void call(const char* name) {
    Environment e;
    e.env->CallStaticVoidMethod(bridge, e.method(name, "()V"));
    e.check();
}
cane::android::ImportStatus importStatus(const char* name) {
    Environment e;
    auto values = static_cast<jobjectArray>(e.env->CallStaticObjectMethod(bridge,
        e.method(name, "()[Ljava/lang/String;")));
    e.check();
    cane::android::ImportStatus result;
    if (values && e.env->GetArrayLength(values) == 4) {
        std::array<std::string, 4> fields;
        for (int i = 0; i < 4; ++i) {
            auto field = static_cast<jstring>(e.env->GetObjectArrayElement(values, i));
            fields[i] = string(e.env, field);
            e.env->DeleteLocalRef(field);
        }
        result.pending = fields[0] == "1";
        result.progress = std::clamp(std::strtod(fields[1].c_str(), nullptr), 0.0, 1.0);
        result.path = fields[2]; result.error = fields[3];
    }
    if (values) e.env->DeleteLocalRef(values);
    return result;
}
} // namespace

namespace cane::android {
ChildProcess startProcess(const std::vector<std::string>& arguments) {
    if (arguments.empty()) throw std::runtime_error("Empty Android process command");
    Environment e;
    auto strings = e.env->FindClass("java/lang/String");
    auto args = e.env->NewObjectArray(static_cast<jsize>(arguments.size()), strings, nullptr);
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto argument = javaString(e.env, arguments[i]);
        e.env->SetObjectArrayElement(args, static_cast<jsize>(i), argument);
        e.env->DeleteLocalRef(argument);
    }
    auto values = static_cast<jlongArray>(e.env->CallStaticObjectMethod(bridge,
        e.method("startChild", "([Ljava/lang/String;)[J"), args));
    e.env->DeleteLocalRef(args); e.env->DeleteLocalRef(strings); e.check();
    if (!values || e.env->GetArrayLength(values) != 3)
        throw std::runtime_error("Cannot start Android application process");
    jlong fields[3]{}; e.env->GetLongArrayRegion(values, 0, 3, fields);
    e.env->DeleteLocalRef(values); e.check();
    return {static_cast<int>(fields[0]), static_cast<int>(fields[1]), fields[2]};
}
bool processRunning(std::int64_t handle) {
    Environment e;
    const bool result = e.env->CallStaticBooleanMethod(bridge,
        e.method("childRunning", "(J)Z"), static_cast<jlong>(handle));
    e.check(); return result;
}
int processExitCode(std::int64_t handle) {
    Environment e;
    const int result = e.env->CallStaticIntMethod(bridge,
        e.method("childExitCode", "(J)I"), static_cast<jlong>(handle));
    e.check(); return result;
}
void cancelProcess(std::int64_t handle) {
    Environment e;
    e.env->CallStaticVoidMethod(bridge, e.method("cancelChild", "(J)V"), static_cast<jlong>(handle));
    e.check();
}
std::string filesDirectory() { std::lock_guard lock(stateMutex); return filesPath; }
std::string nativeLibraryDirectory() { std::lock_guard lock(stateMutex); return libraryPath; }
void showGame() { call("showGame"); }
void showLauncher() { call("showLauncher"); }
void prepareRuntimeShutdown() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    // Native resume only posts SDL's lifecycle semaphore. Pump it here on the
    // owning thread: Android's paused OpenSL device otherwise cannot complete
    // the buffer that its audio thread waits for when CloseAudioDevice joins it.
    // Drain queued lifecycle notifications before a subsequent pump; several
    // focus/activity pauses can already be queued when the session ends.
    for (int pass = 0; pass != 4; ++pass) {
        call("prepareRuntimeShutdown");
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_APP_WILLENTERBACKGROUND, SDL_APP_DIDENTERFOREGROUND);
    }
}
void reportExit(int code) {
    Environment e;
    e.env->CallStaticVoidMethod(bridge, e.method("reportExit", "(I)V"), code); e.check();
}
void requestIsoSelection() { call("selectIso"); }
void requestModImport() { call("importMod"); }
ImportStatus isoSelection() { return importStatus("isoStatus"); }
ImportStatus modImport() { return importStatus("modStatus"); }
void openFolder(const std::string& path) {
    Environment e; auto value = javaString(e.env, path);
    e.env->CallStaticVoidMethod(bridge, e.method("openFolder", "(Ljava/lang/String;)V"), value);
    e.env->DeleteLocalRef(value); e.check();
}
void openUrl(const std::string& url) {
    Environment e; auto value = javaString(e.env, url);
    e.env->CallStaticVoidMethod(bridge, e.method("openUrl", "(Ljava/lang/String;)V"), value);
    e.env->DeleteLocalRef(value); e.check();
}
namespace {
jobjectArray javaStrings(JNIEnv* env, const std::vector<std::string>& values) {
    auto type = env->FindClass("java/lang/String");
    auto array = env->NewObjectArray(static_cast<jsize>(values.size()), type, nullptr);
    for (size_t i = 0; i < values.size(); ++i) {
        auto value = javaString(env, values[i]);
        env->SetObjectArrayElement(array, static_cast<jsize>(i), value);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(type);
    return array;
}
}  // namespace
bool startApp(const std::vector<std::string>& arguments) {
    try {
        Environment e;
        auto args = javaStrings(e.env, arguments);
        const bool ok = e.env->CallStaticBooleanMethod(bridge, e.method("startApp", "([Ljava/lang/String;)Z"), args);
        e.env->DeleteLocalRef(args); e.check();
        return ok;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "start app: %s\n", failure.what());
        return false;
    }
}
bool pinShortcut(const std::string& id, const std::string& label, const std::string& png, const std::vector<std::string>& arguments) {
    Environment e;
    auto jid = javaString(e.env, id), jlabel = javaString(e.env, label);
    auto icon = e.env->NewByteArray(static_cast<jsize>(png.size()));
    e.env->SetByteArrayRegion(icon, 0, static_cast<jsize>(png.size()), reinterpret_cast<const jbyte*>(png.data()));
    auto args = javaStrings(e.env, arguments);
    const bool ok = e.env->CallStaticBooleanMethod(bridge,
        e.method("pinShortcut", "(Ljava/lang/String;Ljava/lang/String;[B[Ljava/lang/String;)Z"), jid, jlabel, icon, args);
    e.env->DeleteLocalRef(jid); e.env->DeleteLocalRef(jlabel); e.env->DeleteLocalRef(icon); e.env->DeleteLocalRef(args);
    e.check();
    return ok;
}
TouchState touchState() { std::lock_guard lock(stateMutex); return touch; }
bool touchInputRecently() {
    std::lock_guard lock(stateMutex);
    return std::chrono::steady_clock::now()-lastTouchInput<std::chrono::milliseconds(300);
}
void setTouchControlsVisible(bool visible) {
    Environment e;
    e.env->CallStaticVoidMethod(bridge,e.method("setTouchControlsVisible","(Z)V"),jboolean(visible));
    e.check();
}
int thermalFrameCap() {
    // PowerManager.THERMAL_STATUS_SEVERE (3) and above: the device is already
    // throttling hard. A steady 30 is easier to play than an uneven 35-50
    // and lets the SoC cool; MODERATE keeps the user's rate.
    return thermalStatus.load(std::memory_order_relaxed) >= 3 ? 30 : 0;
}
void setTouchContext(TouchContext context) {
    static int reported = -1;
    if (int(context) == reported) return;
    reported = int(context); // a failing bridge is reported once, not every frame
    try {
        Environment e;
        e.env->CallStaticVoidMethod(bridge,e.method("setTouchContext","(I)V"),jint(context));
        e.check();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "touch context: %s\n", failure.what());   // (no activity: the tool service)
    }
}
} // namespace cane::android

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* value, void*) {
    vm = value; return JNI_VERSION_1_6;
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_safehouse_android_NativeBridge_nativeDiagnosticIdentity(JNIEnv* env,jclass) {
    try {
    // GNU build IDs survive stripping and identify matching private symbols.
    // Capture only this APK's loaded native modules, never addresses, arguments
    // or the rest of the system's process information.
    struct Modules {std::string directory;std::vector<std::pair<std::string,std::string>> entries;} modules;
    {std::lock_guard lock(stateMutex);modules.directory=libraryPath;}
    dl_iterate_phdr([](dl_phdr_info* info,size_t,void* context)->int {
        auto& result=*static_cast<Modules*>(context);
        const std::string name=info->dlpi_name ? info->dlpi_name : "";
        if(name.empty() || !name.starts_with(result.directory+"/") || result.entries.size()>=128)return 0;
        std::string build;
        for(size_t i=0;i<info->dlpi_phnum;++i) {
            const auto& segment=info->dlpi_phdr[i];
            if(segment.p_type!=PT_NOTE || segment.p_memsz>1024*1024)continue;
            const auto* bytes=reinterpret_cast<const unsigned char*>(info->dlpi_addr+segment.p_vaddr);
            size_t offset=0;
            while(segment.p_memsz-offset>=sizeof(ElfW(Nhdr))) {
                ElfW(Nhdr) note{};std::memcpy(&note,bytes+offset,sizeof(note));offset+=sizeof(note);
                const size_t names=(size_t(note.n_namesz)+3)&~size_t(3);
                const size_t data=(size_t(note.n_descsz)+3)&~size_t(3);
                if(names>segment.p_memsz-offset || data>segment.p_memsz-offset-names)break;
                if(note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 &&
                    std::memcmp(bytes+offset,"GNU\0",4)==0 && note.n_descsz>0 && note.n_descsz<=64) {
                    std::ostringstream hex;
                    for(size_t j=0;j<note.n_descsz;++j)hex<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(bytes[offset+names+j]);
                    build=hex.str();break;
                }
                offset+=names+data;
            }
            if(!build.empty())break;
        }
        result.entries.emplace_back(name.substr(name.find_last_of('/')+1),std::move(build));return 0;
    },&modules);
    std::ostringstream json;json<<'[';
    bool comma=false;
    const auto quote=[](const std::string& value) {
        std::string result="\"";
        for(const unsigned char c:value) {
            if(c=='"' || c=='\\')result+='\\';
            if(c>=32)result+=char(c);
        }
        return result+'"';
    };
    for(const auto& [name,id]:modules.entries) {
        if(comma)json<<',';comma=true;
        json<<"{\"module\":"<<quote(name)<<",\"elf_build_id\":"<<quote(id)<<'}';
    }
    json<<']';return javaString(env,json.str());
    } catch(...) {return env->NewStringUTF("[]");}
}
namespace {
// stdout / stderr into logcat (tag "SafeHouse") until a child process attaches the launcher's pipes instead
// (nativeAttachPipes): the launcher's own output, which the desktop build writes to the terminal.
void redirectOutputToLogcat() {
    static bool started = false;
    if (started) return;
    started = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]] {
        std::string line;
        char buffer[1024];
        for (;;) {
            const auto n = read(fd, buffer, sizeof(buffer));
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (buffer[i] == '\n' || line.size() > 900) {
                    __android_log_write(ANDROID_LOG_INFO, "SafeHouse", line.c_str());
                    line.clear();
                    if (buffer[i] == '\n') continue;
                }
                line += buffer[i];
            }
        }
    }).detach();
}
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_safehouse_android_NativeBridge_nativeInitialize(JNIEnv* env, jclass cls,
                                                       jstring files, jstring libraries) {
    env->GetJavaVM(&vm);
    if (!bridge) bridge = static_cast<jclass>(env->NewGlobalRef(cls));
    std::lock_guard lock(stateMutex);
    filesPath = string(env, files); libraryPath = string(env, libraries);
    setenv("SAFEHOUSE_ANDROID_LIBS", libraryPath.c_str(), 1);
    setenv("HOME", filesPath.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (filesPath + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (filesPath + "/share").c_str(), 1);
    setenv("XDG_STATE_HOME", (filesPath + "/state").c_str(), 1);
    setenv("XDG_CACHE_HOME", (filesPath + "/cache").c_str(), 1);
    redirectOutputToLogcat();
}
extern "C" JNIEXPORT void JNICALL
Java_org_safehouse_android_NativeBridge_nativeAttachPipes(JNIEnv*, jclass, jint input, jint output) {
    if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
    if (output >= 0) {
        // The child's stdout / stderr go to the launcher's pipe (its progress and log lines, as on the desktop) and,
        // line by line, to logcat (tag "SafeHouse") for device tests.
        int fds[2];
        if (pipe(fds) == 0) {
            dup2(fds[1], STDOUT_FILENO); dup2(fds[1], STDERR_FILENO); close(fds[1]);
            std::thread([in = fds[0], out = int(output)] {
                std::string line;
                char buffer[4096];
                for (;;) {
                    const auto n = read(in, buffer, sizeof(buffer));
                    if (n <= 0) break;
                    for (ssize_t done = 0; done < n;) {
                        const auto w = write(out, buffer + done, size_t(n - done));
                        if (w <= 0) break;
                        done += w;
                    }
                    for (ssize_t i = 0; i < n; ++i) {
                        if (buffer[i] == '\n' || line.size() > 900) {
                            __android_log_write(ANDROID_LOG_INFO, "SafeHouse", line.c_str());
                            line.clear();
                            if (buffer[i] == '\n') continue;
                        }
                        line += buffer[i];
                    }
                }
                close(out);
            }).detach();
        } else {
            dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO); close(output);
        }
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}
extern "C" JNIEXPORT void JNICALL
Java_org_safehouse_android_NativeBridge_nativeTouch(JNIEnv* env, jclass, jboolean active,
                                                   jfloatArray axes, jbooleanArray buttons) {
    std::lock_guard lock(stateMutex);
    touch.active = active;
    if(active)lastTouchInput=std::chrono::steady_clock::now();
    if (axes && env->GetArrayLength(axes) == 6) {
        jfloat values[6]; env->GetFloatArrayRegion(axes, 0, 6, values);
        for (size_t i = 0; i < 6; ++i)
            touch.axes[i] = std::isfinite(values[i]) ? std::clamp(values[i], -1.0f, 1.0f) : 0;
    }
    if (buttons && env->GetArrayLength(buttons) == 15) {
        jboolean values[15]; env->GetBooleanArrayRegion(buttons, 0, 15, values);
        for (size_t i = 0; i < 15; ++i) touch.buttons[i] = values[i] ? 1 : 0;
    }
}
extern "C" JNIEXPORT void JNICALL
Java_org_safehouse_android_NativeBridge_nativeThermalStatus(JNIEnv*, jclass, jint status) {
    const int previous=thermalStatus.exchange(int(status));
    if (previous!=status)
        std::fprintf(stderr,"Android thermal status %d%s\n",int(status),status>=3?": limiting presentation to 30 FPS":"");
}
extern "C" JNIEXPORT jint JNICALL
Java_org_safehouse_android_NativeBridge_nativeRunExtractor(JNIEnv* env, jclass, jobjectArray args) {
    using Entry = int (*)(int, char**);
    auto entry = reinterpret_cast<Entry>(dlsym(RTLD_DEFAULT, "cane_extractor_main"));
    if (!entry) { std::fputs("Extractor entry point is unavailable\n", stderr); return 1; }
    std::vector<std::string> arguments{"cane-extractor"};
    for (jsize i = 0; args && i < env->GetArrayLength(args); ++i) {
        auto argument = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        arguments.push_back(string(env, argument)); env->DeleteLocalRef(argument);
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return entry(static_cast<int>(arguments.size()), pointers.data());
}
extern "C" JNIEXPORT jint JNICALL
Java_org_safehouse_android_NativeBridge_nativeRunRuntime(JNIEnv* env, jclass, jobjectArray args) {
    using Entry = int (*)(int, char**);
    auto entry = reinterpret_cast<Entry>(dlsym(RTLD_DEFAULT, "SDL_main"));
    if (!entry) { std::fputs("Runtime entry point is unavailable\n", stderr); return 1; }
    std::vector<std::string> arguments{"Sly2-Release"};
    for (jsize i = 0; args && i < env->GetArrayLength(args); ++i) {
        auto argument = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        arguments.push_back(string(env, argument)); env->DeleteLocalRef(argument);
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return entry(static_cast<int>(arguments.size()), pointers.data());
}
