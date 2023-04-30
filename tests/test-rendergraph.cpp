/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "application.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "muglm/matrix_helper.hpp"
#include "muglm/muglm_impl.hpp"
#include "os_filesystem.hpp"
#include "render_context.hpp"
#include "render_graph.hpp"
#include "task_composer.hpp"
#include <string.h>

using namespace Granite;
using namespace Vulkan;

struct TestRenderGraph : Granite::Application, Granite::EventHandler
{
	TestRenderGraph()
	{
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_swapchain_changed, on_swapchain_destroyed,
		                             SwapchainParameterEvent);
		EVENT_MANAGER_REGISTER_LATCH(TestRenderGraph, on_device_created, on_device_destroyed, DeviceCreatedEvent);

		//imageId =
		//    GRANITE_ASSET_MANAGER()->register_image_resource(*GRANITE_FILESYSTEM(), "J:\\tt2.png", ImageClass::Color);
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

	AttachmentInfo main_output;
	main_output.size_class = SwapchainRelative;
	main_output.format = VK_FORMAT_R8G8B8A8_UNORM;
	main_output.size_x = 1;
	main_output.size_y = 1;

	AttachmentInfo main_depth;
	main_depth.format = swap.get_device().get_default_depth_format();

	float scale = 1.f;

	main_depth.size_x = scale;
	main_depth.size_y = scale;

	AttachmentInfo back;

	auto &pass = graph.add_pass("xxx", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	pass.add_color_output("xxx", main_output);
	pass.set_depth_stencil_output("depth-main", main_depth);
	pass.set_get_clear_color(
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

	pass.set_build_render_pass(
	    [&](CommandBuffer &cmd_buffer)
	    {
		    auto *cmd = &cmd_buffer;
		    //cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
		    cmd->set_program("assets://shaders/triangle.vert", "assets://shaders/triangle.frag");
		    cmd->set_opaque_state();
		    cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

		    vec2 vertices[] = {
			    vec2(-0.5f, -0.5f),
			    vec2(-0.5f, +0.5f),
			    vec2(+0.5f, -0.5f),
		    };

		    auto c = float(muglm::cos(elapsed_time * 2.0));
		    auto s = float(muglm::sin(elapsed_time * 2.0));
		    mat2 m{ vec2(c, -s), vec2(s, c) };
		    for (auto &v : vertices)
			    v = m * v;

		    static const vec4 colors[] = {
			    vec4(1.0f, 0.0f, 0.0f, 1.0f),
			    vec4(0.0f, 1.0f, 0.0f, 1.0f),
			    vec4(0.0f, 0.0f, 1.0f, 1.0f),
		    };

		    auto *verts = static_cast<vec2 *>(cmd->allocate_vertex_data(0, sizeof(vertices), sizeof(vec2)));
		    auto *col = static_cast<vec4 *>(cmd->allocate_vertex_data(1, sizeof(colors), sizeof(vec4)));
		    memcpy(verts, vertices, sizeof(vertices));
		    memcpy(col, colors, sizeof(colors));
		    cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
		    cmd->set_vertex_attrib(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
		    cmd->draw(3);
	    });

	pass.set_get_clear_depth_stencil(
	    [](VkClearDepthStencilValue *value) -> bool
	    {
		    if (value)
		    {
			    value->depth = 0.0f;
			    value->stencil = 0;
		    }
		    return true;
	    });

	auto &tonemap = graph.add_pass("tonemap", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	tonemap.add_color_output("tonemap", back);
	auto &tonemap_res = tonemap.add_texture_input("xxx");
	//tonemap.set_depth_stencil_output("depth-main", main_depth);
	tonemap.set_build_render_pass(
	    [&](CommandBuffer &cmd_buffer)
	    {
		    auto *cmd = &cmd_buffer;
		    auto &input = graph.get_physical_texture_resource(tonemap_res);
		    cmd->set_texture(0, 0, input, StockSampler::LinearClamp);

		    CommandBufferUtil::setup_fullscreen_quad(*cmd, "builtin://shaders/quad.vert", "builtin://shaders/blit.frag",
		                                             {});
		    CommandBufferUtil::draw_fullscreen_quad(*cmd);
	    });
	graph.set_backbuffer_source("tonemap");
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
