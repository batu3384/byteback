// CA-001/CA-002 hygiene: the signature set must not contain text-magic
// garbage generators, duplicate anchors, or dead entries. These are the
// regressions that flooded results with phantom "files".
#include "byteback_carver.h"
#include "byteback_io.h"
#include <gtest/gtest.h>
#include <atomic>
#include <set>
#include <string>
#include <vector>

using namespace byteback;

static std::vector<FileSignature> loadAll() {
    CarvingEngine carver;
    EXPECT_TRUE(carver.loadSignatures(""));
    return carver.getSignatures();
}

TEST(SignatureHygiene, NoTextMagics) {
    // Magics that match ordinary text/config files and opened maxSize-sized
    // phantom carves. None may ever return.
    const std::vector<std::string> forbidden = {
        "From ", "---\n", "# ", "elif", ":status", "import ", "package ",
        "Received:", "<svg", "#!AMR", "Microsoft", " Manager", "REDIS",
        "<html", "<?xml", "\\document", "From:", "#!/",
    };
    for (const auto& sig : loadAll()) {
        for (const auto& f : forbidden) {
            ASSERT_FALSE(sig.header.size() == f.size() &&
                         std::equal(f.begin(), f.end(), sig.header.begin()))
                << "text magic returned: " << sig.format << " (" << f << ")";
        }
    }
}

TEST(SignatureHygiene, NoMicroHeaders) {
    // 1-2 byte headers match everywhere. BMP is the one deliberate exception:
    // its weak magic is guarded by validateBmp at emit and expire paths.
    for (const auto& sig : loadAll()) {
        if (sig.extension == ".bmp") continue;
        ASSERT_GE(sig.header.size(), 3u) << "micro header: " << sig.format;
    }
}

TEST(SignatureHygiene, NoDuplicateHeaders) {
    std::set<std::string> seen;
    for (const auto& sig : loadAll()) {
        std::string hex;
        for (uint8_t b : sig.header) hex += "0123456789abcdef"[b >> 4], hex += "0123456789abcdef"[b & 15];
        ASSERT_TRUE(seen.insert(hex).second) << "duplicate header: " << sig.format << " (" << hex << ")";
    }
}

TEST(SignatureHygiene, NoDeadSignatures) {
    for (const auto& sig : loadAll()) {
        ASSERT_FALSE(sig.header.empty()) << "empty header (never matches): " << sig.format;
        ASSERT_GT(sig.maxSize, 0u) << "maxSize 0 (can never emit): " << sig.format;
    }
}

TEST(SignatureHygiene, TextDiskYieldsNoCarves) {
    // A text/config-heavy region: exactly the content that used to flood
    // results with phantom Python/YAML/EML "recoveries".
    std::string text;
    for (int i = 0; i < 400; ++i) {
        text += "---\ntitle: note\nimport os\npackage main\nelif x:\nFrom: a@b.c\n# heading\n";
    }
    std::vector<uint8_t> disk(text.begin(), text.end());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(disk));

    CarvingEngine carver;
    ASSERT_TRUE(carver.loadSignatures(""));

    std::atomic<bool> running{true};
    int records = 0;
    ASSERT_TRUE(carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id != -1) ++records;
    }, &running));
    EXPECT_EQ(records, 0);
}
