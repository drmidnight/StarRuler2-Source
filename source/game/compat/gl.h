#pragma once
//macOS core profiles need no loader, and this header only declares core GL,
//so any use of the old fixed-function API fails to compile
#include <OpenGL/gl3.h>
