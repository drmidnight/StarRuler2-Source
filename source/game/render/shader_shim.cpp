#include "render/shader_shim.h"
#include "render/shader_builtins.h"
#include <set>
#include <string.h>

namespace render {

enum Declaration {
	D_ModelView = 1<<0,
	D_Projection = 1<<1,
	D_ModelViewProjection = 1<<2,
	D_NormalMatrix = 1<<3,
	D_ProjectionInverse = 1<<4,
	D_LightSource = 1<<5,
	D_FrontMaterial = 1<<6,
	D_LightModel = 1<<7,
	D_Vertex = 1<<8,
	D_Normal = 1<<9,
	D_Color = 1<<10,
	D_MultiTexCoord0 = 1<<11,
	D_ftransform = 1<<12,
	D_FragColor = 1<<13,
	D_FrontColor = 1<<14,
	D_BackColor = 1<<15,
	D_TexCoord = 1<<16,
	D_texture2D = 1<<17,
	D_textureCube = 1<<18,
	D_texture2DLod = 1<<19,
	D_textureCubeLod = 1<<20,
	D_main = 1<<21,
};

enum MapFlags {
	//Stages the mapping applies in
	MF_Vertex = 1<<0,
	MF_Fragment = 1<<1,
	MF_Stages = MF_Vertex | MF_Fragment,
	//Targets the mapping applies to
	MF_Legacy = 1<<2,
	MF_Core = 1<<3,
	MF_Targets = MF_Legacy | MF_Core,
	MF_All = MF_Stages | MF_Targets,
};

//An identifier mapped onto another, with the declarations it needs
struct Mapping {
	const char* from;
	const char* to;
	unsigned decl;
	unsigned flags;
};

static const Mapping mappings[] = {
	{"gl_ModelViewMatrix", "sr_ModelView", D_ModelView, MF_All},
	{"gl_ProjectionMatrix", "sr_Projection", D_Projection, MF_All},
	{"gl_ModelViewProjectionMatrix", "sr_ModelViewProjection", D_ModelViewProjection, MF_All},
	{"gl_NormalMatrix", "sr_NormalMatrix", D_NormalMatrix, MF_All},
	{"gl_ProjectionMatrixInverse", "sr_ProjectionInverse", D_ProjectionInverse, MF_All},
	{"gl_LightSource", "sr_LightSource", D_LightSource, MF_All},
	{"gl_FrontMaterial", "sr_FrontMaterial", D_FrontMaterial, MF_All},
	{"gl_LightModel", "sr_LightModel", D_LightModel, MF_All},
	{"gl_Vertex", "sr_Vertex", D_Vertex, MF_Vertex | MF_Targets},
	{"gl_Normal", "sr_Normal", D_Normal, MF_Vertex | MF_Targets},
	{"gl_Color", "sr_Color", D_Color, MF_Vertex | MF_Targets},
	{"gl_MultiTexCoord0", "sr_MultiTexCoord0", D_MultiTexCoord0, MF_Vertex | MF_Targets},
	{"ftransform", "sr_ftransform", D_ftransform | D_ModelViewProjection | D_Vertex, MF_Vertex | MF_Targets},

	//GLSL 1.20 language that core profiles removed
	{"attribute", "in", 0, MF_Vertex | MF_Core},
	{"varying", "out", 0, MF_Vertex | MF_Core},
	{"varying", "in", 0, MF_Fragment | MF_Core},
	{"gl_FrontColor", "sr_FrontColor", D_FrontColor, MF_Vertex | MF_Core},
	{"gl_BackColor", "sr_BackColor", D_BackColor, MF_Vertex | MF_Core},
	{"gl_Color", "sr_FrontColor", D_FrontColor, MF_Fragment | MF_Core},
	{"gl_TexCoord", "sr_TexCoord", D_TexCoord, MF_Stages | MF_Core},
	{"gl_FragColor", "sr_FragColor", D_FragColor, MF_Fragment | MF_Core},
	//Shaders name samplers "texture", which hides the core function, so calls go
	//through wrappers declared ahead of the shader's own code
	{"texture2D", "sr_texture2D", D_texture2D, MF_Stages | MF_Core},
	{"textureCube", "sr_textureCube", D_textureCube, MF_Stages | MF_Core},
	{"texture2DLod", "sr_texture2DLod", D_texture2DLod, MF_Stages | MF_Core},
	{"textureCubeLod", "sr_textureCubeLod", D_textureCubeLod, MF_Stages | MF_Core},
	//The fragment shader's main is wrapped to apply the alpha test
	{"main", "sr_main", D_main, MF_Fragment | MF_Core},
};

//Built-ins that GLSL 1.20 provides without fixed-function state
static const char* legacyBuiltins[] = {
	"gl_Position", "gl_PointSize", "gl_FragColor", "gl_FragData", "gl_FragCoord",
	"gl_FrontFacing", "gl_PointCoord", "gl_FrontColor", "gl_BackColor", "gl_TexCoord",
	"gl_Color",
};

//Built-ins that core profiles still have
static const char* coreBuiltins[] = {
	"gl_Position", "gl_PointSize", "gl_FragCoord", "gl_FrontFacing", "gl_PointCoord",
	"gl_VertexID", "gl_InstanceID", "gl_FragDepth",
};

//Names that core GLSL reserves, or that Metal has built-in functions for,
//but that GLSL 1.20 shaders could define themselves
static const char* coreKeywords[] = {
	"sample", "smooth", "flat", "noperspective", "patch", "layout", "subroutine", "precise",
	"uint", "uvec2", "uvec3", "uvec4", "dvec2", "dvec3", "dvec4",
	"atan2", "saturate", "rsqrt", "fmod", "fmin", "fmax", "select", "lerp", "frac",
};

//Locations each gl_TexCoord array covers
static const unsigned texCoordCount = 4;

static bool isIdentStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isIdentChar(char c) {
	return isIdentStart(c) || (c >= '0' && c <= '9');
}

static bool isNamed(const char* ident, size_t len, const char* name) {
	return strlen(name) == len && strncmp(name, ident, len) == 0;
}

//Skips the comment starting at <i>, if there is one
static bool skipComment(const std::string& source, size_t& i) {
	if(source[i] != '/' || i + 1 >= source.size())
		return false;
	if(source[i+1] == '/') {
		i = source.find('\n', i);
		if(i == std::string::npos)
			i = source.size();
		return true;
	}
	if(source[i+1] == '*') {
		i = source.find("*/", i + 2);
		i = i == std::string::npos ? source.size() : i + 2;
		return true;
	}
	return false;
}

//A varying declaration: its qualifiers and type, then each declarator ("name[2]")
struct VaryingDeclaration {
	std::string type;
	std::vector<std::string> names, declarators;
	//Position of the closing semicolon
	size_t end;
};

//Parses the varying declaration that continues from <i>
static void parseVarying(const std::string& source, size_t i, VaryingDeclaration& decl) {
	const size_t len = source.size();
	size_t typeStart = i, declStart = std::string::npos;
	int depth = 0;
	std::string name;
	while(i < len && source[i] != ';') {
		if(skipComment(source, i))
			continue;
		char c = source[i];
		if(c == '[')
			++depth;
		else if(c == ']')
			--depth;
		else if(c == ',' && depth == 0 && declStart != std::string::npos) {
			decl.names.push_back(name);
			decl.declarators.push_back(source.substr(declStart, i - declStart));
			declStart = i + 1;
			name.clear();
		}
		if(isIdentStart(c)) {
			size_t end = i + 1;
			while(end < len && isIdentChar(source[end]))
				++end;
			std::string ident = source.substr(i, end - i);
			bool qualifier = ident == "lowp" || ident == "mediump" || ident == "highp"
				|| ident == "centroid" || ident == "invariant";
			if(depth == 0 && !qualifier) {
				if(declStart == std::string::npos) {
					decl.type = source.substr(typeStart, end - typeStart);
					declStart = end;
				}
				else if(name.empty()) {
					name = ident;
				}
			}
			i = end;
			continue;
		}
		++i;
	}
	if(declStart != std::string::npos && !name.empty()) {
		decl.names.push_back(name);
		decl.declarators.push_back(source.substr(declStart, i - declStart));
	}
	decl.end = i;
}

//Every name a shader declares as varying
static std::set<std::string> declaredVaryings(const std::string& source) {
	std::set<std::string> names;
	const size_t len = source.size();
	size_t i = 0;
	while(i < len) {
		if(skipComment(source, i))
			continue;
		if(isIdentStart(source[i])) {
			size_t end = i + 1;
			while(end < len && isIdentChar(source[end]))
				++end;
			if(source.compare(i, end - i, "varying") == 0) {
				VaryingDeclaration decl;
				parseVarying(source, end, decl);
				names.insert(decl.names.begin(), decl.names.end());
			}
			i = end;
			continue;
		}
		++i;
	}
	return names;
}

static void declare(std::string& out, unsigned used, ShaderStage stage, ShimTarget target) {
	const bool core = target != ST_GLSL120;
	const char* input = core ? "in" : "attribute";

	if(used & D_LightSource) {
		out += "struct sr_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position;"
			" float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };\n";
		out += "uniform sr_LightSourceParameters sr_LightSource[" + std::to_string(MAX_LIGHTS) + "];\n";
	}
	if(used & D_FrontMaterial) {
		out += "struct sr_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };\n";
		out += "uniform sr_MaterialParameters sr_FrontMaterial;\n";
	}
	if(used & D_LightModel) {
		out += "struct sr_LightModelParameters { vec4 ambient; };\n";
		out += "uniform sr_LightModelParameters sr_LightModel;\n";
	}
	if(used & D_ModelView)
		out += "uniform mat4 sr_ModelView;\n";
	if(used & D_Projection)
		out += "uniform mat4 sr_Projection;\n";
	if(used & D_ModelViewProjection)
		out += "uniform mat4 sr_ModelViewProjection;\n";
	if(used & D_NormalMatrix)
		out += "uniform mat3 sr_NormalMatrix;\n";
	if(used & D_ProjectionInverse)
		out += "uniform mat4 sr_ProjectionInverse;\n";

	if(stage == SS_Vertex) {
		if(used & D_Vertex)
			out += std::string(input) + " vec4 sr_Vertex;\n";
		if(used & D_Normal)
			out += std::string(input) + " vec3 sr_Normal;\n";
		if(used & D_Color)
			out += std::string(input) + " vec4 sr_Color;\n";
		if(used & D_MultiTexCoord0)
			out += std::string(input) + " vec4 sr_MultiTexCoord0;\n";
		if(used & D_ftransform)
			out += "vec4 sr_ftransform() { return sr_ModelViewProjection * sr_Vertex; }\n";
	}

	if(core) {
		const char* varying = stage == SS_Vertex ? "out" : "in";
		//The vertex shader always passes a colour on, as GL did, so fragment shaders
		//that read it link whether or not the vertex shader wrote one
		if(stage == SS_Vertex || (used & D_FrontColor))
			out += std::string(varying) + " vec4 sr_FrontColor;\n";
		if(used & D_BackColor)
			out += "vec4 sr_BackColor;\n";
		if(used & D_TexCoord)
			out += std::string(varying) + " vec4 sr_TexCoord[" + std::to_string(texCoordCount) + "];\n";
		if(stage == SS_Fragment) {
			out += "out vec4 sr_FragColor;\n";
			out += "uniform int sr_AlphaTest;\n";
			out += "uniform float sr_AlphaRef;\n";
		}
		if(used & D_texture2D) {
			out += "vec4 sr_texture2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n";
			out += "vec4 sr_texture2D(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n";
		}
		if(used & D_textureCube) {
			out += "vec4 sr_textureCube(samplerCube s, vec3 dir) { return texture(s, dir); }\n";
			out += "vec4 sr_textureCube(samplerCube s, vec3 dir, float bias) { return texture(s, dir, bias); }\n";
		}
		if(used & D_texture2DLod)
			out += "vec4 sr_texture2DLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n";
		if(used & D_textureCubeLod)
			out += "vec4 sr_textureCubeLod(samplerCube s, vec3 dir, float lod) { return textureLod(s, dir, lod); }\n";
		if(stage == SS_Fragment)
			out += "void sr_main();\n";
	}
}

//Runs the shader, then applies the alpha test GL did after it
static const char* alphaTestMain =
	"\nvoid main() {\n"
	"\tsr_main();\n"
	"\tif(sr_AlphaTest != 0 && !(sr_FragColor.a > sr_AlphaRef))\n"
	"\t\tdiscard;\n"
	"}\n";

//<vertexVaryings> are the vertex shader's varyings when shimming a program's fragment shader
static bool shimStage(const std::string& source, ShaderStage stage, ShimTarget target,
	const std::set<std::string>* vertexVaryings, std::string& output, std::string& problems)
{
	const unsigned stageFlag = stage == SS_Vertex ? MF_Vertex : MF_Fragment;
	const unsigned targetFlag = target == ST_GLSL120 ? MF_Legacy : MF_Core;
	const bool core = target != ST_GLSL120;

	std::string body, extensions;
	std::set<std::string> unsupported;
	unsigned used = 0;
	body.reserve(source.size());

	const size_t len = source.size();
	bool lineStart = true;
	size_t i = 0;
	while(i < len) {
		char c = source[i];

		//Comments are copied untouched
		if(c == '/' && i + 1 < len && source[i+1] == '/') {
			size_t end = source.find('\n', i);
			if(end == std::string::npos)
				end = len;
			body.append(source, i, end - i);
			i = end;
			continue;
		}
		if(c == '/' && i + 1 < len && source[i+1] == '*') {
			size_t end = source.find("*/", i + 2);
			end = end == std::string::npos ? len : end + 2;
			body.append(source, i, end - i);
			i = end;
			continue;
		}

		//#version is replaced and #extension must come before the declarations,
		//so both lines are moved out, leaving the line itself to keep numbering
		if(c == '#' && lineStart) {
			size_t word = i + 1;
			while(word < len && (source[word] == ' ' || source[word] == '\t'))
				++word;
			size_t end = source.find('\n', i);
			if(end == std::string::npos)
				end = len;
			if(source.compare(word, 7, "version") == 0) {
				i = end;
				continue;
			}
			if(source.compare(word, 9, "extension") == 0) {
				extensions.append(source, i, end - i);
				extensions += "\n";
				i = end;
				continue;
			}
		}

		if(isIdentStart(c)) {
			size_t end = i + 1;
			while(end < len && isIdentChar(source[end]))
				++end;
			const char* ident = source.c_str() + i;
			size_t identLen = end - i;
			lineStart = false;

			const Mapping* mapped = nullptr;
			for(auto& map : mappings) {
				if((map.flags & stageFlag) && (map.flags & targetFlag) && isNamed(ident, identLen, map.from)) {
					mapped = &map;
					break;
				}
			}

			bool keyword = false;
			if(core && !mapped)
				for(auto* name : coreKeywords)
					if(isNamed(ident, identLen, name))
						keyword = true;

			//GL leaves fragment inputs that the vertex shader never declares undefined,
			//but other APIs can't link them, so they become plain globals
			if(mapped && vertexVaryings && isNamed(ident, identLen, "varying")) {
				VaryingDeclaration decl;
				parseVarying(source, end, decl);
				std::string inputs, locals;
				for(size_t d = 0; d < decl.names.size(); ++d) {
					std::string& list = vertexVaryings->count(decl.names[d]) ? inputs : locals;
					if(!list.empty())
						list += ",";
					list += decl.declarators[d];
				}
				if(!locals.empty() && decl.end < len) {
					if(!inputs.empty())
						body += mapped->to + std::string(" ") + decl.type + inputs + "; ";
					body += decl.type + locals + ";";
					//Keeps the line numbering of declarations spread over several lines
					for(size_t n = end; n < decl.end; ++n)
						if(source[n] == '\n')
							body += '\n';
					i = decl.end + 1;
					continue;
				}
			}

			if(mapped) {
				body += mapped->to;
				used |= mapped->decl;
			}
			else if(keyword) {
				body += "sr_kw_";
				body.append(ident, identLen);
			}
			else {
				if(identLen > 3 && strncmp(ident, "gl_", 3) == 0 && strncmp(ident, "gl_Max", 6) != 0) {
					bool known = false;
					if(core) {
						for(auto* name : coreBuiltins)
							if(isNamed(ident, identLen, name))
								known = true;
					}
					else {
						for(auto* name : legacyBuiltins)
							if(isNamed(ident, identLen, name))
								known = true;
					}
					if(!known)
						unsupported.insert(std::string(ident, identLen));
				}
				body.append(ident, identLen);
			}
			i = end;
			continue;
		}

		if(c == '\n')
			lineStart = true;
		else if(c != ' ' && c != '\t' && c != '\r')
			lineStart = false;
		body += c;
		++i;
	}

	output.clear();
	switch(target) {
		case ST_GLSL120:
			output += "#version 120\n";
		break;
		case ST_GLSL410:
			output += "#version 410 core\n";
		break;
	}
	output += extensions;
	declare(output, used, stage, target);
	//GLSL 1.20 numbers the line after "#line N" as N+1, later versions as N
	output += core ? "#line 1\n" : "#line 0\n";
	output += body;
	if(core && stage == SS_Fragment)
		output += alphaTestMain;

	problems.clear();
	for(auto& name : unsupported) {
		if(!problems.empty())
			problems += ", ";
		problems += name;
	}
	return unsupported.empty();
}

bool shimShader(const std::string& source, ShaderStage stage, ShimTarget target,
	std::string& output, std::string& problems)
{
	return shimStage(source, stage, target, nullptr, output, problems);
}

bool shimProgram(const std::string& vertex, const std::string& fragment, ShimTarget target,
	std::string& vertexOutput, std::string& fragmentOutput, std::string& problems)
{
	std::string vertexProblems, fragmentProblems;
	auto varyings = declaredVaryings(vertex);
	bool ok = shimStage(vertex, SS_Vertex, target, nullptr, vertexOutput, vertexProblems);
	ok = shimStage(fragment, SS_Fragment, target, &varyings, fragmentOutput, fragmentProblems) && ok;
	problems = vertexProblems;
	if(!problems.empty() && !fragmentProblems.empty())
		problems += ", ";
	problems += fragmentProblems;
	return ok;
}

};
