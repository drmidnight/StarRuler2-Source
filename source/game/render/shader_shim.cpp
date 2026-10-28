#include "render/shader_shim.h"
#include "render/shader_builtins.h"
#include <set>
#include <string.h>

namespace render {

//Built-ins mapped onto sr_* names, with the declaration each one needs
struct Mapping {
	const char* from;
	const char* to;
	unsigned decl;
	bool vertexOnly;
};

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
};

static const Mapping mappings[] = {
	{"gl_ModelViewMatrix", "sr_ModelView", D_ModelView, false},
	{"gl_ProjectionMatrix", "sr_Projection", D_Projection, false},
	{"gl_ModelViewProjectionMatrix", "sr_ModelViewProjection", D_ModelViewProjection, false},
	{"gl_NormalMatrix", "sr_NormalMatrix", D_NormalMatrix, false},
	{"gl_ProjectionMatrixInverse", "sr_ProjectionInverse", D_ProjectionInverse, false},
	{"gl_LightSource", "sr_LightSource", D_LightSource, false},
	{"gl_FrontMaterial", "sr_FrontMaterial", D_FrontMaterial, false},
	{"gl_LightModel", "sr_LightModel", D_LightModel, false},
	{"gl_Vertex", "sr_Vertex", D_Vertex, true},
	{"gl_Normal", "sr_Normal", D_Normal, true},
	{"gl_Color", "sr_Color", D_Color, true},
	{"gl_MultiTexCoord0", "sr_MultiTexCoord0", D_MultiTexCoord0, true},
	{"ftransform", "sr_ftransform", D_ftransform | D_ModelViewProjection | D_Vertex, true},
};

//Built-ins that GLSL 1.20 provides without fixed-function state
static const char* passThrough[] = {
	"gl_Position", "gl_PointSize", "gl_FragColor", "gl_FragData", "gl_FragCoord",
	"gl_FrontFacing", "gl_PointCoord", "gl_FrontColor", "gl_BackColor", "gl_TexCoord",
	"gl_Color",
};

static bool isIdentStart(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isIdentChar(char c) {
	return isIdentStart(c) || (c >= '0' && c <= '9');
}

static void declare(std::string& out, unsigned used, ShaderStage stage) {
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
			out += "attribute vec4 sr_Vertex;\n";
		if(used & D_Normal)
			out += "attribute vec3 sr_Normal;\n";
		if(used & D_Color)
			out += "attribute vec4 sr_Color;\n";
		if(used & D_MultiTexCoord0)
			out += "attribute vec4 sr_MultiTexCoord0;\n";
		if(used & D_ftransform)
			out += "vec4 sr_ftransform() { return sr_ModelViewProjection * sr_Vertex; }\n";
	}
}

bool shimShader(const std::string& source, ShaderStage stage, ShimTarget target,
	std::string& output, std::string& problems)
{
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

			bool replaced = false;
			for(auto& map : mappings) {
				if(strlen(map.from) == identLen && strncmp(map.from, ident, identLen) == 0) {
					if(map.vertexOnly && stage != SS_Vertex)
						break;
					body += map.to;
					used |= map.decl;
					replaced = true;
					break;
				}
			}

			if(!replaced) {
				if(identLen > 3 && strncmp(ident, "gl_", 3) == 0 && strncmp(ident, "gl_Max", 6) != 0) {
					bool known = false;
					for(auto* name : passThrough)
						if(strlen(name) == identLen && strncmp(name, ident, identLen) == 0)
							known = true;
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
	}
	output += extensions;
	declare(output, used, stage);
	//GLSL 1.20 numbers the line after "#line N" as N+1
	output += "#line 0\n";
	output += body;

	problems.clear();
	for(auto& name : unsupported) {
		if(!problems.empty())
			problems += ", ";
		problems += name;
	}
	return unsupported.empty();
}

};
