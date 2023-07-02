#include "Trackball.h"
#include "transforms.hpp"
#include "input.hpp"
#include "scene.hpp"
#include "muglm/matrix_helper.hpp"

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
	auto &objects = scene
	                ->get_entity_pool()
	                .get_component_group<RenderInfoComponent, RenderableComponent>();
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

	if (state.get_key_pressed(Key::W))
		position += FACTOR  * get_front() * static_cast<float>(state.get_delta_time());
	else if (state.get_key_pressed(Key::S))
		position -= FACTOR * get_front() * static_cast<float>(state.get_delta_time());
	if (state.get_key_pressed(Key::D))
		position += FACTOR * get_right() * static_cast<float>(state.get_delta_time());
	else if (state.get_key_pressed(Key::A))
		position -= FACTOR * get_right() * static_cast<float>(state.get_delta_time());

	float dx = 0.0f;
	float dy = 0.0f;
	if (state.get_key_pressed(Key::Left))
		dx -= 1.20f * state.get_delta_time();
	if (state.get_key_pressed(Key::Right))
		dx += 1.20f * state.get_delta_time();

	if (state.get_key_pressed(Key::Up))
		dy -= 1.20f * state.get_delta_time();
	if (state.get_key_pressed(Key::Down))
		dy += 1.20f * state.get_delta_time();

	quat pitch = angleAxis(dy, vec3(1.0f, 0.0f, .0f));
	quat yaw = angleAxis(dx, vec3(.0f, 0.0f, 1.0f));
	rotation = normalize(pitch * rotation * yaw);

	return true;
}

bool Trackball::on_mouse_move(const MouseMoveEvent &m)
{
	auto dx = static_cast<float>(m.get_delta_x());
	auto dy = static_cast<float>(m.get_delta_y());

	if (preX == DBL_MAX && preY == DBL_MAX)
	{
		preX = m.get_abs_x();
		preY = m.get_abs_y();
	}

	vec2 prev_ndc = ndc(preX, preY, width, height);
	vec3 pre_tbc = tbc(preX, preY, width, height);

	vec2 new_ndc = ndc(m.get_abs_x(), m.get_abs_y(), width, height);
	vec3 new_tbc = tbc(m.get_abs_x(), m.get_abs_y(), width, height);

	if (shift_pressed && m.get_mouse_button_pressed(MouseButton::Left))
	{
		auto dx = float(m.get_delta_x());
		auto dy = float(m.get_delta_y());
		quat pitch = angleAxis(dy * 0.005f, vec3(1.0f, 0.0f, 0.0f));
		quat yaw = angleAxis(dx * 0.005f, vec3(0.0f, 1.0f, 0.0f));
		rotation = normalize(pitch * rotation * yaw);

		//auto up = get_up();
		//position = rotateX(position, dx * 0.005f);
		//position = rotateY(position, dy * 0.005f);
	}
	else if (m.get_mouse_button_pressed(MouseButton::Right))
	{
		vec2 delta = new_ndc - prev_ndc;

		vec3 sideNormal = cross(get_front(), get_up());

		float distance = 1000.0;
		vec3 translation = sideNormal * (-delta.x * distance) + get_up() * (delta.y * distance);
		position += translation;
	}

	preX = m.get_abs_x();
	preY = m.get_abs_y();

	return true;
}

bool Trackball::on_scroll(const ScrollEvent &e)
{
	float f = static_cast<float>(e.get_yoffset()) * SCROLL_FACTOR;
	position += get_front() * f;
	return true;
}
}
