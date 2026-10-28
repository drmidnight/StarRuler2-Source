#include "render/gl_texture.h"
#include "main/references.h"
#include "main/logging.h"
#include <assert.h>
#include <algorithm>

namespace render {
extern bool isIntelCard;

void GLTexture::bind() {
	glBindTexture(GL_TEXTURE_2D, glName);
}

unsigned GLTexture::getID() const {
	return glName;
}

GLTexture::GLTexture()
	: glName(0), pixelDepth(1)
{
	type = TT_2D;
	loaded = false;
}

GLTexture::GLTexture(Image& image, bool mipmap, bool cachePixels)
	: glName(0), pixelDepth(1)
{
	type = TT_2D;
	loaded = false;
	load(image, mipmap, cachePixels);
}

//Core GL has no luminance or alpha formats. One-channel images are stored as red,
//with a swizzle that gives shaders the values the old formats did.
static GLenum pixelFormat(ColorFormat format) {
	switch(format) {
	case FMT_Grey:
	case FMT_Alpha:
		return GL_RED;
	case FMT_RGB:
		return GL_RGB;
	case FMT_RGBA:
		return GL_RGBA;
	NO_DEFAULT
	}
}

static GLenum internalFormat(ColorFormat format) {
	switch(format) {
	case FMT_Grey:
	case FMT_Alpha:
		return GL_R8;
	case FMT_RGB:
		return GL_RGB8;
	case FMT_RGBA:
		return GL_RGBA8;
	NO_DEFAULT
	}
}

static void setSwizzle(GLenum target, ColorFormat format) {
	if(format == FMT_Grey) {
		GLint swizzle[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
		glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
	}
	else if(format == FMT_Alpha) {
		GLint swizzle[] = {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED};
		glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
	}
}

void createTexture(Image& image, bool mipmap) {
	unsigned levels = 1;
	if(mipmap) {
		unsigned smallDim = std::min(image.width, image.height);
		while(smallDim > 2) {
			smallDim /= 2;
			levels += 1;
		}
	}

	GLenum format = pixelFormat(image.format);
	GLenum internal = internalFormat(image.format);
	unsigned w = image.width, h = image.height;
	for(unsigned i = 0; i < levels; ++i) {
		glTexImage2D(GL_TEXTURE_2D, i, internal, w, h, 0, format, GL_UNSIGNED_BYTE, nullptr);
		w /= 2;
		h /= 2;
	}
	setSwizzle(GL_TEXTURE_2D, image.format);

	if(!isIntelCard)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels-1);
	devices.render->reportErrors("creating texture");
}

void loadTexture(Image& image, const recti& pixels, unsigned lod = 0) {
	devices.render->reportErrors();

	GLenum format = pixelFormat(image.format);
	
	assert((unsigned)(pixels.topLeft.x + pixels.getWidth()) <= image.width);
	assert((unsigned)(pixels.topLeft.y + pixels.getHeight()) <= image.height);

	if(pixels.getSize() == vec2i(image.width, image.height)) {
		void* ptrPixels;
		switch(image.format) {
		case FMT_Grey:
			ptrPixels = (void*)image.grey; break;
		case FMT_Alpha:
			ptrPixels = (void*)image.grey; break;
		case FMT_RGB:
			ptrPixels = (void*)image.rgb; break;
		case FMT_RGBA:
			ptrPixels = (void*)image.rgba; break;
		NO_DEFAULT
		}

		glTexSubImage2D(GL_TEXTURE_2D, lod, pixels.topLeft.x, pixels.topLeft.y, pixels.getWidth(), pixels.getHeight(), format, GL_UNSIGNED_BYTE, ptrPixels);
	}
	else if((unsigned)pixels.getWidth() == image.width) {
		assert(pixels.topLeft.x == 0);

		void* ptrPixels;
		switch(image.format) {
		case FMT_Grey:
			ptrPixels = (void*)&image.get_grey(0, pixels.topLeft.y); break;
		case FMT_Alpha:
			ptrPixels = (void*)&image.get_alpha(0, pixels.topLeft.y); break;
		case FMT_RGB:
			ptrPixels = (void*)&image.get_rgb(0, pixels.topLeft.y); break;
		case FMT_RGBA:
			ptrPixels = (void*)&image.get_rgba(0, pixels.topLeft.y); break;
		NO_DEFAULT
		}

		glTexSubImage2D(GL_TEXTURE_2D, lod, pixels.topLeft.x, pixels.topLeft.y, pixels.getWidth(), pixels.getHeight(), format, GL_UNSIGNED_BYTE, ptrPixels);
	}
	else {
		//Must load line by line
		for(unsigned y = pixels.topLeft.y, endY = pixels.botRight.y; y < endY; ++y) {
			void* ptrPixels;
			switch(image.format) {
			case FMT_Grey:
				ptrPixels = (void*)&image.get_grey(pixels.topLeft.x, y); break;
			case FMT_Alpha:
				ptrPixels = (void*)&image.get_alpha(pixels.topLeft.x, y); break;
			case FMT_RGB:
				ptrPixels = (void*)&image.get_rgb(pixels.topLeft.x, y); break;
			case FMT_RGBA:
				ptrPixels = (void*)&image.get_rgba(pixels.topLeft.x, y); break;
			NO_DEFAULT
			}

			glTexSubImage2D(GL_TEXTURE_2D, lod, pixels.topLeft.x, y, pixels.getWidth(), 1, format, GL_UNSIGNED_BYTE, ptrPixels);
		}
	}
	devices.render->reportErrors("filling sub image");
}

void GLTexture::loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) {
	devices.render->reportErrors();
	loaded = true;

	if(lod == 0) {
		//Destroy old texture
		if(glName != 0) {
			glDeleteTextures(1, &glName);
			glName = 0;
		}

		//Generate a new texture
		glGenTextures(1,&glName);

		//Store texture size
		size = vec2i(image.width, image.height);

		pixelDepth = ColorDepths[image.format];
		alphaOnly = image.format == FMT_Alpha;

		if(cachePixels)
			activePixels.resize(size.x * size.y);
	}

	glBindTexture(GL_TEXTURE_2D, glName);
	if(lod == 0)
		createTexture(image, mipmap);
}

void GLTexture::loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) {
	glBindTexture(GL_TEXTURE_2D, glName);
	loadTexture(image, pixels, lod);

	if(cachePixels && lod == 0) {
		for(int y = pixels.topLeft.y; y < pixels.getHeight(); ++y) {
			for(int x = pixels.topLeft.x; x < pixels.getWidth(); ++x) {
				unsigned char a = image.get(x, y).a;
				activePixels[y * size.width + x] = a != 0;
			}
		}
	}
}

void GLTexture::loadFinish(bool mipmap, unsigned lod) {
	devices.render->reportErrors();
	glBindTexture(GL_TEXTURE_2D, glName);

	hasMipMaps = mipmap;
	if(mipmap) {
		//Set linear mipmap filter
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

		if(lod == 0) {
			glGenerateMipmap(GL_TEXTURE_2D);
		}
	}
	else if(lod == 0) {
		//Set linear filter
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else {
		//We're defining mipmap data now, switch back to mipmaping
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lod);
	}

	devices.render->reportErrors("finalizing texture");
}

unsigned GLTexture::getTextureBytes() const {
	unsigned bytes = size.width * size.height * pixelDepth;
	if(hasMipMaps)
		bytes += bytes / 3;
	return bytes;
}

void GLTexture::load(Image& image, bool mipmap, bool cachePixels) {
	loadStart(image, mipmap, cachePixels);
	loadPartial(image, recti(0,0, image.width, image.height), cachePixels);
	loadFinish(mipmap);
}

void GLTexture::save(Image& image) const {
	image.resize(size.width, size.height, FMT_RGBA);
	glBindTexture(GL_TEXTURE_2D, glName);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba);
}

bool GLTexture::isPixelActive(vec2i px) const {
	unsigned index = px.y * size.width + px.x;
	if(activePixels.empty())
		return true;
	if(index >= activePixels.size())
		return false;
	return activePixels[index];
}

GLTexture::~GLTexture() {
	glDeleteTextures(1, &glName);
}

bool GLCubeMap::isPixelActive(vec2i px) const {
	return true;
}

void GLCubeMap::load(Image& image, bool mipmap, bool cachePixels) {
	devices.render->reportErrors();
	loaded = false;

	vec2u tile = vec2u(image.width / 4, image.height / 3);
	if(tile.width != tile.height || tile.width == 0) {
		error("ERROR loading cubemap: Cubemap must have a 4:3 aspect ratio.");
		return;
	}

	//Destroy old texture
	if(glName != 0) {
		glDeleteTextures(1, &glName);
		glName = 0;
	}

	//Generate a new texture
	glGenTextures(1,&glName);

	//Store texture size
	size = vec2i(image.width, image.height);
	loaded = true;

	pixelDepth = ColorDepths[image.format];

	glBindTexture(GL_TEXTURE_CUBE_MAP, glName);

	unsigned char* data = image.grey;

	GLenum format = pixelFormat(image.format);
	GLenum internal = internalFormat(image.format);
	setSwizzle(GL_TEXTURE_CUBE_MAP, image.format);

	auto getTile = [&](unsigned x, unsigned y) -> Image* {
		vec2u top = vec2u(tile.width * x, tile.height * y);
		return image.crop(rect<unsigned>(top, top + tile));
	};
	
	auto* img = getTile(1,1);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	img = getTile(2,1);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	img = getTile(1,2);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	img = getTile(3,1);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	img = getTile(0,1);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	img = getTile(1,0);
	glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 0, internal, tile.width, tile.height, 0, format, GL_UNSIGNED_BYTE, img->grey);
	delete img;
	
	devices.render->reportErrors("loading cubemap images");

	hasMipMaps = mipmap;
	if(mipmap) {
		//Set linear mipmap filter
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

		glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
	}
	else {
		//Set linear filter
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}

	devices.render->reportErrors("finalizing texture");
}

unsigned GLCubeMap::getTextureBytes() const {
	unsigned bytes = size.width * size.height * pixelDepth * 6;
	if(hasMipMaps)
		bytes += bytes / 3;
	return bytes;
}

void GLCubeMap::save(Image& image) const {}

void GLCubeMap::bind() {
	glBindTexture(GL_TEXTURE_CUBE_MAP, glName);
}

unsigned GLCubeMap::getID() const {
	return glName;
}

GLCubeMap::GLCubeMap(Image& image, bool mipmap) : glName(0), pixelDepth(1) {
	loaded = false;
	type = TT_Cubemap;
	load(image, mipmap);
}

GLCubeMap::GLCubeMap() : glName(0), pixelDepth(0) {
	loaded = false;
	type = TT_Cubemap;
}

GLCubeMap::~GLCubeMap() {
	glDeleteTextures(1, &glName);
}

void GLCubeMap::loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) {
	load(image, mipmap);
}

void GLCubeMap::loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) {
}

void GLCubeMap::loadFinish(bool mipmap, unsigned lod) {
}

};
