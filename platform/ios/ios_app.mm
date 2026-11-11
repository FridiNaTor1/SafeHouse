// The iOS app's bridge (ios_app.h), in SafeHouseCore.framework with SDL2: the programs run in this process, the
// folders, the document picker, links, haptics and the app log. The touch pad is touch_overlay.mm.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ios_app.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex stateMutex;
std::string documentsPath, bundlePath;
bool smoke = false;
FILE* logFile = nullptr;
std::mutex logMutex;
// The console (Xcode / simctl --console-pty) as it was at start: a program's run points stdout / stderr at its pipe, and
// the app's own log lines must not land in that pipe (the relay below would read them back).
int consoleFd = STDERR_FILENO;

struct Program {
    std::string path, image;
    std::vector<std::string> arguments;
    int outputFd = -1;  // the write end of the caller's pipe (-1: the app's own output)
    bool mainThread = false;
    // Started by a program whose output is redirected (the extractor runs Sly 2's converter, File Extractor/pcbrx.h
    // RunTool): that program holds outputMutex and waits for this one, so this one must not wait for the mutex.
    bool nested = false;
    std::atomic<bool> running{true};
    std::atomic<int> code{1};
};
std::map<std::int64_t, std::shared_ptr<Program>> programs;
std::deque<std::shared_ptr<Program>> mainQueue;
std::set<std::string> imagesRun;  // (program images that already ran their main in this app session)
std::int64_t nextHandle = 1;
std::mutex outputMutex;  // one program's output redirection at a time (a nested program stacks on its parent's)
// The program running on this thread with its output redirected (nullptr: none): startProcess marks a program it
// starts from here as nested.
thread_local Program* redirectedHere = nullptr;

NSString* ns(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()] ?: @""; }

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
// Sly 2's tool commands run in its second image (Sly2Tools.framework, the same code): the game's image keeps clean
// globals for the game after an install.
bool sly2Tool(const std::vector<std::string>& arguments) {
    static const char* const tools[] = {"--convert", "--export-textures", "--ps3-hd-textures", "--import-languages",
                                        "--import-ps3-languages", "--move-languages", "--clear-texture-cache",
                                        "--build-texture-cache", "--list-languages", "--check-language", "--version",
                                        "--list-keys", "--list-audio-devices"};
    for (size_t i = 1; i < arguments.size(); ++i) {
        const std::string& a = arguments[i];
        for (const char* t : tools)
            if (a == t) return true;
        if (a.size() > 9 && a.compare(0, 2, "--") == 0 && a.compare(a.size() - 9, 9, "-selftest") == 0) return true;
    }
    return false;
}

void writeLine(int fd, const std::string& line) {
    const std::string text = line + "\n";
    if (fd >= 0) (void)!write(fd, text.data(), text.size());
}

// A progress line: JSON progress of a stage other than "error" (the extractor's --progress=jsonl), or a tool's
// "progress|languages|textures|worlds N M item" line. The app log keeps one of these every few seconds, every other line.
bool progressLine(const std::string& line) {
    if (!line.empty() && line.front() == '{')
        return line.find("\"stage\"") != std::string::npos && line.find("\"stage\":\"error\"") == std::string::npos;
    for (const char* word : {"progress ", "languages ", "textures ", "worlds ", "convert "}) {
        const size_t n = std::strlen(word);
        if (line.size() > n && line.compare(0, n, word) == 0 && line[n] >= '0' && line[n] <= '9') return true;
    }
    return false;
}
// Copies a program's output from `from` (its stdout / stderr while it runs) to its caller's pipe `to`, and each line
// to the app log (Documents/logs/ios.log) as "<program>: line": the reason a program failed is in the log even when
// the caller shows only an exit code. Ends at EOF (the program's run restored stdout / stderr).
void relayOutput(int from, int to, std::string name) {
    std::string pending;
    char buffer[8192];
    auto lastProgress = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    size_t skipped = 0;
    auto logLine = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        if (progressLine(line)) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress < std::chrono::seconds(3)) {
                ++skipped;
                return;
            }
            lastProgress = now;
        }
        if (line.size() > 2000) line = line.substr(0, 2000) + " ...";
        cane::ios::log(name + ": " + line + (skipped ? " (" + std::to_string(skipped) + " progress lines skipped)" : std::string()));
        skipped = 0;
    };
    for (;;) {
        const ssize_t n = read(from, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (to >= 0) {
            for (ssize_t done = 0; done < n;) {
                const ssize_t w = write(to, buffer + done, size_t(n - done));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    to = -1;  // (the caller stopped reading: keep logging)
                    break;
                }
                done += w;
            }
        }
        pending.append(buffer, size_t(n));
        for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
            logLine(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
        if (pending.size() > 65536) {
            logLine(pending);
            pending.clear();
        }
    }
    logLine(pending);
    close(from);
}

// Runs the program's main in this process. Its stdout / stderr go to its pipe while it runs.
int runProgram(Program& p) {
    {
        std::lock_guard lock(stateMutex);
        if (p.image == "Sly2Release" && imagesRun.count(p.image)) {
            writeLine(p.outputFd, "SafeHouse on iOS starts Sly 2 once per app start. Close SafeHouse in the app switcher and open it "
                                  "again to play once more.");
            cane::ios::log("refused a second game session in this app start");
            return 75;
        }
        if (imagesRun.count(p.image)) cane::ios::log("note: " + p.image + " runs again in this app session");
        imagesRun.insert(p.image);
    }
    void* image = dlopen(p.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        const std::string why = std::string("Cannot load ") + p.path + ": " + (dlerror() ?: "?");
        writeLine(p.outputFd, why);
        cane::ios::log(why);
        return 127;
    }
    using Entry = int (*)(int, char**);
    auto entry = reinterpret_cast<Entry>(dlsym(image, "cane_program_main"));
    if (!entry) {
        writeLine(p.outputFd, "No entry point in " + p.path);
        return 127;
    }
    std::vector<std::string> arguments = p.arguments;
    arguments[0] = baseName(p.path);
    std::vector<char*> argv;
    for (auto& a : arguments) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::string line = "run:";
    for (const auto& a : p.arguments) line += " " + a;
    cane::ios::log(line + (p.mainThread ? " (main thread)" : " (worker thread)"));

    // stdout / stderr are the process's: while the program runs they point at a relay pipe whose thread copies the
    // output to the caller's pipe and to the app log. A nested program (the extractor's converter) points them at
    // its own relay and gives them back to its parent's when it ends.
    std::unique_lock output(outputMutex, std::defer_lock);
    int savedOut = -1, savedErr = -1;
    std::thread relay;
    Program* const outer = redirectedHere;
    if (p.outputFd >= 0) {
        if (!p.nested) output.lock();
        int relayPipe[2] = {-1, -1};
        std::fflush(nullptr);
        savedOut = dup(STDOUT_FILENO);
        savedErr = dup(STDERR_FILENO);
        if (pipe(relayPipe) == 0) {
            fcntl(relayPipe[0], F_SETFD, FD_CLOEXEC);
            fcntl(relayPipe[1], F_SETFD, FD_CLOEXEC);
            relay = std::thread(relayOutput, relayPipe[0], p.outputFd, p.image);
            dup2(relayPipe[1], STDOUT_FILENO);
            dup2(relayPipe[1], STDERR_FILENO);
            close(relayPipe[1]);
        } else {
            dup2(p.outputFd, STDOUT_FILENO);
            dup2(p.outputFd, STDERR_FILENO);
        }
        setvbuf(stdout, nullptr, _IOLBF, 0);
        redirectedHere = &p;
    }
    int code = 1;
    try {
        code = entry(int(arguments.size()), argv.data());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s stopped: %s\n", p.image.c_str(), e.what());
    } catch (...) {
        std::fprintf(stderr, "%s stopped (unknown exception)\n", p.image.c_str());
    }
    if (p.outputFd >= 0) {
        std::fflush(nullptr);
        dup2(savedOut, STDOUT_FILENO);
        dup2(savedErr, STDERR_FILENO);
        close(savedOut);
        close(savedErr);
        redirectedHere = outer;
        // (the relay's write end is closed now everywhere: it reads to the end and stops)
        if (relay.joinable()) relay.join();
    }
    cane::ios::log(baseName(p.path) + " exited with " + std::to_string(code));
    return code;
}
void finish(const std::shared_ptr<Program>& p, int code) {
    p->code = code;
    if (p->outputFd >= 0) close(p->outputFd);
    p->outputFd = -1;
    p->running = false;
}
void* worker(void* data) {
    auto* holder = static_cast<std::shared_ptr<Program>*>(data);
    std::shared_ptr<Program> p = *holder;
    delete holder;
    int code = 1;
    @autoreleasepool {
        try {
            code = runProgram(*p);
        } catch (...) {
        }
    }
    finish(p, code);
    return nullptr;
}

UIViewController* topViewController() {
    UIWindow* key = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow* window in ((UIWindowScene*)scene).windows)
            if (window.isKeyWindow || (!key && !window.hidden)) key = window;
    }
    UIViewController* top = key.rootViewController;
    while (top.presentedViewController) top = top.presentedViewController;
    return top;
}

cane::ios::ImportStatus isoStatus;

// The app's window scene (the UIScene life cycle: Info.plist UIApplicationSceneManifest, scripts/ios/package.py).
// iOS shows an app without it, or one linked against an older SDK, in a compatibility frame on screens it does not
// know (a foldable's inner screen); an app built with the iOS 27 SDK must have it to start at all. SDL 2.30 has no
// scene support: SDL's windows are put into this scene when they are made (adoptWindow).
__weak UIWindowScene* appScene = nil;
}  // namespace

@interface SHSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation SHSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    appScene = (UIWindowScene*)scene;
    cane::ios::log("iOS: window scene connected");
}
- (void)windowScene:(UIWindowScene*)scene
    didUpdateCoordinateSpace:(id<UICoordinateSpace>)previousCoordinateSpace
        interfaceOrientation:(UIInterfaceOrientation)previousInterfaceOrientation
             traitCollection:(UITraitCollection*)previousTraitCollection {
    // (a fold / unfold, a rotation, a Stage Manager resize: SDL's windows follow the scene; SDL's view controller then
    // reports the new size: SDL_WINDOWEVENT_RESIZED)
    for (UIWindow* window in scene.windows)
        if (!CGRectEqualToRect(window.frame, scene.coordinateSpace.bounds)) window.frame = scene.coordinateSpace.bounds;
}
- (void)sceneDidDisconnect:(UIScene*)scene {
    if (scene == appScene) appScene = nil;
}
@end

@interface SHDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end
@implementation SHDocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url = urls.firstObject;
    std::string path, error;
    // An archive (.7z, .rar, .gz, ...) is not a disc image: the extractor reads .iso / .chd (and the PS3 import a
    // .zip). Say so instead of copying gigabytes into Documents/imports that nothing can read.
    NSString* extension = url.pathExtension.lowercaseString ?: @"";
    if (url && [@[ @"7z", @"rar", @"gz", @"tgz", @"xz", @"bz2", @"tar", @"cso", @"zso", @"ecm" ] containsObject:extension]) {
        error = std::string("This is a .") + extension.UTF8String +
                " archive, not a disc image. Unpack it first (for example with a 7-Zip app or on a computer), then pick the "
                ".iso (or .chd) inside it.";
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];  // (the picker's own copy in our tmp inbox)
        url = nil;
    }
    if (url) {
        // (asCopy: the system copied the file into our tmp inbox; move it to Documents/imports, the same volume)
        NSString* imports = ns(cane::ios::importsDirectory());
        [NSFileManager.defaultManager createDirectoryAtPath:imports withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* target = [imports stringByAppendingPathComponent:url.lastPathComponent];
        [NSFileManager.defaultManager removeItemAtPath:target error:nil];
        NSError* failure = nil;
        BOOL scoped = [url startAccessingSecurityScopedResource];
        if ([NSFileManager.defaultManager moveItemAtURL:url toURL:[NSURL fileURLWithPath:target] error:&failure] ||
            [NSFileManager.defaultManager copyItemAtURL:url toURL:[NSURL fileURLWithPath:target] error:&failure])
            path = target.UTF8String;
        else
            error = failure.localizedDescription.UTF8String ?: "The disc image could not be copied";
        if (scoped) [url stopAccessingSecurityScopedResource];
    }
    std::lock_guard lock(stateMutex);
    isoStatus = {};
    isoStatus.path = path;
    isoStatus.error = error;
    cane::ios::log("document picker: " + (path.empty() ? std::string("nothing (") + error + ")" : path));
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    std::lock_guard lock(stateMutex);
    isoStatus = {};
}
@end

namespace cane::ios {
void initialize(int argc, char** argv) {
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        documentsPath = [paths.firstObject UTF8String] ?: "";
        bundlePath = NSBundle.mainBundle.bundlePath.UTF8String ?: "";
        for (int i = 1; i < argc; ++i)
            if (!std::strcmp(argv[i], "--ios-smoke")) smoke = true;
        NSString* logs = [ns(documentsPath) stringByAppendingPathComponent:@"logs"];
        [NSFileManager.defaultManager createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
        logFile = std::fopen((std::string(logs.UTF8String) + "/ios.log").c_str(), "w");
        if (const int console = dup(STDERR_FILENO); console >= 0) {
            fcntl(console, F_SETFD, FD_CLOEXEC);
            consoleFd = console;
        }
        UIDevice* device = UIDevice.currentDevice;
        log(std::string("SafeHouse iOS: ") + device.model.UTF8String + ", " + device.systemName.UTF8String + " " +
            device.systemVersion.UTF8String + "; documents " + documentsPath);
    }
}
bool smokeTest() { return smoke; }
void log(const std::string& line) {
    std::lock_guard lock(logMutex);
    writeLine(consoleFd, line);
    if (logFile) {
        std::fprintf(logFile, "%s\n", line.c_str());
        std::fflush(logFile);
    }
}
std::string bundleDirectory() { return bundlePath; }
std::string documentsDirectory() { return documentsPath; }
std::string importsDirectory() { return documentsPath + "/imports"; }
std::string programPath(const std::string& name) {
    std::string framework = name;
    if (name == "Sly2-Release") framework = "Sly2Release";
    else if (name == "cane-extractor") framework = "CaneExtractor";
    else if (name == "safehouse" || name == "cane-launcher") framework = "SafeHouseLauncher";
    return bundlePath + "/Frameworks/" + framework + ".framework/" + framework;
}

ChildProcess startProcess(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {};
    int fds[2];
    if (pipe(fds) != 0) return {};
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    auto p = std::make_shared<Program>();
    p->arguments = arguments;
    p->path = arguments[0];
    p->image = baseName(p->path);
    if (p->image == "Sly2Release" && sly2Tool(arguments)) {
        p->image = "Sly2Tools";
        p->path = bundlePath + "/Frameworks/Sly2Tools.framework/Sly2Tools";
    }
    p->outputFd = fds[1];
    // (the game: UIKit wants its windows on the main thread; so does the renderer's self test, in Sly2Tools)
    p->mainThread = p->image == "Sly2Release" || std::find(arguments.begin(), arguments.end(), "--gl-selftest") != arguments.end();
    p->nested = redirectedHere != nullptr && !p->mainThread;
    std::int64_t handle;
    {
        std::lock_guard lock(stateMutex);
        handle = nextHandle++;
        programs[handle] = p;
        if (p->mainThread) mainQueue.push_back(p);
    }
    if (!p->mainThread) {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 16u << 20);  // (the desktop's main thread has 8 MB)
        pthread_t thread;
        auto* holder = new std::shared_ptr<Program>(p);
        if (pthread_create(&thread, &attributes, worker, holder) != 0) {
            delete holder;
            finish(p, 126);
        } else
            pthread_detach(thread);
        pthread_attr_destroy(&attributes);
    }
    return {-1, fds[0], handle};
}
bool programWaiting() {
    std::lock_guard lock(stateMutex);
    return !mainQueue.empty();
}
bool runPendingProgram() {
    std::shared_ptr<Program> p;
    {
        std::lock_guard lock(stateMutex);
        if (mainQueue.empty()) return false;
        p = mainQueue.front();
        mainQueue.pop_front();
    }
    int code = 1;
    @autoreleasepool {
        try {
            code = runProgram(*p);
        } catch (...) {
        }
    }
    finish(p, code);
    return true;
}
int runProgramNow(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return 1;
    Program p;
    p.arguments = arguments;
    p.path = arguments[0];
    p.image = baseName(p.path);
    p.mainThread = NSThread.isMainThread;
    return runProgram(p);
}
bool processRunning(std::int64_t handle) {
    std::lock_guard lock(stateMutex);
    auto it = programs.find(handle);
    return it != programs.end() && it->second->running;
}
int processExitCode(std::int64_t handle) {
    std::lock_guard lock(stateMutex);
    auto it = programs.find(handle);
    if (it == programs.end()) return 1;
    const int code = it->second->code;
    if (!it->second->running) programs.erase(it);
    return code;
}
void cancelProcess(std::int64_t handle) {
    log("cancel requested for program " + std::to_string(handle) + ": it finishes first (iOS runs it in the app)");
}

void openUrl(const std::string& url) {
    NSURL* target = [NSURL URLWithString:ns(url)];
    if (!target) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      [UIApplication.sharedApplication openURL:target options:@{} completionHandler:nil];
    });
}
void openFolder(const std::string& path) {
    // (the Files app at this folder of the app's Documents)
    NSString* encoded = [ns(path) stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
    openUrl(std::string("shareddocuments://") + (encoded.UTF8String ?: ""));
}

void requestIsoSelection() {
    {
        std::lock_guard lock(stateMutex);
        isoStatus = {};
        isoStatus.pending = true;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      static SHDocumentPickerDelegate* delegate = [SHDocumentPickerDelegate new];
      NSMutableArray<UTType*>* types = [NSMutableArray arrayWithObject:UTTypeData];
      for (NSString* extension in @[ @"iso", @"chd", @"bin" ])
          if (UTType* type = [UTType typeWithFilenameExtension:extension]) [types addObject:type];
      UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
      picker.delegate = delegate;
      picker.allowsMultipleSelection = NO;
      UIViewController* top = topViewController();
      if (!top) {
          std::lock_guard lock(stateMutex);
          isoStatus = {};
          isoStatus.error = "No window to show the file picker in";
          return;
      }
      [top presentViewController:picker animated:YES completion:nil];
    });
}
ImportStatus isoSelection() {
    std::lock_guard lock(stateMutex);
    return isoStatus;
}

void adoptWindow(void* uiWindow) {
    UIWindow* window = (__bridge UIWindow*)uiWindow;
    if (!window) return;
    // (SDL's main runs from the app delegate's launch; the scene may connect a moment later: wait for it a little)
    // (up to 5 s; only with a scene manifest in Info.plist)
    static const bool sceneManifest = [NSBundle.mainBundle objectForInfoDictionaryKey:@"UIApplicationSceneManifest"] != nil;
    for (int i = 0; sceneManifest && i < 250 && !appScene; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    UIWindowScene* scene = appScene;
    if (!scene) {
        static bool logged = false;
        if (!logged) log("iOS: no window scene (the app runs without the scene life cycle)");
        logged = true;
        return;
    }
    if (window.windowScene != scene) window.windowScene = scene;
    if (!CGRectEqualToRect(window.frame, scene.coordinateSpace.bounds)) window.frame = scene.coordinateSpace.bounds;
}
std::array<float, 4> safeAreaInsets(void* uiWindow) {
    UIWindow* window = (__bridge UIWindow*)uiWindow;
    if (!window) return {0, 0, 0, 0};
    UIView* view = window.rootViewController.view ?: window;
    const UIEdgeInsets i = view.safeAreaInsets;
    return {float(i.left), float(i.top), float(i.right), float(i.bottom)};
}
namespace {
UIScreen* screenOf(UIWindow* window) {
    if (window.windowScene) return window.windowScene.screen;
    return window.screen ?: UIScreen.mainScreen;
}
}  // namespace
const char* orientations(void* uiWindow, bool game) {
    static const char* const landscape = "LandscapeLeft LandscapeRight";
    static const char* const all = "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown";
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) return all;
    if (game) return landscape;
    UIWindow* window = (__bridge UIWindow*)uiWindow;
    UIScreen* screen = window ? screenOf(window) : UIScreen.mainScreen;
    // (nativeBounds: the panel itself, also when iOS runs the app in a smaller compatibility frame)
    const CGSize size = screen.nativeBounds.size;
    const CGFloat longSide = std::max(size.width, size.height), shortSide = std::min(size.width, size.height);
    return shortSide > 0 && longSide / shortSide < 1.5 ? all : landscape;
}
std::string displayReport(void* uiWindow, const char* orientations) {
    UIWindow* window = (__bridge UIWindow*)uiWindow;
    UIScreen* screen = window ? screenOf(window) : UIScreen.mainScreen;
    const CGRect bounds = screen.bounds, native = screen.nativeBounds;
    char text[512];
    std::snprintf(text, sizeof(text), "screen %.0fx%.0f pt (scale %.2f), panel %.0fx%.0f px (native scale %.2f)",
                  bounds.size.width, bounds.size.height, screen.scale, native.size.width, native.size.height,
                  screen.nativeScale);
    std::string line = text;
    if (window) {
        const CGRect frame = window.frame;
        const auto inset = safeAreaInsets(uiWindow);
        std::snprintf(text, sizeof(text), "; window %.0fx%.0f pt; safe area insets l%.0f t%.0f r%.0f b%.0f",
                      frame.size.width, frame.size.height, inset[0], inset[1], inset[2], inset[3]);
        line += text;
        if (UIWindowScene* scene = window.windowScene) {
            static const char* const names[] = {"unknown", "portrait", "portrait upside down", "landscape right", "landscape left"};
            const long o = long(scene.interfaceOrientation);
            line += std::string("; scene ") + (o >= 0 && o <= 4 ? names[o] : "?");
        }
        // (a window clearly smaller than the panel in points: iOS shows the app in a compatibility frame)
        const CGFloat panelW = native.size.width / std::max<CGFloat>(screen.nativeScale, 1),
                      panelH = native.size.height / std::max<CGFloat>(screen.nativeScale, 1);
        const CGFloat windowArea = frame.size.width * frame.size.height, panelArea = panelW * panelH;
        if (panelArea > 0 && windowArea < panelArea * 0.9) line += "; COMPATIBILITY FRAME (window smaller than the screen)";
    }
    line += std::string("; idiom ") + (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "pad" : "phone");
    line += std::string("; orientations ") + (orientations ? orientations : "?");
    return line;
}

void vibrate(int amplitude) {
    if (amplitude <= 0) return;
    static double last = 0;
    const double now = CFAbsoluteTimeGetCurrent();
    if (now - last < 0.09) return;  // (called every frame a motor runs: one tap per ~90 ms)
    last = now;
    const CGFloat intensity = std::min(1.0, amplitude / 255.0);
    dispatch_async(dispatch_get_main_queue(), ^{
      static UIImpactFeedbackGenerator* generator = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy];
      [generator impactOccurredWithIntensity:intensity];
    });
}
}  // namespace cane::ios
