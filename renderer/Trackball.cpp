#include "Trackball.h"
#include "input.hpp"
#include "muglm/matrix_helper.hpp"
#include "scene.hpp"
#include "transforms.hpp"

float SCROLL_FACTOR = 2.0;
float FACTOR = 8.0;

namespace Granite
{
vec2 ndc(double absX, double absY, unsigned width, unsigned height)
{
	float aspect = static_cast<float>(width) / static_cast<float>(height);
	float x = (static_cast<float>(absX) / static_cast<float>(width) * 2.0 - 1) * aspect;
	float y = (static_cast<float>(absY) / static_cast<float>(width) * 2.0 - 1);

	return vec2(x, y);
}

vec3 tbc(double absX, double absY, unsigned width, unsigned height)
{
	vec2 v = ndc(absX, absY, width, height);

	double l = length(v);
	if (l < 1.0f)
	{
		double h = 0.5 + cos(l * muglm::pi<double>()) * 0.5;
		return vec3(v.x, -v.y, h);
	}
	return vec3(v.x, -v.y, 0.0);
}

Trackball::Trackball(float scrollFactor, float factor)
{
	SCROLL_FACTOR = scrollFactor;
	FACTOR = factor;
	EVENT_MANAGER_REGISTER(Trackball, on_mouse_move, MouseMoveEvent);
	EVENT_MANAGER_REGISTER(Trackball, on_scroll, ScrollEvent);
	EVENT_MANAGER_REGISTER(Trackball, on_input_state, InputStateEvent);
	EVENT_MANAGER_REGISTER_LATCH(Trackball, on_swapchain, on_swapchain, Vulkan::SwapchainParameterEvent);
}

void Trackball::set_factor(float scrollFactor, float factor)
{
	SCROLL_FACTOR = scrollFactor;
	FACTOR = factor;
}

void Trackball::set_scene(Scene *scene_)
{
	scene = scene_;
}

void Trackball::look_at(const vec3 &eye, const vec3 &at, const vec3 &up_)
{
	center = at;
	up = up_;
	Camera::look_at(eye, at, up);
}

void Trackball::on_swapchain(const Vulkan::SwapchainParameterEvent &state)
{
	set_aspect(state.get_aspect_ratio());
	width = state.get_width();
	height = state.get_height();
}

void Trackball::full_screen_scene()
{
	if (!scene)
		return;

	AABB aabb(vec3(FLT_MAX), vec3(-FLT_MAX));
	auto &objects = scene->get_entity_pool().get_component_group<RenderInfoComponent, RenderableComponent>();
	for (auto &caster : objects)
		aabb.expand(get_component<RenderInfoComponent>(caster)->world_aabb);

	double radius = aabb.get_radius();

	vec3 center = aabb.get_center();

	position = center - get_front() * static_cast<float>(radius);
}

bool Trackball::on_input_state(const InputStateEvent &state)
{
	if (state.get_key_pressed(Key::Space))
		full_screen_scene();

	shift_pressed = state.get_key_pressed(Key::LeftShift);
	position += FACTOR * get_front() * static_cast<float>(pointer_count) * static_cast<float>(state.get_delta_time());

	float accelerate = 1.f;
	if (shift_pressed)
		accelerate = 2.f;
	if (state.get_key_pressed(Key::W))
		position += accelerate * FACTOR * get_front() * static_cast<float>(state.get_delta_time());
	else if (state.get_key_pressed(Key::S))
		position -= accelerate * FACTOR * get_front() * static_cast<float>(state.get_delta_time());
	if (state.get_key_pressed(Key::D))
		position += accelerate * FACTOR * get_right() * static_cast<float>(state.get_delta_time());
	else if (state.get_key_pressed(Key::A))
		position -= accelerate * FACTOR * get_right() * static_cast<float>(state.get_delta_time());

	position.z = max(2.0f, position.z);

	float dx = 0.0f;

	if (shift_pressed)
	{
		double delta = 0;
		if (state.get_key_pressed(Key::Up))
			delta -= 10. * state.get_delta_time();
		if (state.get_key_pressed(Key::Down))
			delta += 10. * state.get_delta_time();
		position.z += delta;
		center.z += delta;
	}
	else
	{
		if (state.get_key_pressed(Key::Up))
			dx -= 10 * state.get_delta_time();
		if (state.get_key_pressed(Key::Down))
			dx += 10 * state.get_delta_time();
	}

	if (abs(dx) > FLT_EPSILON)
	{
		position += vec3(0, 0, dx);
		rotation = Granite::look_at(-position, up);
	}

	auto up = get_up();

	float dz = 0.0f;
	if (state.get_key_pressed(Key::Left))
		dz -= 1.20f * state.get_delta_time();
	if (state.get_key_pressed(Key::Right))
		dz += 1.20f * state.get_delta_time();

	if (abs(dz) > FLT_EPSILON)
	{
		up = vec3(0, 0, 1);
		position = angleAxis(dz, up) * position;
		rotation = Granite::look_at(-position, up);
	}

	//rotation = normalize(pitch * rotation * yaw);

	return true;
}

bool Trackball::on_mouse_move(const MouseMoveEvent &m)
{
	if (shift_pressed && m.get_mouse_button_pressed(MouseButton::Left))
	{
		auto dx = float(m.get_delta_x()) * 0.002f;
		auto dy = float(m.get_delta_y()) * 0.002f;

		auto right = get_right();
		position = angleAxis(-dx, up) * position;
		position = angleAxis(-dy, right) * position;
		rotation = Granite::look_at(-position, up);
	}

	return true;
}

bool Trackball::on_scroll(const ScrollEvent &e)
{
	float f = static_cast<float>(e.get_yoffset()) * SCROLL_FACTOR;
	position += get_front() * f;
	return true;
}
} // namespace Granite
