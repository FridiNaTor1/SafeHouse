// Sly1PalExtractor entry point (replaces upstream's NativeTools/PalExtractor/main.cpp; the other PalExtractor
// sources are used verbatim). Usage: Sly1PalExtractor extract <iso> <output-folder> [pal|japanese|korean]
// - pal: the image is read once (upstream's ExtractDetectedRegion reads it, then ExtractAllLanguages reads the
//   whole 2.9 GB image a second time).
// - korean: SCKA_200.04, which upstream does not support. Its executable keeps the USA's single-slot 44-byte
//   level records (at 0x249C70) and sound-bank table (0x2470C0), its levels are the SCPS format (two UTF-16
//   string tables, descriptors in the level data), and its cutscene table is PAL's (0x24A570, same
//   identities, one special descriptor before it). Levels go to <Level>/Korean/ like a one-language PAL
//   package, and .pal-level-identities.txt maps each descriptor identity to its level for the rebuilder.
//
// PalIsoExtractor.cpp is compiled here (not separately) so the Korean path can reuse its file-local helpers.
#include "PalIsoExtractor.cpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
constexpr std::uint32_t kKoreanLevelTableVa = 0x00249C70;
constexpr std::uint32_t kKoreanSoundBankTableVa = 0x002470C0;
constexpr std::uint32_t kKoreanCutsceneTableVa = 0x0024A570;
constexpr std::size_t kKoreanRecordSize = 44;

void ExtractKorean(const std::filesystem::path& isoPath, const std::filesystem::path& outputDirectory) {
    const auto iso = ReadFile(isoPath);
    const auto files = RootFiles(iso);
    const auto elf = std::find_if(files.begin(), files.end(),
        [](const IsoFile& file) { return file.name == "SCKA_200.04"; });
    if (elf == files.end()) throw std::runtime_error("Korean executable SCKA_200.04 was not found");
    auto descriptor = [&](std::size_t at) {
        return std::array<std::uint32_t, 3>{U32(iso, at) ^ U32(iso, at + 20), U32(iso, at + 4) ^ U32(iso, at + 28),
                                            U32(iso, at + 8) ^ U32(iso, at + 24)};
    };

    // Sound banks: the USA's encrypted table, in the USA's order.
    static constexpr std::array<std::string_view, 7> bankNames = {
        "Common.bnk", "Intro.bnk", "Underwater.bnk", "Muggshot.bnk", "Voodoo.bnk", "Snow.bnk", "Clockwerk.bnk"};
    std::array<SoundBankLocation, 7> banks{};
    const auto bankTable = VaToIsoOffset(iso, *elf, kKoreanSoundBankTableVa);
    for (std::size_t i = 0; i < banks.size(); ++i)
        banks[i] = {bankNames[i], descriptor(bankTable + i * kDescriptorSize)[0]};
    ExtractSoundBanks(iso, outputDirectory / "Worlds" / "Sound Banks", banks, "Korean");

    static constexpr std::array<std::string_view, 1> languages = {"Korean"};
    ExtractCutscenes(iso, *elf, outputDirectory / "Cutscenes", kKoreanCutsceneTableVa, languages, "Korean", 1);

    const auto table = VaToIsoOffset(iso, *elf, kKoreanLevelTableVa);
    if (table + kLevelCount * kKoreanRecordSize > iso.size())
        throw std::runtime_error("Korean level table lies outside the executable");
    {
        std::filesystem::create_directories(outputDirectory);
        std::ofstream map(outputDirectory / ".pal-level-identities.txt", std::ios::trunc);
        for (std::size_t i = 0; i < kLevelCount; ++i)
            map << descriptor(table + i * kKoreanRecordSize)[2] << ' '
                << std::quoted(std::string(kLevelNames[i]) + ".brx") << '\n';
        if (!map) throw std::runtime_error("Could not write the Korean level identity map");
    }
    const auto worldsDirectory = outputDirectory / "Worlds";
    for (std::size_t i = 0; i < kLevelCount; ++i) {
        const auto record = table + i * kKoreanRecordSize;
        const auto [sector, compressedSize, identity] = descriptor(record);
        const auto levelId = U32(iso, record + 32);
        const auto source = static_cast<std::size_t>(sector) * kSectorSize;
        if (compressedSize == 0 || compressedSize > kMaximumCompressedSize || source > iso.size() ||
            compressedSize > iso.size() - source)
            throw std::runtime_error("Invalid Korean descriptor at table index " + std::to_string(i));
        const auto decoded = Decompress(std::span<const std::byte>(iso).subspan(source, compressedSize));
        const auto levelDirectory = LevelDirectory(worldsDirectory, i) / "Korean";
        std::filesystem::create_directories(levelDirectory);
        const auto path = levelDirectory / (std::string(kLevelNames[i]) + ".brx");
        std::ofstream output(path, std::ios::binary);
        if (!output || !output.write(reinterpret_cast<const char*>(decoded.data()),
                static_cast<std::streamsize>(decoded.size())))
            throw std::runtime_error("Could not write " + path.string());
        output.close();
        // The level data is the SCPS layout.
        BrxWalker walker(StringEncoding::Auto, BrxRegion::Japanese);
        walker.WalkFile(path);
        ExtractAudio(iso, levelDirectory / "Dialogs", walker.AudioDescriptors());
        ExtractAudio(iso, levelDirectory / "Commentary", walker.CommentaryDescriptors());
        ExtractMusic(iso, worldsDirectory / "Music", walker.MusicDescriptors());
        std::cout << "[Korean " << (i + 1) << '/' << kLevelCount << "] id=0x" << std::hex << levelId
                  << " file=0x" << identity << std::dec << ' ' << path.string() << '\n';
    }
}

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
            ExtractKorean(iso, output);
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
