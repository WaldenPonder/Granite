#include "atmosphere.h"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "scene_renderer.hpp"

using namespace Granite;
AtmosphereParameters push;
UBO ubo;
RendererSuite renderer_suite;
RenderTextureResource *shadows = nullptr;

static inline std::string tagcat(const std::string &a, const std::string &b)
{
	return a + "-" + b;
}

void add_shadow_pass(RenderGraph &graph, Scene &scene, RenderContext &depth_context)
{
	const std::string tag = "main";
	AttachmentInfo shadowmap;
	shadowmap.format = VK_FORMAT_D16_UNORM;
	shadowmap.samples = 4;
	shadowmap.size_class = SizeClass::Absolute;
	shadowmap.size_x = 2048;
	shadowmap.size_y = 2048;
	
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

void setup_atmosphere(Granite::RenderGraph &graph, Renderer &renderer, RenderQueue &queue, VisibilityList &visible,
                      RenderContext &context, Scene &scene, RenderContext &depth_context)
{
	add_shadow_pass(graph, scene, depth_context);

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

				//BINDING_GLOBAL_DIRECTIONAL_SHADOW
	/*		    auto &s = graph.get_physical_texture_resource(*shadows);
				cmd->set_texture(0, 5, s, StockSampler::LinearClamp);*/

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
	}

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
	//graph.set_backbuffer_source("shadow-main");
	graph.set_backbuffer_source("RayMarching");
}
