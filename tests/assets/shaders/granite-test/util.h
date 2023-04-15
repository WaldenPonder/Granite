
//一个场景， 如果只有一个球，但是被instance绘制了10次，则object_size = 10, desc_size = 1
layout(push_constant, std430) uniform Registers
{
  mat4 MVP;
  int object_size;
  int desc_size;
  float scale;
  int dummy;
} registers;

//一个mesh的索引信息
struct MeshId
{
	int vertexOffset;
	int firstIndex;
	int descIndex;
	int indexCount;
	
	uint culledIndex;//剔除完之后的索引
	int debug0;
	int debug1;
	int debug2;
};

struct VkDrawIndexedIndirectCommand
{
	uint   indexCount;
	uint   instanceCount;
	uint   firstIndex;
	int    vertexOffset;
	uint   firstInstance;
};

/*
*数组大小和DrawIndexedIndirectCommand相同
*/
struct DescAdditional
{
	vec4 bbMinPt;
	vec4 bbMaxPt;
	uint innerIndex;
	uint debug0;
	uint debug1;
	uint debug2;
};


bool CheckCull(vec4 cp0, vec4 cp1, vec4 cp2, vec4 cp3, 
			vec4 cp4, vec4 cp5, vec4 cp6, vec4 cp7) {
	// if(cp0.w < 0.001 || cp1.w < 0.001 || cp2.w < 0.001 || cp3.w < 0.001 || 
		// cp4.w < 0.001 || cp5.w < 0.001 || cp6.w < 0.001 || cp7.w < 0.001) return false;

	vec3 p0 = cp0.xyz / cp0.w, p1 = cp1.xyz / cp1.w, p2 = cp2.xyz / cp2.w, p3 = cp3.xyz / cp3.w;
	vec3 p4 = cp4.xyz / cp4.w, p5 = cp5.xyz / cp5.w, p6 = cp6.xyz / cp6.w, p7 = cp7.xyz / cp7.w;

	float thr = 1.0, thz = 1.0;
	vec3 maxP = max(p7, max(p6, max(p5, max(p4, max(p3, max(p2, max(p0, p1)))))));		
	vec3 minP = min(p7, min(p6, min(p5, min(p4, min(p3, min(p2, min(p0, p1)))))));	
	return any(greaterThan(minP, vec3(thr, thr, thz))) || any(lessThan(maxP, vec3(-thr, -thr, -thz)));
}
