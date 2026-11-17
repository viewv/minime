// e2e Vulkan 后端共用的绑定。W 是整个模型文件（按 uint 访问），ACT 是激活值和 logits，
// KV 是 KV 池 [槽位][层][K/V][768]，INP 是这一批的整数输入：
//   tok[0..63] pos[64..127] slot[128..191] ctx_off[192..255] ctx_len[256..319] ctx[320..]
layout(std430, binding = 0) readonly buffer Wb { uint w[]; };
layout(std430, binding = 0) readonly buffer Wb4 { uvec4 w4[]; };      // 同一缓冲按 16 字节读
layout(std430, binding = 1) buffer Act { float act[]; };
layout(std430, binding = 1) readonly buffer Act4 { vec4 act4[]; };   // 同一缓冲按 vec4 读（只读的输入用）
layout(std430, binding = 2) buffer Kv { float kv[]; };
layout(std430, binding = 2) readonly buffer Kv4 { vec4 kv4[]; };     // 同一缓冲按 vec4 读（注意力用）
layout(std430, binding = 3) readonly buffer Inp { int inp[]; };
layout(std430, binding = 4) writeonly buffer Lg { float lg[]; };   // logits（主机可缓存内存，CPU 读）

const uint H = 768u, NL = 12u;
const uint IN_TOK = 0u, IN_POS = 64u, IN_SLOT = 128u, IN_OFF = 192u, IN_LEN = 256u, IN_CTX = 320u;

// W 里第 i 个 fp16（按 fp16 计的下标）
float wHalf(uint i) {
    vec2 v = unpackHalf2x16(w[i >> 1]);
    return (i & 1u) == 0u ? v.x : v.y;
}
float wF32(uint i) { return uintBitsToFloat(w[i]); }
