#version 450

layout(location = 0) out vec4 FragColor;
layout(location = 0) flat in vec4 vColor;

void main()
{
    FragColor = vColor;
	//FragColor = vec4(0,1,1,1);
}