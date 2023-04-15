#pragma once
#include "command_buffer.hpp"
#include "muglm/muglm.hpp"


#include <map>
#include <vector>

using namespace Vulkan;
using namespace muglm;
using std::vector;

/*
 * һ�������� ���ֻ��һ���򣬵��Ǳ�instance������10�Σ���object_size = 10, desc_size = 1
 */
struct Push
{
	mat4 MVP;
	int object_size;
	int desc_size;
	float scale = 1;
	int dummy;
};

static_assert(sizeof(Push) % 16 == 0, "Push��С������Ҫ��");

extern Push push;

//һ��mesh��������Ϣ
struct MeshId
{
	int vertexOffset;
	int firstIndex;
	int descIndex;

	//��ǰ����� ���Ƶĵ��������� �������������Σ� 4�����㣬 ��indexCount==6
	int indexCount;

	unsigned culledIndex; //�޳���֮�������

	int debug0;
	int debug1;
	int debug2;
};

static_assert(sizeof(MeshId) % 16 == 0, "MeshId��С������Ҫ��");

/*
 *�����С��DrawIndexedIndirectCommand��ͬ
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

static_assert(sizeof(DescAdditional) % 16 == 0, "DescAdditional��С������Ҫ��");

//��ǰ���������Ƶ�mesh�ж�����
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

//key:  mesh hash value,  value:  vec4 �洢���ǵ�һ����������
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

	//���һ��object
	MeshId push_description(const vector<vec4> &verts, const vector<int> &indices);

	void push_object(MeshId id, const vec4 &translate, const vec4 &color);

	void allocate_vert(CommandBufferHandle cmd);

	BufferHandle create_mesh_id_buffer(Device &device);

	BufferHandle create_color_buffer(Device &device);

	BufferHandle create_translate_buffer(Device &device);

	BufferHandle create_desc_additional_buffer(Device &device);

	//���ص�ǰ����mesh�����࣬ ��һ�����屻instance���ƶ�Σ���Ҳֻ��һ��
	int get_description_count();

	int get_object_count();

	int GetIndexCount();
	
	void debug();
	void init_buffer();
	void clear();
	void cull();
	void calcul_first_instance(); //size ����desc size
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

	//��ǰchunk�����Ķ��㡢���������Ϣ
	int vert_count = 0;
	int triangle_count = 0;
	int line_count = 0;
};

using ChunkHandle = Util::IntrusivePtr<Chunk>;

vector<ChunkHandle> get_chunks();

ChunkHandle get_current_chunk(Device& device, VkPrimitiveTopology primitive_topology);