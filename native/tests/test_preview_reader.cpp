#include "recovery/preview_reader.h"
#include "byteback_io.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace byteback;

namespace {

std::vector<uint8_t> minimalJpeg() {
    std::vector<uint8_t> jpeg;
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD8);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDB);
    jpeg.push_back(0x00);
    jpeg.push_back(0x03);
    jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDA);
    jpeg.push_back(0x00);
    jpeg.push_back(0x02);
    for (int i = 0; i < 20; ++i) jpeg.push_back(0x00);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

void appendAtom(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    const uint32_t sz = static_cast<uint32_t>(8 + payload.size());
    out.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(sz & 0xFF));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> minimalFtypPayload() {
    return {'i', 's', 'o', 'm', 0, 0, 0, 0, 'i', 's', 'o', 'm'};
}

} // namespace

TEST(PreviewReader, ResidentTextPreview) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "note.txt";
    rec.sizeBytes = 11;
    rec.residentData = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "text");
    EXPECT_EQ(preview.data.size(), 11u);
}

TEST(PreviewReader, ContiguousSectorImagePreview) {
    const auto jpeg = minimalJpeg();
    std::vector<uint8_t> img(512 * 4, 0);
    std::memcpy(img.data() + 512, jpeg.data(), jpeg.size());

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    FileRecord rec;
    rec.name = "photo.jpg";
    rec.sizeBytes = static_cast<int64_t>(jpeg.size());
    rec.startSector = 1;

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.data.size(), jpeg.size());
}

TEST(PreviewReader, MagicBeatsMisleadingBmpExtension) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "carved_1.bmp";
    rec.residentData = minimalJpeg();
    rec.sizeBytes = static_cast<int64_t>(rec.residentData.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.mime, "image/jpeg");
}

TEST(PreviewReader, FakeBmpHeaderIsBinary) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "junk.bmp";
    rec.residentData = {'B', 'M', 0x01, 0x00, 0x00, 0x00}; // too short / invalid
    rec.sizeBytes = 6;

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "binary");
    EXPECT_TRUE(preview.mime.empty());
}

TEST(PreviewReader, UnsniffableImageExtIsBinary) {
    DiskReader reader;
    FileRecord rec;
    rec.name = "photo.heic";
    rec.residentData = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    rec.sizeBytes = 8;

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "binary");
    EXPECT_TRUE(preview.mime.empty());
}

TEST(PreviewReader, PrefixCapAt64KiB) {
    std::vector<uint8_t> payload(kPreviewMaxBytes + 1024, 0x41);
    DiskReader reader;
    FileRecord rec;
    rec.name = "big.bin";
    rec.sizeBytes = static_cast<int64_t>(payload.size());
    rec.residentData = std::move(payload);

    std::vector<uint8_t> out;
    ASSERT_TRUE(readRecordPrefix(reader, rec, out, kPreviewMaxBytes));
    EXPECT_EQ(out.size(), kPreviewMaxBytes);
}

TEST(PreviewReader, VideoEmbeddedJpegFrame) {
    const auto jpeg = minimalJpeg();
    std::vector<uint8_t> avi(512, 0);
    avi[0] = 'R';
    avi[1] = 'I';
    avi[2] = 'F';
    avi[3] = 'F';
    avi[8] = 'A';
    avi[9] = 'V';
    avi[10] = 'I';
    avi[11] = ' ';
    std::memcpy(avi.data() + 64, jpeg.data(), jpeg.size());

    DiskReader reader;
    FileRecord rec;
    rec.name = "clip.avi";
    rec.category = "Video";
    rec.residentData = avi;
    rec.sizeBytes = static_cast<int64_t>(avi.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.mime, "image/jpeg");
    EXPECT_GE(preview.data.size(), jpeg.size());
}

TEST(PreviewReader, Mp4CoverArt) {
    const auto jpeg = minimalJpeg();
    std::vector<uint8_t> dataPayload(8, 0);
    dataPayload.push_back(0);
    dataPayload.push_back(0);
    dataPayload.push_back(0);
    dataPayload.push_back(13);
    dataPayload.insert(dataPayload.end(), jpeg.begin(), jpeg.end());

    std::vector<uint8_t> ilst;
    appendAtom(ilst, "data", dataPayload);
    std::vector<uint8_t> moov;
    appendAtom(moov, "ilst", ilst);
    std::vector<uint8_t> file;
    appendAtom(file, "ftyp", minimalFtypPayload());
    appendAtom(file, "moov", moov);
    appendAtom(file, "mdat", {0x00});

    DiskReader reader;
    FileRecord rec;
    rec.name = "clip.mp4";
    rec.category = "Video";
    rec.residentData = file;
    rec.sizeBytes = static_cast<int64_t>(file.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.mime, "image/jpeg");
}

TEST(PreviewReader, Mp4ClassifiesBinaryNotText) {
    std::vector<uint8_t> file;
    appendAtom(file, "ftyp", minimalFtypPayload());
    appendAtom(file, "mdat", {0x00});

    DiskReader reader;
    FileRecord rec;
    rec.name = "clip.mp4";
    rec.category = "Video";
    rec.residentData = file;
    rec.sizeBytes = static_cast<int64_t>(file.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "binary");
}

TEST(PreviewReader, Mp4MdatIdrHint) {
    const uint8_t sps[] = {0x00, 0x00, 0x00, 0x07, 0x67, 0x42, 0x00, 0x0A,
                           0xF8, 0x41, 0xA2};
    const uint8_t idr[] = {0x00, 0x00, 0x00, 0x05, 0x65, 0x88, 0x84, 0x00, 0x10};
    std::vector<uint8_t> mdatPayload;
    mdatPayload.insert(mdatPayload.end(), sps, sps + sizeof(sps));
    mdatPayload.insert(mdatPayload.end(), idr, idr + sizeof(idr));

    std::vector<uint8_t> file;
    appendAtom(file, "ftyp", minimalFtypPayload());
    appendAtom(file, "mdat", mdatPayload);

    DiskReader reader;
    FileRecord rec;
    rec.name = "clip.mp4";
    rec.category = "Video";
    rec.residentData = file;
    rec.sizeBytes = static_cast<int64_t>(file.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "binary");
    EXPECT_FALSE(preview.note.empty());
    EXPECT_NE(preview.note.find("IDR"), std::string::npos) << preview.note;
    EXPECT_NE(preview.note.find("decode yok"), std::string::npos);
}

TEST(PreviewReader, MkvEmbeddedJpegFrame) {
    const auto jpeg = minimalJpeg();
    std::vector<uint8_t> mkv(512, 0);
    mkv[0] = 0x1a;
    mkv[1] = 0x45;
    mkv[2] = 0xdf;
    mkv[3] = 0xa3;
    std::memcpy(mkv.data() + 128, jpeg.data(), jpeg.size());

    DiskReader reader;
    FileRecord rec;
    rec.name = "clip.mkv";
    rec.category = "Video";
    rec.residentData = mkv;
    rec.sizeBytes = static_cast<int64_t>(mkv.size());

    FilePreviewResult preview = readFilePreview(reader, rec);
    EXPECT_TRUE(preview.success) << preview.error;
    EXPECT_EQ(preview.kind, "image");
    EXPECT_EQ(preview.mime, "image/jpeg");
}
