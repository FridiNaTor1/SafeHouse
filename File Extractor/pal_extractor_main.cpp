// Sly1PalExtractor entry point (replaces upstream's NativeTools/PalExtractor/main.cpp; the other PalExtractor
// sources are shared with the Switch installer). Usage: Sly1PalExtractor extract <iso> <output-folder> [pal|japanese|korean]
// - pal: the image is read once (upstream's ExtractDetectedRegion reads it, then ExtractAllLanguages reads the
//   whole 2.9 GB image a second time).
// - korean: SCKA_200.04, which upstream does not support (PalIsoExtractor::ExtractKorean, shared with the Switch
//   installer). Levels go to <Level>/Korean/ like a one-language PAL package, and .pal-level-identities.txt maps
//   each descriptor identity to its level for the rebuilder.
//
// PalIsoExtractor.cpp is compiled here (not separately).
#include "PalIsoExtractor.cpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
using namespace cane::pal;

template <typename Character>
int Run(int argc, Character** argv) {
    try {
        if ((argc != 4 && argc != 5) || std::filesystem::path(argv[1]).string() != "extract") {
            std::cerr << "Usage: Sly1PalExtractor extract <pal-japanese-or-korean.iso> <output-folder> [pal|japanese|korean]\n";
            return 2;
        }
        const std::filesystem::path iso(argv[2]), output(argv[3]);
        const std::string region = argc == 5 ? std::filesystem::path(argv[4]).string() : std::string();
        if (region == "pal")
            PalIsoExtractor{}.ExtractAllLanguages(iso, output);
        else if (region == "korean")
            PalIsoExtractor{}.ExtractKorean(iso, output);
        else if (region.empty() || region == "japanese")
            PalIsoExtractor{}.ExtractDetectedRegion(iso, output);
        else {
            std::cerr << "region must be pal, japanese or korean\n";
            return 2;
        }
        std::cout << "Extraction completed successfully.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Failed: " << exception.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return Run(argc, argv); }
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
