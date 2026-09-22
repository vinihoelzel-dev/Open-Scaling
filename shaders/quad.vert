#version 450
layout(location = 0) out vec2 uv;

void main() {
    // Fullscreen triangle: 3 vertices que cobrem a tela toda
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    // O upload corrige a origem vertical do X11; aqui preservamos X e
    // invertemos somente Y para a apresentação Vulkan.
    uv = vec2(pos.x, 1.0 - pos.y);
}
