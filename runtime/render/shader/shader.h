/*
 * shader.h - translating the game's compiled shaders (NVIDIA Maxwell machine code) to SPIR-V.
 *
 * Resource layout of every translated shader (descriptor set 0), from base = stage * 64 (each
 * stage has its own buffers and textures, so its own bindings):
 *   base + 0..17    constant buffer c[i] as a uniform buffer (c0 driver data, c1 the shader's own
 *                   constants, c3.. the game's uniform buffers: NVN binding n is c[n + 3])
 *   base + 32..63   combined image samplers, one per texture the shader uses (see ShaderInfo)
 * Vertex inputs are at location = NVN attribute index; varyings at location = generic slot.
 */
#ifndef SHADER_H
#define SHADER_H

#include <stdint.h>
#include <stddef.h>

enum { SHADER_VERTEX, SHADER_FRAGMENT, SHADER_GEOMETRY, SHADER_TESS_CONTROL, SHADER_TESS_EVAL, SHADER_COMPUTE };

#define SHADER_STAGE_BINDINGS  64
#define SHADER_CBUF_BINDING    0
#define SHADER_TEXTURE_BINDING 32
#define SHADER_MAX_TEXTURES    32

/* how a texture is used, so the right image view can be bound */
enum { TEXDIM_1D, TEXDIM_2D, TEXDIM_3D, TEXDIM_CUBE, TEXDIM_BUFFER };

typedef struct {
    uint32_t handle_offset;          /* byte offset of its handle in the texture constant buffer */
    uint8_t  dim, array, shadow, ms;
} ShaderTexture;

typedef struct {
    int stage;
    uint32_t *spirv;                 /* malloc'd; free with shader_free() */
    uint32_t words;
    uint32_t cbuf_mask;              /* bit i: uses constant buffer c[i] */
    uint32_t cbuf_size[18];          /* bytes of each constant buffer it may read */
    uint32_t inputs, outputs;        /* generic attribute / varying locations used (bit i) */
    uint32_t color_outputs;          /* fragment: render targets written (bit i) */
    int writes_depth, discards;
    uint32_t local_size[3];          /* compute: threads per workgroup */
    ShaderTexture textures[SHADER_MAX_TEXTURES];
    uint32_t ntextures;              /* binding = base + SHADER_TEXTURE_BINDING + index */
    char error[256];                 /* why translation failed */
} ShaderInfo;

/* Translate one shader. code: the compiled shader as NVN stores it (graphics stages start
 * with a 0x80-byte header; compute code starts at once), `size` bytes are readable.
 * constants: the shader's own constant data (what the driver binds as c1; its offset and size
 * are in the control section, see shader_constants()). control: the NVN control section
 * (compute: the workgroup size is there), or NULL. extra_outputs (vertex): also declare these
 * generic outputs (bit i: location i) even if the shader never writes them, because the
 * fragment shader reads them and Vulkan requires a matching output. Returns 0 on success; on
 * failure info->error says why. */
enum { SHADER_GL_DEPTH = 1,         /* vertex: clip-space z is -w..w (OpenGL/NVN default) -> 0..w */
       SHADER_DEBUG_CONST = 2,       /* fragment (debugging): colour outputs 0.5, no KIL; with
                                      * (n + 1) << 8 in options: input attribute n instead of 0.5 */
       SHADER_DEBUG_CAPTURE = 4,     /* vertex (debugging): also store position and attribute 1 of
                                      * each vertex at shader_capture_address (slot instance * 4 +
                                      * vertex % 4, 8 floats each, 256 slots) */
       SHADER_FULL_HALF = 16,        /* packed-half arithmetic in full 32-bit precision (not RelaxedPrecision) */
       SHADER_GUARD_REPORT = 32,     /* (debugging) a shader that ends at the dispatch guard (translate.c)
                                      * writes id and why in its slot at shader_guard_address */
       SHADER_DEBUG_STUCK = 64,      /* fragment (debugging): the pixels whose x and y are multiples of
                                      * shader_stuck_every never leave the dispatch loop (until the guard ends
                                      * them): what a loop that never ends costs the GPU */
       SHADER_KIL_DEMOTE = 128,
       SHADER_DEBUG_NO_QUAD = 256 }; /* (debugging, GPU hang test) SHFL gives each lane its own value: no
                                      * exchange between lanes (pictures may be off where the game uses it) */    /* fragment: KIL demotes the pixel to a helper (it carries on, its
                                      * results dropped) instead of ending it: quad operations after a KIL
                                      * then never wait on a pixel that is gone */
extern uint64_t shader_capture_address;   /* a GPU address (buffer device address) */
/* SHADER_GUARD_REPORT: SHADER_GUARD_SLOTS slots of SHADER_GUARD_WORDS words (id low, id high, reason, taken); a
 * shader's slot is shader_guard_slot(id); reason SHADER_GUARD_NO_BLOCK, _JUMPS (ended) or _LONG (4096 jumps: warned).
 * At _JUMPS the first invocation (taken 0 -> 1) also writes: 4 its next block, 5-6 its pixel, 7 P0-P6 as bits,
 * 8-15 the registers shader_guard_regs names (LAA_GUARD_REGS=8,23,...; -1: none) */
#define SHADER_GUARD_SLOTS 1024
#define SHADER_GUARD_WORDS 16
#define SHADER_GUARD_REGS 8
extern int shader_guard_regs[SHADER_GUARD_REGS];
/* the translator remembers each reporting shader's block start offsets (it owns `at`); the runtime asks */
void shader_guard_remember_blocks(uint64_t id, uint32_t *at, uint32_t nblocks);
int64_t shader_guard_block_offset(uint64_t id, uint32_t block);
enum { SHADER_GUARD_NO_BLOCK = 1, SHADER_GUARD_JUMPS = 2, SHADER_GUARD_LONG = 3 };
static inline uint32_t shader_guard_slot(uint64_t id) { return (uint32_t)((id >> 4) * 0x9E3779B1u >> 22) & (SHADER_GUARD_SLOTS - 1); }
extern uint64_t shader_guard_address;
extern uint32_t shader_stuck_every;      /* (SHADER_DEBUG_STUCK; a power of two) */
extern uint32_t shader_dispatch_limit;   /* jumps one invocation may make before the shader is ended (translate.c; LAA_DISPATCH_LIMIT) */
extern uint32_t shader_fs_dispatch_limit; /* fragment shaders' own limit, 0: the same (LAA_FS_DISPATCH_LIMIT) */
extern uint32_t shader_dispatch_warn;    /* with SHADER_GUARD_REPORT: jumps after which a shader reports (LAA_DISPATCH_WARN) */
/* id: the shader's address (what SHADER_GUARD_REPORT writes) */
int shader_translate(const uint8_t *code, size_t size, int stage, const uint8_t *constants,
                     uint32_t constants_size, const uint8_t *control, int options, uint32_t extra_outputs,
                     uint64_t id, ShaderInfo *info);
/* Where a shader's constants are, from its NVN control section: offset from the start of the
 * code and size in bytes. */
void shader_constants(const uint8_t *control, uint32_t *offset, uint32_t *size);
void shader_free(ShaderInfo *info);

#endif
