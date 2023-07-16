#include "atmosphere.h"
#include "flat_renderer.hpp"
#include "path.hpp"
#include "scene_loader.hpp"
#include "ui_manager.hpp"

struct Prophet : Granite::Application, Granite::EventHandler
{
	Prophet()
	    : renderer(RendererType::GeneralForward, /* resolver */ nullptr)
	{
		EVENT_MANAGER_REGISTER_LATCH(Prophet, on_swapchain_changed, on_swapchain_destroyed, SwapchainParameterEvent);
		EVENT_MANAGER_REGISTER_LATCH(Prophet, on_device_created, on_device_destroyed, DeviceCreatedEvent);

		scene_loader.load_scene("J:/Scene/plane.glb");

		renderer_suite_config.directional_light_vsm = true;
	}

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
};

void Prophet::setup_shadow_map()
{
	// Get the scene AABB for shadow casters.
	auto &scene = scene_loader.get_scene();
	auto &shadow_casters =
	    scene.get_entity_pool()
	        .get_component_group<RenderInfoComponent, RenderableComponent, CastsStaticShadowComponent>();
	AABB aabb(vec3(FLT_MAX), vec3(-FLT_MAX));
	for (auto &caster : shadow_casters)
		aabb.expand(get_component<RenderInfoComponent>(caster)->world_aabb);

	mat4 view = mat4_cast(look_at(-lighting.directional.direction, vec3(0.0f, .0f, 1.0f)));
	AABB ortho_range_depth = aabb.transform(view);

	mat4 proj = ortho(ortho_range_depth);
	depth_context.set_camera(proj, view);
	lighting.shadow.transforms[0] = translate(vec3(0.5f, 0.5f, 0.0f)) * scale(vec3(0.5f, 0.5f, 1.0f)) * proj * view;
}

void Prophet::post_frame()
{
	static bool f = true;
	if (f)
	{
		f = false;
		cam.full_screen_scene();
	}
	Application::post_frame();
}

void Prophet::on_device_destroyed(const DeviceCreatedEvent &e)
{
	graph.reset();
	graph.set_device(nullptr);
}

void Prophet::on_device_created(const DeviceCreatedEvent &e)
{
	graph.set_device(&e.get_device());
}

void Prophet::on_swapchain_destroyed(const SwapchainParameterEvent &e)
{
}

void Prophet::on_swapchain_changed(const SwapchainParameterEvent &swap)
{
	auto &wsi = get_wsi();
	auto &device = wsi.get_device();
	graph.reset();

	renderer_suite.set_default_renderers();

	auto &scene = scene_loader.get_scene();

	ResourceDimensions dim;
	dim.width = swap.get_width();
	dim.height = swap.get_height();
	dim.format = swap.get_format();
	dim.transform = swap.get_prerotate();
	graph.set_backbuffer_dimensions(dim);

	push.screenWidth = dim.width;
	push.screenHeight = dim.height;

	vec3 delta = push.MieExtinction - push.MieScattering;
	delta.x = max(delta.x, 0.f), delta.y = max(delta.y, 0.f), delta.z = max(delta.z, 0.f);
	push.MieAbsorption = delta;
	int sz = sizeof(push);

	cam.look_at(vec3(0.0f, -1, .5f), vec3(0.0f, .0f, .5f), vec3(0.0f, .0f, 1.f));
	cam.set_depth_range(.1f, 20000.0f);
	cam.set_fovy(0.6f * half_pi<float>());
	cam.set_scene(&scene_loader.get_scene());
	cam.set_factor(2, 16);
	context.set_camera(cam);

	renderer.set_mesh_renderer_options_from_lighting(lighting);

	lighting.directional.color = vec3(1.0f, 0.9f, 0.8f);
	lighting.directional.direction = normalize(vec3(1.0f, 1.0f, 1.0f));
	context.set_lighting_parameters(&lighting);

	ubo.camarePos = vec4(cam.get_position(), 1);
	ubo.projectMat = cam.get_projection();
	ubo.invProjMat = inverse(ubo.projectMat);
	ubo.invViewMat = inverse(cam.get_view());

	setup_atmosphere(graph, renderer, queue, visible, context, scene, depth_context);

	graph.enable_timestamps(true);

	graph.bake();
	graph.log();
}

void Prophet::render_frame(double frame_time, double e)
{
	auto &scene = scene_loader.get_scene();
	auto q = cam.get_rotation();
	ubo.invViewMat = inverse(cam.get_view());
	context.set_camera(cam);
	elapsed_time = e;

	FrameParameters frame;
	frame.elapsed_time = elapsed_time;
	frame.frame_time = frame_time;
	context.set_frame_parameters(frame);

	setup_shadow_map();
	auto &wsi = get_wsi();
	auto &device = wsi.get_device();
	graph.setup_attachments(device, &device.get_swapchain_view());
	lighting.shadows = graph.maybe_get_physical_texture_resource(shadows);

	scene.set_render_pass_data(&renderer_suite, &context);
	scene.bind_render_graph_resources(graph);
	renderer_suite.update_mesh_rendering_options(context, renderer_suite_config);

	scene.update_all_transforms();

	TaskComposer composer(*GRANITE_THREAD_GROUP());
	graph.enqueue_render_passes(device, composer);
	composer.get_outgoing_task()->wait();
}

namespace Granite
{
Application *application_create(int, char **)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	try
	{
		auto *app = new Prophet();
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite
