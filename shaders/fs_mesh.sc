#include <bgfx_shader.sh>

uniform vec4 u_ambient;

void main()
{
    gl_FragColor = u_ambient;
}
