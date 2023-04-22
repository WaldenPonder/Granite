#include "mesh_chunk.h"
#include "device.hpp"
#include "muglm/muglm_impl.hpp"

static std::map<VkPrimitiveTopology, vector<ChunkHandle>> CHUNKS;

#define USE_DEVICE

unsigned long Chunk::get_memory_used()
{
	unsigned long t1 = description.get_memory_used();
	unsigned long t2 = translates.size() * sizeof(vec4);
	unsigned long t3 = colors.size() * sizeof(vec4);
	unsigned long t4 = matId.size() * sizeof(int);
	unsigned long t5 = meshs.size() * sizeof(MeshId);

	return t1 + t2 + t3 + t4 + t5;
}

MeshId Chunk::push_description(const vector<vec4> &verts, const vector<int> &indices)
{
	auto &desc = this->description;

	auto &VERT = desc.verts;
	auto &INDEX = desc.indices;

	MeshId mesh_id;

	mesh_id.firstIndex = INDEX.size();
	mesh_id.vertexOffset = VERT.size();
	mesh_id.descIndex = desc.meshs.size();
	mesh_id.indexCount = indices.size();
	VERT.insert(VERT.end(), verts.begin(), verts.end());

	INDEX.insert(INDEX.end(), indices.begin(), indices.end());

	desc.meshs.push_back(mesh_id);

	return mesh_id;
}

void Chunk::push_object(MeshId id, const vec4 &translate, const vec4 &color)
{
	this->meshs.push_back(id);
	this->translates.push_back(translate);
	this->colors.push_back(color);

	vert_count += id.indexCount;
}

void Chunk::allocate_vert(CommandBufferHandle cmd)
{
	size_t vert_size = this->description.verts.size();
	auto *positions = static_cast<vec4 *>(cmd->allocate_vertex_data(0, vert_size * sizeof(vec4), sizeof(vec4)));

	for (size_t i = 0; i < vert_size; i++)
		positions[i] = this->description.verts[i];

	size_t index_size = this->description.indices.size();
	auto *indices = static_cast<uint16_t *>(cmd->allocate_index_data(index_size * sizeof(uint16_t),
	                                                                 VK_INDEX_TYPE_UINT16));

	for (size_t i = 0; i < index_size; i++)
		indices[i] = this->description.indices[i];
}

BufferHandle Chunk::create_mesh_id_buffer(Device &device)
{
	BufferCreateInfo info = {};
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
#ifdef USE_DEVICE
	info.domain = BufferDomain::Device;
#else
	info.domain = BufferDomain::CachedHost;
#endif
	info.size = this->meshs.size() * sizeof(MeshId);
	auto buffer = device.create_buffer(info, this->meshs.data());

	return buffer;
}

BufferHandle Chunk::create_color_buffer(Device &device)
{
	BufferCreateInfo info = {};
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
#ifdef USE_DEVICE
	info.domain = BufferDomain::Device;
#else
	info.domain = BufferDomain::CachedHost;
#endif
	info.size = this->colors.size() * sizeof(vec4);

	auto color_buffer = device.create_buffer(info, this->colors.data());

	return color_buffer;
}

BufferHandle Chunk::create_translate_buffer(Device &device)
{
	BufferCreateInfo info = {};
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
#ifdef USE_DEVICE
	info.domain = BufferDomain::Device;
#else
	info.domain = BufferDomain::CachedHost;
#endif
	info.size = this->translates.size() * sizeof(vec4);

	auto translate_buffer = device.create_buffer(info, this->translates.data());

	return translate_buffer;
}

BufferHandle Chunk::create_desc_additional_buffer(Device &device)
{
	const int desc_size = get_description_count();

	BufferCreateInfo info = {};
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

#ifdef USE_DEVICE
	info.domain = BufferDomain::Device;
#else
	info.domain = BufferDomain::CachedHost;
#endif

	info.size = desc_size * sizeof(DescAdditional);

	vector<DescAdditional> additionals(desc_size);
	for (int i = 0; i < desc_size; i++)
	{
		auto &desc = additionals[i];
		MeshId mesh_id = this->description.meshs[i];

		for (int j = mesh_id.firstIndex; j < mesh_id.firstIndex + mesh_id.indexCount; j++)
		{
			int index = this->description.indices[j] + mesh_id.vertexOffset;
			vec4 &pt = this->description.verts[index];

			desc.bbMinPt.x = std::min(desc.bbMinPt.x, pt.x);
			desc.bbMinPt.y = std::min(desc.bbMinPt.y, pt.y);
			desc.bbMinPt.z = std::min(desc.bbMinPt.z, pt.z);

			desc.bbMaxPt.x = std::max(desc.bbMaxPt.x, pt.x);
			desc.bbMaxPt.y = std::max(desc.bbMaxPt.y, pt.y);
			desc.bbMaxPt.z = std::max(desc.bbMaxPt.z, pt.z);
		}
	}

	auto desc_additional = device.create_buffer(info, additionals.data());

	return desc_additional;
}

int Chunk::get_description_count()
{
	return this->description.meshs.size();
}

int Chunk::get_object_count()
{
	return this->meshs.size();
}

int Chunk::GetIndexCount()
{
	return description.indices.size();
}

void Chunk::debug()
{
#if 0
	vector<MeshId> mm;
	MeshId *ids = static_cast<MeshId *>(device.map_host_buffer(*mesh_id_buffer, MEMORY_ACCESS_READ_BIT));
	if (!ids) return;
	
	for (int i = 0; i < GetObjectCount(); i++)
	{
		mm.push_back(ids[i]);
	}
	device.unmap_host_buffer(*mesh_id_buffer, MEMORY_ACCESS_READ_BIT);

	vector<DescAdditional> mm2;
	DescAdditional *ids2 = static_cast<DescAdditional *>(device.map_host_buffer(
		*desc_additional, MEMORY_ACCESS_READ_BIT));

	for (int i = 0; i < GetDescriptionCount(); i++)
	{
		mm2.push_back(ids2[i]);
	}
	device.unmap_host_buffer(*desc_additional, MEMORY_ACCESS_READ_BIT);

	render_object.resize(7);
	for (int i = 0; i < 7; i++)
		render_object[i] = mm[i].debug0;
#endif
}

void Chunk::init_buffer()
{
	//-----------------------------------------------------------------indirect_buffer
	{
		BufferCreateInfo info = {};
		info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		info.domain = BufferDomain::Device;

		vector<VkDrawIndexedIndirectCommand> commands(get_description_count());
		info.size = sizeof(VkDrawIndexedIndirectCommand) * commands.size();
		indirect_buffer = device.create_buffer(info, commands.data());
	}

	//-----------------------------------------------------------------count_buffer
	{
		BufferCreateInfo info = {};
		info.size = sizeof(uint32_t);
		info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		info.domain = BufferDomain::Device;

		uint32_t count = get_description_count();
		count_buffer = device.create_buffer(info, &count);
	}

	color_buffer = create_color_buffer(device);
	translate_buffer = create_translate_buffer(device);

	mesh_id_buffer = create_mesh_id_buffer(device);

	desc_additional = create_desc_additional_buffer(device);

	push.object_size = get_object_count();
	push.desc_size = get_description_count();
}

void Chunk::clear()
{
	if (!cmd_clear)
	{
		cmd_clear = device.request_command_buffer();
		cmd_clear->push_constants(&push, 0, sizeof(push));
		cmd_clear->set_storage_buffer(0, 0, *indirect_buffer);
		cmd_clear->set_storage_buffer(0, 1, *mesh_id_buffer);
		cmd_clear->set_program("assets://shaders/granite-test/clear.comp");
		cmd_clear->dispatch(get_object_count() / 64 + 1, 1, 1);
		auto rp = device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly);
		cmd_clear->begin_render_pass(rp);
		cmd_clear->end_render_pass();
	}

	Fence fence;
	device.submit(cmd_clear, &fence);
	fence->wait();
}

void Chunk::cull()
{
	if (!cmd_cull)
	{
		cmd_cull = device.request_command_buffer();
		cmd_cull->push_constants(&push, 0, sizeof(push));
		cmd_cull->set_storage_buffer(0, 0, *indirect_buffer);
		cmd_cull->set_storage_buffer(0, 1, *mesh_id_buffer);
		cmd_cull->set_storage_buffer(0, 2, *desc_additional);
		cmd_cull->set_storage_buffer(0, 3, *translate_buffer);

		cmd_cull->set_program("assets://shaders/granite-test/cull.comp");
		cmd_cull->dispatch(get_object_count() / 64 + 1, 1, 1);

		auto rp = device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly);
		cmd_cull->begin_render_pass(rp);
		cmd_cull->end_render_pass();
	}

	Fence fence;
	device.submit(cmd_cull, &fence);
	fence->wait();
}

void Chunk::calcul_first_instance()
{
	if (!cmd_first_instance)
	{
		cmd_first_instance = device.request_command_buffer();
		cmd_first_instance->push_constants(&push, 0, sizeof(push));
		cmd_first_instance->set_storage_buffer(0, 0, *indirect_buffer);
		cmd_first_instance->set_storage_buffer(0, 1, *mesh_id_buffer);
		cmd_first_instance->set_storage_buffer(0, 2, *desc_additional);
		cmd_first_instance->set_program("assets://shaders/granite-test/calcul_first_instance.comp");
		cmd_first_instance->dispatch(get_description_count() / 64 + 1, 1, 1);

		auto rp = device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly);
		cmd_first_instance->begin_render_pass(rp);
		cmd_first_instance->end_render_pass();
	}

	Fence fence;
	device.submit(cmd_first_instance, &fence);
	fence->wait();
}

void Chunk::calcul_culled_index_relationship()
{
	if (!cmd_relationship)
	{
		cmd_relationship = device.request_command_buffer();

		cmd_relationship->push_constants(&push, 0, sizeof(push));
		cmd_relationship->set_storage_buffer(0, 0, *indirect_buffer);
		cmd_relationship->set_storage_buffer(0, 1, *mesh_id_buffer);
		cmd_relationship->set_storage_buffer(0, 2, *desc_additional);
		cmd_relationship->set_program("assets://shaders/granite-test/calcul_culled_index_relationship.comp");
		cmd_relationship->dispatch(get_object_count() / 64 + 1, 1, 1);
		auto rp = device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly);
		cmd_relationship->begin_render_pass(rp);
		cmd_relationship->end_render_pass();
	}

	Fence fence;
	device.submit(cmd_relationship, &fence);
	fence->wait();
}

void Chunk::draw(CommandBufferHandle cmd)
{
	unsigned long long ss = get_memory_used();

	cmd->set_opaque_state();
	cmd->set_program("assets://shaders/granite-test/multi_draw_indirect.vert",
	                      "assets://shaders/granite-test/multi_draw_indirect.frag");
	cmd->set_primitive_topology(primitive_topology);

	cmd->push_constants(&push, 0, sizeof(push));
	cmd->set_storage_buffer(0, 0, *color_buffer);
	cmd->set_storage_buffer(0, 1, *translate_buffer);
	cmd->set_storage_buffer(0, 2, *mesh_id_buffer);

	cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	//	cmd->set_vertex_attrib(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0);

	allocate_vert(cmd);

	cmd->draw_indexed_multi_indirect(*indirect_buffer, 0, get_description_count(),
	                                      sizeof(VkDrawIndexedIndirectCommand),
	                                      *count_buffer,
	                                      0);

	//debug();
}

long long Chunk::get_mesh_hash(const vector<vec4> &positions, const vector<int> &indices)
{
	long long h = 0;

	h = h * 31 + positions.size();
	h = h * 31 + indices.size();

	for (int i = 1; i < indices.size(); i++)
	{
		int a = indices[i];
		int b = indices[i - 1];

		vec4 p1 = positions[a];
		vec4 p2 = positions[b];

		vec4 tmp = p1 - p2;
		dvec4 tmp2(tmp.x * 1000, tmp.y * 1000, tmp.z * 1000, tmp.w * 1000);
		h = h * 31 + static_cast<long long>(tmp2.x);
		h = h * 31 + static_cast<long long>(tmp2.y);
		h = h * 31 + static_cast<long long>(tmp2.z);
		h = h * 31 + static_cast<long long>(tmp2.w);
	}

	return h;
}

vector<ChunkHandle> get_chunks()
{
	vector<ChunkHandle> res;
	for (auto &vec : CHUNKS)
	{
		for (auto &v : vec.second)
			res.emplace_back(v);
	}
	return res;
}

ChunkHandle get_current_chunk(Device &device, VkPrimitiveTopology primitive_topology)
{
	auto &chunks = CHUNKS[primitive_topology];
	if (!chunks.size() || chunks.back()->get_memory_used() > 4 * 1024 * 1024
	    || chunks.back()->get_description_count() > 3000)
	{
		//chunks.emplace_back(Util::make_handle<Chunk>(device, primitive_topology));
		chunks.emplace_back(new Chunk(device, primitive_topology));
	}

	return chunks.back();
}
