#pragma once
#include <string>
#include <vector>

namespace render {

//What a translated shader stage expects to be bound, by the names the GLSL used
struct ShaderReflection {
	struct Uniform {
		//Struct members and array elements are named as GL names them: "a[1].b"
		std::string name;
		unsigned offset, size;
		//Distance between elements of an array of plain values, 0 otherwise
		unsigned arrayStride, arraySize;
	};

	struct Texture {
		std::string name;
		bool cube;
		//Arrays of samplers take consecutive texture and sampler indices
		unsigned textureIndex, samplerIndex, arraySize;
	};

	struct Input {
		std::string name;
		unsigned location;
	};

	//The stage's loose uniforms, gathered into one buffer; index -1 when it has none
	int uniformBufferIndex;
	unsigned uniformBufferSize;
	std::vector<Uniform> uniforms;
	std::vector<Texture> textures;
	//Vertex shader inputs
	std::vector<Input> inputs;
};

struct TranslatedStage {
	std::string msl;
	std::string entryPoint;
	ShaderReflection reflection;
};

//Translates a program whose stages the shader shim turned into GLSL 4.10 core,
//through SPIR-V, into Metal Shading Language. Returns false and describes the
//problem in <error>.
bool translateProgram(const std::string& vertexName, const std::string& vertex,
	const std::string& fragmentName, const std::string& fragment,
	TranslatedStage& vs, TranslatedStage& fs, std::string& error);

};
