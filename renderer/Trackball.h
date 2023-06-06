#pragma once
#include "camera.hpp"
#include "scene.hpp"

namespace Granite
{

    class Trackball : public Camera, public EventHandler
    {
	public:
	    Trackball(float scrollFactor = 1.0, float factor = 1);

		void set_factor(float scrollFactor, float factor);
		void set_scene(Scene* scene_);
    	
	private:
		bool on_mouse_move(const MouseMoveEvent& e);
		bool on_scroll(const ScrollEvent& e);
		bool on_input_state(const InputStateEvent& e);
		void on_swapchain(const Vulkan::SwapchainParameterEvent& e);

		void full_screen_scene();
    	
		Scene* scene = nullptr;
		unsigned pointer_count = 0;
		bool ignore_orientation = false;

		unsigned width;
		unsigned height;

		double preX = DBL_MAX;
    	double preY = DBL_MAX;
    };

}
