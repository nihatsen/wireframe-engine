#pragma once

// ═══════════════════════════════════════════════════════════════
//  GLSL SHADER SOURCE CODE
//  All shaders optimized for Adreno 610 / mobile tile-based GPUs
// ═══════════════════════════════════════════════════════════════

namespace shaders {

// ─── Vertex Shader (shared by all modes) ─────────────────────
static constexpr const char* VERTEX_SHADER = R"glsl(
    precision highp float;
    attribute vec2 aPosition;
    varying vec2 vTexCoord;

    void main() {
        gl_Position = vec4(aPosition, 0.0, 1.0);
        // Map [-1,1] to [0,1] for texture coordinates
        vTexCoord = aPosition * 0.5 + 0.5;
    }
)glsl";

// ─── Vertex Shader ES 3.0+ ──────────────────────────────────
static constexpr const char* VERTEX_SHADER_300 = R"glsl(#version 300 es
    precision highp float;
    in vec2 aPosition;
    out vec2 vTexCoord;

    void main() {
        gl_Position = vec4(aPosition, 0.0, 1.0);
        vTexCoord = aPosition * 0.5 + 0.5;
    }
)glsl";

// ─── Roberts Cross Edge Detection (FASTEST) ─────────────────
// 4 texture lookups, minimal ALU
static constexpr const char* FRAG_ROBERTS = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uThreshold;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    void main() {
        // Roberts Cross: 4 samples
        vec3 c  = texture2D(uTexture, vTexCoord).rgb;
        vec3 r  = texture2D(uTexture, vTexCoord + vec2(uTexelSize.x, 0.0)).rgb;
        vec3 b  = texture2D(uTexture, vTexCoord + vec2(0.0, -uTexelSize.y)).rgb;
        vec3 br = texture2D(uTexture, vTexCoord + uTexelSize * vec2(1.0, -1.0)).rgb;

        // Luminance using BT.709 coefficients
        const vec3 W = vec3(0.2126, 0.7152, 0.0722);
        float lc  = dot(c,  W);
        float lr  = dot(r,  W);
        float lb  = dot(b,  W);
        float lbr = dot(br, W);

        // Roberts Cross gradient
        float gx = lc - lbr;
        float gy = lr - lb;
        float edge = sqrt(gx * gx + gy * gy) * uIntensity;

        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);
        vec3 color = mix(uBgColor, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Sobel Edge Detection (BALANCED) ────────────────────────
// 8 texture lookups, good quality
static constexpr const char* FRAG_SOBEL = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uThreshold;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        // Sample 3x3 neighborhood (skip center - not needed for Sobel)
        float tl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        float t  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        float tr = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        float l  = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        float r  = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        float bl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        float b  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        float br = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        // Sobel kernels
        float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
        float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;

        float edge = sqrt(gx * gx + gy * gy) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec3 color = mix(uBgColor, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Scharr Edge Detection (HIGH QUALITY) ───────────────────
// 8 texture lookups, better rotational invariance than Sobel
static constexpr const char* FRAG_SCHARR = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uThreshold;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        float tl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        float t  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        float tr = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        float l  = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        float r  = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        float bl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        float b  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        float br = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        // Scharr kernels: [-3,0,3; -10,0,10; -3,0,3] and transposed
        float gx = -3.0*tl - 10.0*l - 3.0*bl + 3.0*tr + 10.0*r + 3.0*br;
        float gy = -3.0*tl - 10.0*t - 3.0*tr + 3.0*bl + 10.0*b + 3.0*br;

        // Normalize (Scharr produces larger values)
        float edge = sqrt(gx * gx + gy * gy) * 0.03125 * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec3 color = mix(uBgColor, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Frei-Chen Edge Detection (HIGHEST QUALITY) ─────────────
// 9 samples, edge-specific basis decomposition
static constexpr const char* FRAG_FREICHEN = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uThreshold;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        float s[9];
        s[0] = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        s[1] = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        s[2] = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        s[3] = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        s[4] = luminance(texture2D(uTexture, vTexCoord).rgb);
        s[5] = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        s[6] = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        s[7] = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        s[8] = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        // Frei-Chen edge masks (first 4 are edge-detecting)
        float sqrt2 = 1.41421356;

        float g0 = (s[0] + sqrt2*s[1] + s[2]) - (s[6] + sqrt2*s[7] + s[8]);
        float g1 = (s[2] + sqrt2*s[5] + s[8]) - (s[0] + sqrt2*s[3] + s[6]);

        float edgeSq = g0*g0 + g1*g1;

        // Sum of all projections for normalization
        float allSq = 0.0;
        for (int i = 0; i < 9; i++) {
            allSq += s[i] * s[i];
        }
        allSq = max(allSq, 0.001);

        float edge = sqrt(edgeSq / allSq) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec3 color = mix(uBgColor, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Wireframe Overlay Mode ──────────────────────────────────
// Draws edges over the original image
static constexpr const char* FRAG_SOBEL_OVERLAY = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform float uBgOpacity;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        vec3 center = texture2D(uTexture, vTexCoord).rgb;

        float tl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        float t  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        float tr = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        float l  = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        float r  = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        float bl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        float b  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        float br = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
        float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;
        float edge = sqrt(gx * gx + gy * gy) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec3 bg = center * uBgOpacity;
        vec3 color = mix(bg, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Colored Wireframe Mode ──────────────────────────────────
// Edges retain their original color
static constexpr const char* FRAG_SOBEL_COLORED = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        vec3 center = texture2D(uTexture, vTexCoord).rgb;

        float tl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        float t  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        float tr = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        float l  = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        float r  = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        float bl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        float b  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        float br = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
        float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;
        float edge = sqrt(gx * gx + gy * gy) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        // Edge pixels get boosted original color, non-edge gets bg
        vec3 edgeColor = center * (1.0 + edge * 2.0);
        vec3 color = mix(uBgColor, edgeColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Game Wireframe Mode (Multi-Channel + Depth-Aware) ──────
// Optimized for 3D games: detects edges per RGB channel independently
// to preserve detail in colorful scenes (e.g. Brawl Stars, Clash Royale).
// Also applies a Laplacian pass for detecting flat-shaded polygon boundaries
// that luminance-based detection misses.
static constexpr const char* FRAG_GAME_WIREFRAME = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uThreshold;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uBgOpacity;
    uniform float uIntensity;

    void main() {
        vec3 center = texture2D(uTexture, vTexCoord).rgb;

        // Sample 3x3 neighborhood — full RGB (no luminance conversion)
        vec3 tl = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb;
        vec3 t  = texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb;
        vec3 tr = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb;
        vec3 ml = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb;
        vec3 mr = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb;
        vec3 bl = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb;
        vec3 b  = texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb;
        vec3 br = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb;

        // Sobel per-channel: detect edges in R, G, B independently
        vec3 gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
        vec3 gy = -tl - 2.0*t  - tr + bl + 2.0*b  + br;
        vec3 edgeRGB = sqrt(gx * gx + gy * gy);

        // Combine channels: max of per-channel edges catches color boundaries
        // that luminance-based detection misses (e.g. red vs green at same brightness)
        float edgeSobel = max(edgeRGB.r, max(edgeRGB.g, edgeRGB.b));

        // Laplacian for flat-shaded polygon boundaries and UI elements
        // Kernel: [0,1,0; 1,-4,1; 0,1,0]
        const vec3 W = vec3(0.2126, 0.7152, 0.0722);
        float lapCenter = dot(center, W);
        float lapSum = dot(t, W) + dot(b, W) + dot(ml, W) + dot(mr, W);
        float laplacian = abs(lapSum - 4.0 * lapCenter);

        // Blend Sobel + Laplacian (Laplacian catches thin polygon edges)
        float edge = (edgeSobel * 0.75 + laplacian * 0.5) * uIntensity;

        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        // Semi-transparent original underneath with bright wireframe on top
        vec3 dimmed = center * uBgOpacity;
        vec3 color = mix(dimmed, uLineColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Game Wireframe Colored Variant ─────────────────────────
// Edges retain their original game colors for a stylized look
static constexpr const char* FRAG_GAME_WIREFRAME_COLORED = R"glsl(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    uniform vec2 uTexelSize;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uBgColor;
    uniform float uBgOpacity;
    uniform float uIntensity;

    void main() {
        vec3 center = texture2D(uTexture, vTexCoord).rgb;

        vec3 tl = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb;
        vec3 t  = texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb;
        vec3 tr = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb;
        vec3 ml = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb;
        vec3 mr = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb;
        vec3 bl = texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb;
        vec3 b  = texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb;
        vec3 br = texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb;

        vec3 gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
        vec3 gy = -tl - 2.0*t  - tr + bl + 2.0*b  + br;
        vec3 edgeRGB = sqrt(gx * gx + gy * gy);
        float edge = max(edgeRGB.r, max(edgeRGB.g, edgeRGB.b));

        const vec3 W = vec3(0.2126, 0.7152, 0.0722);
        float lapCenter = dot(center, W);
        float lapSum = dot(t, W) + dot(b, W) + dot(ml, W) + dot(mr, W);
        float laplacian = abs(lapSum - 4.0 * lapCenter);

        edge = (edge * 0.75 + laplacian * 0.5) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        // Boost original color at edges, dim non-edges
        vec3 edgeColor = center * (1.0 + edge * 3.0);
        vec3 dimmed = mix(uBgColor, center * uBgOpacity, uBgOpacity);
        vec3 color = mix(dimmed, edgeColor, alpha);
        float srcAlpha = texture2D(uTexture, vTexCoord).a;
        gl_FragColor = vec4(color, srcAlpha);
    }
)glsl";

// ─── Compute Shader (ES 3.1+) ───────────────────────────────
static constexpr const char* COMPUTE_SOBEL = R"glsl(#version 310 es
    precision mediump float;
    precision mediump image2D;

    layout(local_size_x = 16, local_size_y = 16) in;

    layout(binding = 0) uniform sampler2D uInput;
    layout(binding = 0, rgba8) writeonly uniform image2D uOutput;

    uniform vec2 uTexelSize;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    // Shared memory for tile + border (16+2 x 16+2 = 18x18)
    shared float tile[18][18];

    void main() {
        ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
        ivec2 lid = ivec2(gl_LocalInvocationID.xy);
        ivec2 groupBase = ivec2(gl_WorkGroupID.xy) * ivec2(16);

        // Load tile + 1-pixel border into shared memory
        // Each thread loads its own pixel
        vec2 uv = (vec2(gid) + 0.5) * uTexelSize;
        tile[lid.y + 1][lid.x + 1] = luminance(texture(uInput, uv).rgb);

        // Border threads load extra pixels
        if (lid.x == 0) {
            vec2 uvL = (vec2(gid) + vec2(-0.5, 0.5)) * uTexelSize;
            tile[lid.y + 1][0] = luminance(texture(uInput, uvL).rgb);
        }
        if (lid.x == 15) {
            vec2 uvR = (vec2(gid) + vec2(1.5, 0.5)) * uTexelSize;
            tile[lid.y + 1][17] = luminance(texture(uInput, uvR).rgb);
        }
        if (lid.y == 0) {
            vec2 uvT = (vec2(gid) + vec2(0.5, -0.5)) * uTexelSize;
            tile[0][lid.x + 1] = luminance(texture(uInput, uvT).rgb);
        }
        if (lid.y == 15) {
            vec2 uvB = (vec2(gid) + vec2(0.5, 1.5)) * uTexelSize;
            tile[17][lid.x + 1] = luminance(texture(uInput, uvB).rgb);
        }
        // Corners
        if (lid.x == 0 && lid.y == 0)
            tile[0][0] = luminance(texture(uInput, (vec2(gid) + vec2(-0.5, -0.5)) * uTexelSize).rgb);
        if (lid.x == 15 && lid.y == 0)
            tile[0][17] = luminance(texture(uInput, (vec2(gid) + vec2(1.5, -0.5)) * uTexelSize).rgb);
        if (lid.x == 0 && lid.y == 15)
            tile[17][0] = luminance(texture(uInput, (vec2(gid) + vec2(-0.5, 1.5)) * uTexelSize).rgb);
        if (lid.x == 15 && lid.y == 15)
            tile[17][17] = luminance(texture(uInput, (vec2(gid) + vec2(1.5, 1.5)) * uTexelSize).rgb);

        memoryBarrierShared();
        barrier();

        // Read from shared memory (zero-cost vs texture reads)
        int tx = lid.x + 1;
        int ty = lid.y + 1;

        float tl = tile[ty-1][tx-1];
        float t  = tile[ty-1][tx  ];
        float tr = tile[ty-1][tx+1];
        float l  = tile[ty  ][tx-1];
        float r  = tile[ty  ][tx+1];
        float bl = tile[ty+1][tx-1];
        float b  = tile[ty+1][tx  ];
        float br = tile[ty+1][tx+1];

        float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
        float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;

        float edge = sqrt(gx * gx + gy * gy) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec4 color = vec4(mix(uBgColor, uLineColor, alpha), 1.0);
        imageStore(uOutput, gid, color);
    }
)glsl";

// ─── Video Sobel (GL_TEXTURE_EXTERNAL_OES input) ────────────
// For video buffers (YUV) that require samplerExternalOES.
// Reads from external texture, outputs wireframe to a regular FBO.
static constexpr const char* FRAG_VIDEO_SOBEL = R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 vTexCoord;
    uniform samplerExternalOES uTexture;
    uniform vec2 uTexelSize;
    uniform float uSmoothMin;
    uniform float uSmoothMax;
    uniform vec3 uLineColor;
    uniform vec3 uBgColor;
    uniform float uIntensity;

    float luminance(vec3 c) {
        return dot(c, vec3(0.2126, 0.7152, 0.0722));
    }

    void main() {
        float tl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,  uTexelSize.y)).rgb);
        float t  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0,  uTexelSize.y)).rgb);
        float tr = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,  uTexelSize.y)).rgb);
        float l  = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x,           0.0)).rgb);
        float r  = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x,           0.0)).rgb);
        float bl = luminance(texture2D(uTexture, vTexCoord + vec2(-uTexelSize.x, -uTexelSize.y)).rgb);
        float b  = luminance(texture2D(uTexture, vTexCoord + vec2(          0.0, -uTexelSize.y)).rgb);
        float br = luminance(texture2D(uTexture, vTexCoord + vec2( uTexelSize.x, -uTexelSize.y)).rgb);

        float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
        float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;
        float edge = sqrt(gx * gx + gy * gy) * uIntensity;
        float alpha = smoothstep(uSmoothMin, uSmoothMax, edge);

        vec3 color = mix(uBgColor, uLineColor, alpha);
        gl_FragColor = vec4(color, 1.0);
    }
)glsl";

// ─── Passthrough (copy external texture to regular texture) ─
static constexpr const char* FRAG_PASSTHROUGH_EXT = R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 vTexCoord;
    uniform samplerExternalOES uTexture;

    void main() {
        gl_FragColor = texture2D(uTexture, vTexCoord);
    }
)glsl";

} // namespace shaders
