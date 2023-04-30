#version 450
#include "util.h"

layout( location = 0 ) in vec4 Position;
//layout( location = 1 ) in vec4 Color;
layout( location = 0 ) flat out vec4 vColor;

layout( std430, set = 0, binding = 0 ) buffer ColorInput
{
	vec4 colors[];
};

layout( std430, set = 0, binding = 1) buffer TranslateInput
{
	vec4 translates[];
};

layout( std430, set = 0, binding = 2 ) buffer Mesh_Id
{
	MeshId ids[];
};

const float TO_RADIUS = 3.14159265359 * 2.0 / 360.0;
const float theta = 0.0 * TO_RADIUS;

void main()
{
	float c = cos(theta);
	float s = sin(theta);
	
	mat2 rotZ;	
	{
		rotZ[0][0] = c; rotZ[0][1] = -s; // rotZ[0][2] = 0;
		rotZ[1][0] = s; rotZ[1][1] = c; // rotZ[1][2] = 0;
		//rotZ[2][0] = 0; rotZ[2][1] = 0;  rotZ[2][2] = 1;
	}
		  	
	uint index = ids[gl_InstanceIndex].culledIndex;
//  index = gl_InstanceIndex;
	gl_Position = registers.MVP *(Position +  translates[index] + vec4(0, 0,0,0));
	
	//gl_Position =(Position +  translates[index] + vec4(0, 0,0,0));
	
	//ids[gl_InstanceIndex].debug0 = gl_InstanceIndex;

	vColor = colors[index];
	//vColor = vec4(1,0,0,1);
	//if(gl_InstanceIndex == 9) vColor = vec4(1, 1, 1, 1);
	//vColor = vec4(vec3(gl_InstanceIndex / 10.),1);
}
