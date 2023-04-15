#include "indirect_render.h"

IndirectRender::IndirectRender(RenderContext& context) : render_context(context)
{
	EVENT_MANAGER_REGISTER_LATCH(IndirectRender, on_device_created, on_device_destroyed, DeviceCreatedEvent);
}

void IndirectRender::on_device_created(const DeviceCreatedEvent &e)
{
	
}

void IndirectRender::on_device_destroyed(const DeviceCreatedEvent &)
{
}

void IndirectRender::clear()
{
}

void IndirectRender::cull()
{

}

void IndirectRender::calcul_first_instance()
{

}

void IndirectRender::calcul_culled_index_relationship()
{
}

void IndirectRender::draw()
{
}

void IndirectRender::render_frame(double t, double)
{

}
