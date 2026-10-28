#include "render/mtl/mtl_driver.h"
#include "render/driver_base.h"
#include "render/shader.h"
#include "main/references.h"
#include "main/logging.h"
#include "mesh.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

namespace render {

//Frames the CPU may queue ahead of the GPU
static const unsigned framesInFlight = 3;

//Placeholders until each resource type has its Metal implementation
class MtlTexture : public Texture {
public:
	MtlTexture(TextureType Type = TT_2D) {
		type = Type;
		loaded = false;
		hasMipMaps = false;
	}

	bool isPixelActive(vec2i px) const override { return true; }
	void loadStart(Image& image, bool mipmap, bool cachePixels, unsigned lod) override {
		if(lod == 0)
			size = vec2i(image.width, image.height);
		loaded = true;
	}
	void loadPartial(Image& image, const recti& pixels, bool cachePixels, unsigned lod) override {}
	void loadFinish(bool mipmap, unsigned lod) override { hasMipMaps = mipmap; }
	void load(Image& image, bool mipmap, bool cachePixels) override {
		loadStart(image, mipmap, cachePixels, 0);
		loadFinish(mipmap, 0);
	}
	void save(Image& image) const override {}
	void bind() override {}
	unsigned getID() const override { return 0; }
	unsigned getTextureBytes() const override { return 0; }
};

class MtlMesh : public RenderMesh {
	Mesh mesh;
	AABBoxf bbox;
	double lodDistance = 1;
	const RenderMesh* lod = nullptr;
public:
	MtlMesh(const Mesh& m) {
		resetToMesh(m);
	}

	void resetToMesh(const Mesh& m) override {
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
	const Mesh& getMesh() const override { return mesh; }
	unsigned getMeshBytes() const override { return 0; }
	void render() const override {}
};

class MtlShaderProgram : public ShaderProgram {
public:
	int compile() override { return 0; }
};

class MtlShader : public Shader {
public:
	MtlShader() {
		program = nullptr;
		constant = true;
		dynamicFloats = 0;
	}
	int compile() override { return 0; }
	void bind(float* dynamicBuffer) const override {}
	void updateDynamicVars() const override {}
	void saveDynamicVars(float* buffer) const override {}
	void loadDynamicVars(float* buffer) const override {}
};

class MetalDriver : public BaseDriver {
public:
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	CAMetalLayer* layer = nil;
	dispatch_semaphore_t frameSlots;

	//The frame being recorded
	id<MTLCommandBuffer> commandBuffer = nil;
	id<CAMetalDrawable> drawable = nil;
	id<MTLRenderCommandEncoder> encoder = nil;
	//Whether the next pass starts by clearing the screen
	bool clearPending = true;

	int verticalSync = 1;

	MetalDriver() {
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
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		//Screenshots and captures read the drawable back
		layer.framebufferOnly = NO;
		layer.maximumDrawableCount = framesInFlight;
		layer.displaySyncEnabled = verticalSync != 0;
		layer.contentsScale = window.backingScaleFactor;
		NSView* view = window.contentView;
		view.wantsLayer = YES;
		view.layer = layer;

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

	//Starts recording a frame, and a pass into its drawable, if they aren't already
	void beginPass() {
		if(encoder)
			return;
		if(!commandBuffer) {
			dispatch_semaphore_wait(frameSlots, DISPATCH_TIME_FOREVER);
			commandBuffer = [queue commandBuffer];
		}
		if(!drawable)
			drawable = [layer nextDrawable];
		if(!drawable)
			return;

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = clearPending ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
		clearPending = false;
	}

	void endPass() {
		if(encoder) {
			[encoder endEncoding];
			encoder = nil;
		}
	}

	void presentFrame() override {
		renderVertexBuffers();
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
		clearPending = true;
	}

	void clear(unsigned flags) override {
		if(flags & CF_Color) {
			endPass();
			clearPending = true;
		}
	}

	void applyScissor(const recti* clip) override {}

	void setDefaultRenderState() override {
		activeRenderState = RenderState();
		pLastRenderState = 0;
		setAlphaTest(MAT_Solid);
		resetMaterial();
	}

	void switchToRenderState(const RenderState& state) override {
		if(state.baseMat != activeRenderState.baseMat)
			setAlphaTest(state.baseMat);
		activeRenderState = state;
		pLastRenderState = &state;
	}

	void drawBatch(const VertexTCV* vertices, unsigned vertexCount,
		const unsigned short* indices, unsigned indexCount,
		const RenderStep* steps, unsigned stepCount) override {
	}

	void setRenderTarget(Texture* texture, bool intermediate = false) override {
		renderVertexBuffers();
		frameSize = texture ? texture->size : (framebufferSize.x > 0 ? framebufferSize : screenSize);
		intermediateBlend = intermediate;
	}

	Image* getScreen(int x, int y, int w, int h) override {
		return new Image(w, h, FMT_RGB, 0);
	}

	RenderMesh* createMesh(const Mesh& mesh) override {
		return new MtlMesh(mesh);
	}

	Shader* createShader() override {
		return new MtlShader();
	}

	ShaderProgram* createShaderProgram(const char* vertex_shader, const char* fragment_shader) override {
		return new MtlShaderProgram();
	}

	Texture* createEmptyTexture() override {
		return new MtlTexture(TT_2D);
	}

	Texture* createEmptyCubemap() override {
		return new MtlTexture(TT_Cubemap);
	}

	Texture* createTexture(Image& image, bool mipmap = true, bool cachePixels = false) override {
		auto* tex = new MtlTexture(TT_2D);
		tex->load(image, mipmap, cachePixels);
		return tex;
	}

	Texture* createRenderTarget(const vec2i& size) override {
		auto* tex = new MtlTexture(TT_2D);
		tex->size = size;
		tex->loaded = true;
		return tex;
	}
};

RenderDriver* createMetalDriver() {
	return new MetalDriver();
}

};
