#pragma once
#include "command_buffer.hpp"
#include "muglm/muglm.hpp"


#include <map>
#include <vector>

using namespace Vulkan;
using namespace muglm;
using std::vector;

/*
 * 一个场景， 如果只有一个球，但是被instance绘制了10次，则object_size = 10, desc_size = 1
 */
struct Push
{
	mat4 MVP;
	int object_size;
	int desc_size;
	float scale = 1;
	int dummy;
};

static_assert(sizeof(Push) % 16 == 0, "Push大小不满足要求");

extern Push push;

//一个mesh的索引信息
struct MeshId
{
	int vertexOffset;
	int firstIndex;
	int descIndex;

	//当前此命令， 绘制的的索引数， 比如两个三角形， 4个顶点， 但indexCount==6
	int indexCount;

	unsigned culledIndex; //剔除完之后的索引

	int debug0;
	int debug1;
	int debug2;
};

static_assert(sizeof(MeshId) % 16 == 0, "MeshId大小不满足要求");

/*
 *数组大小和DrawIndexedIndirectCommand相同
*/
struct DescAdditional
{
	vec4 bbMinPt = vec4(FLT_MAX, FLT_MAX, FLT_MAX, 1);
	vec4 bbMaxPt = vec4(-1e10, -1e10, -1e10, 1);
	uint innerIndex;
	uint debug0;
	uint debug1;
	uint debug2;
};

static_assert(sizeof(DescAdditional) % 16 == 0, "DescAdditional大小不满足要求");

//当前场景所绘制的mesh有多少种
struct MeshDescription
{
	vector<vec4> verts;
	vector<int> indices;
	vector<MeshId> meshs;

	unsigned long get_memory_used()
	{
		unsigned long t1 = verts.size() * sizeof(vec4);
		unsigned long t2 = indices.size() * sizeof(int);
		unsigned long t3 = verts.size() * sizeof(vec4);

		return t1 + t2 + t3;
	}
};

//key:  mesh hash value,  value:  vec4 存储的是第一个顶点数据
using MeshHashMap = std::map<long long, std::pair<MeshId, vec4>>;

class Chunk : public Util::IntrusivePtrEnabled<Chunk>
{
public:
	Chunk(Device &device_, VkPrimitiveTopology primitive_topology_)
		: device(device_),
		primitive_topology(primitive_topology_)
	{
	}

	unsigned long get_memory_used();

	//添加一个object
	MeshId push_description(const vector<vec4> &verts, const vector<int> &indices);

	void push_object(MeshId id, const vec4 &translate, const vec4 &color);

	void allocate_vert(CommandBufferHandle cmd);

	BufferHandle create_mesh_id_buffer(Device &device);

	BufferHandle create_color_buffer(Device &device);

	BufferHandle create_translate_buffer(Device &device);

	BufferHandle create_desc_additional_buffer(Device &device);

	//返回当前场景mesh的种类， 即一个物体被instance绘制多次，那也只算一次
	int get_description_count();

	int get_object_count();

	int GetIndexCount();
	
	void debug();
	void init_buffer();
	void clear();
	void cull();
	void calcul_first_instance(); //size 等于desc size
	void calcul_culled_index_relationship();
	void draw(CommandBufferHandle cmd);

	long long get_mash_hash(const vector<vec4>& positions, const vector<int>& indices);

	MeshHashMap& get_mesh_hash_map() { return mesh_hash_map; }

	Push& get_push() { return push; }

	VkPrimitiveTopology get_primitive_topology() const { return primitive_topology; }

	int get_vert_count() const { return vert_count; }
	int get_triangle_count() const { return triangle_count; }
	int get_line_count() const { return line_count; }
	
private:
	VkPrimitiveTopology primitive_topology;
	Device &device;
	Push push;
	vector<int> render_object;
	
	MeshHashMap mesh_hash_map;
	
	MeshDescription description;
	vector<vec4> translates;
	vector<vec4> colors;
	vector<int> matId;
	vector<MeshId> meshs;

	BufferHandle translate_buffer;
	BufferHandle color_buffer;
	BufferHandle mesh_id_buffer;
	BufferHandle desc_additional;

	CommandBufferHandle cmd_clear;
	CommandBufferHandle cmd_cull;
	CommandBufferHandle cmd_first_instance;
	CommandBufferHandle cmd_relationship;

	//https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkDrawIndexedIndirectCommand.html
	BufferHandle indirect_buffer;
	BufferHandle count_buffer;	

	//当前chunk包含的顶点、三角面等信息
	int vert_count = 0;
	int triangle_count = 0;
	int line_count = 0;
};

using ChunkHandle = Util::IntrusivePtr<Chunk>;

vector<ChunkHandle> get_chunks();

ChunkHandle get_current_chunk(Device& device, VkPrimitiveTopology primitive_topology);