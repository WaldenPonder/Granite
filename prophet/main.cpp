#include "atmosphere.h"

struct TestRenderGraph : Granite::Application, Granite::EventHandler
{
	TestRenderGraph()
	{
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_swapchain_changed, on_swapchain_destroyed,
		                             SwapchainParameterEvent);
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_device_created, on_device_destroyed, DeviceCreatedEvent);
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

	ubo.camarePos = vec4(cam.get_position(), 1);
	ubo.projectMat = cam.get_projection();
	ubo.invProjMat = inverse(ubo.projectMat);
	ubo.invViewMat = inverse(cam.get_view());
	
	setup_atmosphere(graph);
		
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
