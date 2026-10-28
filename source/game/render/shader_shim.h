#pragma once
#include <string>

namespace render {

enum ShaderStage {
	SS_Vertex,
	SS_Fragment,
};

enum ShimTarget {
	//GLSL 1.20 with built-in state read from sr_* uniforms and attributes
	ST_GLSL120,
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

};
