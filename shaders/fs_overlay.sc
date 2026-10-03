#include <bgfx_shader.sh>

uniform vec4 u_overlayColor;   // straight (non-premultiplied) RGBA

void main()
{
    gl_FragColor = u_overlayColor;
}
