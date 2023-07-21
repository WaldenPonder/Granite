#include "flat_renderer.hpp"
#include "path.hpp"
#include "scene_loader.hpp"
#include "ui_manager.hpp"
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

using namespace muglm;
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
	muglm::vec3 RayleighScattering = vec3(0.005802f, 0.013558f, 0.033100f);

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

class Prophet : public Application, public EventHandler
{
public:
	
	Prophet();
		
	std::string get_name() override
	{
		return "Prophet";
	}

	unsigned get_default_width() override
	{
		return 1920;
	}

	unsigned get_default_height() override
	{
		return 1080;
	}

	void on_device_created(const DeviceCreatedEvent &e);
	void on_device_destroyed(const DeviceCreatedEvent &e);

	void on_swapchain_changed(const SwapchainParameterEvent &e);
	void on_swapchain_destroyed(const SwapchainParameterEvent &e);

	void setup_shadow_map();
	void render_frame(double, double elapsed_time);
	void post_frame();

	void add_shadow_pass();
	void setup_atmosphere();

	void createUi();

private:
	float elapsed_time = 0.f;
	ImageHandle render_target;
	RenderGraph graph;
	ImageAssetID imageId;
	Trackball cam;

	SceneLoader scene_loader;
	FlatRenderer flat_renderer;
	Renderer renderer;
	RenderQueue queue;
	VisibilityList visible;

	RenderContext depth_context;
	RenderContext context;
	LightingParameters lighting = {};
	RendererSuite::Config renderer_suite_config;

	AtmosphereParameters push;
	UBO ubo;
	RendererSuite renderer_suite;
	RenderTextureResource *shadows = nullptr;
};
