vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec4 a_color0    : COLOR0;
vec4 i_data0     : TEXCOORD7;
vec4 i_data1     : TEXCOORD6;

vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec3 v_normal    : NORMAL    = vec3(0.0, 1.0, 0.0);
vec3 v_up        : TEXCOORD1 = vec3(0.0, 1.0, 0.0);
vec3 v_relPos    : TEXCOORD2 = vec3(0.0, 0.0, 0.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec4 v_cloud     : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 0.0);
vec3 v_upView    : TEXCOORD4 = vec3(0.0, 1.0, 0.0);
vec2 v_shadowUv  : TEXCOORD5 = vec2(0.0, 0.0);
vec4 v_cloudAux  : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
