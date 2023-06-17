#include "atmosphere.h"
#include "render_graph.hpp"

AtmosphereParameters push;
UBO ubo;

void setup_atmosphere(Granite::RenderGraph& graph)
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
		    });
	}

	graph.set_backbuffer_source("RayMarching");
}
