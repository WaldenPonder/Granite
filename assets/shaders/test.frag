#version 450
layout(location = 0) out vec4 FragColor;
layout(location = 0) in vec2 vUV;
void main()
{
    FragColor = vec4(vUV, 0, 1);
}