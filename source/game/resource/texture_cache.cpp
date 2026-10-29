#include "resource/texture_cache.h"
#include "main/references.h"
#include "main/logging.h"
#include "render/driver.h"
#include "image.h"
#include "threads.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#ifdef SR_ASTC
#include <sys/stat.h>
#include <dispatch/dispatch.h>
#include <astcenc.h>
#endif

namespace resource {

#ifdef SR_ASTC
//Bump when the file layout, the encoder or its settings change, so older files are rebuilt
const uint32_t CACHE_VERSION = 1;
const float COMPRESS_QUALITY = ASTCENC_PRE_MEDIUM;
//Smaller textures save little memory, and are mostly interface art that blocks would blur
const unsigned MIN_COMPRESS_PIXELS = 256 * 256;

struct CacheHeader {
	char magic[4];
	uint32_t version;
	uint32_t width, height;
	uint32_t levels;
	uint32_t maxPixels;
	uint32_t mipmap;
	uint32_t pathLength;
	int64_t sourceSize;
	int64_t sourceTime;
};

static size_t levelBytes(unsigned width, unsigned height, unsigned level) {
	unsigned w = std::max(1u, width >> level), h = std::max(1u, height >> level);
	return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
}

static std::string cachePath(const std::string& filename, unsigned maxPixels, bool mipmap) {
	size_t hash = std::hash<std::string>()(filename + "|" + std::to_string(maxPixels) + (mipmap ? "|m" : "|n"));
	char name[32];
	snprintf(name, sizeof(name), "/%016zx.astc", hash);
	return devices.mods.getProfile("texture_cache") + name;
}

bool textureCacheAvailable() {
	if(!devices.render || !devices.render->supportsCompressedTextures())
		return false;
	auto* setting = devices.settings.engine.getSetting("bCompressTextures");
	return !setting || setting->getBool();
}

bool mayCompressTexture(const std::string& filename, bool mipmap) {
	//Only textures drawn in 3D have mip levels
	if(!mipmap)
		return false;
	//Lookup tables hold exact values that compression would change
	std::string lower = filename;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return lower.find("lookup") == std::string::npos;
}

bool shouldCompressTexture(const Image& image) {
	//One-channel images are already a byte per pixel
	if(image.format != FMT_RGB && image.format != FMT_RGBA)
		return false;
	return image.width * image.height >= MIN_COMPRESS_PIXELS;
}

bool loadCachedTexture(const std::string& filename, unsigned maxPixels, bool mipmap, render::CompressedImage& out) {
	struct stat source;
	if(stat(filename.c_str(), &source) != 0)
		return false;

	FILE* file = fopen(cachePath(filename, maxPixels, mipmap).c_str(), "rb");
	if(!file)
		return false;
	std::unique_ptr<FILE, int(*)(FILE*)> closer(file, fclose);

	CacheHeader header;
	if(fread(&header, sizeof(header), 1, file) != 1)
		return false;
	if(memcmp(header.magic, "SR2T", 4) != 0 || header.version != CACHE_VERSION
			|| header.maxPixels != maxPixels || header.mipmap != (mipmap ? 1u : 0u)
			|| header.sourceSize != (int64_t)source.st_size || header.sourceTime != (int64_t)source.st_mtime
			|| header.pathLength != filename.size() || header.levels == 0 || header.levels > 16)
		return false;

	std::string path(header.pathLength, '\0');
	if(fread(path.data(), 1, path.size(), file) != path.size() || path != filename)
		return false;

	out.width = header.width;
	out.height = header.height;
	out.blockWidth = out.blockHeight = 4;
	out.levels.resize(header.levels);
	for(unsigned i = 0; i < header.levels; ++i) {
		out.levels[i].resize(levelBytes(header.width, header.height, i));
		if(fread(out.levels[i].data(), 1, out.levels[i].size(), file) != out.levels[i].size())
			return false;
	}
	return true;
}

static void writeCache(const std::string& filename, unsigned maxPixels, bool mipmap, const render::CompressedImage& image) {
	struct stat source;
	if(stat(filename.c_str(), &source) != 0)
		return;

	CacheHeader header;
	memcpy(header.magic, "SR2T", 4);
	header.version = CACHE_VERSION;
	header.width = image.width;
	header.height = image.height;
	header.levels = (uint32_t)image.levels.size();
	header.maxPixels = maxPixels;
	header.mipmap = mipmap ? 1 : 0;
	header.pathLength = (uint32_t)filename.size();
	header.sourceSize = (int64_t)source.st_size;
	header.sourceTime = (int64_t)source.st_mtime;

	//Written next to the final name and renamed, so a reader never sees half a file
	std::string path = cachePath(filename, maxPixels, mipmap);
	std::string temp = path + ".tmp" + std::to_string(threads::getThreadID());
	FILE* file = fopen(temp.c_str(), "wb");
	if(!file)
		return;
	bool ok = fwrite(&header, sizeof(header), 1, file) == 1
		&& fwrite(filename.data(), 1, filename.size(), file) == filename.size();
	for(auto& level : image.levels)
		ok = ok && fwrite(level.data(), 1, level.size(), file) == level.size();
	ok = fclose(file) == 0 && ok;

	if(!ok || rename(temp.c_str(), path.c_str()) != 0) {
		remove(temp.c_str());
		error("Could not write the texture cache file for %s", filename.c_str());
	}
}

//One context compresses one image at a time, using every core
static std::mutex encodeLock;
static astcenc_context* encoder = nullptr;
static unsigned encoderThreads = 0;

static astcenc_context* getEncoder() {
	if(encoder)
		return encoder;
	astcenc_config config;
	if(astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, COMPRESS_QUALITY, 0, &config) != ASTCENC_SUCCESS)
		return nullptr;
	encoderThreads = std::max(1u, threads::getNumberOfProcessors());
	if(astcenc_context_alloc(&config, encoderThreads, &encoder, nullptr) != ASTCENC_SUCCESS) {
		encoder = nullptr;
		return nullptr;
	}
	return encoder;
}

struct CompressJob {
	astcenc_image* image;
	uint8_t* output;
	size_t outputSize;
	std::atomic<int> result{ASTCENC_SUCCESS};
};

static void compressPart(void* data, size_t thread) {
	auto* job = (CompressJob*)data;
	static const astcenc_swizzle identity = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
	astcenc_error result = astcenc_compress_image(encoder, job->image, &identity, job->output, job->outputSize, (unsigned)thread);
	if(result != ASTCENC_SUCCESS)
		job->result = result;
}

static void toRGBA(const Image& image, std::vector<unsigned char>& rgba) {
	size_t count = size_t(image.width) * image.height;
	rgba.resize(count * 4);
	if(image.format == FMT_RGBA) {
		memcpy(rgba.data(), image.rgba, count * 4);
		return;
	}
	for(size_t i = 0; i < count; ++i) {
		rgba[i*4+0] = image.rgb[i].r;
		rgba[i*4+1] = image.rgb[i].g;
		rgba[i*4+2] = image.rgb[i].b;
		rgba[i*4+3] = 0xff;
	}
}

bool compressTexture(const std::string& filename, unsigned maxPixels, bool mipmap, const Image& image, render::CompressedImage& out) {
	//The same levels the renderer would make: halved until the shorter side is 2
	std::vector<std::unique_ptr<Image>> mips;
	std::vector<const Image*> levels = {&image};
	if(mipmap) {
		while(Image* next = levels.back()->makeMipmap()) {
			mips.emplace_back(next);
			levels.push_back(next);
		}
	}

	out.width = image.width;
	out.height = image.height;
	out.blockWidth = out.blockHeight = 4;
	out.levels.clear();

	{
		std::lock_guard<std::mutex> lock(encodeLock);
		if(!getEncoder())
			return false;

		std::vector<unsigned char> rgba;
		for(unsigned i = 0; i < levels.size(); ++i) {
			toRGBA(*levels[i], rgba);
			void* slice = rgba.data();
			astcenc_image level = {levels[i]->width, levels[i]->height, 1, ASTCENC_TYPE_U8, &slice};
			out.levels.emplace_back(levelBytes(image.width, image.height, i));

			CompressJob job;
			job.image = &level;
			job.output = out.levels.back().data();
			job.outputSize = out.levels.back().size();
			dispatch_apply_f(encoderThreads, DISPATCH_APPLY_AUTO, &job, compressPart);
			astcenc_compress_reset(encoder);

			if(job.result != ASTCENC_SUCCESS) {
				error("Could not compress %s: %s", filename.c_str(), astcenc_get_error_string((astcenc_error)job.result.load()));
				out.levels.clear();
				return false;
			}
		}
	}

	writeCache(filename, maxPixels, mipmap, out);
	return true;
}

#else
bool textureCacheAvailable() { return false; }
bool mayCompressTexture(const std::string&, bool) { return false; }
bool shouldCompressTexture(const Image&) { return false; }
bool loadCachedTexture(const std::string&, unsigned, bool, render::CompressedImage&) { return false; }
bool compressTexture(const std::string&, unsigned, bool, const Image&, render::CompressedImage&) { return false; }
#endif

};
