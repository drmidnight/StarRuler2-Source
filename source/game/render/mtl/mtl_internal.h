#pragma once
//Shared between the Metal renderer's Objective-C++ files only
#include "render/texture.h"
#include "render/shader.h"
#include "render/shader_builtins.h"
#include "render/shader_shim.h"
#include "render/shader_translate.h"
#include "image.h"
#include <string>
#include <unordered_map>
#include <vector>
//After the engine headers: a system header declares an extern pi, which would
//otherwise make the engine's pi constant external in every file
#import <Metal/Metal.h>

namespace render {
namespace mtl {

extern id<MTLDevice> device;
extern id<MTLCommandQueue> queue;

//Render targets and the screen share these formats, so pipelines only vary by shader and state
const MTLPixelFormat colorFormat = MTLPixelFormatBGRA8Unorm;
const MTLPixelFormat depthFormat = MTLPixelFormatDepth32Float;

//A texture the renderer can bind
class MetalTexture : public Texture {
public:
	id<MTLTexture> texture = nil;
};

class Texture2D : public MetalTexture {
	std::vector<bool> activePixels;
	unsigned pixelDepth = 1;
public:
	Texture2D();

	bool isPixelActive(vec2i px) const override;
	void loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) override;
	void loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) override;
	void loadFinish(bool mipmap, unsigned lod) override;
	void load(Image& image, bool mipmap, bool cachePixels) override;
	void save(Image& image) const override;
	void bind() override {}
	unsigned getID() const override { return texture ? 1 : 0; }
	unsigned getTextureBytes() const override;
};

class Cubemap : public MetalTexture {
	unsigned pixelDepth = 1;
public:
	Cubemap();

	bool isPixelActive(vec2i px) const override { return true; }
	void loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) override;
	void loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) override {}
	void loadFinish(bool mipmap, unsigned lod) override {}
	void load(Image& image, bool mipmap, bool cachePixels) override;
	void save(Image& image) const override {}
	void bind() override {}
	unsigned getID() const override { return texture ? 1 : 0; }
	unsigned getTextureBytes() const override;
};

//The depth half of a render target, which shaders can sample
class DepthTexture : public MetalTexture {
public:
	bool isPixelActive(vec2i px) const override { return true; }
	void loadStart(Image&, bool, bool, unsigned) override {}
	void loadPartial(Image&, const recti&, bool, unsigned) override {}
	void loadFinish(bool, unsigned) override {}
	void load(Image&, bool, bool) override {}
	void save(Image&) const override {}
	void bind() override {}
	unsigned getID() const override { return texture ? 1 : 0; }
	unsigned getTextureBytes() const override { return size.width * size.height * 4; }
};

class RenderTarget : public MetalTexture {
public:
	DepthTexture depth;

	RenderTarget(const vec2i& size);

	Texture* getDepthTexture() const override { return const_cast<DepthTexture*>(&depth); }
	bool isPixelActive(vec2i px) const override { return true; }
	void loadStart(Image&, bool, bool, unsigned) override {}
	void loadPartial(Image&, const recti&, bool, unsigned) override {}
	void loadFinish(bool, unsigned) override {}
	void load(Image&, bool, bool) override {}
	void save(Image& image) const override;
	void bind() override {}
	unsigned getID() const override { return texture ? 1 : 0; }
	unsigned getTextureBytes() const override { return size.width * size.height * 8; }
};

//Which vertex data a shader input reads
enum VertexInput {
	VI_Vertex,
	VI_UV,
	VI_Normal,
	VI_Tangent,
	VI_Color,
	VI_UV2,
	VI_COUNT,
};

class ShaderProgram : public render::ShaderProgram {
public:
	std::string vertexFile, fragmentFile;
	id<MTLFunction> vertexFunction = nil, fragmentFunction = nil;
	bool valid = false;

	//Where a uniform lives in the uniform buffer both stages share
	struct Uniform {
		unsigned offset, size, arrayStride, arraySize;
	};
	std::unordered_map<std::string, Uniform> uniforms;
	int vertexBuffer = -1, fragmentBuffer = -1;
	unsigned uniformSize = 0;
	//The uniform values, kept between draws as GL kept them in the program
	std::vector<unsigned char> uniformData;
	//Whether the values changed since the driver last sent them
	bool uniformsChanged = true;

	//A sampler, or array of samplers, and the texture units each element reads, which
	//shader variables choose
	struct TextureSlot {
		bool cube;
		int vertexTexture = -1, vertexSampler = -1;
		int fragmentTexture = -1, fragmentSampler = -1;
		std::vector<int> units;
	};
	std::vector<TextureSlot> textures;
	std::unordered_map<std::string, unsigned> texturesByName;

	//Vertex shader inputs, by the attribute index in the shader
	std::vector<std::pair<unsigned, VertexInput>> inputs;

	//Pipelines for each blend mode and vertex layout
	std::unordered_map<unsigned, id<MTLRenderPipelineState>> pipelines;

	//The built-in uniforms the program reads, found once when it compiles
	struct LightUniforms {
		const Uniform *ambient, *diffuse, *specular, *position;
		const Uniform *constantAttenuation, *linearAttenuation, *quadraticAttenuation;
	};
	struct Builtins {
		const Uniform *modelView, *projection, *modelViewProjection, *normalMatrix;
		const Uniform *projectionInverse, *flipY;
		LightUniforms lights[MAX_LIGHTS];
		const Uniform *lightModelAmbient, *lightEnabled;
		const Uniform *emission, *ambient, *diffuse, *specular, *shininess;
		const Uniform *fixedLighting, *fixedTexture, *alphaTest, *alphaRef;
		bool usesMatrices, usesLights, usesMaterial;
	} builtins = {};
	void findBuiltins();

	unsigned matrixSerial = 0, lightSerial = 0, materialSerial = 0;
	const render::Shader* lastShader = nullptr;
	unsigned lastBoundFrame = 0xffffffff;

	ShaderProgram(const std::string& vertex, const std::string& fragment)
		: vertexFile(vertex), fragmentFile(fragment) {}

	int compile() override;

	const Uniform* findUniform(const std::string& name) const {
		auto it = uniforms.find(name);
		return it == uniforms.end() ? nullptr : &it->second;
	}

	//Writes <count> elements of <components> 4-byte values, honouring the array stride
	void write(const Uniform& uniform, const void* data, unsigned components, unsigned count, unsigned columns = 1);

	//Copies the built-in state the program reads, when it changed
	void updateBuiltins(const BuiltinState& state);
};

class Shader : public render::Shader {
public:
	std::vector<std::string> names;
	//What each variable sets: a uniform, a texture unit, or nothing in this program
	std::vector<const ShaderProgram::Uniform*> varUniforms;
	std::vector<int> varTextures;

	Shader() {
		program = nullptr;
		constant = true;
		dynamicFloats = 0;
	}

	ShaderProgram* metalProgram() const { return (ShaderProgram*)program; }

	int compile() override;
	void addVariable(const std::string& name, const Variable& var) override;
	void bind(float* dynamicBuffer) const override;
	void updateDynamicVars() const override;
	void saveDynamicVars(float* buffer) const override;
	void loadDynamicVars(float* buffer) const override;

private:
	bool isActive(size_t index) const { return varUniforms[index] || varTextures[index] >= 0; }
	//Puts one variable's values into the program, from <data> or its own storage
	void apply(size_t index, const void* data) const;
};

};
};
