#include "render/driver.h"
#include "render/driver_base.h"
#include "main/references.h"

namespace render {

Texture* RenderDriver::createTexture() {
	return devices.render->createEmptyTexture();
}

Texture* RenderDriver::createCubemap() {
	return devices.render->createEmptyCubemap();
}

};

//Shader variables read from the driver's state, the same for every backend
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
