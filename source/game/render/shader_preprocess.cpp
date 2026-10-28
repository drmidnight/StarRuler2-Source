#include "render/shader.h"
#include "compat/misc.h"
#include "str_util.h"
#include "files.h"
#include "main/logging.h"
#include "main/references.h"

namespace render {

void preProcessShader(std::string& shader) {
	std::string output;
	size_t start = 0;
	while(start < shader.size()) {
		size_t pos = shader.find('#', start);
		if(pos == std::string::npos)
			break;

		if(pos != start)
			output += shader.substr(start, pos-start);

		if(shader.size() - pos > 3 && shader[pos+1] == '{' && shader[pos+2] == '{') {
			size_t endpos = shader.find("}}", pos, 2);
			if(endpos == std::string::npos)
				break;

			std::string value = shader.substr(pos+3, endpos-pos-3);
			std::string replace;

			if(value == "level:extreme") {
				auto* sl = devices.settings.engine.getSetting("iShaderLevel");
				if(sl)
					output += (sl->getInteger() >= 4) ? "true" : "false";
			}
			else if(value == "level:high") {
				auto* sl = devices.settings.engine.getSetting("iShaderLevel");
				if(sl)
					output += (sl->getInteger() >= 3) ? "true" : "false";
			}
			else if(value == "level:medium") {
				auto* sl = devices.settings.engine.getSetting("iShaderLevel");
				if(sl)
					output += (sl->getInteger() >= 2) ? "true" : "false";
			}
			else if(value == "fallback") {
				auto* fallback = devices.settings.engine.getSetting("bShaderFallback");
				if(fallback)
					output += (fallback->getBool()) ? "true" : "false";
			}
			else {
				auto* setting = devices.settings.engine.getSetting(value.c_str());
				if(!setting)
					setting = devices.settings.mod.getSetting(value.c_str());

				if(setting) {
					switch(setting->type) {
						case GT_Bool:
							output += (setting->getBool()) ? "true" : "false";
						break;
						case GT_Integer:
						case GT_Enum:
							output += toString<int>(setting->getInteger());
						break;
						case GT_Double:
							output += toString<double>(setting->getDouble());
						break;
						case GT_String:
							output += *setting->getString();
						break;
					}
				}
				else {
					error("ERROR: Could not find shader variable %s.", value.c_str());
				}
			}

			start = endpos + 2;
		}
		else if(shader.size() - pos > 11 && shader.compare(pos, 10, "#include \"") == 0) {
			size_t endpos = shader.find("\"", pos+10, 1);
			if(endpos == std::string::npos)
				break;

			std::string value = shader.substr(pos+10, endpos-pos-10);
			std::string fname = devices.mods.resolve(value);
			if(!fileExists(fname)) {
				error("ERROR COMPILING SHADER: Could not find include file %s. (Resolved to %s)", value.c_str(), fname.c_str());
			}
			else {
				std::string includeContents = getFileContents(fname);
				preProcessShader(includeContents);
				output += includeContents;
			}

			start = endpos + 1;
		}
		else {
			output += "#";
			start = pos + 1;
		}
	}
	if(start < shader.size())
		output += shader.substr(start, shader.size()-start);
	shader = output;
}

};
