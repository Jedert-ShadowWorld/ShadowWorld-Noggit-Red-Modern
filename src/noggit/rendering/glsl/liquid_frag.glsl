// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

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

uniform float animtime;
uniform sampler2DArray texture_samplers[14] ;

in float depth_;
in vec2 tex_coord_;
in float dist_from_camera_;
flat in uint tex_array;
flat in uint type;
flat in vec2 anim_uv;
flat in int tex_frame;

out vec4 out_color;

vec4 get_tex_color(vec2 tex_coord, uint tex_sampler, int array_index)
{
  if (tex_sampler == 0)
  {
    return texture(texture_samplers[0], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(texture_samplers[1], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(texture_samplers[2], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(texture_samplers[3], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(texture_samplers[4], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(texture_samplers[5], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(texture_samplers[6], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(texture_samplers[7], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(texture_samplers[8], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(texture_samplers[9], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(texture_samplers[10], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 11)
  {
    return texture(texture_samplers[11], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 12)
  {
    return texture(texture_samplers[12], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 13)
  {
    return texture(texture_samplers[13], vec3(tex_coord, array_index)).rgba;
  }

  return vec4(0);
}

vec2 rot2(vec2 p, float degree)
{
  float a = radians(degree);
  return mat2(cos(a), -sin(a), sin(a), cos(a))*p;
}

void main()
{
  // lava || slime
  if(type == 2 || type == 3)
  {
    out_color = get_tex_color(tex_coord_ + vec2(anim_uv.x*animtime / 2880.0, anim_uv.y*animtime / 2880.0), tex_array, tex_frame);
  }
  else
  {
    vec2 uv = rot2(tex_coord_ * anim_uv.x, anim_uv.y);
    vec4 texel = get_tex_color(uv, tex_array, tex_frame);

    // Compatibility path for modern MH2O when legacy River/Ocean color rows
    // are missing or resolve to nearly black.  Use the scene's ambient/diffuse
    // lighting to obtain the same muddy/tinted WoW-style water appearance as
    // ordinary legacy maps, while retaining depth darkening and ripple detail.
    if (type == 4 || type == 5)
    {
      float depth = clamp(depth_, 0.0, 1.0);
      vec3 scene_light = clamp(
          AmbientColor_FogEnd.rgb * 0.72 + DiffuseColor_FogStart.rgb * 0.28,
          0.0, 1.0);

      // A restrained neutral base prevents black water in zones whose modern
      // liquid color metadata is unavailable. River/lake is slightly warmer;
      // ocean is slightly cooler and darker.
      vec3 neutral = (type == 5)
                   ? vec3(0.070, 0.085, 0.095)
                   : vec3(0.105, 0.090, 0.075);

      vec3 shallow = mix(neutral, scene_light, (type == 5) ? 0.42 : 0.56);
      vec3 deep = mix(neutral * 0.72, scene_light * 0.42, (type == 5) ? 0.32 : 0.40);
      vec3 water_rgb = mix(shallow, deep, depth);

      // The procedural texture is deliberately low-energy; use it as a ripple
      // highlight rather than as the main color source.
      water_rgb += texel.rgb * 0.70;

      float alpha = (type == 5) ? 0.88 : 0.82;
      out_color = vec4(clamp(water_rgb, 0.0, 1.0), alpha);
    }
    else
    {
      vec4 lerp = (type == 1)
                ? mix (OceanColorLight, OceanColorDark, depth_)
                : mix (RiverColorLight, RiverColorDark, depth_)
                ;

      //clamp shouldn't be needed
      out_color = vec4 (clamp(texel + lerp, 0.0, 1.0).rgb, lerp.a);
    }
  }

  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera_ * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
  }
}
