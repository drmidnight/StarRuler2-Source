#pragma once
#include "render/driver.h"
#include "render/vertexBuffer.h"
#include "render/lighting.h"
#include "render/camera.h"
#include "util/mesh_generation.h"
#include "main/tick.h"
#include "compat/misc.h"
#include "frustum.h"
#include <stack>
#include <vector>

namespace render {

//Flags for BaseDriver::clear
enum ClearFlags {
	CF_Color = 1,
	CF_Depth = 2,
};

//Whether fragments of <mat> go through the alpha test
inline bool usesAlphaTest(BaseMaterial mat) {
	return mat == MAT_Alpha || mat == MAT_Font || mat == MAT_Overlay;
}

//The parts of a render driver that don't depend on the graphics API: transforms,
//camera data, lights and the other built-in state, and drawing through the vertex
//batcher. Backends supply state changes, clears, scissors, targets and resources.
class BaseDriver : public RenderDriver {
public:
	const RenderState* pLastRenderState;
	const RenderState* skybox;
	const RenderMesh* skyboxMesh;
	bool cleared;
	bool intermediateBlend;
	vec2i screenSize, frameSize, framebufferSize;
	double fov;
	double zNear, zFar;

	std::stack<recti> viewportClips;

	frustum viewFrustum;
	vec3d cam_right,
		bb_up_p_right, bb_up_m_right;
	quaterniond BBFacingRot;

	//Everything shaders read through gl_* built-ins. Material ambient is set once
	//with the default diffuse, and never changes.
	BuiltinState builtins;

	RenderState state_3d;
	RenderState state_2d;

	float invView[9];

	//The driver owns the transforms; backends only ever get them whole.
	//The model-view stack's top is the current transform, relative to the camera.
	std::vector<Matrix> modelView;

	vec3f lightPosition[2];
	vec2f screenLight[2];
	double lightRadius[2];
	bool lightActive[2];
	bool isPrepared;

	BaseDriver() : pLastRenderState(0), skybox(0), skyboxMesh(0),
		cleared(false), intermediateBlend(false), fov(50.0), zNear(1.0), zFar(240000.0),
		isPrepared(false)
	{
		state_2d.lighting = false;
		state_2d.depthTest = DT_NoDepthTest;
		state_2d.baseMat = MAT_Alpha;
		state_2d.culling = FC_None;
		modelView.push_back(Matrix());
	}

	//The material state of the default render state
	void resetMaterial() {
		builtins.materialDiffuse = Colorf(1.f, 1.f, 1.f, 1.f);
		builtins.materialAmbient = builtins.materialDiffuse;
		builtins.materialSpecular = Colorf(1.f, 1.f, 1.f, 1.f);
		builtins.materialShininess = 8.f;
		++builtins.materialSerial;
	}

	//The lights the game starts with, until a scene sets its own
	void initLights() {
		builtins.lights[0].enabled = true;
		builtins.lights[0].diffuse = Colorf(2.5f, 2.5f, 2.5f, 1.f);
		builtins.lights[0].specular = Colorf(1.f, 1.f, 1.f, 1.f);
		builtins.lights[0].quadraticAttenuation = 1.f/(500.f*500.f);

		builtins.lights[1].enabled = true;
		builtins.lights[1].diffuse = Colorf(0.f, 0.f, 0.f, 1.f);
		builtins.lights[1].specular = Colorf(0.f, 0.f, 0.f, 1.f);
		builtins.lights[1].quadraticAttenuation = 1.f/(500.f*500.f);

		builtins.lightModelAmbient = Colorf(0.175f, 0.175f, 0.175f, 0.f);
		++builtins.lightSerial;
	}

	//Clears the current target, by ClearFlags
	virtual void clear(unsigned flags) = 0;
	//Limits drawing to <clip> in screen coordinates, or stops limiting it when null
	virtual void applyScissor(const recti* clip) = 0;

	const RenderState* getLastRenderState() const override {
		return pLastRenderState ? pLastRenderState : &activeRenderState;
	}

	const frustum& getViewFrustum() const override {
		return viewFrustum;
	}

	void getInverseView(float* mat3) const override {
		memcpy(mat3, invView, 9 * sizeof(float));
	}

	void setCameraData(Camera& camera) override {
		cam_pos = camera.getPosition();
		cam_facing = camera.getFacing();
		cam_up = camera.getUp();
		cam_right = camera.getRight();
		
		bb_up_p_right = cam_up - cam_right;
		bb_up_m_right = cam_up + cam_right;

		vec3d f = cam_facing;
		vec3d s = f.cross(cam_up);
		vec3d u = s.cross(f);

		//s0 s1 s2 0
		//u0 u1 u2 0
		//-f0 -f1 -f2 0
		//0 0 0 1

		invView[0] = s.x;
		invView[1] = u.x;
		invView[2] = -f.x;
		invView[3] = s.y;
		invView[4] = u.y;
		invView[5] = -f.y;
		invView[6] = s.z;
		invView[7] = u.z;
		invView[8] = -f.z;

		auto yaw = quaterniond::fromAxisAngle(vec3d::up(-1.0), atan2(cam_facing.z, cam_facing.x));
		auto pitch = quaterniond::fromAxisAngle(vec3d::right(), asin(-cam_facing.y));

		BBFacingRot = yaw * pitch;

		viewFrustum = frustum(camera.screenToRay(0,0), camera.screenToRay(1,0), camera.screenToRay(0,1), camera.screenToRay(1,1));
	}

	void loadModelView() {
		builtins.modelView = modelView.back();
		++builtins.matrixSerial;
	}

	void pushModelView(const Matrix& matrix) {
		modelView.push_back(matrix);
		loadModelView();
	}

	void setProjection(const Matrix& matrix) {
		builtins.projection = matrix;
		++builtins.matrixSerial;
	}

	//Starts a new model-view stack from <view>
	void setView(const Matrix& view) {
		modelView.clear();
		modelView.push_back(view);
		loadModelView();
	}

	void setTransformation(const Matrix& matrix) override {
		Matrix temp(matrix);
		temp[12] -= cam_pos.x;
		temp[13] -= cam_pos.y;
		temp[14] -= cam_pos.z;
		pushModelView(modelView.back() * temp);
	}

	void setTransformationAbs(const Matrix& matrix) override {
		pushModelView(modelView.back() * matrix);
	}

	void setTransformationIdentity() override {
		pushModelView(Matrix());
	}

	void setBBTransform(vec3d pos, double width, double rot) override {
		Matrix m;
		//BBFacingRot.toTransform(m, pos - cam_pos, vec3d(width * 0.5));
		(quaterniond::fromAxisAngle(cam_facing, -rot) * BBFacingRot).toTransform(m, pos - cam_pos, vec3d(width * 0.5));
		pushModelView(modelView.back() * m);
	}

	void resetTransformation() override {
		if(modelView.size() > 1)
			modelView.pop_back();
		loadModelView();
	}

	void set2DRenderState() override {
		switchToRenderState(state_2d);
	}

	void setAlphaTest(BaseMaterial mat) {
		int test = usesAlphaTest(mat) ? 1 : 0;
		if(test != builtins.alphaTest) {
			builtins.alphaTest = test;
			++builtins.materialSerial;
		}
	}

	void getBillboardVecs(vec3d& upLeft, vec3d& upRight, double rotation) const override {
		if(rotation == 0) {
			upLeft = bb_up_m_right;
			upRight = bb_up_p_right;
		}
		else {
			double st = sin(rotation), ct = cos(rotation);
			upLeft = (bb_up_m_right * ct) - (bb_up_p_right * st);
			upRight = (bb_up_p_right * ct) + (bb_up_m_right * st);
		}
	}

	void getBillboardVecs(const vec3d& from, vec3d& upLeft, vec3d& upRight, double rotation) const override {
		vec3d toward = (from - cam_pos).normalize();
		vec3d right = cam_up.cross(toward).normalize();
		vec3d up = toward.cross(right);

		vec3d ul = up - right;
		vec3d ur = up + right;

		if(rotation == 0) {
			upLeft = ul;
			upRight = ur;
		}
		else {
			double st = sin(rotation), ct = cos(rotation);
			upLeft = (ul * ct) - (ur * st);
			upRight = (ur * ct) + (ul * st);
		}
	}

	void drawBillboard(vec3d center, double width) override {
		center -= cam_pos;
		width *= 0.5;
		
		vec3d UpMinRight, UpPlusRight;
		getBillboardVecs(UpMinRight, UpPlusRight, 0);
		UpMinRight *= width;
		UpPlusRight *= width;

		auto& mat = *pLastRenderState;
		auto* buffer = VertexBufferTCV::fetch(&mat);
		auto* verts = buffer->request(1, PT_Quads);

		Color col = Color();
		
		verts[0].set(vec3f(center + UpMinRight), vec2f(0,0), col);
		verts[1].set(vec3f(center + UpPlusRight), vec2f(1,0), col);
		verts[2].set(vec3f(center - UpMinRight), vec2f(1,1), col);
		verts[3].set(vec3f(center - UpPlusRight), vec2f(0,1), col);
	}

	void drawBillboard(vec3d center, double width, const RenderState& mat, double rotation, Color* color) override {
		center -= cam_pos;
		width *= 0.5;

		vec3d UpMinRight, UpPlusRight;
		getBillboardVecs(UpMinRight, UpPlusRight, rotation);
		UpMinRight *= width;
		UpPlusRight *= width;

		auto* buffer = VertexBufferTCV::fetch(&mat);
		auto* verts = buffer->request(1, PT_Quads);

		Color col = color ? *color : Color();
		
		verts[0].set(vec3f(center + UpMinRight), vec2f(0,0), col);
		verts[1].set(vec3f(center + UpPlusRight), vec2f(1,0), col);
		verts[2].set(vec3f(center - UpMinRight), vec2f(1,1), col);
		verts[3].set(vec3f(center - UpPlusRight), vec2f(0,1), col);
	}

	void drawBillboard( vec3d center, double width, const RenderState& mat, const recti& source, Color* color) override {
		rectf texCoords;
		Texture* tex = mat.textures[0];
		if(tex) {
			texCoords.topLeft.x = (float)source.topLeft.x / tex->size.width;
			texCoords.topLeft.y = (float)source.topLeft.y / tex->size.height;
			texCoords.botRight.x = (float)source.botRight.x / tex->size.width;
			texCoords.botRight.y = (float)source.botRight.y / tex->size.height;
		}

		center -= cam_pos;
		width *= 0.5;

		auto UpMinRight = bb_up_m_right * width, UpPlusRight = bb_up_p_right * width;

		Color col = color ? *color : Color();

		auto* buffer = VertexBufferTCV::fetch(&mat);
		auto* verts = buffer->request(1, PT_Quads);
		
		verts[0].set(vec3f(center + UpMinRight), texCoords.topLeft, col);
		verts[1].set(vec3f(center + UpPlusRight), texCoords.getTopRight(), col);
		verts[2].set(vec3f(center - UpMinRight), texCoords.botRight, col);
		verts[3].set(vec3f(center - UpPlusRight), texCoords.getBotLeft(), col);
	}

	void drawBillboard( vec3d center, double width, const RenderState& mat, const recti& source, double rotation, Color color) override {
		rectf texCoords;
		Texture* tex = mat.textures[0];
		if(tex) {
			texCoords.topLeft.x = (float)source.topLeft.x / tex->size.width;
			texCoords.topLeft.y = (float)source.topLeft.y / tex->size.height;
			texCoords.botRight.x = (float)source.botRight.x / tex->size.width;
			texCoords.botRight.y = (float)source.botRight.y / tex->size.height;
		}

		center -= cam_pos;
		width *= 0.5;

		vec3d UpMinRight, UpPlusRight;
		getBillboardVecs(UpMinRight, UpPlusRight, rotation);
		UpMinRight *= width;
		UpPlusRight *= width;

		auto* buffer = VertexBufferTCV::fetch(&mat);
		auto* verts = buffer->request(1, PT_Quads);
		
		verts[0].set(vec3f(center + UpMinRight), texCoords.topLeft, color);
		verts[1].set(vec3f(center + UpPlusRight), texCoords.getTopRight(), color);
		verts[2].set(vec3f(center - UpMinRight), texCoords.botRight, color);
		verts[3].set(vec3f(center - UpPlusRight), texCoords.getBotLeft(), color);

		if(!mat.constant)
			buffer->draw();
	}

	void drawLine(line3dd line, Color start, Color end) override {
		auto* buffer = VertexBufferTCV::fetch(pLastRenderState);
		auto* verts = buffer->request(1, PT_Lines);
		
		verts[0].set(vec3f(line.start - cam_pos), vec2f(), start);
		verts[1].set(vec3f(line.end - cam_pos), vec2f(1,0), start);

		if(!pLastRenderState->constant)
			buffer->draw();
	}

	void drawQuad(
		const RenderState* mat,
		const vec2<float>* vertices,
		const vec2<float>* textureCoords,
		const Color* color = 0) override
	{
		auto* buffer = VertexBufferTCV::fetch(mat);
		auto* verts = buffer->request(1, PT_Quads);

		Color col = color ? *color : Color();

		for(unsigned i = 0; i < 4; ++i) {
			auto& v = verts[i];
			v.uv = textureCoords[i];
			v.col = col;
			v.pos = vec3f(vertices[i].x, vertices[i].y, 0);
		}

		if(!mat->constant)
			buffer->draw();
	}

	void drawQuad(const vec3d* vertices,
				  const vec2<float>* textureCoords,
				  const Color* colors) override
	{
		auto* buffer = VertexBufferTCV::fetch(pLastRenderState);
		auto* verts = buffer->request(1, PT_Quads);

		for(unsigned i = 0; i < 4; ++i) {
			auto& v = verts[i];
			v.uv = textureCoords[i];
			v.col = colors[i];
			v.pos = vec3f(vertices[i] - cam_pos);
		}

		if(!pLastRenderState->constant)
			buffer->draw();
	}

	void drawQuad(const vec2<float>* vertices,
				  const vec2<float>* textureCoords,
				  const Color* colors) override
	{
		auto* buffer = VertexBufferTCV::fetch(pLastRenderState);
		auto* verts = buffer->request(1, PT_Quads);

		for(unsigned i = 0; i < 4; ++i) {
			auto& v = verts[i];
			v.uv = textureCoords[i];
			v.col = colors ? colors[i] : Color();
			v.pos = vec3f(vertices[i].x, vertices[i].y, 0);
		}

		if(!pLastRenderState->constant)
			buffer->draw();
	}

	void drawRectangle(const recti& rect, const Color& color) {
		auto* buffer = VertexBufferTCV::fetch(&state_2d);
		auto* verts = buffer->request(1, PT_Quads);

		auto& tl = verts[0];
		tl.uv.set(0,0);
		tl.col = color;
		tl.pos = vec3f(vec3i(rect.topLeft.x, rect.topLeft.y, 0));
			
		auto& tr = verts[1];
		tr.uv.set(0,0);
		tr.col = color;
		tr.pos = vec3f(vec3i(rect.botRight.x, rect.topLeft.y, 0));
			
		auto& br = verts[2];
		br.uv.set(0,0);
		br.col = color;
		br.pos = vec3f(vec3i(rect.botRight.x, rect.botRight.y, 0));
			
		auto& bl = verts[3];
		bl.uv.set(0,0);
		bl.col = color;
		bl.pos = vec3f(vec3i(rect.topLeft.x, rect.botRight.y, 0));
	}

	void drawRectangle(const recti& rectangle, const RenderState* mat, Color color, const recti* clip = 0) override {
		if(clip && !clip->overlaps(rectangle))
			return;

		if(!mat)
			mat = &state_2d;

		recti rect = rectangle;
		rectf uv(0,0,1,1);

		if(clip && !clip->isRectInside(rect)) {
			rect = clip->clipAgainst(rect);
			uv.topLeft.x =  (float)(rect.topLeft.x - rectangle.topLeft.x) / (float)rectangle.getWidth();
			uv.topLeft.y =  (float)(rect.topLeft.y - rectangle.topLeft.y) / (float)rectangle.getHeight();
			uv.botRight.x = 1.f - (float)(rectangle.botRight.x - rect.botRight.x) / (float)rectangle.getWidth();
			uv.botRight.y = 1.f - (float)(rectangle.botRight.y - rect.botRight.y) / (float)rectangle.getHeight();
		}

		auto* buffer = VertexBufferTCV::fetch(mat);
		auto* verts = buffer->request(1, PT_Quads);

		auto& tl = verts[0];
		tl.uv = uv.topLeft;
		tl.col = color;
		tl.pos = vec3f(vec3i(rect.topLeft.x, rect.topLeft.y, 0));
			
		auto& tr = verts[1];
		tr.uv.set(uv.botRight.x,uv.topLeft.y);
		tr.col = color;
		tr.pos = vec3f(vec3i(rect.botRight.x, rect.topLeft.y, 0));
			
		auto& br = verts[2];
		br.uv = uv.botRight;
		br.col = color;
		br.pos = vec3f(vec3i(rect.botRight.x, rect.botRight.y, 0));
			
		auto& bl = verts[3];
		bl.uv.set(uv.topLeft.x,uv.botRight.y);
		bl.col = color;
		bl.pos = vec3f(vec3i(rect.topLeft.x, rect.botRight.y, 0));

		if(!mat->constant)
			buffer->draw();
	}

	void drawRectangle(recti rect, const RenderState* mat, const recti* src,
						const Color* color, const recti* clip) override {
		//Store source rect
		recti source;
		if(src)
			source = *src;

		//Clipping
		if(clip) {
			if(!clip->overlaps(rect))
				return;
			if(!clip->isRectInside(rect)) {
				recti clipped = clip->clipAgainst(rect);
				if(src) {
					source = source.clipProportional(rect, clipped);
				}
				else if(mat && mat->textures[0]) {
					source = recti(vec2i(0, 0), mat->textures[0]->size);
					source = source.clipProportional(rect, clipped);
					src = &source;
				}
				rect = clipped;
			}
		}

		if(!mat)
			mat = &state_2d;

		rectf uv(0,0,1.f,1.f);
		if(mat && src && mat->textures[0]) {
			Texture* tex = mat->textures[0];
			uv.topLeft.x = (float)source.topLeft.x / tex->size.width;
			uv.topLeft.y = (float)source.topLeft.y / tex->size.height;
			uv.botRight.x = (float)source.botRight.x / tex->size.width;
			uv.botRight.y = (float)source.botRight.y / tex->size.height;
		}

		auto* buffer = VertexBufferTCV::fetch(mat);
		auto* verts = buffer->request(1, PT_Quads);

		auto& tl = verts[0];
		tl.uv = uv.topLeft;
		tl.col = color ? color[0] : Color();
		tl.pos = vec3f(vec3i(rect.topLeft.x, rect.topLeft.y, 0));
			
		auto& tr = verts[1];
		tr.uv.set(uv.botRight.x,uv.topLeft.y);
		tr.col = color ? color[1] : Color();
		tr.pos = vec3f(vec3i(rect.botRight.x, rect.topLeft.y, 0));
			
		auto& br = verts[2];
		br.uv = uv.botRight;
		br.col = color ? color[2] : Color();
		br.pos = vec3f(vec3i(rect.botRight.x, rect.botRight.y, 0));
			
		auto& bl = verts[3];
		bl.uv.set(uv.topLeft.x,uv.botRight.y);
		bl.col = color ? color[3] : Color();
		bl.pos = vec3f(vec3i(rect.topLeft.x, rect.botRight.y, 0));

		if(!mat->constant)
			buffer->draw();
	}

	void drawRectangle(recti rect, const RenderState* mat, const recti* src,
						const Color* color, const recti* clip, double rotation) override {
		if(rotation == 0.0) {
			drawRectangle(rect, mat, src, color, clip);
			return;
		}

		//Store source rect
		recti source;
		if(src)
			source = *src;

		//Clipping
		if(clip) {
			if(!clip->overlaps(rect))
				return;
			if(!clip->isRectInside(rect)) {
				recti clipped = clip->clipAgainst(rect);
				if(src) {
					source = source.clipProportional(rect, clipped);
				}
				else if(mat && mat->textures[0]) {
					source = recti(vec2i(0, 0), mat->textures[0]->size);
					source = source.clipProportional(rect, clipped);
					src = &source;
				}
				rect = clipped;
			}
		}

		//Render the correct material
		if(!mat)
			mat = &state_2d;

		//Compute texture coordinates
		rectf uv(0,0,1.f,1.f);
		if(mat && src && mat->textures[0]) {
			Texture* tex = mat->textures[0];
			uv.topLeft.x = (float)source.topLeft.x / tex->size.width;
			uv.topLeft.y = (float)source.topLeft.y / tex->size.height;
			uv.botRight.x = (float)source.botRight.x / tex->size.width;
			uv.botRight.y = (float)source.botRight.y / tex->size.height;
		}

		//Handle rotations
		rectf frect = rectf(rect);
		vec2f center = vec2f(frect.getCenter());
		rectf preRot = rectf(frect.topLeft - center, frect.botRight - center);
		vec2f pos;

		auto* buffer = VertexBufferTCV::fetch(mat);
		auto* verts = buffer->request(1, PT_Quads);

		auto& tl = verts[0];
		tl.uv = uv.topLeft;
		tl.col = color ? color[0] : Color();

		pos = preRot.topLeft.rotated(rotation) + center;
		tl.pos = vec3f(pos.x, pos.y, 0);
			
		auto& tr = verts[1];
		tr.uv = uv.getTopRight();
		tr.col = color ? color[1] : Color();

		pos = preRot.getTopRight().rotated(rotation) + center;
		tr.pos = vec3f(pos.x, pos.y, 0);
			
		auto& br = verts[2];
		br.uv = uv.botRight;
		br.col = color ? color[3] : Color();

		pos = preRot.botRight.rotated(rotation) + center;
		br.pos = vec3f(pos.x, pos.y, 0);
			
		auto& bl = verts[3];
		bl.uv = uv.getBotLeft();
		bl.col = color ? color[2] : Color();

		pos = preRot.getBotLeft().rotated(rotation) + center;
		bl.pos = vec3f(pos.x, pos.y, 0);

		if(!mat->constant)
			buffer->draw();
	}

	void clearScreen() override {
		clear(CF_Color | CF_Depth);
	}

	void setScreenSize(int w, int h) override {
		screenSize.x = w;
		screenSize.y = h;
		frameSize = screenSize;
	}

	void setFramebufferSize(int w, int h) override {
		framebufferSize.x = w;
		framebufferSize.y = h;
	}

	void setFOV(double FOV) override {
		fov = FOV;
	}

	void setNearFarPlanes(double near, double far) override {
		zNear = near;
		zFar = far;
	}

	void clearRenderPrepared() override {
		isPrepared = false;
	}

	bool isRenderPrepared() override {
		return isPrepared;
	}

	void prepareRender3D(Camera& camera, const recti* clip) override {
		//Cache camera calculations
		setCameraData(camera);
		cleared = true;
		isPrepared = true;

		if(clip)
			pushScreenClip(*clip);

		//Draw 3D
		double aspect = ((double)screenSize.x)/((double)screenSize.y);
		setProjection(Matrix::projection(fov, aspect, zNear, zFar));
		camera.setRenderConstraints(zNear, zFar, fov, aspect, (double)screenSize.x, (double)screenSize.y);
		{

			vec3d pos, at, lookDir, up;
			camera.toLookAt(pos, at, up);

			lookDir = (at - pos).normalized();

			setView(Matrix::lookAt(vec3d(), lookDir, up));

			{ //Setup lights in the scene
				vec3f camPosf(float(pos.x),float(pos.y),float(pos.z));
				vec3f lightOffset = camPosf * -1.f;

				setLight(0, nullptr);
				light::LightSource* sources[2];
				unsigned lightCount = light::findNearestLights(camPosf,sources,2);

				unsigned lightIndex = 0;
				lightActive[0] = false;
				lightActive[1] = false;

				while(lightIndex < lightCount) {
					lightPosition[lightIndex] = sources[lightIndex]->getPosition();
					lightRadius[lightIndex] = sources[lightIndex]->getRadius();

					vec2i onScreen = camera.screenPos(vec3d(lightPosition[lightIndex]));
					screenLight[lightIndex] = vec2f((float)onScreen.x / (float)screenSize.x, 1.f - (float)onScreen.y / (float)screenSize.y);
					lightActive[lightIndex] = true;

					sources[lightIndex]->enable(lightIndex, lightOffset);
				}
			}

			//Render Skybox
			if(skybox) {
				clear(CF_Depth);
				switchToRenderState(*skybox);

				if(skyboxMesh == 0) {
					Mesh* mesh = generateSphereMesh(64,32);
					skyboxMesh = createMesh(*mesh);
					delete mesh;
				}
				
				//The skybox ignores depth, so its radius only has to keep it between the
				//clip planes. Halfway to the far plane its depth is (far-near)/(far+near),
				//which rounds to the far plane once far reaches the millions; drivers that
				//clip strictly (Apple's) then drop the background on random frames while
				//zooming. Keep it close to the near plane where depth precision is ample.
				double skyRadius = (zNear + zFar) * 0.5;
				if(zFar > zNear * 16.0)
					skyRadius = zNear * 8.0;
				pushModelView(modelView.back() * Matrix::scale(skyRadius, skyRadius, skyRadius));
				skyboxMesh->render();
				resetTransformation();
			}
			else {
				clear(CF_Color | CF_Depth);
			}
		}
	}

	void renderWorld() override {
		rootNode._render(*this);
		scene::renderingNode = nullptr;
		renderVertexBuffers();
		popScreenClip();
	}

	void prepareRender2D() override {
		isPrepared = true;

		if(!cleared)
			clear(CF_Color | CF_Depth);
		else
			clear(CF_Depth);

		setProjection(Matrix::ortho(0, screenSize.x, screenSize.y, 0, -4096.0, 4096.0));
		setView(Matrix());

		light::resetLights();

		switchToRenderState(state_2d);
		if(!intermediateBlend)
			cleared = false;
	}

	void setLight(unsigned index, const LightDesc* desc) override {
		if(index >= MAX_LIGHTS)
			return;
		auto& light = builtins.lights[index];
		++builtins.lightSerial;
		if(!desc) {
			light.enabled = false;
			return;
		}
		light.enabled = true;
		light.diffuse = desc->diffuse;
		light.specular = desc->specular;
		light.quadraticAttenuation = desc->quadraticAttenuation;

		//Positions go into eye space with the current transform, as glLight did
		const Matrix& m = modelView.back();
		const float* p = desc->position;
		for(unsigned i = 0; i < 4; ++i)
			(&light.position.x)[i] = (float)(m[i] * p[0] + m[4+i] * p[1] + m[8+i] * p[2] + m[12+i] * p[3]);
	}

	void drawFPSGraph(const recti& location) override {
		double max_time = 0.03333333;
		foreach(frame, frames)
			if(*frame > max_time)
				max_time = *frame;

		float x = (float)location.topLeft.width, yOff = (float)location.topLeft.y, height = (float)location.getHeight();

		static render::RenderState rs;
		rs.lighting = false;
		rs.culling = render::FC_None;
		rs.depthTest = render::DT_NoDepthTest;

		switchToRenderState(rs);

		auto* buffer = VertexBufferTCV::fetch(&rs);
		auto* v = buffer->request(1, PT_Lines);

		v[0].col = v[1].col = Color(0,0,196);

		float yFrameLine = floor( yOff + height * (float)(1.0 - (0.0166667 / max_time)) );

		v[0].pos = vec3f(x,yFrameLine,0);
		v[1].pos = vec3f((float)location.botRight.x,yFrameLine,0);

		if(frames.size() > 1) {
			v = buffer->request((unsigned)frames.size() - 1, PT_LineStrip);

			for(auto frame = frames.begin(), end = frames.end(); frame != end; ++frame, ++v) {
				v->col = Colorf((float)(*frame/0.033333), 1.f - (float)(*frame/0.0333333), 0);
				v->pos = vec3f(x, yOff + height * (1.f - (float)(*frame/max_time)), 0 );
				
				x += (float)location.getWidth() / (float)max_frames;
			}
		}

	}

	void setSkybox(const RenderState* mat) override {
		skybox = mat;
	}

	void setSkyboxMesh(const RenderMesh* mesh) override {
		skyboxMesh = mesh;
	}

	void pushScreenClip(const recti& box) override {
		viewportClips.push(box);
		applyScissor(&box);
	}

	void popScreenClip() override {
		if(viewportClips.empty())
			return;
		viewportClips.pop();
		applyScissor(viewportClips.empty() ? nullptr : &viewportClips.top());
	}
};

};
