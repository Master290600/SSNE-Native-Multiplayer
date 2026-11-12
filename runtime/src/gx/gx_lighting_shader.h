// GLSL port-reference XF lighting. Normal/channel/light state is std140 immutable.
#pragma once
namespace ssne {
inline constexpr const char* kVertexLightingSource=R"GLSL(
layout(location=11) in vec3 a_normal;
layout(location=12) in vec4 a_rawpos;
uniform int u_xf;
uniform ivec4 light_ctrl[2];
uniform vec4 light_mat[2],light_amb[2],normal_rows[34],light_data[40];
uniform vec4 u_model[66],u_projection[2],u_post[66];
uniform ivec4 u_texcontrol[8];
vec3 xf_view(vec3 input_pos,int matrix){
 vec3 v;
 for(int r=0;r<3;++r)v[r]=u_model[matrix+r].x*input_pos.x+u_model[matrix+r].y*input_pos.y+u_model[matrix+r].z*input_pos.z+u_model[matrix+r].w;
 return v;
}
vec4 xf_project(vec3 v){
 return vec4(u_projection[0].x*v.x+u_projection[0].y*v.z,u_projection[0].z*v.y+u_projection[0].w*v.z,u_projection[1].x*v.z+u_projection[1].y,-v.z);
}
vec4 light_channel_gpu(int chan,vec3 pos,vec3 normal,vec4 vertex_color){
 if(light_ctrl[chan].z==0)return vec4(0);
 vec4 result=vec4(0);
 for(int part=0;part<2;++part){
  uint ctrl=uint(part==0?light_ctrl[chan].x:light_ctrl[chan].y);
  vec4 mat=(ctrl&1u)!=0u?vertex_color:light_mat[chan];
  vec4 lit=(ctrl&64u)!=0u?vertex_color:light_amb[chan];
  if((ctrl&2u)!=0u){
   uint mask=((ctrl>>2)&15u)|(((ctrl>>11)&15u)<<4),diffuse=(ctrl>>7)&3u;
   for(int l=0;l<8;++l)if((mask&(1u<<l))!=0u){
    int b=l*5;vec3 L=light_data[b+3].xyz-pos;
    float dist2=L.x*L.x+L.y*L.y+L.z*L.z,dist=sqrt(dist2);if(dist>0)L/=dist;
    float ndl=normal.x*L.x+normal.y*L.y+normal.z*L.z;
    float diff=diffuse==0u?1:diffuse==1u?ndl:max(0,ndl),att=1;
    if((ctrl&512u)!=0u){
     vec3 a=light_data[b+1].xyz,k=light_data[b+2].xyz;float term,den;
     if((ctrl&1024u)!=0u){
      term=max(0,-(L.x*light_data[b+4].x+L.y*light_data[b+4].y+L.z*light_data[b+4].z));
      den=k.x+k.y*dist+k.z*dist2;
     }else{
      float h=normal.x*light_data[b+4].x+normal.y*light_data[b+4].y+normal.z*light_data[b+4].z;
      term=ndl>0?max(0,h):0;den=k.x+k.y*term+k.z*term*term;
     }
     float num=max(0,a.x+a.y*term+a.z*term*term);att=den>0?num/den:num;
    }
    lit+=light_data[b]*diff*att;
   }
   mat*=clamp(lit/255,0,1);
  }
  if(part==0)result.rgb=mat.rgb;else result.a=mat.a;
 }
 return result;
}
)GLSL";
}
