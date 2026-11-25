#version 450

/* SSX3NX_BOOST_LINES: the boost look while player 1 boosts (vkr.c
 * vkr_speedlines, drawn over the 3D frame before the HUD):
 *
 *   edge blur   a zoom blur towards the focus at the screen's edges only:
 *               none within ~45% of the half-diagonal of the focus, so the
 *               rider and the course ahead stay sharp, growing to the
 *               corners (8 taps along the ray, up to ~28 px at 720 lines);
 *   speed lines thin white rays around the edges, pointing at the focus (a
 *               little above the centre, where the course runs to): a few
 *               dozen of the 160 angular slots carry one at any moment,
 *               re-chosen ~14 times a second so they flicker, each a dash
 *               running outwards; a pixel is covered a third at most.
 *
 * Output premultiplied for blend ONE, ONE_MINUS_SRC_ALPHA:
 *   final = mix(mix(frame, blurred, k), white, la)
 *         = frame (1-k)(1-la) + blurred k (1-la) + la
 * so rgb = blurred k (1-la) + la, alpha = 1 - (1-k)(1-la); the frame's own
 * alpha (rider tags) is kept by the alpha blend factors (vkr.c). */

layout(set = 0, binding = 0) uniform sampler2D img;   /* the frame so far */

layout(push_constant) uniform P {
    vec4 a;     /* strength 0..1, time (s), focus x, focus y (target pixels) */
    vec4 b;     /* target width, height, scale (height / 720), edge blur on (1) / off (0) */
} pc;

layout(location = 0) out vec4 o;

float hash(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

void main()
{
    vec2 p = gl_FragCoord.xy, d = p - pc.a.zw;
    float half_diag = 0.5 * length(pc.b.xy);
    float r = length(d) / half_diag;            /* 0 at the focus, ~1 in the corners */
    if (r < 0.45)
        discard;                                /* the centre stays as it is */

    /* edge blur: k is how much of the blurred colour shows */
    float k = smoothstep(0.45, 1.05, r) * pc.a.x * 0.85 * pc.b.w;
    vec3 blurred = vec3(0.0);
    if (k > 0.004) {
        vec2 dir = d / max(length(d), 1.0);
        float len = 28.0 * pc.b.z * smoothstep(0.45, 1.1, r);
        float j = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;  /* break up banding */
        for (int i = 0; i < 8; i++) {
            float t = (float(i) + 0.5 + j) / 8.0;
            vec2 q = clamp(p - dir * len * t, vec2(0.5), pc.b.xy - 0.5);
            blurred += textureLod(img, q / pc.b.xy, 0.0).rgb;
        }
        blurred *= 0.125;
    } else
        k = 0.0;

    /* speed lines: la is the white line's coverage */
    float la = 0.0;
    if (r >= 0.52) {
        const float N = 160.0;
        float s = (atan(d.y, d.x) / 6.2831853 + 0.5) * N;
        float slot = floor(s), f = fract(s) - 0.5;
        float flick = floor(pc.a.y * 14.0);
        float h = hash(slot + flick * 7.13);     /* this slot's ray, this moment */
        float on = step(0.72, h);                /* ~28% of the slots */
        float slot_px = 6.2831853 * r * half_diag / N;
        float w = (0.75 + 0.75 * hash(slot + 3.1)) * pc.b.z / slot_px;   /* ~1.5 px wide */
        float line = 1.0 - smoothstep(w * 0.5, w, abs(f));
        float start = 0.55 + 0.25 * hash(slot + flick * 3.7 + 1.0);
        float body = smoothstep(start, start + 0.18, r);
        float dash = smoothstep(0.0, 0.25, fract(r * 1.6 - pc.a.y * (2.2 + 1.5 * h)));
        la = on * line * body * dash * pc.a.x * 0.33;
    }
    if (k <= 0.0 && la <= 0.0)
        discard;
    o = vec4(blurred * k * (1.0 - la) + vec3(la), 1.0 - (1.0 - k) * (1.0 - la));
}
