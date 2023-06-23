#include "atmosphere.h"
#include "flat_renderer.hpp"
#include "path.hpp"
#include "scene_loader.hpp"
#include "ui_manager.hpp"

#if 0
struct TestRenderGraph : Granite::Application, Granite::EventHandler
{
	TestRenderGraph()
	    : renderer(RendererType::GeneralForward, /* resolver */ nullptr)
	{
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_swapchain_changed, on_swapchain_destroyed,
		                             SwapchainParameterEvent);
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_device_created, on_device_destroyed, DeviceCreatedEvent);

		scene_loader.load_scene("J:/Scene/plane.glb");
	}

	void on_device_created(const DeviceCreatedEvent &e);
	void on_device_destroyed(const DeviceCreatedEvent &e);

	void on_swapchain_changed(const SwapchainParameterEvent &e);
	void on_swapchain_destroyed(const SwapchainParameterEvent &e);

	void render_frame(double, double elapsed_time);

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

	RenderContext context;
	LightingParameters lighting = {};
};

void TestRenderGraph::on_device_destroyed(const DeviceCreatedEvent &e)
{
	graph.reset();
	graph.set_device(nullptr);
}

void TestRenderGraph::on_device_created(const DeviceCreatedEvent &e)
{
	graph.set_device(&e.get_device());
}

void TestRenderGraph::on_swapchain_destroyed(const SwapchainParameterEvent &e)
{
}

void TestRenderGraph::on_swapchain_changed(const SwapchainParameterEvent &swap)
{
	auto &wsi = get_wsi();
	auto &device = wsi.get_device();
	graph.reset();

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

	//为什么up要是相反的?
	cam.look_at(vec3(0.0f, -1, .5f), vec3(0.0f, .0f, .5f), vec3(0.0f, .0f, -1.f));
	cam.set_depth_range(.1f, 20000.0f);
	cam.set_fovy(0.6f * half_pi<float>());
	context.set_camera(cam);

	renderer.set_mesh_renderer_options_from_lighting(lighting);

	lighting.directional.color = vec3(1.0f, 0.9f, 0.8f);
	lighting.directional.direction = normalize(vec3(1.0f, 1.0f, 1.0f));
	context.set_lighting_parameters(&lighting);

	ubo.camarePos = vec4(cam.get_position(), 1);
	ubo.projectMat = cam.get_projection();
	ubo.invProjMat = inverse(ubo.projectMat);
	ubo.invViewMat = inverse(cam.get_view());

	setup_atmosphere(graph, renderer, queue, visible, context, scene);

	graph.enable_timestamps(true);

	graph.bake();
	graph.log();
}

void TestRenderGraph::render_frame(double, double e)
{
	elapsed_time = e;

	auto &wsi = get_wsi();
	auto &device = wsi.get_device();
	graph.setup_attachments(device, &device.get_swapchain_view());

	context.set_camera(cam);

	auto &scene = scene_loader.get_scene();
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
		auto *app = new TestRenderGraph();
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite
#endif

struct ViewerApplication : Application
{
	explicit ViewerApplication(const std::string &path)
	    : renderer(RendererType::GeneralForward, /* resolver */ nullptr)
	{
		// Using Renderer directly is somewhat low level.
		// Normally, you would use RendererSuite and RenderPassSceneRenderer.

		// Effectively, loads a scene and inserts Entity objects into the Scene.
		scene_loader.load_scene(path);

		// Set initial position.
		fps_camera.set_position(vec3(0.0f, 0.0f, 5.0f));
		fps_camera.set_depth_range(0.1f, 500.0f);

		auto &scene = scene_loader.get_scene();

		auto plane = Util::make_handle<TexturePlane2>("test_normal");

		plane->set_plane(vec3(0, 0, 0), vec3(0, 0, 1), vec3(0, 0, 1), 1, 1);
		plane->set_zfar(500.f);
		plane->set_reflection_name("reflectionName");

		auto entity = scene.create_renderable(plane, nullptr);
		entity->allocate_component<UnboundedComponent>();
		entity->allocate_component<RenderPassSinkComponent>();
		auto *cull_plane = entity->allocate_component<CullPlaneComponent>();
		cull_plane->plane = plane->get_plane();

		auto *rpass = entity->allocate_component<RenderPassComponent>();
		rpass->creator = plane.get();
	}

	void render_frame(double frame_time, double elapsed_time) override
	{
		// Simple serial variant.
		auto &scene = scene_loader.get_scene();

		// First, update game objects. Can modify their scene Node transforms.
		// Objects can be added as well.
		// Animation system could run here.
		scene_loader.get_animation_system().animate(frame_time, elapsed_time);

		// - Traverse the node hierarchy and compute full transform.
		// - Updates the model and skinning matrices.
		scene.update_all_transforms();

		// Update the rendering context.
		// Only use a single directional light.
		// No shadows or anything fancy is used.
		lighting.directional.color = vec3(1.0f, 0.9f, 0.8f);
		lighting.directional.direction = normalize(vec3(1.0f, 1.0f, 1.0f));
		context.set_lighting_parameters(&lighting);

		// The renderer can be configured to handle many different scenarios.
		// Here we reconfigure the renderer to work with the current lighting configuration.
		// This is particularly necessary for forward renderers.
		// For G-buffer renderers, only a few flags are relevant. This is handled automatically
		// by the more advanced APIs such as RendererSuite and the RenderPassSceneRenderer.
		renderer.set_mesh_renderer_options_from_lighting(lighting);

		// The FPS camera registers for input events.
		// Update all rendering matrices based on current Camera state.
		// It is possible to pass down explicit matrices as well.
		context.set_camera(fps_camera);

		// Simple forward renderer, so we render opaque, transparent and background renderables in one go.
		visible.clear();
		scene.gather_visible_opaque_renderables(context.get_visibility_frustum(), visible);
		scene.gather_visible_transparent_renderables(context.get_visibility_frustum(), visible);
		scene.gather_unbounded_renderables(visible);

		// Time to render.
		renderer.begin(queue);
		queue.push_renderables(context, visible.data(), visible.size());

		auto &device = get_wsi().get_device();
		auto cmd = device.request_command_buffer();
		auto rp = device.get_swapchain_render_pass(SwapchainRenderPass::Depth);
		rp.clear_color[0].float32[0] = 0.01f;
		rp.clear_color[0].float32[1] = 0.02f;
		rp.clear_color[0].float32[2] = 0.03f;
		cmd->begin_render_pass(rp);
		renderer.flush(*cmd, queue, context, 0, nullptr);

		// Render some basic 2D on top.
		flat_renderer.begin();

		flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(UI::FontSize::Large), "Hello Granite",
		                          vec3(10.0f, 10.0f, 0.0f), vec2(1000.0f), vec4(1.0f, 0.0f, 1.0f, 1.0f));

		// The camera_pos and camera_size denote the canvas size.
		// We work in pixel units mostly, so using the viewport size as a baseline is a good default.
		// The Z dimension denotes how we subdivide the depth plane.
		// 2D objects also have depth and make use of the depth buffer (opaque 2D objects).
		flat_renderer.flush(*cmd, vec3(0.0f), vec3(cmd->get_viewport().width, cmd->get_viewport().height, 1.0f));

		cmd->end_render_pass();
		device.submit(cmd);
	}

	// Modify these as desired. For now, just call into the parent,
	// so it's effectively the same as not overriding.
	// This code is only here for demonstration purposes.
	void post_frame() override
	{
		Application::post_frame();
	}

	void render_early_loading(double frame_time, double elapsed_time) override
	{
		Application::render_early_loading(frame_time, elapsed_time);
	}

	void render_loading(double frame_time, double elapsed_time) override
	{
		Application::render_loading(frame_time, elapsed_time);
	}

	FPSCamera fps_camera;
	RenderContext context;
	LightingParameters lighting = {};
	SceneLoader scene_loader;
	FlatRenderer flat_renderer;
	Renderer renderer;
	RenderQueue queue;
	VisibilityList visible;
};

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	std::string path = "J:/Scene/plane.glb";

	LOGI("Loading glTF file from %s.\n", path.c_str());

	try
	{
		auto *app = new ViewerApplication(path);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite
