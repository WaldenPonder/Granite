#pragma once

#include "muglm/matrix_helper.hpp"
#include "muglm/muglm_impl.hpp"
#include "Trackball.h"
#include "application.hpp"
#include "command_buffer.hpp"
#include "device.hpp"

#include "os_filesystem.hpp"
#include "render_context.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "task_composer.hpp"
#include <fstream>

using namespace Granite;
using namespace Vulkan;

struct AtmosphereParameters
{
	// Radius of the planet (center to ground)
	float BottomRadius = 6360.0f;
	// Maximum considered atmosphere height (center to atmosphere top)
	float TopRadius = 6460.0f;

	// Rayleigh scattering exponential distribution scale in the atmosphere
	float RayleighDensityExpScale = -1.f / 8.0f;

	// Another medium type in the atmosphere
	float AbsorptionDensity0LayerWidth = 25.f;

	// Rayleigh scattering coefficients
	vec3 RayleighScattering = vec3(0.005802f, 0.013558f, 0.033100f);

	// Mie scattering exponential distribution scale in the atmosphere
	float MieDensityExpScale = -1.f / 1.2f;

	// Mie scattering coefficients
	vec3 MieScattering = vec3(0.003996f, 0.003996f, 0.003996f);

	float AbsorptionDensity0ConstantTerm = -2.0f / 3.0f;

	// Mie extinction coefficients
	vec3 MieExtinction = vec3(0.00443999982, 0.00443999982, 0.00443999982);
	float AbsorptionDensity0LinearTerm = 1.0f / 15.0f;

	// Mie absorption coefficients
	vec3 MieAbsorption;

	// Mie phase function excentricity
	float MiePhaseG = 0.8f;

	// This other medium only absorb light, e.g. useful to represent ozone in the earth atmosphere
	vec3 AbsorptionExtinction = vec3(0.000650f, 0.001881f, 0.000085f);
	float AbsorptionDensity1ConstantTerm = 8.0f / 3.0f;

	// The albedo of the ground.
	vec3 GroundAlbedo = vec3(0.0f, 0.0f, 0.0f);
	float AbsorptionDensity1LinearTerm = -1.0f / 15.0f;

	vec2 RayMarchMinMaxSPP = vec2(4, 14);
	float screenWidth = 1280.f;
	float screenHeight = 720.f;
};

struct UBO
{
	vec4 camarePos;
	mat4 MVP;
	mat4 inversMVP;
	mat4 projectMat;
	mat4 invProjMat;
	mat4 invViewMat;
};

extern AtmosphereParameters push;
extern UBO ubo;

void setup_atmosphere(Granite::RenderGraph &graph, Renderer &renderer, RenderQueue &queue, VisibilityList &visible,
                      RenderContext &context, Scene &scene);

using namespace Granite;

class TexturePlane2 : public AbstractRenderable, public RenderPassCreator
{
public:
	TexturePlane2(const std::string &normal);

	void set_reflection_name(const std::string &name)
	{
		need_reflection = !name.empty();
		reflection_name = name;
	}

	void set_refraction_name(const std::string &name)
	{
		need_refraction = !name.empty();
		refraction_name = name;
	}

	void set_resolution_scale(float x, float y)
	{
		scale_x = x;
		scale_y = y;
	}

	vec4 get_plane() const
	{
		return vec4(normal, -dot(normal, position));
	}

	void get_render_info(const RenderContext &context, const RenderInfoComponent *transform,
	                     RenderQueue &queue) const override;

	void set_plane(const vec3 &position, const vec3 &normal, const vec3 &up, float extent_up, float extent_across);
	void set_base_emissive(const vec3 &color)
	{
		base_emissive = color;
	}

	void set_zfar(float zfar);

private:
	const Vulkan::ImageView *reflection = nullptr;
	const Vulkan::ImageView *refraction = nullptr;
	ImageAssetID normalmap;
	RenderQueue internal_queue;

	vec3 position;
	vec3 normal;
	vec3 up;
	vec3 dpdx;
	vec3 dpdy;
	vec3 base_emissive;
	float rad_up = 0.0f;
	float rad_x = 0.0f;
	float zfar = 100.0f;
	float scale_x = 1.0f;
	float scale_y = 1.0f;

	std::string reflection_name;
	std::string refraction_name;

	const RendererSuite *renderer_suite = nullptr;
	const RenderContext *base_context = nullptr;
	RenderContext context;
	Scene *scene = nullptr;
	VisibilityList visible;

	bool need_reflection = false;
	bool need_refraction = false;

	enum Type
	{
		Reflection,
		Refraction
	};
	void add_render_pass(RenderGraph &graph, Type type);

	void add_render_passes(RenderGraph &graph) override;
	void set_base_renderer(const RendererSuite *suite) override;
	void set_base_render_context(const RenderContext *context) override;
	void setup_render_pass_dependencies(RenderGraph &graph, Granite::RenderPass &target,
	                                    RenderPassCreator::DependencyFlags dep_flags) override;
	void setup_render_pass_dependencies(RenderGraph &graph) override;
	void setup_render_pass_resources(RenderGraph &graph) override;
	void set_scene(Scene *scene) override;

	void render_main_pass(Vulkan::CommandBuffer &cmd, const mat4 &proj, const mat4 &view);
};