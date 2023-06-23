#include "atmosphere.h"
#include "render_graph.hpp"
#include "renderer.hpp"

using namespace Granite;
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



struct TexturePlaneInfo
{
	Vulkan::Program *program;
	const Vulkan::ImageView *reflection;
	const Vulkan::ImageView *refraction;
	const Vulkan::ImageView *normal;

	struct Push
	{
		vec4 normal;
		vec4 tangent;
		vec4 bitangent;
		vec4 position;
		vec4 dPdx;
		vec4 dPdy;
		vec4 offset_scale;
		vec4 base_emissive;
	};
	Push push;
};

static void texture_plane_render(CommandBuffer &cmd, const RenderQueueData *infos, unsigned instances)
{
	for (unsigned i = 0; i < instances; i++)
	{
		auto &info = *static_cast<const TexturePlaneInfo *>(infos[i].render_info);
		cmd.set_program(info.program);
		if (info.reflection)
			cmd.set_texture(2, 0, *info.reflection, Vulkan::StockSampler::DefaultGeometryFilterClamp);
		if (info.refraction)
			cmd.set_texture(2, 1, *info.refraction, Vulkan::StockSampler::DefaultGeometryFilterClamp);
		cmd.set_texture(2, 2, *info.normal, Vulkan::StockSampler::DefaultGeometryFilterWrap);
		CommandBufferUtil::set_quad_vertex_state(cmd);
		cmd.set_cull_mode(VK_CULL_MODE_NONE);
		cmd.push_constants(&info.push, 0, sizeof(info.push));
		CommandBufferUtil::draw_quad(cmd);
	}
}

TexturePlane2::TexturePlane2(const std::string &normal_path)
{
	normalmap =
	    GRANITE_ASSET_MANAGER()->register_image_resource(*GRANITE_FILESYSTEM(), normal_path, ImageClass::Normal);
}

void TexturePlane2::setup_render_pass_resources(RenderGraph &graph)
{
	reflection = nullptr;
	refraction = nullptr;

	if (need_reflection)
		reflection =
		    &graph.get_physical_texture_resource(graph.get_texture_resource(reflection_name).get_physical_index());
	if (need_refraction)
		refraction =
		    &graph.get_physical_texture_resource(graph.get_texture_resource(refraction_name).get_physical_index());
}

void TexturePlane2::setup_render_pass_dependencies(RenderGraph &, Granite::RenderPass &target,
                                                  RenderPassCreator::DependencyFlags dep_type)
{
	if ((dep_type & RenderPassCreator::MATERIAL_BIT) != 0)
	{
		if (need_reflection)
			target.add_texture_input(reflection_name);
		if (need_refraction)
			target.add_texture_input(refraction_name);
	}
}

void TexturePlane2::setup_render_pass_dependencies(RenderGraph &)
{
}

void TexturePlane2::set_scene(Scene *scene_)
{
	scene = scene_;
}

void TexturePlane2::render_main_pass(Vulkan::CommandBuffer &cmd, const mat4 &proj, const mat4 &view)
{
	LightingParameters lighting = *base_context->get_lighting_parameters();
	lighting.shadows = nullptr;
	lighting.cluster = nullptr;

	context.set_lighting_parameters(&lighting);
	context.set_camera(proj, view);

	visible.clear();
	scene->gather_visible_opaque_renderables(context.get_visibility_frustum(), visible);
	scene->gather_visible_transparent_renderables(context.get_visibility_frustum(), visible);
	scene->gather_unbounded_renderables(visible);

	// FIXME: Need to rethink this. We shouldn't be allowed to mutate the renderer suite.
	LOGE("FIXME, TexturePlane2::render_main_pass\n");
	auto &renderer = renderer_suite->get_renderer(RendererSuite::Type::ForwardOpaque);
	//renderer.set_mesh_renderer_options_from_lighting(lighting);
	renderer.begin(internal_queue);
	internal_queue.push_renderables(context, visible.data(), visible.size());
	renderer.flush(cmd, internal_queue, context);
}

void TexturePlane2::set_plane(const vec3 &position_, const vec3 &normal_, const vec3 &up_, float extent_up,
                             float extent_across)
{
	position = position_;
	normal = normal_;
	up = up_;
	rad_up = extent_up;
	rad_x = extent_across;

	dpdx = normalize(cross(normal, up)) * extent_across;
	dpdy = normalize(up) * -extent_up;
}

void TexturePlane2::set_zfar(float zfar_)
{
	zfar = zfar_;
}

void TexturePlane2::add_render_pass(RenderGraph &graph, Type type)
{
	auto &device = graph.get_device();
	bool supports_32bpp =
	    device.image_format_is_supported(VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);

	AttachmentInfo color, depth, reflection_blur;
	color.format = supports_32bpp ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
	depth.format = device.get_default_depth_format();

	color.size_x = scale_x;
	color.size_y = scale_y;
	depth.size_x = scale_x;
	depth.size_y = scale_y;

	reflection_blur.size_x = 0.5f * scale_x;
	reflection_blur.size_y = 0.5f * scale_y;
	reflection_blur.levels = 0;
	reflection_blur.flags |= ATTACHMENT_INFO_MIPGEN_BIT;

	auto &name = type == Reflection ? reflection_name : refraction_name;

	auto &lighting = graph.add_pass(name + "-lighting", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	lighting.add_color_output(name + "-HDR", color);
	lighting.set_depth_stencil_output(name + "-depth", depth);

	lighting.set_get_clear_depth_stencil(
	    [](VkClearDepthStencilValue *value) -> bool
	    {
		    if (value)
		    {
			    value->depth = 1.0f;
			    value->stencil = 0;
		    }
		    return true;
	    });

	lighting.set_get_clear_color(
	    [](unsigned, VkClearColorValue *value) -> bool
	    {
		    if (value)
			    memset(value, 0, sizeof(*value));
		    return true;
	    });

	lighting.set_build_render_pass(
	    [this, type](Vulkan::CommandBuffer &cmd)
	    {
		    if (type == Reflection)
		    {
			    mat4 proj, view;
			    float z_near;
			    compute_plane_reflection(proj, view, base_context->get_render_parameters().camera_position, position,
			                             normal, up, rad_up, rad_x, z_near, zfar);

			    // FIXME: Should not be allowed.
			    LOGE("FIXME, TexturePlane2::add_render_pass\n");
			    //renderer.set_mesh_renderer_options(Renderer::ENVIRONMENT_ENABLE_BIT | Renderer::SHADOW_ENABLE_BIT);

			    if (zfar > z_near)
				    render_main_pass(cmd, proj, view);
		    }
		    else if (type == Refraction)
		    {
			    mat4 proj, view;
			    float z_near;
			    compute_plane_refraction(proj, view, base_context->get_render_parameters().camera_position, position,
			                             normal, up, rad_up, rad_x, z_near, zfar);

			    // FIXME: Should not be allowed.
			    //renderer.set_mesh_renderer_options(Renderer::ENVIRONMENT_ENABLE_BIT | Renderer::SHADOW_ENABLE_BIT | Renderer::REFRACTION_ENABLE_BIT);

			    if (zfar > z_near)
				    render_main_pass(cmd, proj, view);
		    }
	    });

	lighting.add_texture_input("shadow-main");

	auto &reflection_blur_pass = graph.add_pass(name, RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
	auto &reflection_input_res = reflection_blur_pass.add_texture_input(name + "-HDR");
	reflection_blur_pass.add_color_output(name, reflection_blur);
	reflection_blur_pass.set_build_render_pass(
	    [&](Vulkan::CommandBuffer &cmd)
	    {
		    cmd.set_texture(0, 0, graph.get_physical_texture_resource(reflection_input_res),
		                    Vulkan::StockSampler::LinearClamp);
		    CommandBufferUtil::draw_fullscreen_quad(cmd, "builtin://shaders/quad.vert", "builtin://shaders/blur.frag",
		                                            { { "METHOD", 6 } });
	    });
}

void TexturePlane2::add_render_passes(RenderGraph &graph)
{
	if (need_reflection)
		add_render_pass(graph, Reflection);
	if (need_refraction)
		add_render_pass(graph, Refraction);
}

void TexturePlane2::set_base_renderer(const RendererSuite *suite)
{
	renderer_suite = suite;
}

void TexturePlane2::set_base_render_context(const RenderContext *context_)
{
	base_context = context_;
}

void TexturePlane2::get_render_info(const RenderContext &context_, const RenderInfoComponent *, RenderQueue &queue) const
{
	TexturePlaneInfo info;
	info.reflection = reflection;
	info.refraction = refraction;
	info.normal = queue.get_resource_manager().get_image_view(normalmap);
	info.push.normal = vec4(normalize(normal), 0.0f);
	info.push.position = vec4(position, 0.0f);
	info.push.dPdx = vec4(dpdx, 0.0f);
	info.push.dPdy = vec4(dpdy, 0.0f);
	info.push.tangent = vec4(normalize(dpdx), 0.0f);
	info.push.bitangent = vec4(normalize(dpdy), 0.0f);
	info.push.offset_scale = vec4(vec2(0.03 * base_context->get_frame_parameters().elapsed_time), vec2(2.0f));
	info.push.base_emissive = vec4(base_emissive, 0.0f);

	Util::Hasher h;
	if (info.reflection)
		h.u64(info.reflection->get_cookie());
	else
		h.u32(0);

	if (info.refraction)
		h.u64(info.refraction->get_cookie());
	else
		h.u32(0);

	h.u64(info.normal->get_cookie());
	auto instance_key = h.get();
	auto sorting_key = RenderInfo::get_sort_key(context_, Queue::OpaqueEmissive, h.get(), h.get(), position);
	auto *plane_info =
	    queue.push<TexturePlaneInfo>(Queue::OpaqueEmissive, instance_key, sorting_key, texture_plane_render, nullptr);

	if (plane_info)
	{
		unsigned mat_mask = MATERIAL_EMISSIVE_BIT;
		mat_mask |= info.refraction ? MATERIAL_EMISSIVE_REFRACTION_BIT : 0;
		mat_mask |= info.reflection ? MATERIAL_EMISSIVE_REFLECTION_BIT : 0;
		info.program = queue.get_shader_suites()[Util::ecast(RenderableType::TexturePlane)].get_program(
		    VariantSignatureKey::build(DrawPipeline::Opaque, 0, mat_mask));
		*plane_info = info;
	}
}