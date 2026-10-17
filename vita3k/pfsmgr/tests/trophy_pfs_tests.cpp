#include <pfsmgr/functions.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
class TestDirectory {
public:
    TestDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("vita3k-pfsmgr-test-" + std::to_string(suffix));
        fs::create_directories(path_);
    }

    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path &path() const { return path_; }

private:
    fs::path path_;
};

static void write_file(const fs::path &path, const std::vector<uint8_t> &data) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(output);
}
} // namespace

TEST(TrophyPfs, MigratesPlainDataAndPersistsEdits) {
    TestDirectory temporary;
    const auto root = temporary.path() / "NPWR00000_00";
    fs::create_directories(root / "assets");
    const std::vector<uint8_t> progress{ 0x10, 0x20, 0x30, 0x40 };
    const std::vector<uint8_t> icon{ 0x89, 0x50, 0x4e, 0x47 };
    write_file(root / "TROPUSR.DAT", progress);
    write_file(root / "assets" / "TROPHY.PNG", icon);

    ASSERT_TRUE(pfsmgr::migrate_appmeta(root));
    EXPECT_TRUE(fs::is_regular_file(root / "sce_pfs" / "files.db"));
    {
        const pfsmgr::Layer layer(root);
        EXPECT_EQ(layer.read("TROPUSR.DAT"), progress);
        EXPECT_EQ(layer.read(fs::path("assets") / "TROPHY.PNG"), icon);
    }

    const std::array<uint8_t, 1> changed{ 0x7f };
    {
        pfsmgr::PfsSession session(root);
        session.write("TROPUSR.DAT", 1, changed, false, false);
        const auto updated = session.read("TROPUSR.DAT");
        ASSERT_TRUE(updated);
        EXPECT_EQ((*updated)[1], changed[0]);
    }
    const pfsmgr::Layer refreshed(root);
    const auto updated = refreshed.read("TROPUSR.DAT");
    ASSERT_TRUE(updated);
    EXPECT_EQ((*updated)[1], changed[0]);
}

TEST(TrophyPfs, CreatesLayerForMissingDataDirectory) {
    TestDirectory temporary;
    const auto root = temporary.path() / "NPWR00001_00";

    ASSERT_TRUE(pfsmgr::migrate_appmeta(root));
    EXPECT_TRUE(fs::is_regular_file(root / "sce_pfs" / "files.db"));
    pfsmgr::PfsSession session(root);
    const std::array<uint8_t, 3> progress{ 1, 2, 3 };
    session.write("TROPUSR.DAT", 0, progress, true, false, progress.size());
    EXPECT_EQ(session.read("TROPUSR.DAT"), std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{ 1, 2, 3 }));
}

TEST(TrophyPfsCorpus, ExtractsAndRoundTripsDumpedTrophyLayers) {
    const char *corpus_path = std::getenv("VITA3K_TROPHY_CORPUS");
    if (!corpus_path || !*corpus_path)
        GTEST_SKIP() << "Set VITA3K_TROPHY_CORPUS to the extracted Trophy dump directory";

    const fs::path corpus(corpus_path);
    ASSERT_TRUE(fs::is_directory(corpus));
    TestDirectory temporary;
    fs::path sample_root;
    size_t extracted = 0;

    for (const auto &set : fs::directory_iterator(corpus)) {
        if (!set.is_directory() || !set.path().filename().string().starts_with("Set "))
            continue;
        const auto data_root = set.path() / "data";
        if (!fs::is_directory(data_root))
            continue;
        for (const auto &title : fs::directory_iterator(data_root)) {
            if (!title.is_directory())
                continue;
            const pfsmgr::Layer layer(title.path());
            const auto title_data = layer.read("TRPTITLE.DAT");
            const auto translation_data = layer.read("TRPTRANS.DAT");
            ASSERT_TRUE(title_data) << title.path().string();
            ASSERT_TRUE(translation_data) << title.path().string();
            ASSERT_FALSE(title_data->empty()) << title.path().string();
            ASSERT_FALSE(translation_data->empty()) << title.path().string();
            if (sample_root.empty())
                sample_root = title.path();
            ++extracted;
        }
    }
    ASSERT_GT(extracted, 0);

    const auto copy = temporary.path() / "trophy-copy";
    fs::copy(sample_root, copy, fs::copy_options::recursive);
    const pfsmgr::Layer original(sample_root);
    const auto original_data = original.read("TRPTRANS.DAT");
    ASSERT_TRUE(original_data);
    const std::array<uint8_t, 1> changed{ static_cast<uint8_t>((*original_data)[0] ^ 0xff) };
    {
        pfsmgr::PfsSession session(copy);
        session.write("TRPTRANS.DAT", 0, changed, false, false);
        const auto updated = session.read("TRPTRANS.DAT");
        ASSERT_TRUE(updated);
        EXPECT_EQ((*updated)[0], changed[0]);
    }
    const pfsmgr::Layer refreshed(copy);
    const auto updated = refreshed.read("TRPTRANS.DAT");
    ASSERT_TRUE(updated);
    EXPECT_EQ((*updated)[0], changed[0]);
    EXPECT_EQ(pfsmgr::Layer(sample_root).read("TRPTRANS.DAT"), original_data);
    SUCCEED() << "Extracted " << extracted << " Trophy PFS directories";
}
