#include "render/mtl/mtl_internal.h"
#include "main/logging.h"
#include <algorithm>
#include <mutex>

namespace render {
namespace mtl {

//Finishes the frame's GPU work so far, so the CPU can read what it rendered
void finishPendingWork();

//Same mip count as GL used: halve the shorter side down to 2
static unsigned mipLevels(unsigned width, unsigned height, bool mipmap) {
	unsigned levels = 1;
	if(mipmap) {
		unsigned smallDim = std::min(width, height);
		while(smallDim > 2) {
			smallDim /= 2;
			levels += 1;
		}
	}
	return levels;
}

//One-channel images are stored as red, swizzled to give shaders what GL gave them
static void describeFormat(MTLTextureDescriptor* desc, ColorFormat format) {
	switch(format) {
	case FMT_Grey:
		desc.pixelFormat = MTLPixelFormatR8Unorm;
		desc.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
			MTLTextureSwizzleRed, MTLTextureSwizzleOne);
		break;
	case FMT_Alpha:
		desc.pixelFormat = MTLPixelFormatR8Unorm;
		desc.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero, MTLTextureSwizzleZero,
			MTLTextureSwizzleZero, MTLTextureSwizzleRed);
		break;
	default:
		//Metal has no three-byte format, so RGB is expanded to RGBA
		desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
		break;
	}
}

//Uploads <region> of <image> at its <x>, <y> into <texture>
static void upload(id<MTLTexture> texture, const Image& image, unsigned x, unsigned y,
	MTLRegion region, unsigned level, unsigned slice)
{
	unsigned depth = ColorDepths[image.format];
	const unsigned char* base = image.grey + (y * image.width + x) * depth;
	unsigned rowBytes = image.width * depth;

	if(image.format != FMT_RGB) {
		[texture replaceRegion:region mipmapLevel:level slice:slice withBytes:base
			bytesPerRow:rowBytes bytesPerImage:0];
		return;
	}

	std::vector<unsigned char> rgba(region.size.width * region.size.height * 4);
	for(unsigned row = 0; row < region.size.height; ++row) {
		const unsigned char* src = base + row * rowBytes;
		unsigned char* dst = rgba.data() + row * region.size.width * 4;
		for(unsigned col = 0; col < region.size.width; ++col) {
			dst[col*4+0] = src[col*3+0];
			dst[col*4+1] = src[col*3+1];
			dst[col*4+2] = src[col*3+2];
			dst[col*4+3] = 0xff;
		}
	}
	[texture replaceRegion:region mipmapLevel:level slice:slice withBytes:rgba.data()
		bytesPerRow:region.size.width * 4 bytesPerImage:0];
}

static void generateMipmaps(id<MTLTexture> texture) {
	if(texture.mipmapLevelCount <= 1)
		return;
	id<MTLCommandBuffer> commands = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
	[blit generateMipmapsForTexture:texture];
	[blit endEncoding];
	[commands commit];
}

std::mutex loadLock;
std::vector<std::pair<MetalTexture*, std::function<void()>>> queuedLoads;

void queueTextureLoad(MetalTexture* tex, std::function<void()> apply) {
	std::lock_guard<std::mutex> lock(loadLock);
	queuedLoads.emplace_back(tex, std::move(apply));
}

bool applyQueuedLoads() {
	decltype(queuedLoads) loads;
	{
		std::lock_guard<std::mutex> lock(loadLock);
		loads.swap(queuedLoads);
	}
	for(auto& load : loads)
		load.second();
	return !loads.empty();
}

bool hasQueuedLoads() {
	std::lock_guard<std::mutex> lock(loadLock);
	return !queuedLoads.empty();
}

MetalTexture::~MetalTexture() {
	std::lock_guard<std::mutex> lock(loadLock);
	std::erase_if(queuedLoads, [this](auto& load) { return load.first == this; });
}

static id<MTLTexture> newTexture2D(const Image& image, bool mipmap) {
	MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2D;
	desc.width = image.width;
	desc.height = image.height;
	desc.mipmapLevelCount = mipLevels(image.width, image.height, mipmap);
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	describeFormat(desc, image.format);
	return [device newTextureWithDescriptor:desc];
}

Texture2D::Texture2D() {
	type = TT_2D;
	loaded = false;
	hasMipMaps = false;
}

void Texture2D::loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) {
	loaded = true;
	if(lod != 0)
		return;

	size = vec2i(image.width, image.height);
	pixelDepth = ColorDepths[image.format];
	alphaOnly = image.format == FMT_Alpha;
	if(cachePixels)
		activePixels.resize(size.x * size.y);
	texture = newTexture2D(image, mipmap);
}

void Texture2D::loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) {
	if(!texture || lod >= texture.mipmapLevelCount)
		return;
	MTLRegion region = MTLRegionMake2D(pixels.topLeft.x, pixels.topLeft.y, pixels.getWidth(), pixels.getHeight());
	upload(texture, image, pixels.topLeft.x, pixels.topLeft.y, region, lod, 0);

	//Same bounds as the GL version, so clicks hit the same pixels
	if(cachePixels && lod == 0) {
		for(int y = pixels.topLeft.y; y < pixels.getHeight(); ++y) {
			for(int x = pixels.topLeft.x; x < pixels.getWidth(); ++x) {
				unsigned char a = image.get(x, y).a;
				activePixels[y * size.width + x] = a != 0;
			}
		}
	}
}

void Texture2D::loadFinish(bool mipmap, unsigned lod) {
	hasMipMaps = mipmap;
	if(mipmap && lod == 0 && texture)
		generateMipmaps(texture);
}

void Texture2D::load(Image& image, bool mipmap, bool cachePixels) {
	loadStart(image, mipmap, cachePixels, 0);
	loadPartial(image, recti(0, 0, image.width, image.height), cachePixels, 0);
	loadFinish(mipmap, 0);
}

bool Texture2D::loadOffThread(Image& image, bool mipmap, bool cachePixels) {
	id<MTLTexture> made = newTexture2D(image, mipmap);
	upload(made, image, 0, 0, MTLRegionMake2D(0, 0, image.width, image.height), 0, 0);
	if(mipmap)
		generateMipmaps(made);

	std::vector<bool> active;
	if(cachePixels) {
		active.resize(image.width * image.height);
		for(unsigned y = 0; y < image.height; ++y)
			for(unsigned x = 0; x < image.width; ++x)
				active[y * image.width + x] = image.get(x, y).a != 0;
	}

	vec2i newSize(image.width, image.height);
	unsigned depth = ColorDepths[image.format];
	bool alpha = image.format == FMT_Alpha;
	queueTextureLoad(this, [this, made, newSize, depth, alpha, mipmap, active = std::move(active)]() mutable {
		texture = made;
		size = newSize;
		pixelDepth = depth;
		alphaOnly = alpha;
		hasMipMaps = mipmap;
		activePixels = std::move(active);
		loaded = true;
	});
	return true;
}

bool Texture2D::loadCompressedOffThread(const CompressedImage& image) {
	if(image.levels.empty() || image.blockWidth != 4 || image.blockHeight != 4)
		return false;

	MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2D;
	desc.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
	desc.width = image.width;
	desc.height = image.height;
	desc.mipmapLevelCount = image.levels.size();
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	id<MTLTexture> made = [device newTextureWithDescriptor:desc];
	if(!made)
		return false;

	for(unsigned level = 0; level < image.levels.size(); ++level) {
		unsigned w = std::max(1u, image.width >> level), h = std::max(1u, image.height >> level);
		unsigned blocksWide = (w + 3) / 4, blocksHigh = (h + 3) / 4;
		if(image.levels[level].size() < blocksWide * blocksHigh * 16)
			return false;
		[made replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:0
			withBytes:image.levels[level].data() bytesPerRow:blocksWide * 16 bytesPerImage:0];
	}

	vec2i newSize(image.width, image.height);
	bool mipmapped = image.levels.size() > 1;
	queueTextureLoad(this, [this, made, newSize, mipmapped] {
		texture = made;
		size = newSize;
		//ASTC 4x4 takes a byte per pixel
		pixelDepth = 1;
		alphaOnly = false;
		hasMipMaps = mipmapped;
		activePixels.clear();
		loaded = true;
	});
	return true;
}

void Texture2D::save(Image& image) const {
	image.resize(size.width, size.height, FMT_RGBA);
	if(!texture)
		return;
	if(texture.pixelFormat == MTLPixelFormatASTC_4x4_LDR) {
		error("Compressed textures can't be read back");
		return;
	}
	finishPendingWork();
	if(texture.pixelFormat == MTLPixelFormatRGBA8Unorm) {
		[texture getBytes:image.rgba bytesPerRow:size.width * 4
			fromRegion:MTLRegionMake2D(0, 0, size.width, size.height) mipmapLevel:0];
		return;
	}

	std::vector<unsigned char> red(size.width * size.height);
	[texture getBytes:red.data() bytesPerRow:size.width
		fromRegion:MTLRegionMake2D(0, 0, size.width, size.height) mipmapLevel:0];
	for(size_t i = 0; i < red.size(); ++i) {
		if(alphaOnly)
			image.rgba[i] = Color(0, 0, 0, red[i]);
		else
			image.rgba[i] = Color(red[i], red[i], red[i], 255);
	}
}

bool Texture2D::isPixelActive(vec2i px) const {
	unsigned index = px.y * size.width + px.x;
	if(activePixels.empty())
		return true;
	if(index >= activePixels.size())
		return false;
	return activePixels[index];
}

unsigned Texture2D::getTextureBytes() const {
	unsigned bytes = size.width * size.height * pixelDepth;
	if(hasMipMaps)
		bytes += bytes / 3;
	return bytes;
}

Cubemap::Cubemap() {
	type = TT_Cubemap;
	loaded = false;
	hasMipMaps = false;
}

void Cubemap::loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) {
	if(lod == 0)
		load(image, mipmap, cachePixels);
}

//Makes a cubemap from the cross-shaped image, or nil if it isn't one
static id<MTLTexture> newCubemap(const Image& image, bool mipmap) {
	unsigned tile = image.width / 4;
	if(tile != image.height / 3 || tile == 0) {
		error("ERROR loading cubemap: Cubemap must have a 4:3 aspect ratio.");
		return nil;
	}

	MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureTypeCube;
	desc.width = tile;
	desc.height = tile;
	desc.mipmapLevelCount = mipLevels(tile, tile, mipmap);
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	describeFormat(desc, image.format);
	id<MTLTexture> made = [device newTextureWithDescriptor:desc];

	//Tiles of the cross-shaped image, in Metal's face order: +X, -X, +Y, -Y, +Z, -Z
	static const unsigned faces[6][2] = {{2,1}, {0,1}, {1,0}, {1,2}, {1,1}, {3,1}};
	for(unsigned face = 0; face < 6; ++face)
		upload(made, image, faces[face][0] * tile, faces[face][1] * tile,
			MTLRegionMake2D(0, 0, tile, tile), 0, face);

	if(mipmap)
		generateMipmaps(made);
	return made;
}

void Cubemap::load(Image& image, bool mipmap, bool cachePixels) {
	loaded = false;
	id<MTLTexture> made = newCubemap(image, mipmap);
	if(!made)
		return;

	texture = made;
	size = vec2i(image.width, image.height);
	pixelDepth = ColorDepths[image.format];
	hasMipMaps = mipmap;
	loaded = true;
}

bool Cubemap::loadOffThread(Image& image, bool mipmap, bool cachePixels) {
	id<MTLTexture> made = newCubemap(image, mipmap);
	vec2i newSize(image.width, image.height);
	unsigned depth = ColorDepths[image.format];
	queueTextureLoad(this, [this, made, newSize, depth, mipmap] {
		if(!made) {
			loaded = false;
			return;
		}
		texture = made;
		size = newSize;
		pixelDepth = depth;
		hasMipMaps = mipmap;
		loaded = true;
	});
	return true;
}

unsigned Cubemap::getTextureBytes() const {
	unsigned bytes = size.width * size.height * pixelDepth / 2;
	if(hasMipMaps)
		bytes += bytes / 3;
	return bytes;
}

RenderTarget::RenderTarget(const vec2i& Size) {
	type = TT_2D;
	size = Size;
	loaded = true;
	hasMipMaps = false;

	MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:colorFormat
		width:std::max(1, size.width) height:std::max(1, size.height) mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	texture = [device newTextureWithDescriptor:desc];

	desc.pixelFormat = depthFormat;
	depth.texture = [device newTextureWithDescriptor:desc];
	depth.type = TT_2D;
	depth.size = size;
	depth.loaded = true;
	depth.hasMipMaps = false;
}

void RenderTarget::save(Image& image) const {
	image.resize(size.width, size.height, FMT_RGBA);
	if(!texture)
		return;
	finishPendingWork();

	NSUInteger rowBytes = size.width * 4;
	id<MTLBuffer> buffer = [device newBufferWithLength:rowBytes * size.height options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> commands = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
	[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(size.width, size.height, 1) toBuffer:buffer destinationOffset:0
		destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * size.height];
	[blit endEncoding];
	[commands commit];
	[commands waitUntilCompleted];

	//BGRA to RGBA
	const unsigned char* src = (const unsigned char*)buffer.contents;
	for(int i = 0, cnt = size.width * size.height; i < cnt; ++i)
		image.rgba[i] = Color(src[i*4+2], src[i*4+1], src[i*4+0], src[i*4+3]);
}

};
};
