// The iOS app's executable (cmake/ios.cmake): SDL's UIKit application, then the SafeHouse launcher's framework on
// the main thread. Everything else is in the frameworks (ios_app.h).
//
//   --ios-smoke      (CI) the launcher saves a screenshot of its 300th frame (Documents/smoke-launcher.png) and quits
//   --ios-selftest   (CI) Sly 2's self tests (no game data) in its tools image, then quits; Documents/selftest.log
//   --ios-extract-smoke  (CI) the extractor as the launcher runs it (worker thread, output pipe): --help, a missing
//                    and a damaged disc image must each end with their reason in the output and in the app log;
//                    Sly 2's converter must be found where the extractor looks. Documents/extract-smoke.log
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_main.h>

#include "ios_app.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
int selfTests() {
    const std::string tools = cane::ios::bundleDirectory() + "/Frameworks/Sly2Tools.framework/Sly2Tools";
    int failures = 0;
    for (const char* test : {"--rumble-selftest", "--settings-selftest", "--renderer-selftest", "--action-input-selftest", "--language-selftest"}) {
        const int code = cane::ios::runProgramNow({tools, test});
        cane::ios::log(std::string("selftest ") + test + ": " + (code == 0 ? "passed" : "FAILED (" + std::to_string(code) + ")"));
        if (code != 0) ++failures;
    }
    if (FILE* f = std::fopen((cane::ios::documentsDirectory() + "/selftest.log").c_str(), "w")) {
        std::fprintf(f, "%d failed\n", failures);
        std::fclose(f);
    }
    return failures ? 1 : 0;
}

// Runs a program as platform/process.h does on iOS (startProcess: its own thread, output through a pipe); its output
// and exit code.
int runPiped(const std::vector<std::string>& arguments, std::string& output) {
    const auto child = cane::ios::startProcess(arguments);
    if (child.outputFd < 0) return -1;
    char buffer[4096];
    for (ssize_t n; (n = read(child.outputFd, buffer, sizeof(buffer))) != 0;) {
        if (n > 0) output.append(buffer, size_t(n));
        else if (errno != EINTR) break;
    }
    close(child.outputFd);
    while (cane::ios::processRunning(child.handle)) usleep(20000);
    return cane::ios::processExitCode(child.handle);
}
int extractSmoke() {
    const std::string documents = cane::ios::documentsDirectory(), extractor = cane::ios::programPath("cane-extractor");
    std::string report;
    int failures = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail) {
        report += std::string(ok ? "ok    " : "FAIL  ") + name + "\n" + detail + "\n";
        cane::ios::log(std::string("extract smoke ") + name + ": " + (ok ? "ok" : "FAILED"));
        if (!ok) ++failures;
    };
    std::string out;
    int code = runPiped({extractor, "--help"}, out);
    check("--help", code == 0 && out.find("--iso PATH") != std::string::npos, "exit " + std::to_string(code) + ": " + out);
    out.clear();
    code = runPiped({extractor, "--iso", documents + "/no such disc.iso", "--output", documents + "/smoke-install", "--non-interactive",
                     "--progress=jsonl"},
                    out);
    check("missing disc image", code == 1 && out.find("ISO does not exist") != std::string::npos, "exit " + std::to_string(code) + ": " + out);
    // (a damaged image: 4 MB of zeros, no ISO9660 volume)
    const std::string damaged = documents + "/smoke-damaged.iso";
    {
        std::ofstream file(damaged, std::ios::binary);
        const std::vector<char> zeros(4u << 20);
        file.write(zeros.data(), std::streamsize(zeros.size()));
    }
    out.clear();
    code = runPiped({extractor, "--game", "sly2", "--iso", damaged, "--output", documents + "/smoke-install", "--non-interactive",
                     "--progress=jsonl", "--languages-dir", documents + "/smoke-languages"},
                    out);
    check("damaged disc image", code == 1 && out.size() > 20, "exit " + std::to_string(code) + ": " + out);
    std::remove(damaged.c_str());
    // (the converter the extractor runs for Sly 2: File Extractor/pcbrx.h Tool -> programPath)
    struct stat info {};
    const std::string converter = cane::ios::programPath("Sly2-Release");
    check("Sly 2 converter present", stat(converter.c_str(), &info) == 0 && S_ISREG(info.st_mode), converter);
    // (the app log has the extractor's own lines, not only its exit code)
    std::ifstream logFile(documents + "/logs/ios.log");
    const std::string log((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    check("extractor output in ios.log", log.find("CaneExtractor: ISO does not exist") != std::string::npos, "");
    if (FILE* f = std::fopen((documents + "/extract-smoke.log").c_str(), "w")) {
        std::fprintf(f, "%d failed\n%s", failures, report.c_str());
        std::fclose(f);
    }
    return failures ? 1 : 0;
}

int appMain(int argc, char** argv) {
    cane::ios::initialize(argc, argv);
    std::vector<std::string> arguments{cane::ios::programPath("safehouse")};
    bool selftest = false, extract = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ios-selftest")) selftest = true;
        else if (!std::strcmp(argv[i], "--ios-extract-smoke")) extract = true;
        else if (std::strncmp(argv[i], "--ios-", 6) != 0 && std::strncmp(argv[i], "-NS", 3) != 0 && std::strncmp(argv[i], "-Apple", 6) != 0)
            arguments.push_back(argv[i]);
    }
    const int code = selftest ? selfTests() : extract ? extractSmoke() : cane::ios::runProgramNow(arguments);
    cane::ios::log("SafeHouse leaves with " + std::to_string(code));
    // (iOS apps normally never quit; the launcher's Quit and the CI runs end the process here)
    std::exit(code);
}
}  // namespace

int main(int argc, char** argv) {
    return SDL_UIKitRunApp(argc, argv, appMain);
}
