#include "render/mtl/mtl_internal.h"
#include "main/logging.h"
#include "files.h"
#include <string.h>

extern unsigned frameNumber;

namespace render {
namespace mtl {

//Shader inputs by the names shaders give them
static bool inputFor(const std::string& name, VertexInput& input) {
	static const std::pair<const char*, VertexInput> names[] = {
		{"in_vertex", VI_Vertex}, {"in_position", VI_Vertex}, {"sr_Vertex", VI_Vertex},
		{"in_uv", VI_UV}, {"sr_MultiTexCoord0", VI_UV},
		{"in_normal", VI_Normal}, {"sr_Normal", VI_Normal},
		{"in_tangent", VI_Tangent},
		{"in_color", VI_Color}, {"sr_Color", VI_Color},
		{"in_uv2", VI_UV2},
	};
	for(auto& entry : names) {
		if(name == entry.first) {
			input = entry.second;
			return true;
		}
	}
	return false;
}

static id<MTLFunction> loadFunction(const TranslatedStage& stage, const std::string& file) {
	NSError* err = nil;
	MTLCompileOptions* options = [MTLCompileOptions new];
	options.languageVersion = MTLLanguageVersion3_0;
	id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:stage.msl.c_str()]
		options:options error:&err];
	if(!library) {
		error("Metal could not compile %s: %s", file.c_str(), err.localizedDescription.UTF8String);
		return nil;
	}
	return [library newFunctionWithName:[NSString stringWithUTF8String:stage.entryPoint.c_str()]];
}

int ShaderProgram::compile() {
	valid = false;
	uniforms.clear();
	textures.clear();
	texturesByName.clear();
	inputs.clear();
	pipelines.clear();
	matrixSerial = lightSerial = materialSerial = 0;
	lastShader = nullptr;

	std::string vertex = getFileContents(vertexFile);
	std::string fragment = getFileContents(fragmentFile);
	preProcessShader(vertex);
	preProcessShader(fragment);
	if(vertex.empty() || fragment.empty()) {
		error("Could not open %s or %s, or it was empty", vertexFile.c_str(), fragmentFile.c_str());
		return 1;
	}

	std::string vertexShimmed, fragmentShimmed, problems, err;
	if(!shimProgram(vertex, fragment, ST_GLSL410, vertexShimmed, fragmentShimmed, problems)) {
		error("Shaders %s and %s use built-ins with no replacement: %s",
			vertexFile.c_str(), fragmentFile.c_str(), problems.c_str());
		return 1;
	}

	TranslatedStage vs, fs;
	if(!translateProgram(vertexFile, vertexShimmed, fragmentFile, fragmentShimmed, vs, fs, err)) {
		error("Could not translate shaders for Metal: %s", err.c_str());
		return 1;
	}

	vertexFunction = loadFunction(vs, vertexFile);
	fragmentFunction = loadFunction(fs, fragmentFile);
	if(!vertexFunction || !fragmentFunction)
		return 2;

	//Both stages share one uniform layout; take the members of both in case either is missing one
	vertexBuffer = vs.reflection.uniformBufferIndex;
	fragmentBuffer = fs.reflection.uniformBufferIndex;
	uniformSize = std::max(vs.reflection.uniformBufferSize, fs.reflection.uniformBufferSize);
	for(auto* stage : {&vs, &fs})
		for(auto& u : stage->reflection.uniforms)
			uniforms[u.name] = {u.offset, u.size, u.arrayStride, u.arraySize};
	uniformData.assign(uniformSize, 0);

	auto addTextures = [this](const ShaderReflection& reflection, bool vertexStage) {
		for(auto& tex : reflection.textures) {
			auto it = texturesByName.find(tex.name);
			unsigned index;
			if(it == texturesByName.end()) {
				index = (unsigned)textures.size();
				textures.push_back(TextureSlot());
				textures.back().cube = tex.cube;
				//Unset samplers read unit 0, as GL's default sampler value did
				textures.back().units.assign(std::max(1u, tex.arraySize), 0);
				texturesByName[tex.name] = index;
			}
			else {
				index = it->second;
			}
			auto& slot = textures[index];
			if(vertexStage) {
				slot.vertexTexture = tex.textureIndex;
				slot.vertexSampler = tex.samplerIndex;
			}
			else {
				slot.fragmentTexture = tex.textureIndex;
				slot.fragmentSampler = tex.samplerIndex;
			}
		}
	};
	addTextures(vs.reflection, true);
	addTextures(fs.reflection, false);

	for(auto& input : vs.reflection.inputs) {
		VertexInput kind;
		if(inputFor(input.name, kind))
			inputs.push_back({input.location, kind});
		else
			error("%s: unknown vertex input %s", vertexFile.c_str(), input.name.c_str());
	}

	valid = true;
	return 0;
}

void ShaderProgram::write(const Uniform& uniform, const void* data, unsigned components, unsigned count, unsigned columns) {
	const unsigned char* src = (const unsigned char*)data;
	unsigned columnBytes = components * 4;
	//Matrix columns are 16 bytes apart in std140
	unsigned elementStride = uniform.arrayStride ? uniform.arrayStride : (columns > 1 ? columns * 16 : columnBytes);
	for(unsigned i = 0; i < count; ++i) {
		for(unsigned c = 0; c < columns; ++c) {
			unsigned offset = uniform.offset + i * elementStride + c * 16;
			if(offset + columnBytes > uniformData.size())
				return;
			memcpy(uniformData.data() + offset, src, columnBytes);
			src += columnBytes;
		}
	}
}

static void toFloats(const double* m, float* out) {
	for(unsigned i = 0; i < 16; ++i)
		out[i] = (float)m[i];
}

//Inverse of a column-major 4x4 matrix, or false if it has none
static bool invertMatrix(const double* m, double* inv) {
	inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if(det == 0)
		return false;
	det = 1.0 / det;
	for(unsigned i = 0; i < 16; ++i)
		inv[i] *= det;
	return true;
}

void ShaderProgram::updateBuiltins(const BuiltinState& state) {
	if(matrixSerial != state.matrixSerial) {
		matrixSerial = state.matrixSerial;
		float f[16];
		const double* mv = state.modelView.m;
		if(auto* u = findUniform("sr_ModelView")) {
			toFloats(mv, f);
			write(*u, f, 4, 1, 4);
		}
		if(auto* u = findUniform("sr_Projection")) {
			toFloats(state.projection.m, f);
			write(*u, f, 4, 1, 4);
		}
		if(auto* u = findUniform("sr_ModelViewProjection")) {
			toFloats((state.projection * state.modelView).m, f);
			write(*u, f, 4, 1, 4);
		}
		if(auto* u = findUniform("sr_NormalMatrix")) {
			//Inverse transpose of the upper 3x3: its cofactor matrix over the determinant
			double c[9];
			c[0] = mv[5]*mv[10] - mv[9]*mv[6];
			c[1] = mv[8]*mv[6] - mv[4]*mv[10];
			c[2] = mv[4]*mv[9] - mv[8]*mv[5];
			c[3] = mv[9]*mv[2] - mv[1]*mv[10];
			c[4] = mv[0]*mv[10] - mv[8]*mv[2];
			c[5] = mv[8]*mv[1] - mv[0]*mv[9];
			c[6] = mv[1]*mv[6] - mv[5]*mv[2];
			c[7] = mv[4]*mv[2] - mv[0]*mv[6];
			c[8] = mv[0]*mv[5] - mv[4]*mv[1];
			double det = mv[0]*c[0] + mv[4]*c[3] + mv[8]*c[6];
			double scale = det != 0 ? 1.0 / det : 0.0;
			float n[9];
			for(unsigned i = 0; i < 9; ++i)
				n[i] = (float)(c[i] * scale);
			write(*u, n, 3, 1, 3);
		}
		if(auto* u = findUniform("sr_FlipY"))
			write(*u, &state.flipY, 1, 1);
		if(auto* u = findUniform("sr_ProjectionInverse")) {
			double inv[16];
			if(invertMatrix(state.projection.m, inv)) {
				toFloats(inv, f);
				write(*u, f, 4, 1, 4);
			}
		}
	}

	if(lightSerial != state.lightSerial) {
		lightSerial = state.lightSerial;
		const Colorf noAmbient(0.f, 0.f, 0.f, 1.f);
		for(unsigned i = 0; i < MAX_LIGHTS; ++i) {
			auto& light = state.lights[i];
			std::string prefix = "sr_LightSource[" + std::to_string(i) + "].";
			if(auto* u = findUniform(prefix + "ambient"))
				write(*u, &noAmbient, 4, 1);
			if(auto* u = findUniform(prefix + "diffuse"))
				write(*u, &light.diffuse, 4, 1);
			if(auto* u = findUniform(prefix + "specular"))
				write(*u, &light.specular, 4, 1);
			if(auto* u = findUniform(prefix + "position"))
				write(*u, &light.position, 4, 1);
			if(auto* u = findUniform(prefix + "constantAttenuation"))
				write(*u, &light.constantAttenuation, 1, 1);
			if(auto* u = findUniform(prefix + "linearAttenuation"))
				write(*u, &light.linearAttenuation, 1, 1);
			if(auto* u = findUniform(prefix + "quadraticAttenuation"))
				write(*u, &light.quadraticAttenuation, 1, 1);
		}
		if(auto* u = findUniform("sr_LightModel.ambient"))
			write(*u, &state.lightModelAmbient, 4, 1);
		if(auto* u = findUniform("sr_LightEnabled")) {
			int enabled[MAX_LIGHTS];
			for(unsigned i = 0; i < MAX_LIGHTS; ++i)
				enabled[i] = state.lights[i].enabled ? 1 : 0;
			write(*u, enabled, 1, MAX_LIGHTS);
		}
	}

	if(materialSerial != state.materialSerial) {
		materialSerial = state.materialSerial;
		const Colorf noEmission(0.f, 0.f, 0.f, 1.f);
		if(auto* u = findUniform("sr_FrontMaterial.emission"))
			write(*u, &noEmission, 4, 1);
		if(auto* u = findUniform("sr_FrontMaterial.ambient"))
			write(*u, &state.materialAmbient, 4, 1);
		if(auto* u = findUniform("sr_FrontMaterial.diffuse"))
			write(*u, &state.materialDiffuse, 4, 1);
		if(auto* u = findUniform("sr_FrontMaterial.specular"))
			write(*u, &state.materialSpecular, 4, 1);
		if(auto* u = findUniform("sr_FrontMaterial.shininess"))
			write(*u, &state.materialShininess, 1, 1);
		if(auto* u = findUniform("sr_FixedLighting"))
			write(*u, &state.fixedLighting, 1, 1);
		if(auto* u = findUniform("sr_FixedTexture"))
			write(*u, &state.fixedTexture, 1, 1);
		if(auto* u = findUniform("sr_AlphaTest"))
			write(*u, &state.alphaTest, 1, 1);
		if(auto* u = findUniform("sr_AlphaRef"))
			write(*u, &state.alphaRef, 1, 1);
	}
}

void Shader::addVariable(const std::string& name, const Variable& var) {
	names.push_back(name);
	render::Shader::addVariable(name, var);
}

int Shader::compile() {
	varUniforms.assign(vars.size(), nullptr);
	varTextures.assign(vars.size(), -1);
	auto* prog = metalProgram();
	if(!prog || !prog->valid)
		return 4;

	for(size_t i = 0; i < vars.size(); ++i) {
		auto tex = prog->texturesByName.find(names[i]);
		if(tex != prog->texturesByName.end())
			varTextures[i] = (int)tex->second;
		else
			varUniforms[i] = prog->findUniform(names[i]);
	}
	return 0;
}

static unsigned componentsOf(Shader::VarType type) {
	switch(type) {
		case Shader::VT_int: case Shader::VT_float: return 1;
		case Shader::VT_int2: case Shader::VT_float2: return 2;
		case Shader::VT_int3: case Shader::VT_float3: return 3;
		case Shader::VT_int4: case Shader::VT_float4: return 4;
		case Shader::VT_mat3: return 9;
		default: return 0;
	}
}

static bool isInt(Shader::VarType type) {
	return type == Shader::VT_int || type == Shader::VT_int2 || type == Shader::VT_int3 || type == Shader::VT_int4;
}

void Shader::apply(size_t index, const void* data) const {
	const Variable& var = vars[index];
	auto* prog = metalProgram();
	if(varTextures[index] >= 0) {
		//Samplers choose which texture unit they read, one per array element
		auto& units = prog->textures[varTextures[index]].units;
		const int* values = (const int*)data;
		for(size_t i = 0; i < units.size() && i < var.count; ++i)
			units[i] = values[i];
		return;
	}
	if(auto* uniform = varUniforms[index]) {
		if(var.type == VT_mat3)
			prog->write(*uniform, data, 3, var.count, 3);
		else
			prog->write(*uniform, data, componentsOf(var.type), var.count);
	}
}

void Shader::bind(float* dynamic) const {
	auto* prog = metalProgram();
	if(!prog || !prog->valid)
		return;

	//As on GL, constant values are refreshed once a frame, or when another shader used the program
	const bool bindConst = prog->lastBoundFrame != frameNumber || prog->lastShader != this;
	prog->lastBoundFrame = frameNumber;
	prog->lastShader = this;

	float* floats = dynamic;
	for(size_t i = 0; i < vars.size(); ++i) {
		if(!isActive(i))
			continue;
		const Variable& var = vars[i];
		if(var.constant) {
			if(!bindConst)
				continue;
			if(isInt(var.type)) {
				if(var._intcall)
					var._intcall(var._ints, var.count, var._args);
				apply(i, var._ints);
			}
			else {
				if(var._floatcall)
					var._floatcall(var._floats, var.count, var._args);
				apply(i, var._floats);
			}
		}
		else if(dynamic) {
			if(var._intcall) {
				apply(i, floats);
				floats += componentsOf(var.type) * var.count;
			}
		}
		else {
			if(var._intcall)
				var._intcall(var._ints, var.count, var._args);
			apply(i, var._ints);
		}
	}
}

void Shader::updateDynamicVars() const {
	if(!metalProgram() || !metalProgram()->valid)
		return;
	for(size_t i = 0; i < vars.size(); ++i) {
		if(!isActive(i) || vars[i].constant)
			continue;
		const Variable& var = vars[i];
		if(var._intcall)
			var._intcall(var._ints, var.count, var._args);
		apply(i, var._ints);
	}
}

void Shader::saveDynamicVars(float* buffer) const {
	float* floats = buffer;
	for(size_t i = 0; i < vars.size(); ++i) {
		if(!isActive(i) || vars[i].constant)
			continue;
		const Variable& var = vars[i];
		if(var._intcall) {
			var._intcall((int*)floats, var.count, var._args);
			floats += componentsOf(var.type) * var.count;
		}
	}
}

void Shader::loadDynamicVars(float* buffer) const {
	if(!metalProgram() || !metalProgram()->valid)
		return;
	float* floats = buffer;
	for(size_t i = 0; i < vars.size(); ++i) {
		if(!isActive(i) || vars[i].constant)
			continue;
		const Variable& var = vars[i];
		if(var._intcall) {
			apply(i, floats);
			floats += componentsOf(var.type) * var.count;
		}
	}
}

};
};
