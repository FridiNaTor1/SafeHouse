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
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {
std::mutex stateMutex;
std::string documentsPath, bundlePath;
bool smoke = false;
FILE* logFile = nullptr;
std::mutex logMutex;

struct Program {
    std::string path, image;
    std::vector<std::string> arguments;
    int outputFd = -1;  // the write end of the caller's pipe (-1: the app's own output)
    bool mainThread = false;
    std::atomic<bool> running{true};
    std::atomic<int> code{1};
};
std::map<std::int64_t, std::shared_ptr<Program>> programs;
std::deque<std::shared_ptr<Program>> mainQueue;
std::set<std::string> imagesRun;  // (program images that already ran their main in this app session)
std::int64_t nextHandle = 1;
std::mutex outputMutex;  // one program's output redirection at a time

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

    std::unique_lock output(outputMutex, std::defer_lock);
    int savedOut = -1, savedErr = -1;
    if (p.outputFd >= 0) {
        output.lock();
        std::fflush(nullptr);
        savedOut = dup(STDOUT_FILENO);
        savedErr = dup(STDERR_FILENO);
        dup2(p.outputFd, STDOUT_FILENO);
        dup2(p.outputFd, STDERR_FILENO);
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    const int code = entry(int(arguments.size()), argv.data());
    if (p.outputFd >= 0) {
        std::fflush(nullptr);
        dup2(savedOut, STDOUT_FILENO);
        dup2(savedErr, STDERR_FILENO);
        close(savedOut);
        close(savedErr);
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
}  // namespace

@interface SHDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end
@implementation SHDocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url = urls.firstObject;
    std::string path, error;
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
        UIDevice* device = UIDevice.currentDevice;
        log(std::string("SafeHouse iOS: ") + device.model.UTF8String + ", " + device.systemName.UTF8String + " " +
            device.systemVersion.UTF8String + "; documents " + documentsPath);
    }
}
bool smokeTest() { return smoke; }
void log(const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
    std::lock_guard lock(logMutex);
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
    p->mainThread = p->image == "Sly2Release";  // (the game: UIKit wants its windows on the main thread)
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
