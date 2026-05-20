#include <drogon/drogon_test.h>
#include "../helpers/ImageProcessor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Minimal valid PNG: 1x1 transparent pixel. Used because libvips can decode
// it without us needing external test fixtures.
const std::vector<uint8_t> kOnePxPng = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0D, 'I', 'D', 'A', 'T',
    0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05,
    0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00,
    0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82,
};

std::string tmpPath(const std::string& tag)
{
    auto base = fs::temp_directory_path();
    return (base / ("blog_img_test_" + tag)).string();
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

struct VipsBootstrap {
    VipsBootstrap()  { image::initLibrary(); }
    ~VipsBootstrap() { /* don't shutdown — other tests share the lib */ }
};
VipsBootstrap g_vipsBoot;

} // namespace

DROGON_TEST(Image_RejectsSvg)
{
    // SVG markup. Magic-byte sniffer must reject — we don't trust libvips with
    // arbitrary scriptable formats. Status 415 (unsupported media type).
    const std::string src = tmpPath("evil.svg");
    const std::string out = tmpPath("evil.jpg");
    std::ofstream(src) << R"(<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>)";

    auto r = image::processAvatar(src, out);
    CHECK(r.ok == false);
    CHECK(r.status == 415);
    CHECK(fs::exists(out) == false);
    fs::remove(src);
}

DROGON_TEST(Image_RejectsRandomBinary)
{
    const std::string src = tmpPath("blob.bin");
    const std::string out = tmpPath("blob.jpg");
    std::ofstream(src, std::ios::binary) << "ABCDEF\x00garbage\x01";

    auto r = image::processAvatar(src, out);
    CHECK(r.ok == false);
    CHECK(r.status == 415);
    CHECK(fs::exists(out) == false);
    fs::remove(src);
}

DROGON_TEST(Image_AcceptsValidPng_ProducesStrippedJpeg)
{
    const std::string src = tmpPath("ok.png");
    const std::string out = tmpPath("ok.jpg");
    writeBytes(src, kOnePxPng);

    auto r = image::processAvatar(src, out);
    REQUIRE(r.ok == true);
    REQUIRE(fs::exists(out));
    REQUIRE(fs::file_size(out) > 0);

    // Output must start with JPEG SOI marker FF D8 FF and contain no EXIF
    // APP1 marker (FF E1) — strip:true was honoured. We also disallow
    // APP13/Photoshop and any XMP packets.
    std::ifstream f(out, std::ios::binary);
    std::vector<unsigned char> bytes(
        std::istreambuf_iterator<char>(f), {});
    REQUIRE(bytes.size() >= 4u);
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[1] == 0xD8);
    CHECK(bytes[2] == 0xFF);

    bool hasExif = false, hasXmp = false;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xE1) {
            hasExif = true;
            // Look for "http://ns.adobe.com/xap/1.0/" inside an APP1
            // segment (XMP packets travel in APP1).
            if (i + 30 < bytes.size()) {
                const auto* p = reinterpret_cast<const char*>(&bytes[i + 4]);
                if (std::string(p, 28).find("ns.adobe.com/xap") != std::string::npos)
                    hasXmp = true;
            }
        }
    }
    CHECK(hasExif == false);
    CHECK(hasXmp  == false);

    fs::remove(src);
    fs::remove(out);
}
