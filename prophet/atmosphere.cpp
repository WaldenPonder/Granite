#include "atmosphere.h"
#include "render_graph.hpp"
#include "renderer.hpp"

AtmosphereParameters push;
UBO ubo;

void setup_atmosphere(Granite::RenderGraph &graph, Renderer& renderer, RenderQueue& queue, VisibilityList& visible,
                      RenderContext &context, Scene& scene)
{
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
			    //std::ifstream os("../assets/atmosphere.json");
			    // cereal::JSONInputArchive ar(os);
			    // ar(push);
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

			    {
				    // Simple forward renderer, so we render opaque, transparent and background renderables in one go.
				    visible.clear();
				    scene.gather_visible_opaque_renderables(context.get_visibility_frustum(), visible);
				    scene.gather_visible_transparent_renderables(context.get_visibility_frustum(), visible);
				    scene.gather_unbounded_renderables(visible);

				    // Time to render.
				    renderer.begin(queue);
				    queue.push_renderables(context, visible.data(), visible.size());
				    renderer.flush(*cmd, queue, context, 0, nullptr);
			    }
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
			    value->depth = 0.0f;
			    value->stencil = 0;
		    }
		    return true;
	    });
	graph.set_backbuffer_source("RayMarching");
}
