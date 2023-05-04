#version 450
layout(location = 0) out vec4 FragColor;

#define TRANSMITTANCE_TEXTURE_WIDTH  256
#define TRANSMITTANCE_TEXTURE_HEIGHT 64

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
};

void main()
{
    //FragColor = Scale * textureLod(uImage, vUV, 0.0);
    FragColor.rgb =  vec3(RayleighDensityExpScale);//vec3(BottomRadius, TopRadius, RayleighDensityExpScale);
    FragColor.a = 1;
    
    if( BottomRadius == 6360.0f)
    if(TopRadius == 6460.0f)
    if(RayleighDensityExpScale == 8.0f)
    if(AbsorptionDensity0LayerWidth == 25.f)
    if(RayleighScattering == vec3(0.005802f, 0.013558f, 0.033100f))
    if(MieDensityExpScale == 1.2f)
    if(AbsorptionExtinction == vec3(0.000650f, 0.001881f, 0.000085f))
    if(MieExtinction == vec3(0.004440f, 0.004440f, 0.004440f))
    if(MiePhaseG == .18f)
    if(MieScattering == vec3(0.003996f, 0.003996f, 0.003996f))
    if(AbsorptionDensity0ConstantTerm == -2.0f / 3.0f)
    if(AbsorptionDensity0LinearTerm == 1.0f / 15.0f)
    if(AbsorptionDensity1ConstantTerm ==  8.0f / 3.0f)
    if(AbsorptionDensity1LinearTerm ==  -1.0f / 15.0f)
        FragColor = vec4(1,0,0,1);
}