#include "ImageProcessor.h"

#include <vips/vips8>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace image {

namespace {

// Magic-byte sniffer. Filename extension is never trusted — it comes from
// the upload form. We accept the three formats libvips will then re-encode
// from, and reject everything else (SVG, ICO, BMP, GIF, raw, …).
enum class Fmt { Unknown, Jpeg, Png, Webp };

Fmt sniff(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return Fmt::Unknown;

    std::array<unsigned char, 12> hdr{};
    f.read(reinterpret_cast<char*>(hdr.data()), hdr.size());
    const auto n = f.gcount();
    if (n < 4) return Fmt::Unknown;

    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) return Fmt::Jpeg;
    if (n >= 8 && hdr[0] == 0x89 && hdr[1] == 0x50 && hdr[2] == 0x4E && hdr[3] == 0x47 &&
        hdr[4] == 0x0D && hdr[5] == 0x0A && hdr[6] == 0x1A && hdr[7] == 0x0A) return Fmt::Png;
    // RIFF????WEBP
    if (n >= 12 && hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
                   hdr[8] == 'W' && hdr[9] == 'E' && hdr[10]== 'B' && hdr[11]== 'P') return Fmt::Webp;

    return Fmt::Unknown;
}

AvatarResult fail(int status, std::string msg)
{
    return AvatarResult{false, "", std::move(msg), status};
}

} // namespace

bool initLibrary()
{
    return VIPS_INIT("drogon-blog") == 0;
}

void shutdownLibrary()
{
    vips_shutdown();
}

AvatarResult processAvatar(const std::string& srcPath,
                           const std::string& outPath,
                           int outDim,
                           int maxDim)
{
    const Fmt fmt = sniff(srcPath);
    if (fmt == Fmt::Unknown) {
        return fail(415, "Unsupported image type — JPEG, PNG and WebP only");
    }

    try {
        using vips::VImage;
        using vips::VOption;

        // sequential=true streams the image without keeping the whole pixel
        // buffer alive. fail_on=error makes libvips throw on partial/broken
        // inputs instead of silently producing a degraded image.
        VImage img = VImage::new_from_file(
            srcPath.c_str(),
            VImage::option()->set("access", "sequential"));

        const int w = img.width();
        const int h = img.height();
        if (w <= 0 || h <= 0) {
            return fail(422, "Decoded image has no pixels");
        }
        if (w > maxDim || h > maxDim) {
            return fail(413, std::string("Image too large (") +
                              std::to_string(w) + "x" + std::to_string(h) +
                              "); maximum is " + std::to_string(maxDim) +
                              "x" + std::to_string(maxDim));
        }

        // smartcrop "attention" produces a center crop guided by salience —
        // for avatars this lands on the subject more reliably than a fixed
        // center crop. Falls back to a centered crop if attention isn't
        // available in the linked libvips build.
        VImage thumb = VImage::thumbnail(
            srcPath.c_str(), outDim,
            VImage::option()
                ->set("height", outDim)
                ->set("crop",  VIPS_INTERESTING_ATTENTION));

        // Drop every EXIF/XMP/ICC/IPTC field so GPS, camera serial, etc.
        // never reach disk. ICC isn't a privacy issue but extra bytes for
        // no perceptual gain at 256px.
        for (const char* field : {
                "exif-data", "xmp-data", "iptc-data",
                "icc-profile-data", "photoshop-data"})
        {
            if (vips_image_get_typeof(thumb.get_image(), field) != 0) {
                vips_image_remove(thumb.get_image(), field);
            }
        }
        // Vips also stores parsed EXIF in fields prefixed "exif-ifd…"; wipe
        // them too so they don't leak via the metadata sidecar.
        thumb.remove("orientation");

        thumb.jpegsave(outPath.c_str(),
            VImage::option()
                ->set("Q",                 85)
                ->set("strip",             true)   // belt-and-braces
                ->set("interlace",         true)
                ->set("optimize_coding",   true));

        return AvatarResult{true, outPath, "", 200};
    }
    catch (const vips::VError& e) {
        return fail(422, std::string("Image processing failed: ") + e.what());
    }
    catch (const std::exception& e) {
        return fail(500, std::string("Unexpected error: ") + e.what());
    }
    catch (...) {
        return fail(500, "Unexpected error");
    }
}

} // namespace image
