#include "render/shader_translate.h"
#include "glslang/Public/ShaderLang.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/SPIRV/GlslangToSpv.h"
#include "spirv_cross/spirv_msl.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace render {

//Loose uniforms are gathered into a block with this name
static const char* uniformBlockName = "SRUniforms";

static const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);

static void initGlslang() {
	static std::once_flag once;
	std::call_once(once, [] { glslang::InitializeProcess(); });
}

static bool parseStage(glslang::TShader& shader, EShLanguage stage, const std::string& name,
	const std::string& source, std::string& error)
{
	const char* text = source.c_str();
	const char* fileName = name.c_str();
	int length = (int)source.size();
	shader.setStringsWithLengthsAndNames(&text, &length, &fileName, 1);

	//Vulkan's relaxed rules let GL-style loose uniforms and samplers through
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	shader.setEnvInputVulkanRulesRelaxed();
	shader.setGlobalUniformBlockName(uniformBlockName);
	shader.setGlobalUniformSet(0);
	shader.setGlobalUniformBinding(0);
	shader.setAutoMapLocations(true);
	shader.setAutoMapBindings(true);

	if(!shader.parse(GetDefaultResources(), 410, ECoreProfile, false, false, messages)) {
		error = name + ": " + shader.getInfoLog();
		return false;
	}
	return true;
}

//Adds the members of a uniform block, named the way GL names them
static void reflectMembers(const spirv_cross::CompilerMSL& msl, const spirv_cross::SPIRType& type,
	unsigned base, const std::string& prefix, std::vector<ShaderReflection::Uniform>& out)
{
	for(unsigned i = 0; i < (unsigned)type.member_types.size(); ++i) {
		auto& member = msl.get_type(type.member_types[i]);
		std::string name = prefix + msl.get_member_name(type.self, i);
		unsigned offset = base + msl.type_struct_member_offset(type, i);

		if(!member.array.empty()) {
			unsigned count = member.array[0];
			unsigned stride = msl.type_struct_member_array_stride(type, i);
			if(member.basetype == spirv_cross::SPIRType::Struct) {
				auto& element = msl.get_type(member.self);
				for(unsigned e = 0; e < count; ++e)
					reflectMembers(msl, element, offset + e * stride, name + "[" + std::to_string(e) + "].", out);
			}
			else {
				out.push_back({name, offset, stride * count, stride, count});
			}
		}
		else if(member.basetype == spirv_cross::SPIRType::Struct) {
			reflectMembers(msl, msl.get_type(member.self), offset, name + ".", out);
		}
		else {
			unsigned size = (unsigned)msl.get_declared_struct_member_size(type, i);
			out.push_back({name, offset, size, 0, 0});
		}
	}
}

static bool toMSL(std::vector<unsigned>&& spirv, spv::ExecutionModel model, const char* entryPoint,
	const std::unordered_map<std::string, unsigned>* inputLocations,
	std::unordered_map<std::string, unsigned>* outputLocations,
	TranslatedStage& out, std::string& error)
{
	try {
		spirv_cross::CompilerMSL msl(std::move(spirv));

		auto options = msl.get_msl_options();
		options.platform = spirv_cross::CompilerMSL::Options::macOS;
		options.set_msl_version(3, 0);
		msl.set_msl_options(options);

		//GL clip space has depth from -1 to 1, Metal's from 0 to 1
		auto common = msl.get_common_options();
		common.vertex.fixup_clipspace = true;
		msl.set_common_options(common);

		msl.rename_entry_point("main", entryPoint, model);

		auto resources = msl.get_shader_resources();

		//Fragment inputs take the location of the vertex output with the same name
		if(inputLocations) {
			for(auto& input : resources.stage_inputs) {
				auto it = inputLocations->find(input.name);
				if(it == inputLocations->end()) {
					error = "fragment input '" + input.name + "' is not written by the vertex shader";
					return false;
				}
				msl.set_decoration(input.id, spv::DecorationLocation, it->second);
			}
		}
		if(outputLocations)
			for(auto& output : resources.stage_outputs)
				(*outputLocations)[output.name] = msl.get_decoration(output.id, spv::DecorationLocation);

		out.msl = msl.compile();
		out.entryPoint = entryPoint;

		auto& reflection = out.reflection;
		reflection.uniformBufferIndex = -1;
		reflection.uniformBufferSize = 0;
		for(auto& buffer : resources.uniform_buffers) {
			auto& type = msl.get_type(buffer.base_type_id);
			reflection.uniformBufferIndex = (int)msl.get_automatic_msl_resource_binding(buffer.id);
			reflection.uniformBufferSize = (unsigned)msl.get_declared_struct_size(type);
			reflectMembers(msl, type, 0, "", reflection.uniforms);
		}
		for(auto& image : resources.sampled_images) {
			auto& type = msl.get_type(image.type_id);
			ShaderReflection::Texture texture;
			texture.name = image.name;
			texture.cube = type.image.dim == spv::DimCube;
			texture.textureIndex = msl.get_automatic_msl_resource_binding(image.id);
			texture.samplerIndex = msl.get_automatic_msl_resource_binding_secondary(image.id);
			reflection.textures.push_back(texture);
		}
		if(model == spv::ExecutionModelVertex)
			for(auto& input : resources.stage_inputs)
				reflection.inputs.push_back({input.name, msl.get_decoration(input.id, spv::DecorationLocation)});
	}
	catch(const std::exception& e) {
		error = e.what();
		return false;
	}
	return true;
}

bool translateProgram(const std::string& vertexName, const std::string& vertex,
	const std::string& fragmentName, const std::string& fragment,
	TranslatedStage& vs, TranslatedStage& fs, std::string& error)
{
	initGlslang();

	glslang::TShader vertexShader(EShLangVertex), fragmentShader(EShLangFragment);
	if(!parseStage(vertexShader, EShLangVertex, vertexName, vertex, error))
		return false;
	if(!parseStage(fragmentShader, EShLangFragment, fragmentName, fragment, error))
		return false;

	glslang::TProgram program;
	program.addShader(&vertexShader);
	program.addShader(&fragmentShader);
	if(!program.link(messages) || !program.mapIO()) {
		error = vertexName + " + " + fragmentName + ": " + program.getInfoLog();
		return false;
	}

	std::vector<unsigned> vertexSpirv, fragmentSpirv;
	glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), vertexSpirv);
	glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), fragmentSpirv);

	std::unordered_map<std::string, unsigned> varyings;
	if(!toMSL(std::move(vertexSpirv), spv::ExecutionModelVertex, "sr_vertex", nullptr, &varyings, vs, error)) {
		error = vertexName + ": " + error;
		return false;
	}
	if(!toMSL(std::move(fragmentSpirv), spv::ExecutionModelFragment, "sr_fragment", &varyings, nullptr, fs, error)) {
		error = fragmentName + ": " + error;
		return false;
	}
	return true;
}

};
