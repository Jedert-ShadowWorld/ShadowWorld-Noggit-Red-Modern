// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 f_uv;

out vec4 out_color;

uniform sampler2DArray tex;
uniform int tex_index;
uniform vec4 color;
uniform float alpha_test;

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));
  out_color = color * t;

  if (out_color.a < alpha_test)
  {
    discard;
  }
}
