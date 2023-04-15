/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
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
#include "os_filesystem.hpp"
#include "muglm/muglm_impl.hpp"
#include <string.h>
#include "mesh_chunk.h"
#include "read_json_file.h"
#include "Trackball.h"

using namespace Granite;
using namespace Vulkan;

struct MDIApplication : Application, EventHandler
{
	Trackball cam;

	clock_t time = clock();
	
	MDIApplication()
	{
		EVENT_MANAGER_REGISTER(MDIApplication, on_scroll, ScrollEvent);
		EVENT_MANAGER_REGISTER(MDIApplication, on_input_state, InputStateEvent);
		EVENT_MANAGER_REGISTER_LATCH(MDIApplication, on_device_created, on_device_destroyed, DeviceCreatedEvent);
		EVENT_MANAGER_REGISTER_LATCH(MDIApplication, on_swapchain_created, on_swapchain_destroyed,
		                             SwapchainParameterEvent);
	}

	void on_swapchain_created(const SwapchainParameterEvent &e)
	{
		//cam.set_ortho(true);
		cam.look_at(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f, 0.f, 0.f), vec3(0, -1, 0));
		cam.set_aspect(e.get_aspect_ratio());
		cam.set_fovy(0.6f * half_pi<float>());
		cam.set_depth_range(0.05f, 100000.0f);
	}

	void on_swapchain_destroyed(const SwapchainParameterEvent &)
	{
	}

	bool on_scroll(const ScrollEvent &e)
	{
		float f = static_cast<float>(e.get_yoffset());

		return true;
	}

	bool on_input_state(const InputStateEvent &state)
	{
		//if (state.get_key_pressed(Key::Space))
		//	push.scale = 1.f;

		return true;
	}

	void on_device_created(const DeviceCreatedEvent &e)
	{
		auto &wsi = get_wsi();
		auto &device = wsi.get_device();
		load_scene(device);

		for (auto &c : get_chunks())
			c->init_buffer();
	}

	void on_device_destroyed(const DeviceCreatedEvent &)
	{
	}

	void render_frame(double t, double e) override
	{
		auto delta = clock() - time;
		time = clock();
		
		auto &wsi = get_wsi();
		auto &device = wsi.get_device();

		long memory_used = 0;
		long long desc_cnt = 0, obj_cnt = 0, index_cnt = 0;
		for (auto chunk : get_chunks())
		{
			chunk->get_push().MVP = cam.get_projection() * cam.get_view();
			chunk->clear();
			chunk->cull();
			chunk->calcul_first_instance();
			chunk->calcul_culled_index_relationship();
			memory_used += chunk->get_memory_used();
			desc_cnt += chunk->get_description_count();
			obj_cnt += chunk->get_object_count();
			index_cnt += chunk->get_vert_count();
		}

		auto cmd_draw = device.request_command_buffer();

		cmd_draw->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::Depth));
		for (auto chunk : get_chunks())
		{
			chunk->draw(cmd_draw);
		}
		cmd_draw->end_render_pass();
		device.submit(cmd_draw);

		LOGI("desc cnt: %d, obj cnt:  %d  index_cnt  %d\t\t", desc_cnt, obj_cnt, index_cnt);
		LOGI("fps:  %f, elapsed_time %f,  time:  %d  memory %d\t\n", 1. / t, e, delta, memory_used / 1024);
		//LOGI("DEBUG VAL  v0:  %d, v1:  %d,  v2:  %d, v3:  %d, v4:  %d, v5:  %d,  v6:  %d\n",
		//     render_object[0], render_object[1], render_object[2], render_object[3],
		//     render_object[4], render_object[5], render_object[6]);
	}
};

namespace Granite
{
Application *application_create(int, char **)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	try
	{
		auto *app = new MDIApplication();
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
}
