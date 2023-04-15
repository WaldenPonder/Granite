#include "application_wsi_events.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "event.hpp"
#include "os_filesystem.hpp"
#include "muglm/muglm_impl.hpp"
#include <string.h>
#include "mesh_chunk.h"
#include "render_context.hpp"

using namespace Granite;
using namespace Vulkan;

class IndirectRender : public EventHandler
{
	RenderContext& render_context;
	Device* device_ptr = nullptr;
	BufferHandle translate_buffer;
	BufferHandle color_buffer;
	BufferHandle mesh_id_buffer;
	BufferHandle desc_additional;
	CommandBufferHandle cmd;

	//https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkDrawIndexedIndirectCommand.html
	BufferHandle indirect_buffer;

	BufferHandle count_buffer;

	/*
	 * 一个场景， 如果只有一个球，但是被instance绘制了10次，则object_size = 10, desc_size = 1
	 */
	struct Push
	{
		int object_size;
		int desc_size;
		mat4 view_projection;
	} push;

public:
	IndirectRender(RenderContext& context);

	void on_device_created(const DeviceCreatedEvent& e);

	void on_device_destroyed(const DeviceCreatedEvent&);

	void clear();
	
	void cull();
	
	void calcul_first_instance();

	void calcul_culled_index_relationship();
	
	void draw();
	
	void render_frame(double t, double);
};
