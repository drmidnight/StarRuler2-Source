//Checks that every shader program translates to Metal: each Shader: entry in
//data/shaders/*.txt goes through the shim, glslang, SPIRV-Cross and the Metal
//compiler. Run it from the repository root; failures are printed and make the
//exit code 1.
//
//Usage: shadertool [--mod <folder>]... [--out <folder>] [--no-metal] [--verbose]
#include "render/shader_shim.h"
#include "render/shader_translate.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Program {
	std::string name, vertex, fragment;
	//Folder the program's paths are relative to first
	fs::path root;
};

static std::vector<fs::path> roots;

static bool readFile(const fs::path& path, std::string& out) {
	std::ifstream file(path, std::ios::binary);
	if(!file)
		return false;
	std::stringstream ss;
	ss << file.rdbuf();
	out = ss.str();
	return true;
}

//Mods override files of the base game, the same way the game resolves them
static fs::path resolve(const fs::path& first, const std::string& file) {
	if(fs::exists(first / file))
		return first / file;
	for(auto it = roots.rbegin(); it != roots.rend(); ++it)
		if(fs::exists(*it / file))
			return *it / file;
	return first / file;
}

static std::string trim(const std::string& s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if(start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

static void readPrograms(const fs::path& root, std::vector<Program>& programs) {
	fs::path folder = root / "data/shaders";
	if(!fs::is_directory(folder))
		return;
	std::vector<fs::path> files;
	for(auto& entry : fs::directory_iterator(folder))
		if(entry.path().extension() == ".txt")
			files.push_back(entry.path());
	std::sort(files.begin(), files.end());

	for(auto& file : files) {
		std::ifstream in(file);
		std::string line;
		Program current;
		while(std::getline(in, line)) {
			line = trim(line);
			auto value = [&]() { return trim(line.substr(line.find(':') + 1)); };
			if(line.rfind("Shader:", 0) == 0) {
				current = Program();
				current.name = value();
				current.root = root;
			}
			else if(line.rfind("Vertex:", 0) == 0) {
				current.vertex = value();
			}
			else if(line.rfind("Fragment:", 0) == 0) {
				current.fragment = value();
				if(!current.name.empty() && !current.vertex.empty())
					programs.push_back(current);
			}
		}
	}
}

//Settings placeholders all take a boolean, so true stands in for each of them
static bool preprocess(const fs::path& root, const std::string& file, std::string& out, std::string& error) {
	fs::path path = resolve(root, file);
	if(!readFile(path, out)) {
		error = "could not read " + path.string();
		return false;
	}
	out = std::regex_replace(out, std::regex("#\\{\\{[^}]*\\}\\}"), "true");

	std::regex include("#include \"([^\"]*)\"");
	std::smatch match;
	while(std::regex_search(out, match, include)) {
		std::string contents;
		if(!preprocess(root, match[1].str(), contents, error))
			return false;
		out = match.prefix().str() + contents + match.suffix().str();
	}
	return true;
}

static bool shim(const Program& program, std::string& vertex, std::string& fragment, std::string& error) {
	std::string vertexSource, fragmentSource, problems;
	if(!preprocess(program.root, program.vertex, vertexSource, error)
		|| !preprocess(program.root, program.fragment, fragmentSource, error))
		return false;
	if(!render::shimProgram(vertexSource, fragmentSource, render::ST_GLSL410, vertex, fragment, problems)) {
		error = "uses built-ins with no mapping: " + problems;
		return false;
	}
	return true;
}

static bool compileMetal(const fs::path& source, std::string& error) {
	fs::path air = source;
	air.replace_extension(".air");
	std::string command = "xcrun -sdk macosx metal -std=metal3.0 -c \"" + source.string()
		+ "\" -o \"" + air.string() + "\" 2>&1";
	FILE* pipe = popen(command.c_str(), "r");
	if(!pipe) {
		error = "could not run the Metal compiler";
		return false;
	}
	std::string output;
	char buffer[512];
	while(fgets(buffer, sizeof(buffer), pipe))
		output += buffer;
	if(pclose(pipe) != 0) {
		error = output;
		return false;
	}
	return true;
}

int main(int argc, char** argv) {
	fs::path out = fs::temp_directory_path() / "sr2-shadertool";
	bool metal = true, verbose = false;

	roots.push_back(fs::current_path());
	for(int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if(arg == "--mod" && i + 1 < argc)
			roots.push_back(argv[++i]);
		else if(arg == "--out" && i + 1 < argc)
			out = argv[++i];
		else if(arg == "--no-metal")
			metal = false;
		else if(arg == "--verbose")
			verbose = true;
		else {
			fprintf(stderr, "Usage: shadertool [--mod <folder>]... [--out <folder>] [--no-metal] [--verbose]\n");
			return 2;
		}
	}
	fs::create_directories(out);

	std::vector<Program> programs;
	for(auto& root : roots)
		readPrograms(root, programs);

	unsigned failed = 0;
	for(auto& program : programs) {
		std::string error, vertex, fragment;
		render::TranslatedStage vs, fs;
		bool ok = shim(program, vertex, fragment, error)
			&& render::translateProgram(program.vertex, vertex, program.fragment, fragment, vs, fs, error);

		if(ok) {
			std::string base = (out / program.name).string();
			std::ofstream(base + ".vs.metal") << vs.msl;
			std::ofstream(base + ".fs.metal") << fs.msl;
			if(metal)
				ok = compileMetal(base + ".vs.metal", error) && compileMetal(base + ".fs.metal", error);
		}

		if(!ok) {
			++failed;
			printf("FAIL %s: %s\n", program.name.c_str(), error.c_str());
		}
		else if(verbose) {
			printf("ok   %s: vertex %u uniform bytes, fragment %u uniform bytes, %zu textures\n",
				program.name.c_str(), vs.reflection.uniformBufferSize, fs.reflection.uniformBufferSize,
				fs.reflection.textures.size() + vs.reflection.textures.size());
		}
	}

	printf("%zu programs, %u failed. Output in %s\n", programs.size(), failed, out.string().c_str());
	return failed ? 1 : 0;
}
