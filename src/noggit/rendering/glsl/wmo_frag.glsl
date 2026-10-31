// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

// flags
#define eWMOBatch_ExteriorLit 0x1u
#define eWMOBatch_HasMOCV 0x2u
#define eWMOBatch_Unlit 0x4u
#define eWMOBatch_Unfogged 0x8u
#define eWMOBatch_SIDN 0x20u
#define eWMOBatch_RetailMaterials 0x40u
#define eWMOBatch_NoEnvironment 0x80u
// Bits 8..16 identify authored zero FileDataIDs, not failed texture loads.
#define eWMOBatch_MissingTextureShift 8u

layout (std140) uniform lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
};

uniform vec3 camera;
uniform sampler2DArray texture_samplers[15];
uniform vec3 ambient_color;
uniform vec3 ambient_horizon_color;
uniform vec3 ambient_ground_color;
uniform float sidn_intensity;
// Preview sampling only; terrain and water use their own shaders.
uniform float preview_texture_lod_bias;

in vec3 f_position;
in vec3 f_normal;
in vec2 f_texcoord;
in vec2 f_texcoord_2;
in vec2 f_texcoord_3;
in vec2 f_texcoord_4;
in vec4 f_material_weights;
in vec2 f_env_texcoord;
in vec4 f_vertex_color;
in float f_material_blend_alpha;

flat in uint flags;
flat in uint shader;
flat in uint tex_array0;
flat in uint tex_array1;
flat in uint tex0;
flat in uint tex1;
flat in uint alpha_test_mode;
flat in uint sidn_color;
flat in uvec2 extra_textures[7];

out vec4 out_color;

vec4 get_tex_color(vec2 tex_coord, uint tex_sampler, int array_index)
{
  if (tex_sampler == 0)
  {
    return texture(texture_samplers[0], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(texture_samplers[1], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(texture_samplers[2], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(texture_samplers[3], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(texture_samplers[4], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(texture_samplers[5], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(texture_samplers[6], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(texture_samplers[7], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(texture_samplers[8], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(texture_samplers[9], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(texture_samplers[10], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 11)
  {
    return texture(texture_samplers[11], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 12)
  {
    return texture(texture_samplers[12], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 13)
  {
    return texture(texture_samplers[13], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }
  else if (tex_sampler == 14)
  {
    return texture(texture_samplers[14], vec3(tex_coord, array_index), preview_texture_lod_bias).rgba;
  }

  return vec4(0);
}

vec3 unpack_rgb(uint value)
{
    return vec3(
        float((value >> 16u) & 0xFFu),
        float((value >> 8u) & 0xFFu),
        float((value >> 0u) & 0xFFu)
    ) / 255.0;
}

vec4 get_optional_tex_color(vec2 uv, uint sampler_index, int layer, uint slot, vec4 absent)
{
  if (bool(flags & eWMOBatch_RetailMaterials) &&
      bool(flags & (1u << (eWMOBatch_MissingTextureShift + slot))))
    return absent;
  return get_tex_color(uv, sampler_index, layer);
}

vec3 apply_lighting(vec3 material, float ambient_visibility)
{
  vec3 ambient_term;
  vec3 diffuse_term;
  vec3 vertex_color = bool(flags & eWMOBatch_HasMOCV) ? f_vertex_color.rgb : vec3(0.);

  if(bool(flags & eWMOBatch_Unlit))
  {
    return material.rgb;
  }
  else if(bool(flags & eWMOBatch_ExteriorLit))
  {
    ambient_term = AmbientColor_FogEnd.xyz;
    diffuse_term = DiffuseColor_FogStart.xyz;
  }
  else
  {
    ambient_term = ambient_color;
    if (bool(flags & eWMOBatch_RetailMaterials))
    {
      // Retail MAVG/MAVD carry authored sky/horizon/ground indoor ambient.
      // The CPU supplies the same fallback color in all three slots when the
      // selected doodad set has no directional ambient record.
      float up = clamp(normalize(f_normal).y, -1.0, 1.0);
      ambient_term = mix(ambient_horizon_color,
          up >= 0.0 ? ambient_color : ambient_ground_color, abs(up));
    }
    diffuse_term = vec3(0.0);
  }

  // apply world lighting
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0, 0.0, 0.0);
  vec3 accumlatedLight = vec3(1.0, 1.0, 1.0);

  if(!bool(flags & eWMOBatch_Unlit))
  {
    // Terrain stores MCNR normals in (-world.z, world.y, -world.x).
    // Convert its shared light vector into the model's world-normal basis.
    vec3 light_dir = bool(flags & eWMOBatch_RetailMaterials)
        ? vec3(-LightDir_FogRate.z, LightDir_FogRate.y, -LightDir_FogRate.x)
        : vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y);
    float nDotL = clamp(dot(normalize(f_normal), -normalize(light_dir)), 0.0, 1.0);

    vec3 ambientColor = ambient_term * ambient_visibility + vertex_color;

    vec3 skyColor = (ambientColor * 1.10000002);
    vec3 groundColor = (ambientColor * 0.699999988);

    currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
    lDiffuse = diffuse_term * nDotL;
  }
  else
  {
    currColor = ambient_color + vertex_color;
    accumlatedLight = vec3(0.0f, 0.0f, 0.0f);
  }

  return clamp(material.rgb * (currColor + lDiffuse), 0.0, 1.0);
}

vec3 apply_lighting(vec3 material)
{
  return apply_lighting(material, 1.0);
}

void main()
{

  float dist_from_camera = distance(camera, f_position);
  bool fog = FogColor_FogOn.w != 0 && !bool(flags & eWMOBatch_Unfogged);

  vec4 tex = get_tex_color(f_texcoord, tex_array0, int(tex0));
  // An authored zero secondary FDID binds transparent black, including in
  // two-layer diffuse materials. It must not contribute the debug checker.
  // Primary diffuse and failed nonzero references retain their diagnostics.
  vec4 tex_2 = get_optional_tex_color(f_texcoord_2, tex_array1, int(tex1), 1u, vec4(0.0));
  if (bool(flags & eWMOBatch_RetailMaterials) && bool(flags & eWMOBatch_NoEnvironment) &&
      (shader == 3u || shader == 5u || shader == 12u))
    tex_2 = vec4(0.0);

  float alpha_test = !bool(alpha_test_mode) ? -1.f : (alpha_test_mode < 2 ? 0.878431372 : 0.003921568);

  if (!bool(flags & eWMOBatch_RetailMaterials) && tex.a < alpha_test)
  {
    discard;
  }

  vec4 vertex_color = vec4(0., 0., 0., 1.f);
  vec3 light_color = vec3(1.);

  if(bool(flags & eWMOBatch_HasMOCV))
  {
    vertex_color = f_vertex_color;
  }


  // see: https://github.com/Deamon87/WebWowViewerCpp/blob/master/wowViewerLib/src/glsl/wmoShader.glsl
  float blend_alpha = bool(flags & eWMOBatch_RetailMaterials) ? f_material_blend_alpha : vertex_color.a;
  if(shader == 3) // Env
  {
    vec3 env = tex_2.rgb * (bool(flags & eWMOBatch_RetailMaterials) ? vec3(tex.a) : tex.rgb);
    out_color = vec4(apply_lighting(tex.rgb) + env, 1.);
  }
  else if(shader == 5) // EnvMetal
  {
    vec3 env = tex_2.rgb * tex.rgb * tex.a;
    out_color = vec4(apply_lighting(tex.rgb) + env, 1.);
  }
  else if(shader == 6) // TwoLayerDiffuse
  {
    vec3 layer2 = mix(tex.rgb, tex_2.rgb, tex_2.a);
    out_color = vec4(apply_lighting(mix(layer2, tex.rgb, blend_alpha)), 1.);
  }
  else if (shader == 7 && bool(flags & eWMOBatch_RetailMaterials))
  {
    vec4 environment = bool(flags & eWMOBatch_NoEnvironment) ? vec4(0.0) :
        get_optional_tex_color(f_env_texcoord, extra_textures[0].x, int(extra_textures[0].y), 2u, vec4(0.0));
    vec4 mixed = mix(tex, tex_2, 1.0 - blend_alpha);
    out_color = vec4(apply_lighting(mixed.rgb) + mixed.rgb * mixed.a * environment.rgb, 1.0);
  }
  else if (bool(flags & eWMOBatch_RetailMaterials) &&
      (shader == 8u || shader == 11u || shader == 12u || shader == 15u ||
       shader == 17u || shader == 18u || shader == 19u || shader == 20u))
  {
    vec4 third = get_tex_color(f_texcoord_3, extra_textures[0].x, int(extra_textures[0].y));
    if (shader == 12u)
      third = get_optional_tex_color(f_texcoord_3, extra_textures[0].x, int(extra_textures[0].y), 2u, vec4(0.0));
    vec3 diffuse = tex.rgb;
    vec3 emission = vec3(0.0);
    if (shader == 8u) diffuse = mix(tex_2.rgb, tex.rgb, blend_alpha);
    else if (shader == 11u)
      diffuse = mix(mix(tex.rgb * tex_2.rgb * 2.0, third.rgb, clamp(third.a * blend_alpha, 0.0, 1.0)), tex.rgb, tex.a);
    else if (shader == 12u)
      emission = tex.rgb * tex.a * tex_2.rgb + third.rgb * third.a * blend_alpha;
    else if (shader == 15u)
    {
      diffuse = mix(tex_2.rgb * (1.0 - tex_2.a), tex.rgb, blend_alpha);
      emission = tex_2.rgb * tex_2.a * (1.0 - blend_alpha);
    }
    else if (shader == 17u)
      diffuse = mix(tex.rgb * tex_2.rgb * 2.0 + third.rgb * clamp(third.a * blend_alpha, 0.0, 1.0), tex.rgb, tex.a);
    else if (shader == 18u)
      diffuse = mix(mix(tex.rgb, tex_2.rgb, tex_2.a), tex.rgb, blend_alpha) * third.rgb * 2.0;
    else if (shader == 19u) diffuse = mix(tex.rgb, tex.rgb * tex_2.rgb * 2.0, blend_alpha);
    else if (shader == 20u)
      diffuse = mix(mix(tex.rgb, tex_2.rgb, tex_2.a), tex.rgb, third.a) * third.rgb * 2.0;
    out_color = vec4(apply_lighting(diffuse) + emission, shader == 17u ? 1.0 : tex.a);
  }
  else if (shader == 9 && bool(flags & eWMOBatch_RetailMaterials))
  {
    // DiffuseEmissive: the second texture emits light independently of the sun.
    out_color = vec4(apply_lighting(tex.rgb) + tex_2.rgb * tex_2.a * blend_alpha, tex.a);
  }
  else if (shader == 13 && bool(flags & eWMOBatch_RetailMaterials))
  {
    // TwoLayerDiffuseOpaque uses the material-blend stream, not lighting alpha
    // or either texture's alpha (the same stream as the other two-layer modes).
    out_color = vec4(apply_lighting(mix(tex_2.rgb, tex.rgb, blend_alpha)), 1.0);
  }
  else if (shader == 23 && bool(flags & eWMOBatch_RetailMaterials))
  {
    // MOMT 23: environment, four diffuse layers, then four height/normal maps.
    // MOC2 is decoded from BGRA into RGBA on load; RGB are the first three weights.
    vec2 uv[4] = vec2[4](f_texcoord, f_texcoord_2, f_texcoord_3, f_texcoord_4);
    vec4 layers[4];
    layers[0] = get_tex_color(uv[0], tex_array1, int(tex1));
    for (int i = 1; i < 4; ++i)
      layers[i] = get_tex_color(uv[i], extra_textures[i - 1].x, int(extra_textures[i - 1].y));
    vec4 weights = vec4(clamp(f_material_weights.rgb, 0.0, 1.0),
        1.0 - clamp(dot(f_material_weights.rgb, vec3(1.0)), 0.0, 1.0));
    vec4 heights;
    for (int i = 0; i < 4; ++i)
      heights[i] = max(get_optional_tex_color(uv[i], extra_textures[i + 3].x,
          int(extra_textures[i + 3].y), uint(i + 5), vec4(0.0, 0.0, 0.0, 1.0)).a, 0.004);
    vec4 weighted = weights * heights;
    float peak = max(max(weighted.x, weighted.y), max(weighted.z, weighted.w));
    weighted *= 1.0 - clamp(vec4(peak) - weighted, 0.0, 1.0);
    weighted /= max(dot(weighted, vec4(1.0)), 0.000001);
    vec4 mixed = layers[0] * weighted.x + layers[1] * weighted.y + layers[2] * weighted.z + layers[3] * weighted.w;
    vec3 environment = bool(flags & eWMOBatch_NoEnvironment) ? vec3(0.0) :
        get_optional_tex_color(f_env_texcoord, tex_array0, int(tex0), 0u, vec4(0.0)).rgb;
    // Preview approximation: treat MOC2 alpha as ambient occlusion, preserving
    // direct light, authored vertex lighting, emission and unlit materials.
    float ambient_visibility = 1.0 - clamp(f_material_weights.a, 0.0, 1.0);
    out_color = vec4(apply_lighting(mixed.rgb, ambient_visibility) + mixed.rgb * mixed.a * environment, alpha_test_mode == 0u ? 1.0 : mixed.a);
  }
  else if (shader == 21 && bool(flags & eWMOBatch_RetailMaterials))
  {
    // Retail LOD materials store diffuse in texture 1; texture 2 is a light map.
    out_color = vec4(apply_lighting(tex.rgb), 1.);
  }
  else if (shader == 21 || shader == 23)
  {
    out_color = vec4(apply_lighting(tex_2.rgb), 1.);
  }
  else // default shader, used for shader 0,1,2,4 (Diffuse, Specular, Metal, Opaque)
  {
    out_color = vec4(apply_lighting(tex.rgb), 1.);
  }

  // MOMT opaque variants have fixed opacity; other supported variants retain
  // texture alpha for the material's blend state. Shader 23 uses its mixed alpha.
  if (bool(flags & eWMOBatch_RetailMaterials) && shader != 23u)
    out_color.a = shader == 3u || shader == 4u || shader == 5u || shader == 13u || shader == 17u ? 1.0 : tex.a;

  if (bool(flags & eWMOBatch_SIDN))
  {
    vec3 sidn_rgb = unpack_rgb(sidn_color);
    out_color.rgb = clamp(out_color.rgb + sidn_rgb * sidn_intensity, 0.0, 1.0);
  }

  if(fog)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  if(out_color.a < alpha_test)
  {
    discard;
  }
}
