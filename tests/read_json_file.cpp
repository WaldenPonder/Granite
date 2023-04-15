#include "read_json_file.h"

#include <rapidjson/document.h>
#include <fstream>
#include "logging.hpp"
#include "mesh_chunk.h"
#include "../scene-export/camera_export.hpp"
//#include "../scene-export/obj.hpp"
#include "muglm/muglm_impl.hpp"

#include <map>
#include <random>
#define TINYOBJLOADER_IMPLEMENTATION

#include "tiny_obj_loader.h"

using namespace Granite;
static std::default_random_engine eng(time(nullptr));
static std::uniform_real_distribution<float> rd(-10000.f, 10000.f);
static std::uniform_real_distribution<float> rd2(0.1f, 1.f);
static std::uniform_real_distribution<float> rd3(0.f, 100.f);

namespace
{
union my_union
{
	struct
	{
		uint8_t a, b, c, d;
	};

	int index;
};

struct ObjInfo
{
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	bool triangulate = true;
	std::string filename;
};
}

static void read_json(const std::string &file_name, Device &device)
{
	std::ifstream in(file_name);
	if (!in.is_open())
	{
		LOGE("fail to read json file: %s\n", file_name.c_str());
		return;
	}

	std::string json_content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();

	rapidjson::Document dom;
	if (dom.Parse(json_content.c_str()).HasParseError())
	{
		LOGE("JSON Parse  Error\n");
		return;
	}

	int NumberOfElement = dom["NumberOfElement"].GetInt();

	const rapidjson::Value &ElementInfo = dom["ElementInfo"];

	for (auto &element : ElementInfo.GetArray())
	{
		auto &color = element["Color"].GetArray();
		//LOGI("%s\n", e["Category"].GetString());
		//LOGI("c:  %d, %d, %d, %d\n", color[0].GetInt(), color[1].GetInt(), color[2].GetInt(), color[3].GetInt());

		vec4 col(color[0].GetInt(), color[1].GetInt(), color[2].GetInt(), color[3].GetInt());

		for (auto &geometry : element["Geometry"].GetArray())
		{
			if (!geometry.IsObject())
			{
				LOGI("geometry.IsObject is false\n");
				continue;
			}

			if (geometry.HasMember("Triangles")) //--------------------------------------Triangles
			{
				Chunk &chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
				vector<vec4> positions;
				positions.reserve(geometry["NumberOfPoint"].GetInt());

				vector<int> indices;
				indices.reserve(geometry["NumberOfTriangle"].GetInt() * 3);

				for (auto &pt : geometry["Points"].GetArray())
				{
					auto &tmp = pt["XYZ"];
					positions.emplace_back(tmp[0].GetFloat(), tmp[1].GetFloat(), tmp[2].GetFloat(), 1);
				}

				for (auto &tri : geometry["Triangles"].GetArray())
				{
					auto &tmp = tri["Points"];

					int a = tmp[0].GetInt() - 1;
					int b = tmp[1].GetInt() - 1;
					int c = tmp[2].GetInt() - 1;

					indices.emplace_back(a);
					indices.emplace_back(b);
					indices.emplace_back(c);
				}

				long long h = chunk.get_mash_hash(positions, indices);
				auto &mp = chunk.get_mesh_hash_map();
				auto itor = mp.find(h);
				if (itor == mp.end())
				{
					MeshId id = chunk.push_description(positions, indices);
					chunk.push_object(id, vec4(0, 0, .0, 0), col);

					mp[h] = { id, positions.front() };
				}
				else
				{
					std::pair<MeshId, vec4> item = itor->second;
					chunk.push_object(item.first, positions.front() - item.second, col);

					vec4 color;
					for (int k = 0; k < 100; k++)
					{
						if (k % 3 == 0)
						{
							color = vec4(1, rd2(eng), rd2(eng), 1);
						}
						else if (k % 3 == 1)
						{
							color = vec4(rd2(eng), 1, rd2(eng), 1);
						}
						else
						{
							color = vec4(rd2(eng), rd2(eng), 1, 1);
						}
						chunk.push_object(
							item.first, positions.front() - item.second + vec4(rd(eng), rd(eng), rd(eng), 0), color);
					}
				}
			}
			else if (geometry.HasMember("Curves")) //--------------------------------------Curves
			{
				Chunk &chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
				vector<vec4> positions;
				positions.reserve(geometry["NumberOfPoint"].GetInt());

				vector<int> indices;
				indices.reserve(geometry["NumberOfCurve"].GetInt() * 2);

				for (auto &pt : geometry["Points"].GetArray())
				{
					auto &tmp = pt["XYZ"];
					positions.emplace_back(tmp[0].GetFloat(), tmp[1].GetFloat(), tmp[2].GetFloat(), 1);
				}

				for (auto &tri : geometry["Curves"].GetArray())
				{
					auto &tmp = tri["Points"];

					int a = tmp[0].GetInt() - 1;
					int b = tmp[1].GetInt() - 1;

					indices.emplace_back(a);
					indices.emplace_back(b);
				}

				long long h = chunk.get_mash_hash(positions, indices);
				auto &mp = chunk.get_mesh_hash_map();
				auto itor = mp.find(h);
				if (itor == mp.end())
				{
					MeshId id = chunk.push_description(positions, indices);
					chunk.push_object(id, vec4(0, 0, .0, 0), col);

					mp[h] = { id, positions.front() };
				}
				else
				{
					std::pair<MeshId, vec4> item = itor->second;
					chunk.push_object(item.first, positions.front() - item.second, col);

					vec4 color;
					for (int k = 0; k < 100; k++)
					{
						if (k % 3 == 0)
						{
							color = vec4(1, rd2(eng), rd2(eng), 1);
						}
						else if (k % 3 == 1)
						{
							color = vec4(rd2(eng), 1, rd2(eng), 1);
						}
						else
						{
							color = vec4(rd2(eng), rd2(eng), 1, 1);
						}
						chunk.push_object(
							item.first, positions.front() - item.second + vec4(rd(eng), rd(eng), rd(eng), 0), color);
					}
				}
			}
		}
	}
}

static void add_mesh(Chunk &chunk, const vector<vec4> &positions, const vector<int> &indices)
{
	MeshId id;
	{
		long long h = chunk.get_mash_hash(positions, indices);

		auto &mp = chunk.get_mesh_hash_map();
		auto itor = mp.find(h);
		if (itor == mp.end())
		{
			id = chunk.push_description(positions, indices);
			mp[h] = { id, positions.front() };
			chunk.push_object(id, vec4(0), { rd2(eng), rd2(eng), 0, 1 });
		}
		else
		{
			std::pair<MeshId, vec4> item = itor->second;
			id = item.first;

			static int val = 0;
			vec4 color;

			if (val % 3 == 0)
			{
				color = vec4(1, rd2(eng), rd2(eng), 1);
			}
			else if (val % 3 == 1)
			{
				color = vec4(rd2(eng), 1, rd2(eng), 1);
			}
			else
			{
				color = vec4(rd2(eng), rd2(eng), 1, 1);
			}
			val++;

			chunk.push_object(id, vec4(rd(eng), rd(eng), rd3(eng), 1) - item.second, color);
		}
	}
}

static void load_object_file(const std::string &file_name, Device &device)
{
	ObjInfo info;

	std::vector<float> &vertices = info.attrib.vertices;
	std::vector<float> &normals = info.attrib.normals;
	std::vector<float> &texcoords = info.attrib.texcoords;

	std::string warn;
	std::string err;

	std::vector<tinyobj::shape_t> shapes;

	LoadObj(&info.attrib, &shapes, nullptr, &err, &warn, file_name.c_str(),
	        nullptr, false);

	Chunk &chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	for (tinyobj::shape_t &shape : shapes)
	{
		std::vector<tinyobj::index_t>& index = shape.mesh.indices;

		vector<vec4> pts;
		vector<int> idxs;

		for (int i = 0; i < vertices.size(); i+=3)
		{
			pts.emplace_back(vertices[i+0], vertices[i+1], vertices[i+2], 1);
		}

		for (auto i : index)
			idxs.emplace_back(i.vertex_index);

		for (int i = 0; i < 1000; i++)
			add_mesh(chunk, pts, idxs);
	}

#if 0
  	OBJ::Parser parser(file_name);
	SceneFormats::Mesh mesh = parser.get_meshes()[0];
	SceneFormats::Mesh mesh2 = SceneFormats::mesh_optimize_index_buffer(parser.get_meshes()[0], false);

	auto& pts = parser.get_positions();
	Chunk& chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	vector<vec4> positions(pts.size());

	for (int i = 0; i < positions.size(); i++)
	{
		auto pt = pts[i];
		positions[i] = vec4(pt.xyz(), 1);
	}

	vector<int> indices;
	for (int i = 0; i < mesh.indices.size(); )
	{
		my_union u;
		u.a = mesh.indices[i++];
		u.b = mesh.indices[i++];
		u.c = mesh.indices[i++];
		u.d = mesh.indices[i++];

		if (u.index >= positions.size())
		{
			u.index = 0;
		}
		indices.push_back(u.index);
	}

	for(int i = 0; i < 1000; i++)
		add_mesh(chunk, positions, indices);
#endif
}

void load_scene(Device &device)
{
#if 0
	load_object_file("F:\\mep-model\\JsonFormatMEP\\sphere.obj", device);
	load_object_file("F:\\mep-model\\JsonFormatMEP\\torusknot.obj", device);

	read_json("F:\\mep-model\\JsonFormatMEP\\HVACModel.json", device);
	read_json("F:\\mep-model\\JsonFormatMEP\\Sample.json", device);
	read_json("F:\\mep-model\\JsonFormatMEP\\PlumbingModel.json", device);
	return;
#endif

	//-----------------------------------------------------------------PushObject
	{
#if 1// _DEBUG
		Chunk &line_chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);

		MeshId line1;
		{
			vector<vec4> positions(2);
			int i = 0;
			positions[i++] = vec4(-0.5, 0, 0.02, 1);
			positions[i++] = vec4(0.5, 0, 0.02, 1);

			i = 0;
			vector<int> indices(2);
			indices[i++] = 0;
			indices[i++] = 1;
			line1 = line_chunk.push_description(positions, indices);
		}
		line_chunk.push_object(line1, vec4(0, 0, .0, 0), { 1, .1, .1, 1 });

		//return;
		Chunk &chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

		MeshId id1;
		{
			vector<vec4> positions(3);
			int i = 0;
			positions[i++] = vec4(-0.5, -1, 0.2, 1);
			positions[i++] = vec4(-1, 0, 0.2, 1);
			positions[i++] = vec4(0., 0, 0.2, 1);

			i = 0;
			vector<int> indices(3);
			indices[i++] = 0;
			indices[i++] = 1;
			indices[i++] = 2;
			id1 = chunk.push_description(positions, indices);
		}

		MeshId id2;
		{
			vector<vec4> positions(4);
			int i = 0;
			float sz = 0.8;
			positions[i++] = vec4(0, -1 * sz, 0.3, 1);
			positions[i++] = vec4(0, 0, 0.3, 1);
			positions[i++] = vec4(1 * sz, 0, 0.3, 1);
			positions[i++] = vec4(1 * sz, -1 * sz, 0.3, 1);

			i = 0;
			vector<int> indices(6);
			indices[i++] = 0;
			indices[i++] = 1;
			indices[i++] = 2;
			indices[i++] = 0;
			indices[i++] = 2;
			indices[i++] = 3;
			id2 = chunk.push_description(positions, indices);
		}

		MeshId id3;
		{
			vector<vec4> positions(3);
			int i = 0;
			positions[i++] = vec4(-0.2, .8, 0, 1) + vec4(-.5, 0, 0.1, 0);
			positions[i++] = vec4(.2, 0.8, 0, 1) + vec4(-.5, 0, 0.1, 0);
			positions[i++] = vec4(0, 0.2, 0, 1) + vec4(-.5, 0, 0.1, 0);

			i = 0;
			vector<int> indices(3);
			indices[i++] = 0;
			indices[i++] = 1;
			indices[i++] = 2;
			id3 = chunk.push_description(positions, indices);
		}
		chunk.push_object(id3, vec4(3, 0, .0, 0), { .1, .1, .1, 1 });

		chunk.push_object(id2, vec4(4, -.2, .5, 0), { 1, .3, 0, 1 });

		chunk.push_object(id1, vec4(-3, .1, .4, 0), { 0, 1, 0, 1 });

		chunk.push_object(id3, vec4(2, 0, .0, 0), { .1, .9, 1, 1 });

		chunk.push_object(id1, vec4(-4, .2, .3, 0), { 0, .5, .8, 1 });

		chunk.push_object(id2, vec4(0, 0, .7, 0), { 0, 0, 1, 1 });

		chunk.push_object(id3, vec4(1, 0, .0, 0), { 1, 1, 1, 1 });
		chunk.push_object(id1, vec4(-5, 0, 0.18, 0), { 1, 0, 0, 1 });
		chunk.push_object(id3, vec4(0, 0, .0, 0), { 1, 0, 1, 1 });

		chunk.push_object(id2, vec4(2, .1, .1, 0), { 1, 1, 0, 1 });
#else

		for (int j = 0; j < 3000 * 100; j++)
		{
			Chunk &chunk = *get_current_chunk(device, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			{
				vector<vec4> positions(3);
				int i = 0;
				positions[i++] = vec4(-0.5, -1, 0.2, 1);
				positions[i++] = vec4(-1, 0, 0.2, 1);
				positions[i++] = vec4(0., 0, 0.2, 1);

				i = 0;
				vector<int> indices(3);
				indices[i++] = 0;
				indices[i++] = 1;
				indices[i++] = 2;
				add_mesh(chunk, positions, indices);
			}

			{
				vector<vec4> positions(4);
				int i = 0;
				float sz = 0.8;
				positions[i++] = vec4(0, -1 * sz, 0.3, 1);
				positions[i++] = vec4(0, 0, 0.3, 1);
				positions[i++] = vec4(1 * sz, 0, 0.3, 1);
				positions[i++] = vec4(1 * sz, -1 * sz, 0.3, 1);

				i = 0;
				vector<int> indices(6);
				indices[i++] = 0;
				indices[i++] = 1;
				indices[i++] = 2;
				indices[i++] = 0;
				indices[i++] = 2;
				indices[i++] = 3;
				add_mesh(chunk, positions, indices);
			}

			{
				vector<vec4> positions(3);
				int i = 0;
				positions[i++] = vec4(-0.2, .8, 0, 1) + vec4(-.5, 0, 0.1, 0);
				positions[i++] = vec4(.2, 0.8, 0, 1) + vec4(-.5, 0, 0.1, 0);
				positions[i++] = vec4(0, 0.2, 0, 1) + vec4(-.5, 0, 0.1, 0);

				i = 0;
				vector<int> indices(3);
				indices[i++] = 0;
				indices[i++] = 1;
				indices[i++] = 2;
				add_mesh(chunk, positions, indices);
			}
		}
#endif
	}
}
