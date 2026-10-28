#include "render/mtl/mtl_driver.h"
#include "render/driver_base.h"
#include "render/mtl/mtl_internal.h"
#include "main/references.h"
#include "main/logging.h"
#include "mesh.h"
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

namespace render {
namespace mtl {

id<MTLDevice> device = nil;
id<MTLCommandQueue> queue = nil;

//Frames the CPU may queue ahead of the GPU
static const unsigned framesInFlight = 3;

//Where vertex data is bound; uniform buffers take the low indices
static const unsigned vertexDataBuffer = 30;
static const unsigned defaultsBuffer = 29;

//Vertex layouts pipelines are built for
enum Layout {
	L_Batch,
	L_Mesh,
};

//Placeholder until meshes have their Metal implementation
class Mesh : public RenderMesh {
	::Mesh mesh;
	AABBoxf bbox;
	double lodDistance = 1;
	const RenderMesh* lod = nullptr;
public:
	Mesh(const ::Mesh& m) {
		resetToMesh(m);
	}

	void resetToMesh(const ::Mesh& m) override {
		mesh = m;
		bbox = AABBoxf();
		for(auto& v : mesh.vertices)
			bbox.addPoint(v.position);
	}
	const RenderMesh* selectLOD(double distance) const override {
		if(lod && distance > lodDistance)
			return lod->selectLOD(distance);
		return this;
	}
	void setLOD(double distance, const RenderMesh* m) override {
		lodDistance = distance;
		lod = m;
	}
	const AABBoxf& getBoundingBox() const override { return bbox; }
	const ::Mesh& getMesh() const override { return mesh; }
	unsigned getMeshBytes() const override { return 0; }
	void render() const override {}
};

class Driver : public BaseDriver {
public:
	CAMetalLayer* layer = nil;
	dispatch_semaphore_t frameSlots;
	unsigned frameIndex = 0;

	//The frame being recorded
	id<MTLCommandBuffer> commandBuffer = nil;
	id<CAMetalDrawable> drawable = nil;
	id<MTLTexture> screenDepth = nil;
	id<MTLRenderCommandEncoder> encoder = nil;

	//Where drawing goes: a render target, or the screen when null
	RenderTarget* target = nullptr;
	//Clears the next pass starts with
	bool clearColor = true, clearDepth = true;
	//The current screen clip, in screen coordinates
	bool clipping = false;
	recti clip;

	//Per-frame memory for batched vertices and indices
	struct FrameBuffers {
		std::vector<id<MTLBuffer>> buffers;
		size_t used = 0;
	};
	FrameBuffers frameBuffers[framesInFlight];
	id<MTLBuffer> defaults = nil;

	//What each texture unit holds, as GL kept it between states
	Texture* boundTextures[RENDER_MAX_TEXTURES] = {};
	id<MTLSamplerState> boundSamplers[RENDER_MAX_TEXTURES] = {};
	std::unordered_map<unsigned, id<MTLSamplerState>> samplers;
	std::unordered_map<unsigned, id<MTLDepthStencilState>> depthStates;

	Texture* whiteTexture = nullptr;
	Texture* blackTexture = nullptr;
	render::Shader* fixedFunction = nullptr;
	bool fixedFunctionFailed = false;
	//The shader the next draw uses, null for FixedFunction
	const Shader* currentShader = nullptr;
	//Saved dynamic shader values for the batch step being drawn
	float* shaderUniforms = nullptr;

	int verticalSync = 1;

	Driver() {
		frameSlots = dispatch_semaphore_create(framesInFlight);
	}

	bool init() override {
		device = MTLCreateSystemDefaultDevice();
		if(!device) {
			error("No Metal device available");
			return false;
		}
		queue = [device newCommandQueue];

		NSWindow* window = (__bridge NSWindow*)devices.driver->getNativeWindow();
		if(!window) {
			error("Metal needs a native window to draw into");
			return false;
		}

		layer = [CAMetalLayer layer];
		layer.device = device;
		layer.pixelFormat = colorFormat;
		layer.opaque = YES;
		//Screenshots and captures read the drawable back
		layer.framebufferOnly = NO;
		layer.maximumDrawableCount = framesInFlight;
		layer.displaySyncEnabled = verticalSync != 0;
		layer.contentsScale = window.backingScaleFactor;
		NSView* view = window.contentView;
		view.wantsLayer = YES;
		view.layer = layer;

		//Values for shader inputs a vertex layout doesn't have, as GL's current attributes
		float values[VI_COUNT][4] = {
			{0, 0, 0, 1}, //Vertex
			{0, 0, 0, 1}, //UV
			{0, 0, 1, 0}, //Normal
			{0, 0, 0, 1}, //Tangent
			{1, 1, 1, 1}, //Color
			{0, 0, 0, 1}, //UV2
		};
		defaults = [device newBufferWithBytes:values length:sizeof(values) options:MTLResourceStorageModeShared];

		Image white(1, 1, FMT_RGBA, 0xff);
		whiteTexture = createTexture(white, false);
		Image black(1, 1, FMT_RGBA, 0);
		black.rgba[0].a = 255;
		blackTexture = createTexture(black, false);

		setDefaultRenderState();
		initLights();

		print("Metal device '%s'", device.name.UTF8String);
		return true;
	}

	void reportErrors(const char* context = nullptr) const override {}

	void setVerticalSync(int frames) override {
		verticalSync = frames;
		if(layer)
			layer.displaySyncEnabled = frames != 0;
	}

	void setFramebufferSize(int w, int h) override {
		BaseDriver::setFramebufferSize(w, h);
		if(layer && w > 0 && h > 0) {
			CGSize size = layer.drawableSize;
			if((int)size.width != w || (int)size.height != h)
				layer.drawableSize = CGSizeMake(w, h);
		}
	}

	//-- Frames and passes

	void beginFrame() {
		if(commandBuffer)
			return;
		dispatch_semaphore_wait(frameSlots, DISPATCH_TIME_FOREVER);
		frameIndex = (frameIndex + 1) % framesInFlight;
		frameBuffers[frameIndex].used = 0;
		commandBuffer = [queue commandBuffer];
	}

	id<MTLTexture> screenDepthFor(id<MTLTexture> color) {
		if(!screenDepth || screenDepth.width != color.width || screenDepth.height != color.height) {
			MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depthFormat
				width:color.width height:color.height mipmapped:NO];
			desc.usage = MTLTextureUsageRenderTarget;
			desc.storageMode = MTLStorageModePrivate;
			screenDepth = [device newTextureWithDescriptor:desc];
		}
		return screenDepth;
	}

	//Starts a pass into the current target, if one isn't open
	bool beginPass() {
		if(encoder)
			return true;
		beginFrame();

		id<MTLTexture> color, depth;
		if(target) {
			color = target->texture;
			depth = target->depth.texture;
		}
		else {
			if(!drawable)
				drawable = [layer nextDrawable];
			if(!drawable)
				return false;
			color = drawable.texture;
			depth = screenDepthFor(color);
		}

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = color;
		pass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.depthAttachment.texture = depth;
		pass.depthAttachment.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.depthAttachment.clearDepth = 1.0;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
		clearColor = clearDepth = false;

		[encoder setFrontFacingWinding:MTLWindingCounterClockwise];
		[encoder setViewport:(MTLViewport){0, 0, (double)color.width, (double)color.height, 0, 1}];
		applyClip(color.width, color.height);
		return true;
	}

	void endPass() {
		if(encoder) {
			[encoder endEncoding];
			encoder = nil;
		}
	}

	void presentFrame() override {
		renderVertexBuffers();
		target = nullptr;
		//Even a frame that drew nothing on screen has to present a drawable
		beginPass();
		endPass();
		if(!commandBuffer)
			return;

		if(drawable)
			[commandBuffer presentDrawable:drawable];
		dispatch_semaphore_t slots = frameSlots;
		[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
			dispatch_semaphore_signal(slots);
		}];
		[commandBuffer commit];
		commandBuffer = nil;
		drawable = nil;
		clearColor = clearDepth = true;
	}

	//Runs everything recorded so far, so the CPU can read it, and keeps recording the frame
	void finishPendingWork() {
		if(!commandBuffer)
			return;
		endPass();
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];
		commandBuffer = [queue commandBuffer];
	}

	void clear(unsigned flags) override {
		endPass();
		if(flags & CF_Color)
			clearColor = true;
		if(flags & CF_Depth)
			clearDepth = true;
	}

	void applyClip(NSUInteger width, NSUInteger height) {
		MTLScissorRect rect = {0, 0, width, height};
		if(clipping && screenSize.width > 0 && screenSize.height > 0) {
			double sx = (double)width / (double)screenSize.width;
			double sy = (double)height / (double)screenSize.height;
			long x = (long)(clip.topLeft.x * sx), y = (long)(clip.topLeft.y * sy);
			long w = (long)(clip.getWidth() * sx), h = (long)(clip.getHeight() * sy);
			if(x < 0) { w += x; x = 0; }
			if(y < 0) { h += y; y = 0; }
			w = std::max(0l, std::min(w, (long)width - x));
			h = std::max(0l, std::min(h, (long)height - y));
			rect = {(NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h};
		}
		[encoder setScissorRect:rect];
	}

	void applyScissor(const recti* box) override {
		clipping = box != nullptr;
		if(box)
			clip = *box;
		if(encoder) {
			id<MTLTexture> color = target ? target->texture : drawable.texture;
			applyClip(color.width, color.height);
		}
	}

	void setRenderTarget(Texture* texture, bool intermediate = false) override {
		renderVertexBuffers();
		endPass();
		target = dynamic_cast<RenderTarget*>(texture);
		if(target) {
			//GL cleared render targets when they were bound
			clearColor = clearDepth = true;
			frameSize = target->size;
		}
		else {
			frameSize = framebufferSize.x > 0 ? framebufferSize : screenSize;
		}
		intermediateBlend = intermediate;
	}

	Image* getScreen(int x, int y, int w, int h) override {
		Image* img = new Image(w, h, FMT_RGB);
		beginFrame();
		if(!drawable)
			drawable = [layer nextDrawable];
		if(!drawable)
			return img;
		endPass();

		//GL counts rows from the bottom; Metal from the top
		id<MTLTexture> color = drawable.texture;
		NSUInteger top = color.height - y - h;
		NSUInteger rowBytes = w * 4;
		id<MTLBuffer> buffer = [device newBufferWithLength:rowBytes * h options:MTLResourceStorageModeShared];
		id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
		[blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, top, 0)
			sourceSize:MTLSizeMake(w, h, 1) toBuffer:buffer destinationOffset:0
			destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * h];
		[blit endEncoding];
		finishPendingWork();

		const unsigned char* src = (const unsigned char*)buffer.contents;
		for(int row = 0; row < h; ++row) {
			const unsigned char* line = src + (h - 1 - row) * rowBytes;
			for(int col = 0; col < w; ++col)
				img->rgb[row * w + col] = ColorRGB(line[col*4+2], line[col*4+1], line[col*4+0]);
		}
		return img;
	}

	//-- State

	void setDefaultRenderState() override {
		activeRenderState = RenderState();
		pLastRenderState = 0;
		currentShader = nullptr;
		setAlphaTest(MAT_Solid);
		resetMaterial();
	}

	void switchToRenderState(const RenderState& state) override {
		if(state.constant && pLastRenderState == &state) {
			if(state.shader && !state.shader->constant) {
				if(shaderUniforms)
					state.shader->loadDynamicVars(shaderUniforms);
				else
					state.shader->updateDynamicVars();
			}
			return;
		}

		//Texture units keep their last texture when a state leaves them empty, as on GL
		for(unsigned i = 0; i < RENDER_MAX_TEXTURES; ++i) {
			Texture* tex = state.textures[i];
			if(tex && tex != activeRenderState.textures[i]) {
				boundTextures[i] = tex;
				boundSamplers[i] = samplerFor(state, tex->hasMipMaps);
			}
		}

		if(state.baseMat != activeRenderState.baseMat)
			setAlphaTest(state.baseMat);

		if(state.lighting) {
			if(state.diffuse != builtins.materialDiffuse) {
				builtins.materialDiffuse = state.diffuse;
				++builtins.materialSerial;
			}
			if(state.specular != builtins.materialSpecular) {
				builtins.materialSpecular = state.specular;
				++builtins.materialSerial;
			}
			if(state.shininess != builtins.materialShininess) {
				builtins.materialShininess = state.shininess;
				++builtins.materialSerial;
			}
		}

		activeRenderState = state;
		pLastRenderState = &state;

		if(state.shader) {
			currentShader = (const Shader*)state.shader;
			state.shader->bind(shaderUniforms);
		}
		else {
			currentShader = nullptr;
		}
	}

	id<MTLSamplerState> samplerFor(const RenderState& state, bool mipmaps) {
		unsigned key = state.wrapHorizontal | (state.wrapVertical << 2) | (state.filterMin << 4)
			| (state.filterMag << 5) | ((mipmaps ? 1 : 0) << 6);
		auto it = samplers.find(key);
		if(it != samplers.end())
			return it->second;

		auto wrap = [](TextureWrap w) {
			switch(w) {
				case TW_Repeat: return MTLSamplerAddressModeRepeat;
				case TW_Clamp: return MTLSamplerAddressModeClampToBorderColor;
				case TW_ClampEdge: return MTLSamplerAddressModeClampToEdge;
				case TW_Mirror: return MTLSamplerAddressModeMirrorRepeat;
			}
			return MTLSamplerAddressModeRepeat;
		};
		MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
		desc.sAddressMode = wrap(state.wrapHorizontal);
		desc.tAddressMode = wrap(state.wrapVertical);
		desc.rAddressMode = wrap(state.wrapVertical);
		desc.borderColor = MTLSamplerBorderColorTransparentBlack;
		desc.minFilter = state.filterMin == TF_Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
		desc.magFilter = state.filterMag == TF_Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
		if(mipmaps)
			desc.mipFilter = state.filterMin == TF_Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
		id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:desc];
		samplers[key] = sampler;
		return sampler;
	}

	id<MTLDepthStencilState> depthStateFor(const RenderState& state) {
		unsigned key = state.depthTest | (state.depthWrite ? 16 : 0);
		auto it = depthStates.find(key);
		if(it != depthStates.end())
			return it->second;

		MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
		//Without a depth test GL neither tests nor writes depth
		desc.depthWriteEnabled = state.depthTest != DT_NoDepthTest && state.depthWrite;
		switch(state.depthTest) {
			case DT_Never: desc.depthCompareFunction = MTLCompareFunctionNever; break;
			case DT_Less: desc.depthCompareFunction = MTLCompareFunctionLess; break;
			case DT_Equal: desc.depthCompareFunction = MTLCompareFunctionEqual; break;
			case DT_LessEqual: desc.depthCompareFunction = MTLCompareFunctionLessEqual; break;
			case DT_Greater: desc.depthCompareFunction = MTLCompareFunctionGreater; break;
			case DT_NotEqual: desc.depthCompareFunction = MTLCompareFunctionNotEqual; break;
			case DT_GreaterEqual: desc.depthCompareFunction = MTLCompareFunctionGreaterEqual; break;
			default: desc.depthCompareFunction = MTLCompareFunctionAlways; break;
		}
		id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:desc];
		depthStates[key] = depthState;
		return depthState;
	}

	//The blend functions GL's setRenderFuncs used for each base material
	static void describeBlend(MTLRenderPipelineColorAttachmentDescriptor* color, BaseMaterial mat, bool intermediate) {
		switch(mat) {
			case MAT_Solid:
				color.blendingEnabled = NO;
				return;
			case MAT_Add:
				color.blendingEnabled = YES;
				color.sourceRGBBlendFactor = MTLBlendFactorOne;
				color.destinationRGBBlendFactor = MTLBlendFactorOne;
				color.sourceAlphaBlendFactor = MTLBlendFactorOne;
				color.destinationAlphaBlendFactor = MTLBlendFactorZero;
				return;
			case MAT_Alpha:
			case MAT_Font:
				color.blendingEnabled = YES;
				color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
				color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				color.sourceAlphaBlendFactor = intermediate ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
				color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				return;
			case MAT_Overlay:
				color.blendingEnabled = YES;
				color.sourceRGBBlendFactor = MTLBlendFactorOne;
				color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				color.sourceAlphaBlendFactor = MTLBlendFactorOne;
				color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				return;
		}
	}

	id<MTLRenderPipelineState> pipelineFor(ShaderProgram* program, Layout layout) {
		unsigned key = activeRenderState.baseMat | (intermediateBlend ? 8 : 0) | (layout << 4);
		auto it = program->pipelines.find(key);
		if(it != program->pipelines.end())
			return it->second;

		MTLVertexDescriptor* vertex = [MTLVertexDescriptor vertexDescriptor];
		bool usesData = false, usesDefaults = false;
		for(auto& input : program->inputs) {
			auto* attr = vertex.attributes[input.first];
			attr.bufferIndex = defaultsBuffer;
			attr.format = MTLVertexFormatFloat4;
			attr.offset = input.second * 16;
			if(layout == L_Batch) {
				switch(input.second) {
					case VI_Vertex:
						attr.bufferIndex = vertexDataBuffer;
						attr.format = MTLVertexFormatFloat3;
						attr.offset = offsetof(VertexTCV, pos);
						break;
					case VI_UV:
						attr.bufferIndex = vertexDataBuffer;
						attr.format = MTLVertexFormatFloat2;
						attr.offset = offsetof(VertexTCV, uv);
						break;
					case VI_Color:
						attr.bufferIndex = vertexDataBuffer;
						attr.format = MTLVertexFormatUChar4Normalized;
						attr.offset = offsetof(VertexTCV, col);
						break;
					default:
						break;
				}
			}
			if(attr.bufferIndex == vertexDataBuffer)
				usesData = true;
			else
				usesDefaults = true;
		}
		//Metal rejects layouts no attribute reads
		if(usesData)
			vertex.layouts[vertexDataBuffer].stride = sizeof(VertexTCV);
		if(usesDefaults) {
			vertex.layouts[defaultsBuffer].stride = 16;
			vertex.layouts[defaultsBuffer].stepFunction = MTLVertexStepFunctionConstant;
			vertex.layouts[defaultsBuffer].stepRate = 0;
		}

		MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
		desc.vertexFunction = program->vertexFunction;
		desc.fragmentFunction = program->fragmentFunction;
		desc.vertexDescriptor = vertex;
		desc.colorAttachments[0].pixelFormat = colorFormat;
		describeBlend(desc.colorAttachments[0], activeRenderState.baseMat, intermediateBlend);
		desc.depthAttachmentPixelFormat = depthFormat;

		NSError* err = nil;
		id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&err];
		if(!pipeline)
			error("Could not create a pipeline for %s: %s", program->vertexFile.c_str(), err.localizedDescription.UTF8String);
		program->pipelines[key] = pipeline;
		return pipeline;
	}

	//-- FixedFunction

	const Shader* getFixedFunction() {
		if(fixedFunction || fixedFunctionFailed)
			return (const Shader*)fixedFunction;

		std::string vertex = devices.mods.resolve("data/shaders/source/fixed_function_vs.txt");
		std::string fragment = devices.mods.resolve("data/shaders/source/fixed_function_ps.txt");
		auto* program = createShaderProgram(vertex.c_str(), fragment.c_str());
		auto* shader = createShader();
		shader->program = program;
		if(program->compile() != 0 || shader->compile() != 0) {
			error("Could not compile the FixedFunction shader; drawing without one");
			delete shader;
			delete program;
			fixedFunctionFailed = true;
			return nullptr;
		}
		fixedFunction = shader;
		return (const Shader*)fixedFunction;
	}

	//Draws the current state, which has no shader of its own, with FixedFunction
	const Shader* bindFixedFunction() {
		const RenderState& state = activeRenderState;
		int lighting = state.lighting ? 1 : 0;
		//Fixed-function GL ignored a texture that wasn't loaded yet and drew the vertex colour
		int texture = 0;
		if(Texture* tex = state.textures[0])
			if(tex->type == TT_2D && tex->loaded && tex->getID() != 0)
				texture = tex->alphaOnly ? 2 : 1;
		if(lighting != builtins.fixedLighting || texture != builtins.fixedTexture) {
			builtins.fixedLighting = lighting;
			builtins.fixedTexture = texture;
			++builtins.materialSerial;
		}
		if(texture == 0) {
			boundTextures[0] = whiteTexture;
			activeRenderState.textures[0] = whiteTexture;
		}

		const Shader* shader = getFixedFunction();
		if(shader)
			shader->bind(nullptr);
		return shader;
	}

	//-- Drawing

	//Sets up the encoder for a draw with the current state, or returns false to skip it
	bool prepareDraw(Layout layout) {
		if(activeRenderState.culling == FC_Both)
			return false;
		if(!beginPass())
			return false;

		const Shader* shader = currentShader;
		if(!shader || !shader->metalProgram() || !shader->metalProgram()->valid)
			shader = bindFixedFunction();
		if(!shader)
			return false;
		ShaderProgram* program = shader->metalProgram();

		id<MTLRenderPipelineState> pipeline = pipelineFor(program, layout);
		if(!pipeline)
			return false;
		program->updateBuiltins(builtins);

		[encoder setRenderPipelineState:pipeline];
		[encoder setDepthStencilState:depthStateFor(activeRenderState)];
		switch(activeRenderState.culling) {
			case FC_Front: [encoder setCullMode:MTLCullModeFront]; break;
			case FC_Back: [encoder setCullMode:MTLCullModeBack]; break;
			default: [encoder setCullMode:MTLCullModeNone]; break;
		}
		[encoder setTriangleFillMode:activeRenderState.drawMode == DM_Line ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];

		for(auto& slot : program->textures) {
			unsigned unit = (unsigned)slot.unit < RENDER_MAX_TEXTURES ? slot.unit : 0;
			auto* tex = dynamic_cast<MetalTexture*>(boundTextures[unit]);
			id<MTLTexture> texture = tex ? tex->texture : nil;
			id<MTLSamplerState> sampler = boundSamplers[unit];
			bool matches = texture && (texture.textureType == MTLTextureTypeCube) == slot.cube;
			if(!matches)
				texture = slot.cube ? nil : ((MetalTexture*)blackTexture)->texture;
			if(!sampler)
				sampler = samplerFor(RenderState(), false);
			if(!texture)
				continue;
			if(slot.vertexTexture >= 0) {
				[encoder setVertexTexture:texture atIndex:slot.vertexTexture];
				[encoder setVertexSamplerState:sampler atIndex:slot.vertexSampler];
			}
			if(slot.fragmentTexture >= 0) {
				[encoder setFragmentTexture:texture atIndex:slot.fragmentTexture];
				[encoder setFragmentSamplerState:sampler atIndex:slot.fragmentSampler];
			}
		}

		if(program->uniformSize > 0) {
			if(program->vertexBuffer >= 0)
				[encoder setVertexBytes:program->uniformData.data() length:program->uniformSize atIndex:program->vertexBuffer];
			if(program->fragmentBuffer >= 0)
				[encoder setFragmentBytes:program->uniformData.data() length:program->uniformSize atIndex:program->fragmentBuffer];
		}
		[encoder setVertexBuffer:defaults offset:0 atIndex:defaultsBuffer];
		return true;
	}

	//Room for <bytes> in this frame's batch memory
	id<MTLBuffer> allocate(size_t bytes, size_t& offset) {
		auto& frame = frameBuffers[frameIndex];
		bytes = (bytes + 255) & ~(size_t)255;
		if(!frame.buffers.empty() && frame.used + bytes <= frame.buffers.back().length) {
			offset = frame.used;
			frame.used += bytes;
			return frame.buffers.back();
		}
		size_t size = std::max(bytes, (size_t)8 << 20);
		frame.buffers.push_back([device newBufferWithLength:size options:MTLResourceStorageModeShared]);
		frame.used = bytes;
		offset = 0;
		return frame.buffers.back();
	}

	void drawBatch(const VertexTCV* vertices, unsigned vertexCount,
		const unsigned short* indices, unsigned indexCount,
		const RenderStep* steps, unsigned stepCount) override {
		beginFrame();

		size_t vertexBytes = vertexCount * sizeof(VertexTCV), indexBytes = indexCount * sizeof(unsigned short);
		size_t vertexOffset, indexOffset;
		id<MTLBuffer> vertexData = allocate(vertexBytes, vertexOffset);
		memcpy((unsigned char*)vertexData.contents + vertexOffset, vertices, vertexBytes);
		id<MTLBuffer> indexData = allocate(indexBytes, indexOffset);
		memcpy((unsigned char*)indexData.contents + indexOffset, indices, indexBytes);

		for(unsigned i = 0; i < stepCount; ++i) {
			auto& step = steps[i];
			shaderUniforms = step.shaderCache;
			switchToRenderState(*step.material);
			unsigned count = ((i+1) < stepCount ? steps[i+1].indexOffset : indexCount) - step.indexOffset;
			if(count == 0 || !prepareDraw(L_Batch))
				continue;

			[encoder setVertexBuffer:vertexData offset:vertexOffset atIndex:vertexDataBuffer];
			[encoder drawIndexedPrimitives:step.lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
				indexCount:count indexType:MTLIndexTypeUInt16 indexBuffer:indexData
				indexBufferOffset:indexOffset + step.indexOffset * sizeof(unsigned short)];
		}
		shaderUniforms = nullptr;
	}

	//-- Resources

	RenderMesh* createMesh(const ::Mesh& mesh) override {
		return new Mesh(mesh);
	}

	render::Shader* createShader() override {
		return new Shader();
	}

	render::ShaderProgram* createShaderProgram(const char* vertex_shader, const char* fragment_shader) override {
		return new ShaderProgram(vertex_shader, fragment_shader);
	}

	Texture* createEmptyTexture() override {
		return new Texture2D();
	}

	Texture* createEmptyCubemap() override {
		return new Cubemap();
	}

	Texture* createTexture(Image& image, bool mipmap = true, bool cachePixels = false) override {
		auto* tex = new Texture2D();
		tex->load(image, mipmap, cachePixels);
		return tex;
	}

	Texture* createRenderTarget(const vec2i& size) override {
		return new RenderTarget(size);
	}
};

void finishPendingWork() {
	((Driver*)devices.render)->finishPendingWork();
}

};

RenderDriver* createMetalDriver() {
	return new mtl::Driver();
}

};
