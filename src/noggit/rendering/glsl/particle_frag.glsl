// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 f_uv;
in vec4 f_color;

out vec4 out_color;

uniform sampler2DArray tex;
uniform int tex_index;

uniform float alpha_test;

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));
  vec4 c = f_color * t;

  if(c.a < alpha_test)
  {
    discard;
  }

  out_color = c;
}
