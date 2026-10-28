#pragma once
#include "matrix.h"
#include "vec4.h"
#include "color.h"

namespace render {

//Most lights that can be active at once
const unsigned MAX_LIGHTS = 8;

//A light as shaders read it, with its position in eye space
struct BuiltinLight {
	bool enabled;
	vec4f position;
	Colorf diffuse, specular;
	float constantAttenuation, linearAttenuation, quadraticAttenuation;
};

//The state that GLSL 1.20 shaders read through gl_* built-ins. Shimmed shaders
//read it from sr_* uniforms instead, which the backend fills from here.
//Each serial changes whenever its part of the state does.
struct BuiltinState {
	Matrix projection, modelView;
	BuiltinLight lights[MAX_LIGHTS];
	Colorf materialAmbient, materialDiffuse, materialSpecular;
	float materialShininess;
	Colorf lightModelAmbient;

	//Read by the FixedFunction shader, which stands in when a material has none:
	//whether to light vertices, and 0 for no texture, 1 for a colour texture or
	//2 for an alpha-only one. They count as material state.
	int fixedLighting, fixedTexture;

	//Fragments with alpha at or below alphaRef are discarded when alphaTest is set
	int alphaTest;
	float alphaRef;

	//Multiplies clip-space y; -1 flips drawing for render targets stored upside down.
	//It counts as matrix state.
	float flipY;

	unsigned matrixSerial, lightSerial, materialSerial;

	BuiltinState() : materialShininess(0.f), fixedLighting(0), fixedTexture(0),
		alphaTest(0), alphaRef(1.f/255.f), flipY(1.f), matrixSerial(1), lightSerial(1), materialSerial(1) {
		//GL's initial state
		for(unsigned i = 0; i < MAX_LIGHTS; ++i) {
			auto& light = lights[i];
			Colorf initial = i == 0 ? Colorf(1.f, 1.f, 1.f, 1.f) : Colorf(0.f, 0.f, 0.f, 1.f);
			light.enabled = false;
			light.position = vec4f(0.f, 0.f, 1.f, 0.f);
			light.diffuse = initial;
			light.specular = initial;
			light.constantAttenuation = 1.f;
			light.linearAttenuation = 0.f;
			light.quadraticAttenuation = 0.f;
		}
		materialAmbient = Colorf(0.2f, 0.2f, 0.2f, 1.f);
		materialDiffuse = Colorf(0.8f, 0.8f, 0.8f, 1.f);
		materialSpecular = Colorf(0.f, 0.f, 0.f, 1.f);
		lightModelAmbient = Colorf(0.2f, 0.2f, 0.2f, 1.f);
	}
};

};
