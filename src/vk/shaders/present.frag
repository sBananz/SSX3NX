#version 450

/* The frame on the screen (vkr.c, Switch swapchain): one triangle over the
 * whole swapchain image (crt.vert). The frame fills `rect` (its aspect kept,
 * black around it), through the gamma ramp when there is one (the NV2A's
 * ramp, fb_gamma.c: 256 texels, R/G/B looked up separately -- what the CPU
 * presenter's blit did), then the FPS overlay's corner blended on top: its
 * alpha is the box's darkening (0.75) and 1 for the white text. */
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D ramp;
layout(set = 0, binding = 2) uniform sampler2D overlay;

layout(push_constant) uniform PC {
    vec4 rect;          /* x, y, w, h in swapchain pixels */
    vec2 ov_size;       /* the overlay image, from the top-left corner */
    float gamma_on;
    float ov_on;
} pc;

layout(location = 0) out vec4 o;

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 uv = (p - pc.rect.xy) / pc.rect.zw;
    vec3 c = vec3(0.0);
    if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        c = texture(frame, uv).rgb;
        if (pc.gamma_on > 0.5) {
            vec3 i = (floor(c * 255.0 + 0.5) + 0.5) / 256.0;
            c = vec3(texture(ramp, vec2(i.r, 0.5)).r,
                     texture(ramp, vec2(i.g, 0.5)).g,
                     texture(ramp, vec2(i.b, 0.5)).b);
        }
    }
    if (pc.ov_on > 0.5 && p.x < pc.ov_size.x && p.y < pc.ov_size.y) {
        vec4 v = texelFetch(overlay, ivec2(p), 0);
        c = mix(c, v.rgb, v.a);
    }
    o = vec4(c, 1.0);
}
