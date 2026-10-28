#include "render/shader.h"
#include "render/gl_shader.h"
#include "render/shader_shim.h"
#include "render/shader_builtins.h"
#include "compat/gl.h"
#include "compat/misc.h"
#include "str_util.h"
#include "files.h"
#include "main/logging.h"
#include "main/references.h"

extern unsigned frameNumber;

namespace render {

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

static void uniformMatrix(GLint location, const double* m) {
	float f[16];
	for(unsigned i = 0; i < 16; ++i)
		f[i] = (float)m[i];
	glUniformMatrix4fv(location, 1, GL_FALSE, f);
}

static void uniformColor(GLint location, const Colorf& color) {
	glUniform4fv(location, 1, &color.r);
}

class GLShader;
class GLShaderProgram;

//The program GL is using, so binding the same one again is skipped
static GLShaderProgram* boundProgram = nullptr;
static GLuint boundProgramID = 0;

class GLShaderProgram : public ShaderProgram {
public:
	GLuint vertex_shader, fragment_shader, program;
	std::string vertex_file, fragment_file;

	std::unordered_map<GLuint, unsigned> cached_uniforms;
	std::vector<float> mem_buffer;

	const GLShader* lastShader;

	//Locations of the sr_* uniforms the shim declared, -1 for ones the program doesn't use
	struct LightLocations {
		GLint ambient, diffuse, specular, position;
		GLint constantAttenuation, linearAttenuation, quadraticAttenuation;
	};
	GLint modelView, projection, modelViewProjection, normalMatrix, projectionInverse;
	LightLocations lights[MAX_LIGHTS];
	GLint materialEmission, materialAmbient, materialDiffuse, materialSpecular, materialShininess;
	GLint lightModelAmbient, lightEnabled;
	GLint fixedLighting, fixedTexture, alphaTest, alphaRef;
	bool usesMatrices, usesLights, usesMaterial;
	unsigned matrixSerial, lightSerial, materialSerial;

	GLShaderProgram(const std::string& vFile, const std::string& fFile)
		: vertex_file(vFile), fragment_file(fFile), program(0), lastShader(0),
		usesMatrices(false), usesLights(false), usesMaterial(false)
	{
	}

	~GLShaderProgram() {
		reset();
	}

	void reset() {
		if(boundProgram == this) {
			boundProgram = nullptr;
			boundProgramID = 0;
			glUseProgram(0);
		}
		if(program != 0) {
			glDeleteProgram(program);
			program = 0;
		}

		mem_buffer.clear();
		cached_uniforms.clear();
		usesMatrices = usesLights = usesMaterial = false;
	}

	void findBuiltins() {
		auto find = [this](const std::string& name) -> GLint {
			return glGetUniformLocation(program, name.c_str());
		};
		modelView = find("sr_ModelView");
		projection = find("sr_Projection");
		modelViewProjection = find("sr_ModelViewProjection");
		normalMatrix = find("sr_NormalMatrix");
		projectionInverse = find("sr_ProjectionInverse");
		usesMatrices = modelView >= 0 || projection >= 0 || modelViewProjection >= 0
			|| normalMatrix >= 0 || projectionInverse >= 0;

		usesLights = false;
		for(unsigned i = 0; i < MAX_LIGHTS; ++i) {
			std::string prefix = "sr_LightSource[" + std::to_string(i) + "].";
			auto& light = lights[i];
			light.ambient = find(prefix + "ambient");
			light.diffuse = find(prefix + "diffuse");
			light.specular = find(prefix + "specular");
			light.position = find(prefix + "position");
			light.constantAttenuation = find(prefix + "constantAttenuation");
			light.linearAttenuation = find(prefix + "linearAttenuation");
			light.quadraticAttenuation = find(prefix + "quadraticAttenuation");
			for(GLint* loc = &light.ambient; loc <= &light.quadraticAttenuation; ++loc)
				if(*loc >= 0)
					usesLights = true;
		}
		lightModelAmbient = find("sr_LightModel.ambient");
		lightEnabled = find("sr_LightEnabled");
		if(lightModelAmbient >= 0 || lightEnabled >= 0)
			usesLights = true;

		materialEmission = find("sr_FrontMaterial.emission");
		materialAmbient = find("sr_FrontMaterial.ambient");
		materialDiffuse = find("sr_FrontMaterial.diffuse");
		materialSpecular = find("sr_FrontMaterial.specular");
		materialShininess = find("sr_FrontMaterial.shininess");
		fixedLighting = find("sr_FixedLighting");
		fixedTexture = find("sr_FixedTexture");
		alphaTest = find("sr_AlphaTest");
		alphaRef = find("sr_AlphaRef");
		usesMaterial = materialEmission >= 0 || materialAmbient >= 0 || materialDiffuse >= 0
			|| materialSpecular >= 0 || materialShininess >= 0 || fixedLighting >= 0 || fixedTexture >= 0
			|| alphaTest >= 0 || alphaRef >= 0;

		matrixSerial = lightSerial = materialSerial = 0;
	}

	//Sends the built-in state this program reads, if it changed since the last upload
	void uploadBuiltins(const BuiltinState& state) {
		if(usesMatrices && matrixSerial != state.matrixSerial) {
			matrixSerial = state.matrixSerial;
			const double* mv = state.modelView.m;
			if(modelView >= 0)
				uniformMatrix(modelView, mv);
			if(projection >= 0)
				uniformMatrix(projection, state.projection.m);
			if(modelViewProjection >= 0)
				uniformMatrix(modelViewProjection, (state.projection * state.modelView).m);
			if(normalMatrix >= 0) {
				//Inverse transpose of the upper 3x3, which is its cofactor matrix over the determinant
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
				//c is column-major, like the uniform
				float n[9];
				for(unsigned i = 0; i < 9; ++i)
					n[i] = (float)(c[i] * scale);
				glUniformMatrix3fv(normalMatrix, 1, GL_FALSE, n);
			}
			if(projectionInverse >= 0) {
				double inv[16];
				if(invertMatrix(state.projection.m, inv))
					uniformMatrix(projectionInverse, inv);
			}
		}

		if(usesLights && lightSerial != state.lightSerial) {
			lightSerial = state.lightSerial;
			const Colorf noAmbient(0.f, 0.f, 0.f, 1.f);
			for(unsigned i = 0; i < MAX_LIGHTS; ++i) {
				auto& loc = lights[i];
				auto& light = state.lights[i];
				if(loc.ambient >= 0)
					uniformColor(loc.ambient, noAmbient);
				if(loc.diffuse >= 0)
					uniformColor(loc.diffuse, light.diffuse);
				if(loc.specular >= 0)
					uniformColor(loc.specular, light.specular);
				if(loc.position >= 0)
					glUniform4fv(loc.position, 1, &light.position.x);
				if(loc.constantAttenuation >= 0)
					glUniform1f(loc.constantAttenuation, light.constantAttenuation);
				if(loc.linearAttenuation >= 0)
					glUniform1f(loc.linearAttenuation, light.linearAttenuation);
				if(loc.quadraticAttenuation >= 0)
					glUniform1f(loc.quadraticAttenuation, light.quadraticAttenuation);
			}
			if(lightModelAmbient >= 0)
				uniformColor(lightModelAmbient, state.lightModelAmbient);
			if(lightEnabled >= 0) {
				GLint enabled[MAX_LIGHTS];
				for(unsigned i = 0; i < MAX_LIGHTS; ++i)
					enabled[i] = state.lights[i].enabled ? 1 : 0;
				glUniform1iv(lightEnabled, MAX_LIGHTS, enabled);
			}
		}

		if(usesMaterial && materialSerial != state.materialSerial) {
			materialSerial = state.materialSerial;
			if(materialEmission >= 0)
				uniformColor(materialEmission, Colorf(0.f, 0.f, 0.f, 1.f));
			if(materialAmbient >= 0)
				uniformColor(materialAmbient, state.materialAmbient);
			if(materialDiffuse >= 0)
				uniformColor(materialDiffuse, state.materialDiffuse);
			if(materialSpecular >= 0)
				uniformColor(materialSpecular, state.materialSpecular);
			if(materialShininess >= 0)
				glUniform1f(materialShininess, state.materialShininess);
			if(fixedLighting >= 0)
				glUniform1i(fixedLighting, state.fixedLighting);
			if(fixedTexture >= 0)
				glUniform1i(fixedTexture, state.fixedTexture);
			if(alphaTest >= 0)
				glUniform1i(alphaTest, state.alphaTest);
			if(alphaRef >= 0)
				glUniform1f(alphaRef, state.alphaRef);
		}
	}

	unsigned cacheUniform(GLuint position, size_t size) {
		auto it = cached_uniforms.find(position);
		if(it != cached_uniforms.end())
			return it->second;

		unsigned pos = (unsigned)mem_buffer.size();
		cached_uniforms[position] = pos;

		for(size_t i = 0; i < size; ++i)
			mem_buffer.push_back(0);
		return pos;
	}

	GLuint compileShader(GLenum type, const std::string& fname, const std::string& source) {
		GLuint shader;

		//Compile
		const GLchar* str_ptr = (const GLchar*)source.c_str();
		GLint str_len = (GLint)source.size();
		shader = glCreateShader(type);
		glShaderSource(shader, 1, &str_ptr, &str_len);
		glCompileShader(shader);
		
		//Check compile value
		GLint shader_ok;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &shader_ok);

		if(!shader_ok) {
			error("Failed to compile shader: %s", fname.c_str());

			GLint log_length;
			char* log;

			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
			log = (char*)malloc(log_length);

			glGetShaderInfoLog(shader, log_length, NULL, log);
			error("%s", log);

			free(log);

			glDeleteShader(shader);
			return 0;
		}
		else if(getLogLevel() == LL_Info) {
			//Print warnings in verbose
			GLint log_length;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

			//We're always provided at least an empty string, just ignore very small logs
			if(log_length > 8) {
				char* log;
				log = (char*)malloc(log_length);

				glGetShaderInfoLog(shader, log_length, NULL, log);
				error("Shader '%s' has warnings:\n%s", fname.c_str(), log);

				free(log);
			}
		}

		return shader;
	}

	int compile() override {
		reset();
		auto vertex = getFileContents(vertex_file);
		auto fragment = getFileContents(fragment_file);
		preProcessShader(vertex);
		preProcessShader(fragment);
		if(vertex.empty()) {
			error("Could not open vertex shader, or the file was empty");
			return 1;
		}
		if(fragment.empty()) {
			error("Could not open fragment shader, or the file was empty");
			return 2;
		}

		//The core profile only takes modern GLSL, so the shim's output is all there is
		std::string vertexShimmed, fragmentShimmed, problems;
		if(!shimProgram(vertex, fragment, ST_GLSL410, vertexShimmed, fragmentShimmed, problems)) {
			error("Shaders %s and %s use built-ins with no replacement: %s",
				vertex_file.c_str(), fragment_file.c_str(), problems.c_str());
			return 1;
		}

		vertex_shader = compileShader(GL_VERTEX_SHADER, vertex_file, vertexShimmed);
		if(!vertex_shader)
			return 1;

		fragment_shader = compileShader(GL_FRAGMENT_SHADER, fragment_file, fragmentShimmed);
		if(!fragment_shader) {
			glDeleteShader(vertex_shader);
			return 2;
		}

		//Link entire program
		program = glCreateProgram();
		
		//Shaders name the same inputs in different ways; a program only uses one of each
		glBindAttribLocation(program, SA_Vertex, "in_vertex");
		glBindAttribLocation(program, SA_Vertex, "in_position");
		glBindAttribLocation(program, SA_Vertex, "sr_Vertex");
		glBindAttribLocation(program, SA_UV, "in_uv");
		glBindAttribLocation(program, SA_UV, "sr_MultiTexCoord0");
		glBindAttribLocation(program, SA_Normal, "in_normal");
		glBindAttribLocation(program, SA_Normal, "sr_Normal");
		glBindAttribLocation(program, SA_Tangent, "in_tangent");
		glBindAttribLocation(program, SA_Color, "in_color");
		glBindAttribLocation(program, SA_Color, "sr_Color");
		glBindAttribLocation(program, SA_UV2, "in_uv2");
		glBindFragDataLocation(program, 0, "sr_FragColor");

		glAttachShader(program, vertex_shader);
		glAttachShader(program, fragment_shader);
		glLinkProgram(program);

		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//Make sure it linked correctly
		GLint status;
		glGetProgramiv(program,GL_LINK_STATUS,&status);
		if(status == GL_FALSE) {
			error("Linking failed:");
			GLint log_length;
			char* log;

			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
			log = (char*)malloc(log_length);

			glGetProgramInfoLog(program, log_length, NULL, log);
			error("%s", log);

			free(log);

			glDeleteProgram(program);
			program = 0;
			return 3;
		}

		findBuiltins();
		return 0;
	}
};

class GLShader : public Shader {
public:
	mutable unsigned lastBoundFrame;

	std::vector<std::string> uniform_names;
	std::vector<GLuint> uniform_locations;
	std::vector<unsigned> uniform_vars;
	std::vector<unsigned> uniform_cache;

	GLShader()
	{
		program = 0;
		constant = true;
		dynamicFloats = 0;
		lastBoundFrame = 0xffffffff;
	}

	~GLShader() {
		reset();
	}

	void reset() {
		uniform_locations.clear();
		uniform_cache.clear();
		uniform_vars.clear();
	}

	virtual int compile() {
		reset();

		if(!program) {
			error("No associated program");
			return 4;
		}

		auto glProgram = (GLShaderProgram*)program;
		if(glProgram->program == 0)
			return 4;

		bool missingVars = false;
		for(unsigned i = 0; i < uniform_names.size(); ++i) {
			GLuint location = glGetUniformLocation(glProgram->program, uniform_names[i].c_str());

			if(location == (GLuint)-1) {
				//NOTE: This is no longer considered an error, because dynamic shader levels
				//are going to be causing uniforms to be missing all the time.
				//
				//error("Unable to find uniform '%s'", uniform_names[i].c_str());
				//missingVars = true;
			}
			else {
				uniform_locations.push_back(location);
				uniform_vars.push_back(i);
			}
		}

		for(unsigned i = 0; i < uniform_locations.size(); ++i) {
			auto& v = vars[uniform_vars[i]];
			size_t size = 0;
			switch(v.type) {
			case VT_int: case VT_float: size = 1; break;
			case VT_int2: case VT_float2: size = 2; break;
			case VT_int3: case VT_float3: size = 3; break;
			case VT_int4: case VT_float4: size = 4; break;
			case VT_mat3: size = 9; break;
			}

			size *= v.count;
			uniform_cache.push_back(glProgram->cacheUniform(uniform_locations[i], size));
		}

		if(missingVars)
			return 4;
		else
			return 0;
	}

	virtual void addVariable(const std::string& name, const Variable& var) {
		uniform_names.push_back(name);
		Shader::addVariable(name, var);
	}

	virtual void bind(float* dynamic) const {
		if(!program) {
			glUnbindProgram();
			return;
		}

		auto glProgram = (GLShaderProgram*)program;
		if(auto programID = glProgram->program) {
			if(boundProgramID != programID) {
				glUseProgram(programID);
				boundProgramID = programID;
			}
			boundProgram = glProgram;
		}
		else {
			glUnbindProgram();
			return;
		}

		const bool bindConst = lastBoundFrame != frameNumber || glProgram->lastShader != this;
		lastBoundFrame = frameNumber;
		glProgram->lastShader = this;

		if(dynamic) {
			float* floats = dynamic;

			for(size_t i = 0, cnt = uniform_locations.size(); i < cnt; ++i) {
				const Shader::Variable& var = vars[uniform_vars[i]];
				if(!bindConst && var.constant)
					continue;

				GLuint uniform = uniform_locations[i];
				void* uniformMem = (void*)&glProgram->mem_buffer[uniform_cache[i]];

				switch(var.type) {
					case VT_invalid:
					break;

					case VT_int:
						if(var.constant) {
							if(var._intcall)
								var._intcall(var._ints, var.count, var._args);

							if(memcmp(uniformMem, var._ints, var.count * 4) != 0) {
								glUniform1iv(uniform, var.count, var._ints);
								memcpy(uniformMem, var._ints, var.count * 4);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 4) != 0) {
								glUniform1iv(uniform, var.count, (int*)floats);
								memcpy(uniformMem, floats, var.count * 4);
							}
							floats += var.count;
						}
						break;
					case VT_int2:
						if(var.constant) {
							if(var._intcall)
								var._intcall(var._ints, var.count, var._args);

							if(memcmp(uniformMem, var._ints, var.count * 8) != 0) {
								glUniform2iv(uniform, var.count, var._ints);
								memcpy(uniformMem, var._ints, var.count * 8);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 8) != 0) {
								glUniform2iv(uniform, var.count, (int*)floats);
								memcpy(uniformMem, floats, var.count * 8);
							}
							floats += var.count * 2;
						}
						break;
					case VT_int3:
						if(var.constant) {
							if(var._intcall)
								var._intcall(var._ints, var.count, var._args);

							if(memcmp(uniformMem, var._ints, var.count * 12) != 0) {
								glUniform3iv(uniform, var.count, var._ints);
								memcpy(uniformMem, var._ints, var.count * 12);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 12) != 0) {
								glUniform3iv(uniform, var.count, (int*)floats);
								memcpy(uniformMem, floats, var.count * 12);
							}
							floats += var.count * 3;
						}
						break;
					case VT_int4:
						if(var.constant) {
							if(var._intcall)
								var._intcall(var._ints, var.count, var._args);

							if(memcmp(uniformMem, var._ints, var.count * 16) != 0) {
								glUniform4iv(uniform, var.count, var._ints);
								memcpy(uniformMem, var._ints, var.count * 16);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 16) != 0) {
								glUniform4iv(uniform, var.count, (int*)floats);
								memcpy(uniformMem, floats, var.count * 16);
							}
							floats += var.count * 4;
						}
						break;

					case VT_float:
						if(var.constant) {
							if(var._floatcall)
								var._floatcall(var._floats, var.count, var._args);

							if(memcmp(uniformMem, var._floats, var.count * 4) != 0) {
								glUniform1fv(uniform, var.count, var._floats);
								memcpy(uniformMem, var._floats, var.count * 4);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 4) != 0) {
								glUniform1fv(uniform, var.count, floats);
								memcpy(uniformMem, floats, var.count * 4);
							}
							floats += var.count;
						}
						break;
					case VT_float2:
						if(var.constant) {
							if(var._floatcall)
								var._floatcall(var._floats, var.count, var._args);

							if(memcmp(uniformMem, var._floats, var.count * 8) != 0) {
								glUniform2fv(uniform, var.count, var._floats);
								memcpy(uniformMem, var._floats, var.count * 8);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 8) != 0) {
								glUniform2fv(uniform, var.count, floats);
								memcpy(uniformMem, floats, var.count * 8);
							}
							floats += var.count * 2;
						}
						break;
					case VT_float3:
						if(var.constant) {
							if(var._floatcall)
								var._floatcall(var._floats, var.count, var._args);

							if(memcmp(uniformMem, var._floats, var.count * 12) != 0) {
								glUniform3fv(uniform, var.count, var._floats);
								memcpy(uniformMem, var._floats, var.count * 12);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 12) != 0) {
								glUniform3fv(uniform, var.count, floats);
								memcpy(uniformMem, floats, var.count * 12);
							}
							floats += var.count * 3;
						}
						break;
					case VT_float4:
						if(var.constant) {
							if(var._floatcall)
								var._floatcall(var._floats, var.count, var._args);

							if(memcmp(uniformMem, var._floats, var.count * 16) != 0) {
								glUniform4fv(uniform, var.count, var._floats);
								memcpy(uniformMem, var._floats, var.count * 16);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 16) != 0) {
								glUniform4fv(uniform, var.count, floats);
								memcpy(uniformMem, floats, var.count * 16);
							}
							floats += var.count * 4;
						}
						break;
					case VT_mat3:
						if(var.constant) {
							if(var._floatcall)
								var._floatcall(var._floats, var.count, var._args);

							if(memcmp(uniformMem, var._floats, var.count * 36) != 0) {
								glUniformMatrix3fv(uniform, var.count, false, var._floats);
								memcpy(uniformMem, var._floats, var.count * 36);
							}
						}
						else if(var._intcall) {
							if(memcmp(uniformMem, floats, var.count * 36) != 0) {
								glUniformMatrix3fv(uniform, var.count, false, floats);
								memcpy(uniformMem, floats, var.count * 36);
							}
							floats += var.count * 9;
						}
					NO_DEFAULT
				}
			}
		}
		else {
			for(size_t i = 0, cnt = uniform_locations.size(); i < cnt; ++i) {
				const Shader::Variable& var = vars[uniform_vars[i]];
				if(!bindConst && var.constant)
					continue;

				GLuint uniform = uniform_locations[i];
				void* uniformMem = (void*)&glProgram->mem_buffer[uniform_cache[i]];

				switch(var.type) {
					case VT_invalid:
					break;

					case VT_int:
						if(var._intcall)
							var._intcall(var._ints,var.count,var._args);
						if(memcmp(uniformMem, var._ints, var.count * 4) != 0) {
							glUniform1iv(uniform, var.count, var._ints);
							memcpy(uniformMem, var._ints, var.count * 4);
						}
						break;
					case VT_int2:
						if(var._intcall)
							var._intcall(var._ints,var.count,var._args);
						if(memcmp(uniformMem, var._ints, var.count * 8) != 0) {
							glUniform2iv(uniform, var.count, var._ints);
							memcpy(uniformMem, var._ints, var.count * 8);
						}
						break;
					case VT_int3:
						if(var._intcall)
							var._intcall(var._ints,var.count,var._args);
						if(memcmp(uniformMem, var._ints, var.count * 12) != 0) {
							glUniform3iv(uniform, var.count, var._ints);
							memcpy(uniformMem, var._ints, var.count * 12);
						}
						break;
					case VT_int4:
						if(var._intcall)
							var._intcall(var._ints,var.count,var._args);
						if(memcmp(uniformMem, var._ints, var.count * 16) != 0) {
							glUniform4iv(uniform, var.count, var._ints);
							memcpy(uniformMem, var._ints, var.count * 16);
						}
						break;

					case VT_float:
						if(var._floatcall)
							var._floatcall(var._floats,var.count,var._args);
						if(memcmp(uniformMem, var._floats, var.count * 4) != 0) {
							glUniform1fv(uniform, var.count, var._floats);
							memcpy(uniformMem, var._floats, var.count * 4);
						}
						break;
					case VT_float2:
						if(var._floatcall)
							var._floatcall(var._floats,var.count,var._args);
						if(memcmp(uniformMem, var._floats, var.count * 8) != 0) {
							glUniform2fv(uniform, var.count, var._floats);
							memcpy(uniformMem, var._floats, var.count * 8);
						}
						break;
					case VT_float3:
						if(var._floatcall)
							var._floatcall(var._floats,var.count,var._args);
						if(memcmp(uniformMem, var._floats, var.count * 12) != 0) {
							glUniform3fv(uniform, var.count, var._floats);
							memcpy(uniformMem, var._floats, var.count * 12);
						}
						break;
					case VT_float4:
						if(var._floatcall)
							var._floatcall(var._floats,var.count,var._args);
						if(memcmp(uniformMem, var._floats, var.count * 16) != 0) {
							glUniform4fv(uniform, var.count, var._floats);
							memcpy(uniformMem, var._floats, var.count * 16);
						}
						break;

					case VT_mat3:
						if(var._floatcall)
							var._floatcall(var._floats,var.count,var._args);
						if(memcmp(uniformMem, var._floats, var.count * 36) != 0) {
							glUniformMatrix3fv(uniform, var.count, false, var._floats);
							memcpy(uniformMem, var._floats, var.count * 36);
						}
						break;
					NO_DEFAULT
				}
			}
		}
	}

	void updateDynamicVars() const {
		for(size_t i = 0, cnt = uniform_locations.size(); i < cnt; ++i) {
			const Shader::Variable& var = vars[uniform_vars[i]];
			if(var.constant)
				continue;
			GLuint uniform = uniform_locations[i];
			void* uniformMem = (void*)&((GLShaderProgram*)program)->mem_buffer[uniform_cache[i]];

			switch(var.type) {
				case VT_invalid:
				break;

				case VT_int:
					if(var._intcall)
						var._intcall(var._ints,var.count,var._args);
					if(memcmp(uniformMem, var._ints, var.count * 4) != 0) {
						glUniform1iv(uniform, var.count, var._ints);
						memcpy(uniformMem, var._ints, var.count * 4);
					}
					break;
				case VT_int2:
					if(var._intcall)
						var._intcall(var._ints,var.count,var._args);
					if(memcmp(uniformMem, var._ints, var.count * 8) != 0) {
						glUniform2iv(uniform, var.count, var._ints);
						memcpy(uniformMem, var._ints, var.count * 8);
					}
					break;
				case VT_int3:
					if(var._intcall)
						var._intcall(var._ints,var.count,var._args);
					if(memcmp(uniformMem, var._ints, var.count * 12) != 0) {
						glUniform3iv(uniform, var.count, var._ints);
						memcpy(uniformMem, var._ints, var.count * 12);
					}
					break;
				case VT_int4:
					if(var._intcall)
						var._intcall(var._ints,var.count,var._args);
					if(memcmp(uniformMem, var._ints, var.count * 16) != 0) {
						glUniform4iv(uniform, var.count, var._ints);
						memcpy(uniformMem, var._ints, var.count * 16);
					}
					break;

				case VT_float:
					if(var._floatcall)
						var._floatcall(var._floats,var.count,var._args);
					if(memcmp(uniformMem, var._floats, var.count * 4) != 0) {
						glUniform1fv(uniform, var.count, var._floats);
						memcpy(uniformMem, var._floats, var.count * 4);
					}
					break;
				case VT_float2:
					if(var._floatcall)
						var._floatcall(var._floats,var.count,var._args);
					if(memcmp(uniformMem, var._floats, var.count * 8) != 0) {
						glUniform2fv(uniform, var.count, var._floats);
						memcpy(uniformMem, var._floats, var.count * 8);
					}
					break;
				case VT_float3:
					if(var._floatcall)
						var._floatcall(var._floats,var.count,var._args);
					if(memcmp(uniformMem, var._floats, var.count * 12) != 0) {
						glUniform3fv(uniform, var.count, var._floats);
						memcpy(uniformMem, var._floats, var.count * 12);
					}
					break;
				case VT_float4:
					if(var._floatcall)
						var._floatcall(var._floats,var.count,var._args);
					if(memcmp(uniformMem, var._floats, var.count * 16) != 0) {
						glUniform4fv(uniform, var.count, var._floats);
						memcpy(uniformMem, var._floats, var.count * 16);
					}
					break;

				case VT_mat3:
					if(var._floatcall)
						var._floatcall(var._floats,var.count,var._args);
					if(memcmp(uniformMem, var._floats, var.count * 36) != 0) {
						glUniformMatrix3fv(uniform, var.count, false, var._floats);
						memcpy(uniformMem, var._floats, var.count * 36);
					}
					break;
				NO_DEFAULT
			}
		}
	}

	void saveDynamicVars(float* buffer) const {
		float* floats = buffer;

		for(size_t i = 0, cnt = uniform_locations.size(); i < cnt; ++i) {
			const Shader::Variable& var = vars[uniform_vars[i]];
			if(var.constant)
				continue;

			switch(var.type) {
				case VT_invalid:
				break;

				case VT_int:
					if(var._intcall) {
						var._intcall((int*)floats,var.count,var._args);
						floats += var.count;
					}
					break;
				case VT_int2:
					if(var._intcall) {
						var._intcall((int*)floats,var.count,var._args);
						floats += var.count * 2;
					}
					break;
				case VT_int3:
					if(var._intcall) {
						var._intcall((int*)floats,var.count,var._args);
						floats += var.count * 3;
					}
					break;
				case VT_int4:
					if(var._intcall) {
						var._intcall((int*)floats,var.count,var._args);
						floats += var.count * 4;
					}
					break;

				case VT_float:
					if(var._floatcall) {
						var._floatcall(floats,var.count,var._args);
						floats += var.count;
					}
					break;
				case VT_float2:
					if(var._floatcall) {
						var._floatcall(floats,var.count,var._args);
						floats += var.count * 2;
					}
					break;
				case VT_float3:
					if(var._floatcall) {
						var._floatcall(floats,var.count,var._args);
						floats += var.count * 3;
					}
					break;
				case VT_float4:
					if(var._floatcall) {
						var._floatcall(floats,var.count,var._args);
						floats += var.count * 4;
					}
					break;

				case VT_mat3:
					if(var._floatcall) {
						var._floatcall(floats,var.constant,var._args);
						floats += var.constant * 9;
					}
				NO_DEFAULT
			}
		}
	}

	void loadDynamicVars(float* buffer) const {
		float* floats = buffer;

		for(size_t i = 0, cnt = uniform_locations.size(); i < cnt; ++i) {
			const Shader::Variable& var = vars[uniform_vars[i]];
			if(var.constant)
				continue;

			GLuint uniform = uniform_locations[i];
			void* uniformMem = (void*)&((GLShaderProgram*)program)->mem_buffer[uniform_cache[i]];

			switch(var.type) {
				case VT_invalid:
				break;

				case VT_int:
					if(var._intcall) {
						if(memcmp(uniformMem, floats, var.count * 4) != 0) {
							glUniform1iv(uniform, var.count, (int*)floats);
							memcpy(uniformMem, floats, var.count * 4);
						}
						floats += var.count;
					}
					break;
				case VT_int2:
					if(var._intcall) {
						if(memcmp(uniformMem, floats, var.count * 8) != 0) {
							glUniform2iv(uniform, var.count, (int*)floats);
							memcpy(uniformMem, floats, var.count * 8);
						}
						floats += var.count * 2;
					}
					break;
				case VT_int3:
					if(var._intcall) {
						if(memcmp(uniformMem, floats, var.count * 12) != 0) {
							glUniform3iv(uniform, var.count, (int*)floats);
							memcpy(uniformMem, floats, var.count * 12);
						}
						floats += var.count * 3;
					}
					break;
				case VT_int4:
					if(var._intcall) {
						if(memcmp(uniformMem, floats, var.count * 16) != 0) {
							glUniform4iv(uniform, var.count, (int*)floats);
							memcpy(uniformMem, floats, var.count * 16);
						}
						floats += var.count * 4;
					}
					break;

				case VT_float:
					if(var._floatcall) {
						if(memcmp(uniformMem, floats, var.count * 4) != 0) {
							glUniform1fv(uniform, var.count, floats);
							memcpy(uniformMem, floats, var.count * 4);
						}
						floats += var.count;
					}
					break;
				case VT_float2:
					if(var._floatcall) {
						if(memcmp(uniformMem, floats, var.count * 8) != 0) {
							glUniform2fv(uniform, var.count, floats);
							memcpy(uniformMem, floats, var.count * 8);
						}
						floats += var.count * 2;
					}
					break;
				case VT_float3:
					if(var._floatcall) {
						if(memcmp(uniformMem, floats, var.count * 12) != 0) {
							glUniform3fv(uniform, var.count, floats);
							memcpy(uniformMem, floats, var.count * 12);
						}
						floats += var.count * 3;
					}
					break;
				case VT_float4:
					if(var._floatcall) {
						if(memcmp(uniformMem, floats, var.count * 16) != 0) {
							glUniform4fv(uniform, var.count, floats);
							memcpy(uniformMem, floats, var.count * 16);
						}
						floats += var.count * 4;
					}
					break;

				case VT_mat3:
					if(var._floatcall) {
						if(memcmp(uniformMem, floats, var.count * 36) != 0) {
							glUniformMatrix3fv(uniform, var.count, false, floats);
							memcpy(uniformMem, floats, var.count * 36);
						}
						floats += var.count * 9;
					}
					break;
				NO_DEFAULT
			}
		}
	}
};

void glUnbindProgram() {
	if(boundProgramID != 0) {
		glUseProgram(0);
		boundProgramID = 0;
	}
	boundProgram = nullptr;
}

bool glProgramBound() {
	return boundProgram != nullptr;
}

void glUploadBuiltins(const BuiltinState& state) {
	if(boundProgram)
		boundProgram->uploadBuiltins(state);
}

Shader* createGLShader() {
	return new GLShader();
}

ShaderProgram* createGLShaderProgram(const char* vertex_shader, const char* fragment_shader) {
	return new GLShaderProgram(vertex_shader, fragment_shader);
}

};
