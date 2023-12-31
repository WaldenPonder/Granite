
#include "prophet.h"

#include "Trackball.h"
#include "application.hpp"
#include "click_button.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "muglm/matrix_helper.hpp"
#include "muglm/muglm_impl.hpp"
#include "render_graph.hpp"

#include "os_filesystem.hpp"
#include "render_context.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "scene_renderer.hpp"
#include "task_composer.hpp"
#include "window.hpp"

#include <fstream>

static std::string tagcat(const std::string &a, const std::string &b)
{
	return a + "-" + b;
}

Prophet::Prophet()
    : renderer(RendererType::GeneralForward, /* resolver */ nullptr)
    , renderer_mv(RendererType::MotionVector, /* resolver */ nullptr)
{
	EVENT_MANAGER_REGISTER_LATCH(Prophet, on_swapchain_changed, on_swapchain_destroyed, SwapchainParameterEvent);
	EVENT_MANAGER_REGISTER_LATCH(Prophet, on_device_created, on_device_destroyed, DeviceCreatedEvent);

	scene_loader.load_scene("J:/Scene/prophet.glb");

	renderer_suite_config.directional_light_vsm = true;

	createUi();
}

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

void Prophet::add_shadow_pass()
{
	const std::string tag = "main";
	AttachmentInfo shadowmap;
	shadowmap.format = VK_FORMAT_D16_UNORM;
	shadowmap.samples = 4;
	shadowmap.size_class = SizeClass::Absolute;
	shadowmap.size_x = 2048;
	shadowmap.size_y = 2048;
	auto &scene = scene_loader.get_scene();

	auto &shadowpass = graph.add_pass(tagcat("shadow", tag), RENDER_GRAPH_QUEUE_GRAPHICS_BIT);

	{
		auto shadowmap_vsm_color = shadowmap;
		auto shadowmap_vsm_resolved_color = shadowmap;
		shadowmap_vsm_color.format = VK_FORMAT_R32G32_SFLOAT;
		shadowmap_vsm_color.samples = 4;
		shadowmap_vsm_resolved_color.format = VK_FORMAT_R32G32_SFLOAT;
		shadowmap_vsm_resolved_color.samples = 1;

		auto shadowmap_vsm_half = shadowmap_vsm_resolved_color;
		shadowmap_vsm_half.size_x *= 0.5f;
		shadowmap_vsm_half.size_y *= 0.5f;

		shadowpass.set_depth_stencil_output(tagcat("shadow-depth", tag), shadowmap);
		shadowpass.add_color_output(tagcat("shadow-msaa", tag), shadowmap_vsm_color);
		shadowpass.add_resolve_output(tagcat("shadow-raw", tag), shadowmap_vsm_resolved_color);

		auto &down_pass = graph.add_pass(tagcat("shadow-down", tag), RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
		down_pass.add_color_output(tagcat("shadow-down", tag), shadowmap_vsm_half);
		auto &down_pass_res = down_pass.add_texture_input(tagcat("shadow-raw", tag));

		auto &up_pass = graph.add_pass(tagcat("shadow-up", tag), RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
		up_pass.add_color_output(tagcat("shadow", tag), shadowmap_vsm_resolved_color);
		auto &up_pass_res = up_pass.add_texture_input(tagcat("shadow-down", tag));

		down_pass.set_build_render_pass(
		    [&, layered = shadowmap.layers > 1](CommandBuffer &cmd)
		    {
			    auto &input = graph.get_physical_texture_resource(down_pass_res);
			    vec2 inv_size(1.0f / input.get_image().get_create_info().width,
			                  1.0f / input.get_image().get_create_info().height);
			    cmd.push_constants(&inv_size, 0, sizeof(inv_size));
			    cmd.set_texture(0, 0, input, StockSampler::LinearClamp);
			    CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                            "builtin://shaders/post/vsm_down_blur.frag",
			                                            { { "LAYERED", layered ? 1 : 0 } });
		    });

		up_pass.set_build_render_pass(
		    [&, layered = shadowmap.layers > 1](CommandBuffer &cmd)
		    {
			    auto &input = graph.get_physical_texture_resource(up_pass_res);
			    vec2 inv_size(1.0f / input.get_image().get_create_info().width,
			                  1.0f / input.get_image().get_create_info().height);
			    cmd.set_texture(0, 0, input, StockSampler::LinearClamp);
			    cmd.push_constants(&inv_size, 0, sizeof(inv_size));
			    CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert",
			                                            "builtin://shaders/post/vsm_up_blur.frag",
			                                            { { "LAYERED", layered ? 1 : 0 } });
		    });
	}

	Util::IntrusivePtr<RenderPassSceneRenderer> handle;
	RenderPassSceneRenderer::Setup setup = {};
	setup.scene = &scene;
	setup.suite = &renderer_suite;
	setup.flags = SCENE_RENDERER_DEPTH_BIT;
	setup.flags |= SCENE_RENDERER_SHADOW_VSM_BIT;

	setup.context = &depth_context;
	setup.flags |= SCENE_RENDERER_DEPTH_DYNAMIC_BIT;

	handle = Util::make_handle<RenderPassSceneRenderer>();
	handle->init(setup);

	VkClearColorValue value = {};
	value.float32[0] = 1.0f;
	value.float32[1] = 1.0f;
	handle->set_clear_color(value);
	shadowpass.set_render_pass_interface(std::move(handle));
}

void Prophet::setup_atmosphere()
{
	inv_resolution.inv_reso = vec2(1.0 / get_default_width(), 1.0 / get_default_height());

	auto &scene = scene_loader.get_scene();

	//-------------------------------------------------------------------------------TransmittanceLut
	auto &transmittance = graph.add_pass("TransmittanceLut", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	{
		AttachmentInfo back;
		back.size_class = Absolute;
		back.size_x = 256;
		back.size_y = 64;
		back.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		transmittance.add_color_output("TransmittanceLut", back);
		transmittance.set_build_render_pass(
		    [&](CommandBuffer &cmd_buffer)
		    {
			    auto *cmd = &cmd_buffer;
			    auto *global = static_cast<UBO *>(cmd->allocate_constant_data(0, 0, sizeof(UBO)));
			    *global = ubo;
			    cmd->push_constants(&push, 0, sizeof(push));

			    CommandBufferUtil::setup_fullscreen_quad(*cmd, "builtin://shaders/quad.vert",
			                                             "builtin://shaders/atmosphere/transmittance_lut.frag", {});
			    CommandBufferUtil::draw_fullscreen_quad(*cmd);
		    });
	}

	//-------------------------------------------------------------------------------RayMarching
	auto &rayMarching = graph.add_pass("RayMarching", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	{
		AttachmentInfo back;
		rayMarching.add_color_output("RayMarching", back);

		AttachmentInfo main_depth;
		main_depth.format = VK_FORMAT_D32_SFLOAT;
		main_depth.size_x = 1.;
		main_depth.size_y = 1.;
		rayMarching.set_depth_stencil_output("depth-main", main_depth);
		shadows = &rayMarching.add_texture_input("shadow-main");
		auto &transmittance_lut = rayMarching.add_texture_input("TransmittanceLut");
		rayMarching.set_build_render_pass(
		    [&](CommandBuffer &cmd_buffer)
		    {
			    auto *cmd = &cmd_buffer;
			    auto &input = graph.get_physical_texture_resource(transmittance_lut);
			    cmd->set_texture(0, 0, input, StockSampler::LinearClamp);

			    auto *global = static_cast<UBO *>(cmd->allocate_constant_data(0, 1, sizeof(UBO)));
			    *global = ubo;
			    cmd->push_constants(&push, 0, sizeof(push));

			    CommandBufferUtil::setup_fullscreen_quad(*cmd, "builtin://shaders/quad.vert",
			                                             "builtin://shaders/atmosphere/ray_marching.frag", {});
			    CommandBufferUtil::draw_fullscreen_quad(*cmd);

			    // Simple forward renderer, so we render opaque, transparent and background renderables in one go.
			    visible.clear();
			    scene.gather_visible_opaque_renderables(context.get_visibility_frustum(), visible);
			    scene.gather_visible_transparent_renderables(context.get_visibility_frustum(), visible);
			    scene.gather_unbounded_renderables(visible);

			    // Time to render.
			    renderer.begin(queue);
			    queue.push_renderables(context, visible.data(), visible.size());
			    renderer.flush(*cmd, queue, context, 0, nullptr);
		    });

		rayMarching.set_get_clear_color(
		    [](unsigned, VkClearColorValue *value) -> bool
		    {
			    if (value)
			    {
				    value->float32[0] = 0.0f;
				    value->float32[1] = 0.0f;
				    value->float32[2] = 0.0f;
				    value->float32[3] = 0.0f;
			    }

			    return true;
		    });

		rayMarching.set_get_clear_depth_stencil(
		    [](VkClearDepthStencilValue *value) -> bool
		    {
			    if (value)
			    {
				    value->depth = 1.0f;
				    value->stencil = 0;
			    }
			    return true;
		    });
	}

	//-------------------------------------------------------------------------------mv
	auto &mv_pass = graph.add_pass("mv-main", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	{
		bool full_mv = false;
		AttachmentInfo mv;
		mv.size_class = SwapchainRelative;
		mv.format = VK_FORMAT_R16G16_SFLOAT;

		mv_pass.set_depth_stencil_input("depth-main");
		mv_pass.add_color_output("mv-main", mv);

	    auto renderer = Util::make_handle<RenderPassSceneRenderer>();
		RenderPassSceneRenderer::Setup setup = {};
		setup.scene = &scene_loader.get_scene();
		setup.context = &context;
		setup.suite = &renderer_suite;
		setup.flags = SCENE_RENDERER_MOTION_VECTOR_BIT;
		if (full_mv)
			setup.flags |= SCENE_RENDERER_MOTION_VECTOR_FULL_BIT;
		renderer->init(setup);

		mv_pass.set_render_pass_interface(std::move(renderer));
	}

//#define USE_FXAA

 
#ifndef USE_FXAA
	//-------------------------------------------------------------------------------taa

	auto &taa = graph.add_pass("taa-resolve", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	{
		jitter.init(TemporalJitter::Type::TAA_16Phase,
		            vec2(graph.get_backbuffer_dimensions().width, graph.get_backbuffer_dimensions().height) *
		                1.f);

		AttachmentInfo taa_output;
		taa_output.format = graph.get_device().image_format_is_supported(VK_FORMAT_B10G11R11_UFLOAT_PACK32,
		                                                                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ?
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32 :
                                VK_FORMAT_R16G16B16A16_SFLOAT;

		AttachmentInfo taa_history = taa_output;
		taa_history.format = VK_FORMAT_R16G16B16A16_SFLOAT;

		taa.add_color_output("taa-resolve", taa_output);
		taa.add_color_output( "taa-resolve-history", taa_history);
		auto &input_res = taa.add_texture_input("RayMarching");
		auto &input_res_mv = taa.add_texture_input("mv-main");
		auto &input_depth_res = taa.add_texture_input("depth-main");
		auto &history = taa.add_history_input( "taa-resolve-history");

		taa.set_build_render_pass(
		    [&, q = Util::ecast(TAAQuality::High)](Vulkan::CommandBuffer &cmd)
		    {
			    auto &image = graph.get_physical_texture_resource(input_res);
			    auto &image_mv = graph.get_physical_texture_resource(input_res_mv);
			    auto &depth = graph.get_physical_texture_resource(input_depth_res);
			    auto *prev = graph.get_physical_history_texture_resource(history);

			    struct Push
			    {
				    mat4 reproj;
				    vec4 inv_resolution;
			    };
			    Push push;

			    push.reproj = translate(vec3(0.5f, 0.5f, 0.0f)) * scale(vec3(0.5f, 0.5f, 1.0f)) *
			                  jitter.get_history_view_proj(1) * jitter.get_history_inv_view_proj(0);

			    push.inv_resolution = vec4(
			        1.0f / image.get_image().get_create_info().width, 1.0f / image.get_image().get_create_info().height,
			        image.get_image().get_create_info().width, image.get_image().get_create_info().height);

			    cmd.push_constants(&push, 0, sizeof(push));

			    cmd.set_texture(0, 0, image, Vulkan::StockSampler::NearestClamp);
			    cmd.set_texture(0, 1, depth, Vulkan::StockSampler::NearestClamp);
			    cmd.set_texture(0, 2, image_mv, Vulkan::StockSampler::NearestClamp);
			    if (prev)
				    cmd.set_texture(0, 3, *prev, Vulkan::StockSampler::LinearClamp);

			    Vulkan::CommandBufferUtil::draw_fullscreen_quad(
			        cmd, "builtin://shaders/quad.vert", "builtin://shaders/post/taa_resolve.frag",
			        { { "REPROJECTION_HISTORY", prev ? 1 : 0 }, { "TAA_QUALITY", q } });
		    });
	}

	graph.set_backbuffer_source("taa-resolve");

#else
	//-------------------------------------------------------------------------------fxaa

	auto &fxaa = graph.add_pass("Final", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	{
		AttachmentInfo back;
		back.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		fxaa.add_color_output("Final", back);
		fxaa.add_texture_input("RayMarching");
		fxaa.add_texture_input("mv-main");

		fxaa.set_build_render_pass(
		    [&](CommandBuffer &cmd_buffer)
		    {
			    auto *cmd = &cmd_buffer;
			    auto &resource = fxaa.add_texture_input("RayMarching");
			    auto &input = graph.get_physical_texture_resource(resource);
			    cmd->set_texture(0, 0, input, StockSampler::LinearClamp);

			    auto &resourceMv = fxaa.add_texture_input("mv-main");
			    auto &inputMv = graph.get_physical_texture_resource(resourceMv);
			    cmd->set_texture(0, 1, inputMv, StockSampler::LinearClamp);

			    auto *global = static_cast<InvResolution *>(cmd->allocate_constant_data(0, 2, sizeof(InvResolution)));
			    *global = inv_resolution;

			    CommandBufferUtil::setup_fullscreen_quad(*cmd, "builtin://shaders/quad.vert",
			                                             "builtin://shaders/fxaa-test.frag", {});
			    CommandBufferUtil::draw_fullscreen_quad(*cmd);

		    	GRANITE_UI_MANAGER()->render(*cmd);
		    });
	}
	graph.set_backbuffer_source("Final");
#endif
}

void Prophet::createUi()
{
	auto &ui = *GRANITE_UI_MANAGER();
	ui.reset_children();

	{
		auto button = Util::make_handle<UI::ClickButton>();
		button->on_click(
		    [&]()
		    {
			    OPENFILENAME ofn;
			    char szFileName[MAX_PATH] = "";

			    ZeroMemory(&ofn, sizeof(ofn));

			    ofn.lStructSize = sizeof(ofn);
			    ofn.hwndOwner = nullptr;
			    ofn.lpstrFilter = "GLB Files (*.glb)\0*.glb\0GLTF Files (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0";
			    ofn.lpstrFile = szFileName;
			    ofn.nMaxFile = sizeof(szFileName);
			    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

			    if (GetOpenFileName(&ofn) == TRUE)
			    {
				    auto load_model = scene_loader.load_scene_to_root_node(szFileName);
				    scene_loader.get_scene().get_root_node()->add_child(load_model);
				    LOGI("file load %s\n", szFileName);
			    }
		    });
		ui.add_child(button);
		button->set_floating(true);
		button->set_text("Import");
		button->set_font_size(UI::FontSize::Large);
		button->set_floating_position(vec2(10.0f, 20.f));
		button->set_font_color(vec4(0, 1, 1, 1));
	}

	{
		auto button = Util::make_handle<UI::ClickButton>();
		button->on_click([]() { LOGI("button clicked2\n"); });
		ui.add_child(button);
		button->set_floating(true);
		button->set_text("Test");
		button->set_font_size(UI::FontSize::Large);
		button->set_floating_position(vec2(10.0f, 70.f));
		button->set_font_color(vec4(0, 1, 1, 1));
	}
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

	lighting.directional.color = vec3(1.0f, 0.9f, 0.8f);
	lighting.directional.direction = normalize(vec3(1.0f, 1.0f, 1.0f));
	context.set_lighting_parameters(&lighting);

	ubo.camarePos = vec4(cam.get_position(), 1);
	ubo.projectMat = cam.get_projection();
	ubo.invProjMat = inverse(ubo.projectMat);
	ubo.invViewMat = inverse(cam.get_view());

	add_shadow_pass();
	setup_atmosphere();

	graph.enable_timestamps(true);

	scene_loader.get_scene().add_render_pass_dependencies(graph);
	graph.bake();

	auto physical_buffers = graph.consume_physical_buffers();
	graph.install_physical_buffers(std::move(physical_buffers));
	graph.log();
}

void Prophet::render_frame(double frame_time, double e)
{
	auto &scene = scene_loader.get_scene();
	scene.update_all_transforms();

	auto q = cam.get_rotation();
	ubo.invViewMat = inverse(cam.get_view());
	context.set_camera(cam);
	elapsed_time = e;

	FrameParameters frame;
	frame.elapsed_time = elapsed_time;
	frame.frame_time = frame_time;
	context.set_frame_parameters(frame);

	jitter.step(cam.get_projection(), cam.get_view());
	context.set_camera(jitter.get_jittered_projection(), cam.get_view());
	context.set_motion_vector_projections(jitter);

	renderer.set_mesh_renderer_options_from_lighting(lighting);

	setup_shadow_map();
	auto &wsi = get_wsi();
	auto &device = wsi.get_device();
	graph.setup_attachments(device, &device.get_swapchain_view());
	lighting.shadows = graph.maybe_get_physical_texture_resource(shadows);

	scene.set_render_pass_data(&renderer_suite, &context);
	scene.bind_render_graph_resources(graph);
	renderer_suite.update_mesh_rendering_options(context, renderer_suite_config);

	TaskComposer composer(*GRANITE_THREAD_GROUP());
	graph.enqueue_render_passes(device, composer);
	composer.get_outgoing_task()->wait();
}