#pragma once
#include "render/texture.h"
#include <string>

struct Image;

namespace resource {

//Textures are compressed to ASTC once and kept in the profile's texture cache, so later
//runs skip decoding the image and the textures take a quarter of the memory.

//Whether the texture cache can be used at all
bool textureCacheAvailable();

//Whether textures from <filename> may be compressed, before the image is decoded
bool mayCompressTexture(const std::string& filename, bool mipmap);
//Whether the decoded image is worth compressing
bool shouldCompressTexture(const Image& image);

//Reads the cached compression of <filename>, if it is up to date for the size limit
bool loadCachedTexture(const std::string& filename, unsigned maxPixels, bool mipmap, render::CompressedImage& out);
//Compresses <image> and its mip levels, and caches the result for <filename>
bool compressTexture(const std::string& filename, unsigned maxPixels, bool mipmap, const Image& image, render::CompressedImage& out);

};
