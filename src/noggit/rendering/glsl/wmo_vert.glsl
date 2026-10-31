// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

in vec4 position;
in vec3 normal;
in vec4 vertex_color;
in vec2 texcoord;
in vec2 texcoord_2;
in vec2 texcoord_3;
in vec2 texcoord_4;
in vec4 material_weights;
in float material_blend_alpha;
in uint batch_mapping;

out vec3 f_position;
out vec3 f_normal;
out vec2 f_texcoord;
out vec2 f_texcoord_2;
out vec2 f_texcoord_3;
out vec2 f_texcoord_4;
out vec4 f_material_weights;
out vec2 f_env_texcoord;
out vec4 f_vertex_color;
out float f_material_blend_alpha;

flat out uint flags;
flat out uint shader;
flat out uint tex_array0;
flat out uint tex_array1;
flat out uint tex0;
flat out uint tex1;
flat out uint alpha_test_mode;
flat out uint sidn_color;
flat out uvec2 extra_textures[7];

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
};

uniform mat4 transform;
uniform usamplerBuffer render_batches_tex;

float makeNaN(float nonneg)
{
  return sqrt(-nonneg-1.0);
}

void main()
{
  float NaN = makeNaN(1);
  f_texcoord_3 = texcoord_3;
  f_texcoord_4 = texcoord_4;
  f_material_weights = material_weights;
  f_material_blend_alpha = material_blend_alpha;
  f_env_texcoord = vec2(0.5);
  for (int slot = 0; slot < 7; ++slot) extra_textures[slot] = uvec2(0);

  if (!bool(batch_mapping)) // discard
  {
    gl_Position = vec4(NaN, NaN, NaN, NaN);

    f_position = vec3(0);
    f_normal = vec3(0);
    f_texcoord = vec2(0);
    f_texcoord_2 = vec2(0);
    f_vertex_color = vec4(0);

    flags = 0;
    shader = 0;
    tex_array0 = 0;
    tex_array1 = 0;
    tex0 = 0;
    tex1 = 0;
    alpha_test_mode = 0;
    sidn_color = 0;
  }
  else
  {
    vec4 pos = transform * position;
    vec4 view_space_pos = model_view * pos;
    gl_Position = projection * view_space_pos;

    f_position = pos.xyz;
    f_normal = mat3(transform) * normal;

    uvec4 batch_first_half = texelFetch(render_batches_tex, int((batch_mapping - 1) * 6));
    uvec4 batch_second_half = texelFetch(render_batches_tex, int((batch_mapping - 1) * 6 + 1));

    flags = batch_first_half.r;
    shader = batch_first_half.g;
    tex_array0 = batch_first_half.b;
    tex_array1 = batch_first_half.a;
    tex0 = batch_second_half.r;
    tex1 = batch_second_half.g;
    alpha_test_mode = batch_second_half.b;
    sidn_color = batch_second_half.a;
    for (int slot = 0; slot < 7; ++slot)
    {
      uvec4 texture_pair = texelFetch(render_batches_tex, int((batch_mapping - 1) * 6 + 2) + slot / 2);
      extra_textures[slot] = (slot % 2 == 0) ? texture_pair.rg : texture_pair.ba;
    }
    vec3 view_normal = normalize(mat3(model_view) * f_normal);
    vec3 reflection = reflect(normalize(view_space_pos.xyz), view_normal);
    // TwoLayerEnvMetal uses reflection XY; shader 23 uses sphere-map UVs.
    f_env_texcoord = shader == 7 ? reflection.xy :
        normalize(vec3(reflection.xy, reflection.z + 1.0)).xy * 0.5 + 0.5;


    // Env and EnvMetal
    if(shader == 3 || shader == 5)
    {
      f_texcoord = texcoord;
      f_texcoord_2 = bool(flags & 0x40u) ? reflection.xy : reflect(normalize(view_space_pos.xyz), f_normal).xy;
    }
    else
    {
      f_texcoord = texcoord;
      f_texcoord_2 = texcoord_2;
    }

    if (bool(flags & 0x40u))
    {
      if (shader == 11u || shader == 12u || shader == 17u)
      {
        f_texcoord_2 = normalize(vec3(reflection.xy, reflection.z + 1.0)).xy * 0.5 + 0.5;
        f_texcoord_3 = texcoord_2;
      }
      // Terrain composition projects onto the model plane, independent of camera motion.
      else if (shader == 8u) f_texcoord_2 = vec2(position.x, -position.z) * -0.239999995;
    }
    f_vertex_color = vertex_color;
  }
}
