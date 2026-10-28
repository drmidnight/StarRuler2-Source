#pragma once
#include "render/shader.h"
#include "render/shader_builtins.h"

namespace render {
Shader* createGLShader();
ShaderProgram* createGLShaderProgram(const char* vertex_shader, const char* fragment_shader);
//Stops using any program, for fixed-function drawing
void glUnbindProgram();
bool glProgramBound();
//Sends the bound program the built-in state it reads
void glUploadBuiltins(const BuiltinState& state);
};
