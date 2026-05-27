#pragma once

#include <string>

namespace image {

// Output of avatar processing. Either ok=true with a path that the caller can
// move into place, or ok=false with a human-readable reason and an HTTP-style
// status code that the controller can surface (415, 413, 422, 500).
struct AvatarResult {
    bool        ok;
    std::string outputPath;     // populated when ok
    std::string error;          // populated when !ok
    int         status;         // HTTP status code suggestion when !ok
};

// Reads `srcPath`, sniffs the magic bytes, rejects anything that isn't
// JPEG / PNG / WebP. Refuses inputs larger than `maxDim`x`maxDim` pixels
// (decompression-bomb defense). Resizes to a square `outDim`x`outDim` JPEG
// using a center "cover" crop, strips all metadata including EXIF GPS, and
// writes the result to `outPath`. Quality is fixed at 85.
//
// Implementation uses libvips.
AvatarResult processAvatar(const std::string& srcPath,
                           const std::string& outPath,
                           int outDim  = 256,
                           int maxDim  = 6000);

// Inline post images. Same magic-byte sniffing (JPEG/PNG/WebP only), EXIF/
// metadata stripping and decompression-bomb guard as processAvatar, but
// preserves aspect ratio: the image is downscaled so its longest edge is at
// most `maxEdge` px (never upscaled, never cropped) and re-encoded to JPEG.
// Reuses AvatarResult for the ok/path/error/status shape.
AvatarResult processPostImage(const std::string& srcPath,
                              const std::string& outPath,
                              int maxEdge = 1600,
                              int maxDim  = 10000);

// Initializes libvips. Idempotent; call once at process startup.
bool initLibrary();

// Releases libvips resources. Best called on shutdown.
void shutdownLibrary();

} // namespace image
