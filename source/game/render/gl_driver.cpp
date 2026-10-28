#include "compat/misc.h"
#include "compat/gl.h"
#include "render/driver.h"
#include "render/gl_driver.h"
#include "render/driver_base.h"
#include "render/gl_mesh.h"
#include "render/gl_shader.h"
#include "render/gl_texture.h"
#include "render/lighting.h"
#include "render/camera.h"
#include "render/gl_framebuffer.h"
#include "render/vertexBuffer.h"
#include "util/mesh_generation.h"
#include "main/references.h"
#include "main/tick.h"
#include "main/logging.h"
#include "matrix.h"
#include "frustum.h"
#include <algorithm>
#include <vector>
extern char lockText[1024];


namespace render {
unsigned drawCalls = 0;

extern const RenderMesh* lastRenderedMesh;
void glDrawBatch(const VertexTCV* vertices, unsigned vertexCount,
	const unsigned short* indices, unsigned indexCount,
	const RenderStep* steps, unsigned stepCount);
float* shaderUniforms = 0;

bool blend = false;

static inline GLint wrapMode(TextureWrap wrap) {
	switch(wrap) {
	case TW_Repeat:
		return GL_REPEAT;
	case TW_Clamp:
		//GL_CLAMP blends in the border colour at the edge; core GL calls it this
		return GL_CLAMP_TO_BORDER;
	case TW_ClampEdge:
		return GL_CLAMP_TO_EDGE;
	case TW_Mirror:
		return GL_MIRRORED_REPEAT;
	NO_DEFAULT
	}
}

static inline void setRenderFuncs(BaseMaterial mat, bool intermediate) {
	switch(mat) {
		case MAT_Solid:
			if(blend) {
				glDisable(GL_BLEND);
				blend = false;
			}
		break;
		case MAT_Add:
			if(!blend) {
				glEnable(GL_BLEND);
				blend = true;
			}
			glBlendFuncSeparate(
				GL_ONE, GL_ONE,
				GL_ONE, GL_ZERO);
			break;
		case MAT_Alpha:
		case MAT_Font:
			if(!blend) {
				glEnable(GL_BLEND);
				blend = true;
			}
			if(intermediate) {
				glBlendFuncSeparate(
					GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
					GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			}
			else {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		break;
		case MAT_Overlay:
			if(!blend) {
				glEnable(GL_BLEND);
				blend = true;
			}
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

bool isIntelCard = false;
class GLDriver : public BaseDriver {
public:
	unsigned textureStage;

	GLDriver() : textureStage(GL_TEXTURE0) {
	}

	~GLDriver() {
	}
	
	void reportErrors(const char* context = nullptr) const override {
		GLenum err = glGetError();
		while(err != GL_NO_ERROR) {
			error("Error %s: %d (0x%04x)", context ? context : "in OpenGL", err, err);
			err = glGetError();
		}
	}

	void setTextureStage(unsigned stage) {
		if(stage != textureStage) {
			glActiveTexture(stage);
			textureStage = stage;
		}
	}

	void setDefaultRenderState() {
		activeRenderState = RenderState();
		pLastRenderState = 0;

		lastRenderedMesh = 0;

		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);

		glDepthMask(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);

		setRenderFuncs(MAT_Solid, false);
		setAlphaTest(MAT_Solid);

		glUnbindProgram();

		glActiveTexture(GL_TEXTURE0);
		textureStage = GL_TEXTURE0;

		resetMaterial();
	}

	void switchToRenderState(const RenderState& state) {
		if(state.constant && pLastRenderState == &state) {
			if(state.shader && !state.shader->constant) {
				if(shaderUniforms)
					state.shader->loadDynamicVars(shaderUniforms);
				else
					state.shader->updateDynamicVars();
			}
			return;
		}

		//Front/Back face culling
		if(state.culling != activeRenderState.culling) {
			if(state.culling == FC_None) {
				glDisable(GL_CULL_FACE);
			}
			else {
				if(activeRenderState.culling == FC_None)
					glEnable(GL_CULL_FACE);

				switch(state.culling) {
					case FC_Front:
						glCullFace(GL_FRONT); break;
					case FC_Back:
						glCullFace(GL_BACK); break;
					case FC_Both:
						glCullFace(GL_FRONT_AND_BACK); break;
					NO_DEFAULT
				}
			}
		}

		//Depth write
		if(state.depthWrite != activeRenderState.depthWrite)
			glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);

		//Depth test
		if(state.depthTest != activeRenderState.depthTest) {
			if(state.depthTest == DT_NoDepthTest)
				glDisable(GL_DEPTH_TEST);
			else {
				if(activeRenderState.depthTest == DT_NoDepthTest)
					glEnable(GL_DEPTH_TEST);

				switch(state.depthTest) {
					case DT_Never:
						glDepthFunc(GL_NEVER); break;
					case DT_Less:
						glDepthFunc(GL_LESS); break;
					case DT_Equal:
						glDepthFunc(GL_EQUAL); break;
					case DT_LessEqual:
						glDepthFunc(GL_LEQUAL); break;
					case DT_Greater:
						glDepthFunc(GL_GREATER); break;
					case DT_NotEqual:
						glDepthFunc(GL_NOTEQUAL); break;
					case DT_GreaterEqual:
						glDepthFunc(GL_GEQUAL); break;
					//case DT_Always:
					//	glDepthFunc(GL_ALWAYS); break;
					NO_DEFAULT
				}
			}
		}

		//Texture states
		for(int i = 0; i < RENDER_MAX_TEXTURES; ++i) {
			if(activeRenderState.textures[i] == state.textures[i])
				continue;
			Texture* tex = state.textures[i];
			if(tex == 0)
				continue;

			setTextureStage(GL_TEXTURE0 + i);
			GLenum glType = tex->type == TT_2D ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP;
			tex->bind();

			//Wrapping settings
			bool changed = false;
			if(state.wrapHorizontal != tex->prevRenderState.wrapHorizontal) {
				glTexParameteri(glType, GL_TEXTURE_WRAP_S, wrapMode(state.wrapHorizontal));
				changed = true;
			}

			if(state.wrapVertical != tex->prevRenderState.wrapVertical) {
				glTexParameteri(glType, GL_TEXTURE_WRAP_T, wrapMode(state.wrapVertical));
				changed = true;
			}

			//Mipmap settings
			if(state.filterMin != tex->prevRenderState.filterMin) {
				GLint mode;
				switch(state.filterMin) {
				case TF_Nearest:
					mode = tex->hasMipMaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST; break;
				case TF_Linear:
					mode = tex->hasMipMaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR; break;
				NO_DEFAULT
				}
				glTexParameteri(glType, GL_TEXTURE_MIN_FILTER, mode);
				changed = true;
			}

			if(state.filterMag != tex->prevRenderState.filterMag) {
				//Magnification never uses mipmaps, a mipmap mode here is GL_INVALID_ENUM
				GLint mode;
				switch(state.filterMag) {
				case TF_Nearest:
					mode = GL_NEAREST; break;
				case TF_Linear:
					mode = GL_LINEAR; break;
				NO_DEFAULT
				}
				glTexParameteri(glType, GL_TEXTURE_MAG_FILTER, mode);
				changed = true;
			}

			//Update the previous render state
			if(changed)
				tex->prevRenderState = state;
		}

		if(state.drawMode != activeRenderState.drawMode) {
			switch(state.drawMode) {
				case DM_Line:
					glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
				break;
				case DM_Fill:
					glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
				break;
			}
		}

		if(state.baseMat != activeRenderState.baseMat) {
			setRenderFuncs(state.baseMat, intermediateBlend);
			setAlphaTest(state.baseMat);
		}

		if(state.lighting) {
			//Lighting model material settings
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
		
		//The shader bind can inspect the current material
		if(state.shader)
			state.shader->bind(shaderUniforms);
		else
			bindFixedFunction(state);
	}

	//The shader that stands in for fixed-function GL. The driver owns it, so it
	//outlives library reloads; it compiles on first use.
	Shader* fixedFunction = nullptr;
	bool fixedFunctionFailed = false;

	const Shader* getFixedFunction() {
		if(fixedFunction || fixedFunctionFailed)
			return fixedFunction;

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
		return fixedFunction;
	}

	//Draws <state>, which has no shader of its own, with the FixedFunction shader
	void bindFixedFunction(const RenderState& state) {
		int lighting = state.lighting ? 1 : 0;
		//Fixed-function GL ignored a texture that wasn't loaded yet and drew the
		//vertex colour, where sampling it in a shader gives zero
		int texture = 0;
		if(Texture* tex = state.textures[0])
			if(tex->type == TT_2D && tex->loaded && tex->getID() != 0)
				texture = tex->alphaOnly ? 2 : 1;
		if(lighting != builtins.fixedLighting || texture != builtins.fixedTexture) {
			builtins.fixedLighting = lighting;
			builtins.fixedTexture = texture;
			++builtins.materialSerial;
		}

		//FixedFunction declares its sampler either way, and drivers check what it is
		//bound to, so it gets white when it samples nothing. Unit 0 is then marked as
		//holding white, so the next state that wants its own texture there rebinds it.
		if(texture == 0) {
			setTextureStage(GL_TEXTURE0);
			getWhiteTexture()->bind();
			activeRenderState.textures[0] = getWhiteTexture();
		}

		if(auto* shader = getFixedFunction())
			shader->bind(nullptr);
		else
			glUnbindProgram();
	}

	//A 1x1 white texture for samplers with nothing to sample
	Texture* whiteTexture = nullptr;

	Texture* getWhiteTexture() {
		if(!whiteTexture) {
			Image white(1, 1, FMT_RGBA, 0xff);
			whiteTexture = createTexture(white, false);
		}
		return whiteTexture;
	}

	//Every draw needs a program; draws after setDefaultRenderState have none yet
	void prepareDraw() {
		if(!glProgramBound())
			bindFixedFunction(activeRenderState);
		glUploadBuiltins(builtins);
	}

	RenderMesh* createMesh(const Mesh& mesh) {
		return createGLMesh(mesh);
	}

	Shader* createShader() {
		return createGLShader();
	}

	ShaderProgram* createShaderProgram(const char* vertex_shader, const char* fragment_shader) {
		return createGLShaderProgram(vertex_shader, fragment_shader);
	}

	Texture* createEmptyTexture() override {
		return new GLTexture();
	}

	Texture* createEmptyCubemap() override {
		return new GLCubeMap();
	}

	Texture* createTexture(Image& image, bool mipmap = true, bool cachePixels = false) {
		return new GLTexture(image, mipmap, cachePixels);
	}

	Texture* createRenderTarget(const vec2i& size) {
		return new glFrameBuffer(size);
	}

	void clear(unsigned flags) override {
		GLbitfield bits = 0;
		if(flags & CF_Color)
			bits |= GL_COLOR_BUFFER_BIT;
		//If we need to clear the depth buffer, we need to enable the depth buffer for writing
		if(flags & CF_Depth) {
			bits |= GL_DEPTH_BUFFER_BIT;
			if(!activeRenderState.depthWrite) {
				glDepthMask(GL_TRUE);
				activeRenderState.depthWrite = true;
				pLastRenderState = 0;
			}
		}
		glClear(bits);
#ifdef _DEBUG
		reportErrors("Clearing");
#endif
	}

	void setRenderTarget(Texture* texture, bool intermediate = false) {
		renderVertexBuffers();

		glFrameBuffer* frame = dynamic_cast<glFrameBuffer*>(texture);
		if(!frame) {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			frameSize = framebufferSize.x > 0 ? framebufferSize : screenSize;
			glViewport(0,0,frameSize.x,frameSize.y);
		}
		else {
			frame->setAsTarget();
			clear(CF_Color | CF_Depth);
			frameSize = frame->size;
		}
		if(intermediate != intermediateBlend)
			setRenderFuncs(activeRenderState.baseMat, intermediate);
		intermediateBlend = intermediate;
	}

	Image* getScreen(int x, int y, int w, int h) {
		Image* img = new Image(w, h, FMT_RGB);

		glReadBuffer(GL_BACK);
		glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, img->rgb);

		return img;
	}

	bool init() {
		//Check for sufficient opengl version
		GLint major = 0, minor = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &major);
		glGetIntegerv(GL_MINOR_VERSION, &minor);
		if(major < 4 || (major == 4 && minor < 1)) {
			fprintf(stderr, "Error: Requires OpenGL 4.1\n");
			return false;
		}

		//Initialize state
		setDefaultRenderState();

		initLights();

		glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

		//Support strange sizes of image
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		//Report vendor
		const char* vendorString = (const char*)glGetString(GL_VENDOR);
		const char* rendererString = (const char*)glGetString(GL_RENDERER);
		const char* versionString = (const char*)glGetString(GL_VERSION);
		isIntelCard = vendorString != nullptr && std::string(vendorString) == "Intel";
		print("OpenGL vendor '%s', renderer '%s'", vendorString, rendererString);
		print("       version '%s'", versionString);
		if(isIntelCard)
			print("-- Using Intel mode.");

		return true;
	}

	void drawBatch(const VertexTCV* vertices, unsigned vertexCount,
		const unsigned short* indices, unsigned indexCount,
		const RenderStep* steps, unsigned stepCount) override {
		glDrawBatch(vertices, vertexCount, indices, indexCount, steps, stepCount);
	}

	void applyScissor(const recti* box) override {
		if(!box) {
			glDisable(GL_SCISSOR_TEST);
			return;
		}
		glEnable(GL_SCISSOR_TEST);
		const recti& clip = *box;
		double x = (double)clip.topLeft.x / (double)screenSize.width;
		double y = (double)(screenSize.height - clip.botRight.y) / (double)screenSize.height;
		double w = clip.getWidth() / (double)screenSize.width;
		double h = clip.getHeight() / (double)screenSize.height;

		glScissor((int)(x * frameSize.x), (int)(y * frameSize.y), (int)(w * frameSize.x), (int)(h * frameSize.y));
	}

};

RenderDriver* createGLDriver() {
	return new GLDriver();
}

void glPrepareDraw() {
	((GLDriver*)devices.render)->prepareDraw();
}

};

void shader_tex_size(float* out,unsigned short n,void* args) {
	vec2f* sizes = (vec2f*)out;
	auto* texs = ((render::BaseDriver*)devices.render)->activeRenderState.textures;
	unsigned* texIndices = (unsigned*)args;
	for(unsigned short i = 0; i < n; ++i) {
		unsigned index = texIndices[i];
		if(index >= RENDER_MAX_TEXTURES)
			new(sizes+i) vec2f(0);
		else if(render::Texture* tex = texs[index])
			new(sizes+i) vec2f(tex->size);
		else
			new(sizes+i) vec2f(0);
	}
}

void shader_light_radius(float* out,unsigned short n,void* args) {
	auto* radii = ((render::BaseDriver*)devices.render)->lightRadius;

	unsigned* indices = (unsigned*)args;
	for(unsigned short i = 0; i < n; ++i) {
		unsigned index = indices[i];
		if(index < 2)
			out[i] = radii[index];
		else
			out[i] = 0.f;
	}
}

void shader_light_position(float* out,unsigned short n,void* args) {
	auto* positions = ((render::BaseDriver*)devices.render)->lightPosition;

	unsigned* indices = (unsigned*)args;
	for(unsigned short i = 0; i < n; ++i) {
		unsigned index = indices[i];
		if(index < 2) {
			out[i*3+0] = positions[index].x;
			out[i*3+1] = positions[index].y;
			out[i*3+2] = positions[index].z;
		}
		else {
			out[i*3+0] = 0.f;
			out[i*3+1] = 0.f;
			out[i*3+2] = 0.f;
		}
	}
}

void shader_light_screen(float* out,unsigned short n,void* args) {
	auto* positions = ((render::BaseDriver*)devices.render)->screenLight;

	unsigned* indices = (unsigned*)args;
	for(unsigned short i = 0; i < n; ++i) {
		unsigned index = indices[i];
		if(index < 2) {
			out[i*2+0] = positions[index].x;
			out[i*2+1] = positions[index].y;
		}
		else {
			out[i*2+0] = 0.f;
			out[i*2+1] = 0.f;
		}
	}
}

void shader_light_active(float* out,unsigned short n,void* args) {
	auto* active = ((render::BaseDriver*)devices.render)->lightActive;

	unsigned* indices = (unsigned*)args;
	for(unsigned short i = 0; i < n; ++i) {
		unsigned index = indices[i];
		if(index < 2)
			out[i] = active[index] ? 1.f : 0.f;
		else
			out[i] = 0.f;
	}
}

void setShaderLightRadius(unsigned index, double radius) {
	auto* active = ((render::BaseDriver*)devices.render)->lightRadius;
	if(index < 2)
		active[index] = radius;
}
