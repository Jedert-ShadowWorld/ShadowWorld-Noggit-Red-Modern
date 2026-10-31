// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 uv1;
in vec2 uv2;
in vec2 uv3;
in float edge_fade;
in float camera_dist;
in vec3 norm;

out vec4 out_color;

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

uniform vec4 mesh_color;
uniform int blend_mode;
// Retail foliage alpha-key coverage uses the midpoint cutoff.
uniform bool retail_alpha_key;
uniform bool retail_materials;
// Preview sampling only; shared texture storage and saved materials stay unchanged.
uniform float preview_texture_lod_bias;
uniform float preview_decal_lod_bias;

uniform sampler2DArray tex1;
uniform sampler2DArray tex2;
uniform sampler2DArray tex3;
uniform int tex1_index;
uniform int tex2_index;
uniform int tex3_index;

uniform int unfogged;
uniform int unlit;

uniform int pixel_shader;

void main()
{

  float alpha_test;
  int fog_mode;

  switch (blend_mode)
  {
      default:
      case 0: // Opaque
      {
          alpha_test = -1.0;
          fog_mode = 1;
          break;
      }
      case 1: // Alpha_key
      {
          alpha_test = ((retail_alpha_key ? 128.f : 224.f) / 255.f) * mesh_color.w;
          fog_mode = 1;
          break;
      }
      case 2: // Alpha
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 1;
          break;
      }
      case 3: // No_Add_Alpha
      case 4: // Add
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 2; // Warning: wiki is unsure on that for No_Add_Alpha
          break;
      }
      case 5: // Mod
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 3;
          break;
      }
      case 6: // Mod2X
      {
          alpha_test = (1.f / 255.f) * mesh_color.w;
          fog_mode = 4;
          break;
      }
      case 7: // InvSrcAlphaAdd
      {
          alpha_test = -1.0;
          fog_mode = 2;
          break;
      }
  }

  vec4 color = vec4(0.0);

  if(mesh_color.a < alpha_test)
  {
    discard;
  }
  
  // code from Deamon87 and https://wowdev.wiki/M2/Rendering#Pixel_Shaders
  if (pixel_shader == 0) //Combiners_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 1) // Combiners_Decal
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = mix(mesh_color.rgb, texture1.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 2) // Combiners_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture1.rgba + mesh_color.rgba;
  } 
  else if (pixel_shader == 3) // Combiners_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 4) // Combiners_Fade
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = mix(texture1.rgb, mesh_color.rgb, mesh_color.a);
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 5) // Combiners_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 6) // Combiners_Opaque_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 7) // Combiners_Opaque_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a + texture1.a;
  } 
  else if (pixel_shader == 8) // Combiners_Opaque_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = texture2.a * mesh_color.a * 2.0;
  } 
  else if (pixel_shader == 9)  // Combiners_Opaque_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * mesh_color.rgb * texture2.rgb * vec3(2.0);
      color.a  = mesh_color.a;
  } 
  else if (pixel_shader == 10) // Combiners_Opaque_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = mesh_color.a;
  } 
  else if (pixel_shader == 11) // Combiners_Opaque_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture2.a * mesh_color.a;
  } 
  else if (pixel_shader == 12) // Combiners_Mod_Opaque
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb;
      color.a = texture1.a;
  } 
  else if (pixel_shader == 13) // Combiners_Mod_Add
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture2.rgba + texture1.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 14) // Combiners_Mod_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(2.0);
  } 
  else if (pixel_shader == 15) // Combiners_Mod_Mod2xNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture1.rgb * texture2.rgb * mesh_color.rgb * vec3(2.0);
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 16) // Combiners_Mod_AddNA
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = texture2.rgb + texture1.rgb * mesh_color.rgb;
      color.a = texture1.a * mesh_color.a;
  } 
  else if (pixel_shader == 17) // Combiners_Mod_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba;
  } 
  else if (pixel_shader == 18) // Combiners_Add_Mod
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgb = (texture1.rgb + mesh_color.rgb) * texture2.a;
      color.a = (texture1.a + mesh_color.a) * texture2.a;
  } 
  else if (pixel_shader == 19) // Combiners_Mod2x_Mod2x
  {
      vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
      vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
      color.rgba = texture1.rgba * texture2.rgba * mesh_color.rgba * vec4(4.0);
  }
  else if (pixel_shader == 20)  // Combiners_Opaque_Mod2xNA_Alpha
  {
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
    color.rgb = (mesh_color.rgb * texture1.rgb) * mix(texture2.rgb * 2.0, vec3(1.0), texture1.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 21)   //Combiners_Opaque_AddAlpha
  {
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
    color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a);
    color.a = mesh_color.a;
  }
  else if (pixel_shader == 22)   // Combiners_Opaque_AddAlpha_Alpha
  {
    vec4 texture1 = texture(tex1, vec3(uv1, tex1_index), preview_texture_lod_bias + preview_decal_lod_bias);
    vec4 texture2 = texture(tex2, vec3(uv2, tex2_index), preview_texture_lod_bias + preview_decal_lod_bias);
    color.rgb = (mesh_color.rgb * texture1.rgb) + (texture2.rgb * texture2.a * texture1.a);
    color.a = mesh_color.a;
  }

  // Authored Retail SKIN combiners: diffuse receives lighting; additive glow does not.
  // Contracts follow wow.export ShaderMapper and mpv_m2.fragment.shader.
  // Attribution/license: docs/third-party/wow.export-MIT.txt.
  vec3 retail_emission = vec3(0.0);
  if (retail_materials && pixel_shader >= 100)
  {
    int id = pixel_shader - 100;
    float bias = preview_texture_lod_bias + preview_decal_lod_bias;
    vec4 a = texture(tex1, vec3(uv1, tex1_index), bias);
    vec4 b = texture(tex2, vec3(uv2, tex2_index), bias);
    vec4 c = texture(tex3, vec3(uv3, tex3_index), bias);
    vec3 diffuse = a.rgb;
    float opacity = 1.0;
    bool has_alpha = false;
    if (id == 0 || id == 3 || id == 17 || id == 24)
    {
      diffuse = mix(a.rgb * b.rgb * 2.0, a.rgb, a.a);
      if (id == 3) retail_emission = c.rgb * c.a;
      if (id == 17 || id == 24)
      {
        diffuse *= 1.0 - clamp(c.a, 0.0, 1.0);
        retail_emission = c.rgb * clamp(c.a, 0.0, 1.0);
      }
    }
    else if (id == 1 || id == 5 || id == 10 || id == 15)
      retail_emission = b.rgb * b.a;
    else if (id == 2)
      retail_emission = b.rgb * b.a * (1.0 - a.a);
    else if (id == 4 || id == 6 || id == 13 || id == 14)
    {
      retail_emission = b.rgb * b.a; opacity = a.a; has_alpha = true;
    }
    else if (id == 7)
    {
      retail_emission = b.rgb * b.a * (1.0 - a.a);
      opacity = a.a + b.a * dot(b.rgb, vec3(0.3, 0.59, 0.11)); has_alpha = true;
    }
    else if (id == 8)
      diffuse = mix(mix(a.rgb, b.rgb, b.a), a.rgb, a.a);
    else if (id == 9)
      diffuse = mix(a.rgb * b.rgb * 2.0, c.rgb, c.a);
    else if (id == 11)
    {
      retail_emission = b.rgb * (1.0 - a.a); opacity = a.a + b.a; has_alpha = true;
    }
    else if (id == 12)
      diffuse = mix(a.rgb * b.rgb, a.rgb, a.a);
    else if (id == 16)
    {
      diffuse = mix(a.rgb, b.rgb, b.a); retail_emission = a.rgb * a.a;
    }
    else if (id == 21)
    {
      diffuse = a.rgb * b.rgb; opacity = a.a * b.a; has_alpha = true;
    }
    else if (id == 23)
      diffuse = mix(a.rgb, b.rgb, b.a);
    else if (id == 26 || id == 28)
    {
      diffuse = mix(a.rgb * mix(vec3(1.0), b.rgb, b.a), c.rgb, c.a);
      if (id == 26) { opacity = a.a; has_alpha = true; }
    }
    else if (id == 27)
    {
      diffuse = a.rgb * mix(vec3(1.0), b.rgb, b.a); opacity = a.a; has_alpha = true;
    }
    else if (id == 30)
    {
      diffuse = a.rgb * b.rgb * c.rgb; opacity = a.a * b.a * c.a; has_alpha = true;
    }
    else if (id == 33)
    {
      diffuse = a.rgb * b.rgb * 2.0; opacity = a.a * b.a * 2.0; has_alpha = true;
    }
    else if (id == 34)
    {
      opacity = a.a; has_alpha = true;
    }
    // Alpha-key discards authored coverage, then emits mesh opacity; ordinary alpha
    // uses texture opacity. Both include the optional view-dependent edge fade.
    if (has_alpha && blend_mode == 1 && opacity < 128.0 / 255.0) discard;
    color.rgb = mesh_color.rgb * diffuse;
    color.a = mesh_color.a * edge_fade * ((blend_mode == 0 || blend_mode == 1) ? 1.0 : opacity);
    alpha_test = -1.0;
  }

  if(color.a < alpha_test)
  {
    discard;
  }

  // apply world lighting
  vec3 currColor;
  vec3 lDiffuse = vec3(0.0, 0.0, 0.0);
  vec3 accumlatedLight = vec3(1.0, 1.0, 1.0);

  if(unlit == 0)
  {
      // Terrain stores MCNR normals in (-world.z, world.y, -world.x).
      // Convert its shared light vector into the model's world-normal basis.
      vec3 light_dir = retail_materials
          ? vec3(-LightDir_FogRate.z, LightDir_FogRate.y, -LightDir_FogRate.x)
          : vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y);
      float nDotL = clamp(dot(normalize(norm), -normalize(light_dir)), 0.0, 1.0);

      vec3 ambientColor = AmbientColor_FogEnd.xyz;

      vec3 skyColor = (ambientColor * 1.10000002);
      vec3 groundColor = (ambientColor * 0.699999988);

      currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
      lDiffuse = DiffuseColor_FogStart.xyz * nDotL;
  }
  else
  {
      // Retail unlit materials retain their authored color independently of world lighting.
      currColor = retail_materials ? vec3(1.0) : AmbientColor_FogEnd.xyz;
      accumlatedLight = vec3(0.0f, 0.0f, 0.0f);
  }

  color.rgb = clamp(color.rgb * (currColor + lDiffuse) + retail_emission, 0.0, 1.0);

  if(FogColor_FogOn.w != 0 && unfogged == 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (camera_dist * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    vec3 fog;

    // see https://wowdev.wiki/M2/Rendering#Fog_Modes
    if(fog_mode == 1)
    {
      fog = FogColor_FogOn.rgb;
    }
    else if(fog_mode == 2)
    {
      fog = vec3(0.);
    }
    else if(fog_mode == 3)
    {
      fog = vec3(1.);
    }
    else if(fog_mode == 4)
    {
      fog = vec3(0.5);
    }

    color.rgb = mix(color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  out_color = color;
}
