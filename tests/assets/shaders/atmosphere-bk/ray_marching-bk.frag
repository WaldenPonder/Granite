#version 450
layout(location = 0) out vec4 FragColor;
layout(location = 0) in  vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uImage;

#define PI 3.1415926535897932384626433832795f
#define PLANET_RADIUS_OFFSET 0.01f

layout(push_constant, std430) uniform AtmosphereParameters
{		
	// Radius of the planet (center to ground)
	float BottomRadius;
	// Maximum considered atmosphere height (center to atmosphere top)
	float TopRadius;

	// Rayleigh scattering exponential distribution scale in the atmosphere
	float RayleighDensityExpScale;

		// Another medium type in the atmosphere
	float AbsorptionDensity0LayerWidth;

	// Rayleigh scattering coefficients
	vec3 RayleighScattering;

	// Mie scattering exponential distribution scale in the atmosphere
	float MieDensityExpScale;

	// Mie scattering coefficients
	vec3 MieScattering;

	float AbsorptionDensity0ConstantTerm;

	// Mie extinction coefficients
	vec3 MieExtinction;
	float AbsorptionDensity0LinearTerm;

	// Mie absorption coefficients
	vec3 MieAbsorption;
	// Mie phase function excentricity
	float MiePhaseG;
		
	// This other medium only absorb light, e.g. useful to represent ozone in the earth atmosphere
	vec3 AbsorptionExtinction;
	float AbsorptionDensity1ConstantTerm;

	// The albedo of the ground.
	vec3 GroundAlbedo;
	float AbsorptionDensity1LinearTerm;
    
	vec2 RayMarchMinMaxSPP;
	float screenWidth;
	float screenHeight;
} PARAM;


layout(std140, set = 0, binding = 1) uniform UBO
{   
    vec4 camarePos;
	mat4 MVP;
	mat4 inversMVP;
	mat4 projectMat;
	mat4 invProjMat;
    mat4 invViewMat;
};

vec2 RayMarchMinMaxSPP = vec2(4, 14);


float RayleighPhase(float cosTheta)
{
	float factor = 3.0f / (16.0f * PI);
	return factor * (1.0f + cosTheta * cosTheta);
}

float CornetteShanksMiePhaseFunction(float g, float cosTheta)
{
	float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
	return k * (1.0 + cosTheta * cosTheta) / pow(1.0 + g * g - 2.0 * g * -cosTheta, 1.5);
}

float hgPhase(float g, float cosTheta)
{
#ifdef USE_CornetteShanks
	return CornetteShanksMiePhaseFunction(g, cosTheta);
#else
	// Reference implementation (i.e. not schlick approximation). 
	// See http://www.pbr-book.org/3ed-2018/Volume_Scattering/Phase_Functions.html
	float numer = 1.0f - g * g;
	float denom = 1.0f + g * g + 2.0f * g * cosTheta;
	return numer / (4.0f * PI * denom * sqrt(denom));
#endif
}

void UvToLutTransmittanceParams(out float viewHeight, out float viewZenithCosAngle, in vec2 uv)
{
	float x_mu = uv.x;
	float x_r = uv.y;

	float H = sqrt(PARAM.TopRadius * PARAM.TopRadius - PARAM.BottomRadius * PARAM.BottomRadius);
	float rho = H * x_r;
	viewHeight = sqrt(rho * rho + PARAM.BottomRadius * PARAM.BottomRadius);

	float d_min = PARAM.TopRadius - viewHeight;
	float d_max = rho + H;
	float d = d_min + x_mu * (d_max - d_min);
	viewZenithCosAngle = d == 0.0 ? 1.0f : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
	viewZenithCosAngle = clamp(viewZenithCosAngle, -1.0, 1.0);
}


// - r0: ray origin
// - rd: normalized ray direction
// - s0: sphere center
// - sR: sphere radius
// - Returns distance from r0 to first intersecion with sphere,
//   or -1.0 if no intersection.
float raySphereIntersectNearest(vec3 r0, vec3 rd, vec3 s0, float sR)
{
	float a = dot(rd, rd);
	vec3 s0_r0 = r0 - s0;
	float b = 2.0 * dot(rd, s0_r0);
	float c = dot(s0_r0, s0_r0) - (sR * sR);
	float delta = b * b - 4.0*a*c;
	if (delta < 0.0 || a == 0.0)
	{
		return -1.0;
	}
	float sol0 = (-b - sqrt(delta)) / (2.0*a);
	float sol1 = (-b + sqrt(delta)) / (2.0*a);
	if (sol0 < 0.0 && sol1 < 0.0)
	{
		return -1.0;
	}
	if (sol0 < 0.0)
	{
		return max(0.0, sol1);
	}
	else if (sol1 < 0.0)
	{
		return max(0.0, sol0);
	}
	return max(0.0, min(sol0, sol1));
}

struct MediumSampleRGB
{
	vec3 scattering;
	vec3 absorption;
	vec3 extinction;

	vec3 scatteringMie;
	vec3 absorptionMie;
	vec3 extinctionMie;

	vec3 scatteringRay;
	vec3 absorptionRay;
	vec3 extinctionRay;

	vec3 scatteringOzo;
	vec3 absorptionOzo;
	vec3 extinctionOzo;

	vec3 albedo;
};

float getAlbedo(float scattering, float extinction)
{
	return scattering / max(0.001f, extinction);
}
vec3 getAlbedo(vec3 scattering, vec3 extinction)
{
	return scattering / max(vec3(0.001f), extinction);
}

MediumSampleRGB sampleMediumRGB(in vec3 WorldPos)
{
	const float viewHeight = length(WorldPos) - PARAM.BottomRadius;

	const float densityMie = exp(PARAM.MieDensityExpScale * viewHeight);
	const float densityRay = exp(PARAM.RayleighDensityExpScale * viewHeight);
	const float densityOzo = clamp(viewHeight < PARAM.AbsorptionDensity0LayerWidth ?
		PARAM.AbsorptionDensity0LinearTerm * viewHeight + PARAM.AbsorptionDensity0ConstantTerm :
		PARAM.AbsorptionDensity1LinearTerm * viewHeight + PARAM.AbsorptionDensity1ConstantTerm, 0, 1.0);

	MediumSampleRGB s;

	s.scatteringMie = densityMie * PARAM.MieScattering;
	s.absorptionMie = densityMie * PARAM.MieAbsorption;
	s.extinctionMie = densityMie * PARAM.MieExtinction;

	s.scatteringRay = densityRay * PARAM.RayleighScattering;
	s.absorptionRay = vec3(0.0f);
	s.extinctionRay = s.scatteringRay + s.absorptionRay;

	s.scatteringOzo = vec3(0.0);
	s.absorptionOzo = densityOzo * PARAM.AbsorptionExtinction;
	s.extinctionOzo = s.scatteringOzo + s.absorptionOzo;

	s.scattering = s.scatteringMie + s.scatteringRay + s.scatteringOzo;
	s.absorption = s.absorptionMie + s.absorptionRay + s.absorptionOzo;
	s.extinction = s.extinctionMie + s.extinctionRay + s.extinctionOzo;
	s.albedo = getAlbedo(s.scattering, s.extinction);

	return s;
}

void LutTransmittanceParamsToUv(in float viewHeight, in float viewZenithCosAngle, out vec2 uv)
{
	float H = sqrt(max(0.0f, PARAM.TopRadius * PARAM.TopRadius - PARAM.BottomRadius * PARAM.BottomRadius));
	float rho = sqrt(max(0.0f, viewHeight * viewHeight - PARAM.BottomRadius * PARAM.BottomRadius));

	float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0) + PARAM.TopRadius * PARAM.TopRadius;
	float d = max(0.0, (-viewHeight * viewZenithCosAngle + sqrt(discriminant))); // Distance to atmosphere boundary

	float d_min = PARAM.TopRadius - viewHeight;
	float d_max = rho + H;
	float x_mu = (d - d_min) / (d_max - d_min);
	float x_r = rho / H;

	uv = vec2(x_mu, x_r);	
}

vec3 IntegrateScatteredLuminance(
	in vec2 pixPos, in vec3 WorldPos, in vec3 WorldDir, in vec3 SunDir,
	in bool ground, in float SampleCountIni, in float DepthBufferValue, in bool VariableSampleCount,
	in bool MieRayPhase, in float tMaxMax /*= 9000000.0f*/)
{
	vec3 ClipSpace = vec3((pixPos / vec2(PARAM.screenWidth, PARAM.screenHeight))*vec2(2.0, -2.0) - vec2(1.0, -1.0), 1.0);

	// Compute next intersection with atmosphere or ground 
	vec3 earthO = vec3(0.0f, 0.0f, 0.0f);
	float tBottom = raySphereIntersectNearest(WorldPos, WorldDir, earthO, PARAM.BottomRadius);
	float tTop = raySphereIntersectNearest(WorldPos, WorldDir, earthO, PARAM.TopRadius);
	float tMax = 0.0f;
	if (tBottom < 0.0f)
	{
		if (tTop < 0.0f)
		{
			tMax = 0.0f; // No intersection with earth nor atmosphere: stop right away  
			return vec3(1, 0, 1);
		}
		else
		{
			tMax = tTop;
		}
	}
	else
	{
		if (tTop > 0.0f)
		{
			tMax = min(tTop, tBottom);
		}
	}

	if (DepthBufferValue >= 0.0f)
	{
		ClipSpace.z = DepthBufferValue;
		if (ClipSpace.z < 1.0f)
		{
			vec4 DepthBufferWorldPos = inversMVP * vec4(ClipSpace, 1.0);
			DepthBufferWorldPos /= DepthBufferWorldPos.w;

			float tDepth = length(DepthBufferWorldPos.xyz - (WorldPos + vec3(0.0, 0.0, -PARAM.BottomRadius))); // apply earth offset to go back to origin as top of earth mode. 
			if (tDepth < tMax)
			{
				tMax = tDepth;
			}
            
            return vec3(1, 0, 1);
		}
		//		if (VariableSampleCount && ClipSpace.z == 1.0f)
		//			return result;
	}
	tMax = min(tMax, tMaxMax);

	// Sample count 
	float SampleCount = SampleCountIni;
	float SampleCountFloor = SampleCountIni;
	float tMaxFloor = tMax;
	if (VariableSampleCount)
	{
		SampleCount = mix(PARAM.RayMarchMinMaxSPP.x, PARAM.RayMarchMinMaxSPP.y, clamp(tMax*0.01, 0, 1));
		SampleCountFloor = floor(SampleCount);
		tMaxFloor = tMax * SampleCountFloor / SampleCount;	// rescale tMax to map to the last entire step segment.
	}
	float dt = tMax / SampleCount;

	// Phase functions
	const float uniformPhase = 1.0 / (4.0 * PI);
	const vec3 wi = SunDir;
	const vec3 wo = WorldDir;
	float cosTheta = dot(wi, wo);
	float MiePhaseValue = hgPhase(PARAM.MiePhaseG, -cosTheta);	// mnegate cosTheta because due to WorldDir being a "in" direction. 
	float RayleighPhaseValue = RayleighPhase(cosTheta);

	// Ray march the atmosphere to integrate optical depth
	vec3 OpticalDepth = vec3(0.0);
	float t = 0.0f;
	float tPrev = 0.0;
	const float SampleSegmentT = 0.3f;
    
    vec3 L = vec3(0,0,0);
    vec3 globalL = vec3(1);
    
    vec3 throughput = vec3(1.0f);
	for (float s = 0.0f; s < SampleCount; s += 1.0f)
	{
		if (VariableSampleCount)
		{
			// More expenssive but artefact free
			float t0 = (s) / SampleCountFloor;
			float t1 = (s + 1.0f) / SampleCountFloor;
			// Non linear distribution of sample within the range.
			t0 = t0 * t0;
			t1 = t1 * t1;
			// Make t0 and t1 world space distances.
			t0 = tMaxFloor * t0;
			if (t1 > 1.0)
			{
				t1 = tMax;
				//	t1 = tMaxFloor;	// this reveal depth slices
			}
			else
			{
				t1 = tMaxFloor * t1;
			}
			//t = t0 + (t1 - t0) * (whangHashNoise(pixPos.x, pixPos.y, gFrameId * 1920 * 1080)); // With dithering required to hide some sampling artefact relying on TAA later? This may even allow volumetric shadow?
			t = t0 + (t1 - t0)*SampleSegmentT;
			dt = t1 - t0;
		}
		else
		{
			//t = tMax * (s + SampleSegmentT) / SampleCount;
			// Exact difference, important for accuracy of multiple scattering
			float NewT = tMax * (s + SampleSegmentT) / SampleCount;
			dt = NewT - t;
			t = NewT;
		}
		vec3 P = WorldPos + t * WorldDir;

		MediumSampleRGB medium = sampleMediumRGB(P);
		const vec3 SampleOpticalDepth = medium.extinction * dt;
		const vec3 SampleTransmittance = exp(-SampleOpticalDepth);
		OpticalDepth += SampleOpticalDepth;

		float pHeight = length(P);
		const vec3 UpVector = P / pHeight;
		float SunZenithCosAngle = dot(SunDir, UpVector);
		vec2 uv;
		LutTransmittanceParamsToUv(pHeight, SunZenithCosAngle, uv);
		vec3 TransmittanceToSun =  texture(uImage, uv, 0).rgb;
//return TransmittanceToSun * 0.01;
		vec3 PhaseTimesScattering;
		if (MieRayPhase)
		{
			PhaseTimesScattering = medium.scatteringMie * MiePhaseValue + medium.scatteringRay * RayleighPhaseValue;
		}
		else
		{
			PhaseTimesScattering = medium.scattering * uniformPhase;
		}

		// Earth shadow 
		float tEarth = raySphereIntersectNearest(P, SunDir, earthO + PLANET_RADIUS_OFFSET * UpVector, PARAM.BottomRadius);
		float earthShadow = tEarth >= 0.0f ? 0.0f : 1.0f;

		// Dual scattering for multi scattering 

		vec3 multiScatteredLuminance = vec3(0.0f);

		float shadow = 1.0f;
		vec3 S = globalL * (earthShadow * shadow * TransmittanceToSun * PhaseTimesScattering + multiScatteredLuminance * medium.scattering);

		vec3 MS = medium.scattering * 1;
		vec3 MSint = (MS - MS * SampleTransmittance) / medium.extinction;
	//	result.MultiScatAs1 += throughput * MSint;
    
		// See slide 28 at http://www.frostbite.com/2015/08/physically-based-unified-volumetric-rendering-in-frostbite/ 
		vec3 Sint = (S - S * SampleTransmittance) / medium.extinction;	// integrate along the current step segment 
		L += throughput * Sint;														// accumulate and also take into account the transmittance from previous steps
		throughput *= SampleTransmittance;
		tPrev = t;
	}
    
	return L;
}


void main()
{

    vec2 pixPos = gl_FragCoord.xy;

    vec3 camera = camarePos.xyz;// vec3(0, 150, 0.9);
    vec2 ttUV = pixPos / vec2(PARAM.screenWidth, PARAM.screenHeight);
    vec3 ClipSpace = vec3(ttUV * vec2(2.0, -2.0) - vec2(1.0, -1.0), 1.0);
	vec4 HViewPos = invProjMat * vec4(ClipSpace, 1.0);
    vec4 temp = invViewMat * HViewPos;
	vec3 WorldDir = normalize(temp.xyz / temp.w);
    
     //   FragColor = texture(uImage, ttUV) ;
  // if(FragColor.rgb == vec3(0))
   // FragColor = vec4(1,0,0,1);
  //  return;
    //FragColor.rgb = WorldDir;
   // FragColor.a = 1;
   // return;
     //  A few extra needed constants
	vec3 WorldPos = camera + vec3(0.0f, 0.0f, PARAM.BottomRadius);
	  
	const bool ground = false;
	const float SampleCountIni = 40.0f;	// Can go a low as 10 sample but energy lost starts to be visible.
	const float DepthBufferValue = -1.0;
	const bool VariableSampleCount = false;
	const bool MieRayPhase = true;
    
    vec3 sun_direction = normalize(vec3(1, 1, 1.));
   
    vec3 L = IntegrateScatteredLuminance(pixPos, WorldPos, WorldDir, sun_direction, 
    ground, SampleCountIni, DepthBufferValue, VariableSampleCount, MieRayPhase, 9000000.0f);
    
    FragColor = 5.0 * vec4(L,1);
    FragColor.a = 1;
    
 #if 0   
    if(PARAM.BottomRadius == 6360.0f)
    if(PARAM.TopRadius == 6460.0f)
    if(PARAM.RayleighDensityExpScale == 8.0f)
    if(PARAM.AbsorptionDensity0LayerWidth == 25.f)
    if(PARAM.RayleighScattering == vec3(0.005802f, 0.013558f, 0.033100f))
    if(PARAM.MieDensityExpScale == 1.2f)
    if(PARAM.AbsorptionExtinction == vec3(0.000650f, 0.001881f, 0.000085f))
    if(PARAM.MieExtinction == vec3(0.004440f, 0.004440f, 0.004440f))
    if(PARAM.MiePhaseG == .8f)
    if(PARAM.MieScattering == vec3(0.003996f, 0.003996f, 0.003996f))
    if(PARAM.AbsorptionDensity0ConstantTerm == -2.0f / 3.0f)
    if(PARAM.AbsorptionDensity0LinearTerm == 1.0f / 15.0f)
    if(PARAM.AbsorptionDensity1ConstantTerm ==  8.0f / 3.0f)
    if(PARAM.AbsorptionDensity1LinearTerm ==  -1.0f / 15.0f)
    if(PARAM.screenWidth == 1280)
    if(PARAM.screenHeight == 720)
    if(PARAM.RayMarchMinMaxSPP == vec2(4, 14))
        FragColor = vec4(1,0,0,1);
#endif
}