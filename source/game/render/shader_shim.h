#pragma once
#include <string>

namespace render {

enum ShaderStage {
	SS_Vertex,
	SS_Fragment,
};

enum ShimTarget {
	//GLSL 4.10 core, as glslang takes it before translation. Fragment shaders apply
	//the alpha test through sr_AlphaTest and sr_AlphaRef.
	ST_GLSL410,
};

//Generic attribute locations of the vertex inputs, shared by every backend
enum ShaderAttribute {
	SA_Vertex = 0,
	SA_UV = 1,
	SA_Normal = 2,
	SA_Tangent = 3,
	SA_Color = 4,
	SA_UV2 = 5,
};

//Rewrites a GLSL 1.20 shader so it reads no fixed-function state. Returns false
//when the shader uses built-ins the shim can't map, and lists them in <problems>.
bool shimShader(const std::string& source, ShaderStage stage, ShimTarget target,
	std::string& output, std::string& problems);

//Shims both stages of a program. Unlike shimShader, this also handles fragment
//inputs that the vertex shader never declares, which translation can't link.
bool shimProgram(const std::string& vertex, const std::string& fragment, ShimTarget target,
	std::string& vertexOutput, std::string& fragmentOutput, std::string& problems);

};
