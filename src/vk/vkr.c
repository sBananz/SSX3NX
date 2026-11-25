#include "vkr.h"
#include "../log.h"

#include "vkr_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__SWITCH__)
/* libnx's default window (declared here: <switch.h> clashes with Vulkan's) */
struct NWindow;
extern struct NWindow *nwindowGetDefault(void);
extern uint32_t nwindowSetDimensions(struct NWindow *nw, uint32_t width, uint32_t height);
extern uint32_t nwindowGetDimensions(struct NWindow *nw, uint32_t *width, uint32_t *height);
#endif
#if defined(__SWITCH__)
#define VK_USE_PLATFORM_VI_NN 1          /* VK_NN_vi_surface: the swapchain on the NWindow */
#endif
#include <vulkan/vulkan.h>

#include "xbox_vert_spv.h"
#include "xbox_frag_spv.h"
#include "xbox_frag_wdepth_spv.h"
#include "xbox_frag_tags_spv.h"
#include "xbox_frag_wdepth_tags_spv.h"
#include "xbox_vsh_vert_spv.h"
#include "crt_vert_spv.h"
#include "crt_frag_spv.h"
#include "scene_frag_spv.h"
#include "scene_frag_ms_spv.h"
#include "present_frag_spv.h"
#include "speedlines_frag_spv.h"
#ifdef _WIN32
#include <windows.h>
#define strtok_r strtok_s
#else
#include <time.h>
#endif

static double vkr_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

#define VKR_BUF_BYTES (64u << 20)       /* vertex+index staging per frame (32 MB overflowed on
                                          * the first frame of a new area: 15.6 MB game
                                          * textures + 7.3 MB HD + 9.9 MB draws, Snow Jam) */
/* HD replacements (BC7, vkr_tex_create_bc7) may fill the frame's staging
 * only up to this reserve: riding into a new area loads dozens at once,
 * and the draws that no longer fit were dropped for that frame ("upload
 * buffer full", a flicker of missing pieces). A replacement that does not
 * fit waits for the next frame; the original texture shows until then. */
#define VKR_DRAW_RESERVE (12u << 20)
#define VKR_FRAMES 2                    /* frames in flight */
#define VKR_SPEC_WORDS 39               /* xbox.frag SPEC + S0..S37 */
#define VKR_PIPE_INDEX 8192

struct VkrTex {
    uint64_t key;
    uint32_t w, h;
    uint64_t bytes;                 /* device memory it counts for */
    uint32_t flags;                 /* VKR_TEX_* */
    uint32_t last_use;              /* flip serial of the last lookup/update */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};

/* What one frame in flight owns: recorded while the previous one renders. */
typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf, rb;
    VkDeviceMemory buf_mem, rb_mem;
    uint8_t *buf_map, *rb_map;
    int rb_noncoherent;             /* cached but not coherent: invalidate before reading */
    VkBuffer rbh;                   /* the full-resolution image (scale != 1) */
    VkDeviceMemory rbh_mem;
    uint8_t *rbh_map;
    int rbh_noncoherent;
    int busy;                       /* submitted, read-back not released yet */
    /* RECOMP_VKR_GPUPROF: a timestamp after every draw of the frame */
    VkQueryPool qp;
    uint32_t nq;
    int qreset;
    uint32_t npass;                 /* render passes begun in the frame */
    struct VkrProfDraw *meta;
} VkrFrame;

/* What a profiled draw was (RECOMP_VKR_GPUPROF). */
typedef struct VkrProfDraw {
    uint32_t nidx, topo, vsh, blend, src, dst, eq, mask, stages, final0, alpha;
    uint32_t tw[2], th[2], scw, sch;
    float psize;
} VkrProfDraw;
#define VKR_PROF_MAX 8192

struct VkrContext {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkrFrame fr[VKR_FRAMES];
    int cur;                        /* the frame being recorded */
    pthread_mutex_t lock;           /* fr[].busy */
    pthread_cond_t freed;
    uint32_t tex_gen;               /* bumped when every texture is freed */
    volatile int purge_req;         /* vkr_tex_purge: free them all at the next flip */
    uint32_t up_tex, up_hd, up_draw;  /* this frame's staging bytes by kind (overflow report) */
    int up_full_said;
    /* aliases of fr[cur] */
    VkCommandBuffer cmd;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties mem;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs, fs_wdepth, vs_vsh;  /* fs_wdepth: W-buffered draws */
    VkShaderModule fs_tags[2];                 /* fs, fs_wdepth with rider tags (TAGS) */
    int dual_src;                              /* dualSrcBlend enabled */
    int tags;                                  /* rider tags on: main-target draws write theirs to alpha */

    /* Pipelines by key: blend/topology/... (8 words) + the fragment
     * shader's specialization (VKR_SPEC_WORDS, see spec_words). A
     * specialized one is compiled by the worker (pipe_worker) while draws
     * use the interpreting one; pipe stays VK_NULL_HANDLE until then. */
    struct PipeEntry {
        uint32_t key[8 + VKR_SPEC_WORDS];
        VkPipeline pipe;                /* render thread */
        VkPipeline built;               /* the worker's result, under pj_lock */
        struct PipeEntry *next_job;
        int pending;                    /* queued or compiling (render thread) */
    } **pipes;
    int npipes, cappipes;
    int pipe_index[VKR_PIPE_INDEX];     /* open addressing: pipes[] index + 1, 0 = empty */
    int spec, spec_sync;                /* RECOMP_VKR_SPEC (default 1), RECOMP_VKR_SPEC_SYNC */
    pthread_mutex_t pj_lock;
    pthread_cond_t pj_cond;
    struct PipeEntry *pj_head, *pj_tail;
    int pj_started, pj_quit, pj_done;
    /* The driver's pipeline cache (Switch: on the SD card, see pcache_load);
     * VK_NULL_HANDLE elsewhere. */
    VkPipelineCache pcache;
    unsigned pcache_new, pcache_saved;
    struct SamplerEntry { uint32_t key; VkSampler s; } samplers[16];
    int nsamplers;

    VkrTex *white;                  /* 1x1 fallback for unbound stages */
    VkrTex **texs;                  /* every texture, for lookup and teardown */
    uint32_t ntexs, captexs;
    uint64_t tex_bytes;
    int bc7;                        /* BC7 sampling: -1 not asked yet */
    float aniso;                    /* RECOMP_ANISO: max anisotropy, 0 = off */
    int tex_mips;                   /* RECOMP_TEX_MIPS: -1 not read yet */
    /* Lookup by key: open addressing over indices into texs (-1 empty),
     * a power of two at least twice ntexs. The linear search it replaces
     * scanned every texture ever made, for every stage of every draw --
     * after ~6 minutes of play the cache held tens of thousands and the
     * race fell from 60 to ~23 FPS. */
    int32_t *hidx;
    uint32_t hcap;
    uint32_t flip_serial;

    /* Internal resolution (RECOMP_RES): the target is sw x sh = w x h times
     * scale; positions, the guest's copy and the clip rectangle stay in guest
     * pixels (w x h). */
    float scale, scale_x;           /* scale_x = scale x 4/3 in widescreen */
    float squeeze2d;                /* widescreen: 2D (w == 1) narrowed to 4:3 */
    float squeeze_all;              /* 1: pillarbox -- every vertex narrowed to 4:3 */
    uint32_t gsc[4];                /* the clip rectangle in guest pixels */
    uint32_t sw, sh;
    int wide_lines;
    float max_line;
    VkImage down;                   /* scale != 1: w x h copy for guest memory */
    VkDeviceMemory down_mem;
    VkImageView down_view;

    /* target */
    uint32_t w, h;
    float zmax;
    VkRect2D sc;                    /* current scissor (the NV2A clip rect) */
    VkImage color, depth;           /* what draws go to (multisampled with MSAA) */
    VkDeviceMemory color_mem, depth_mem;
    VkImageView color_view, depth_view;
    /* RECOMP_MSAA: samples per pixel of color/depth (1 = off); the frame is
     * resolved into `resolved` at the flip, and everything that reads the
     * frame (window, guest copy, CRT) reads that. */
    VkSampleCountFlagBits samples;
    /* Offscreen GPU targets (vkr_offscreen_use): the guest surface's address
     * as key; while one is current, the target fields above are its own and
     * the main target's are kept in `main`. */
    struct VkrOff {
        uint32_t key, w, h, sw, sh, valid, in_rt;
        uint32_t last_use;              /* flip_serial when last drawn or sampled */
        float scale;
        VkImage color, depth;
        VkDeviceMemory color_mem, depth_mem;
        VkImageView color_view, depth_view;
        struct VkrTex *tex;
    } off[8];
    int noff, cur_off;
    struct {
        VkImage color, depth; VkImageView color_view, depth_view;
        uint32_t w, h, sw, sh; float scale, scale_x, squeeze2d, squeeze_all, zmax;
        VkRect2D sc; uint32_t gsc[4]; int color_valid; VkSampleCountFlagBits samples;
    } main;
    VkImage resolved;
    VkDeviceMemory resolved_mem;
    VkImageView resolved_view;
    VkFormat depth_fmt;             /* D32_SFLOAT_S8_UINT (or D24S8; D32 = no stencil) */
    int has_stencil;
    VkImageView depth_sview;        /* depth only, for sampling (the scene pass) */
    int recording, in_pass;
    int color_valid;

    /* per-frame upload buffer (alias of fr[cur]) */
    VkBuffer buf;
    uint8_t *buf_map;
    uint32_t buf_used;
    uint32_t upload_serial;         /* bumped whenever buf_used restarts */

    /* vkr_draw_vsh: the constants and program last uploaded in this
     * recording, reused while unchanged. */
    uint32_t vsh_serial, vsh_const_off, vsh_prog_off, vsh_prog_hash, vsh_prog_n;
    /* What the draws of the current rendering pass last set (VkrBound):
     * a draw only records the commands whose state changed. Reset at every
     * vkCmdBeginRendering (begin_pass); RECOMP_VKR_NO_DEDUP=1 turns it off. */
    struct VkrBound {
        int valid, bufs;            /* bufs: vertex + index buffer bound at 0 */
        VkPipeline pipe;
        float pcv[8], sq[4];
        uint32_t pcf[5], pcu[4];
        int have_pcv, have_sq, have_pcf, have_pcu;
        VkDescriptorImageInfo di[4];
        uint32_t ub, ds_n;          /* uniform offset and descriptor count last pushed */
        int have_ds;
        int depth_test, depth_write;
        VkCompareOp depth_op;
        int have_depth;
        uint32_t stencil[8];
        int have_stencil;
        float blend[4];
        int have_blend;
    } bnd;
    /* The combiner uniforms last uploaded in this recording, and where:
     * an unchanged block is not uploaded again (nor the descriptor pushed). */
    uint32_t ubo_serial, ubo_off;
    VkrUniforms ubo_last;
    int dedup;                      /* -1 unread, 0 off, 1 on */
    /* RECOMP_VKR_GPUPROF=<ms> */
    double prof_thr, ts_period;
    unsigned prof_said;
    int prof_frame;                 /* RECOMP_VKR_GPUPROF_FRAME=1: frame start/end only */
    float vsh_consts[192][4];

    /* RECOMP_CRT=1: CRT post-process of the full-resolution image (crt.frag).
     * crt_in: the frame with mips, crt_out: what the window gets. */
    int crt;
    float crt_p[10];                /* curv scan mask vig ab glow bright mpitch flicker noise;
                                     * mpitch 0 = auto */
    VkDescriptorSetLayout crt_dsl;
    VkPipelineLayout crt_layout;
    VkShaderModule crt_vs, crt_fs;
    VkPipeline crt_pipe;
    VkSampler crt_samp;
    VkImage crt_in, crt_out;
    VkDeviceMemory crt_in_mem, crt_out_mem;
    VkImageView crt_in_view, crt_out_view;
    uint32_t crt_levels;

    /* The scene pass (scene.frag): RECOMP_MBLUR, on frames with a camera
     * (vkr_scene_set). sc_in: a copy of the frame for it to read. */
    int scene;
    float sc_p[9];                  /* mblur | near mindist edge cap speed s0 s1 reach */
    VkrScene cam;
    int cam_valid;
    VkDescriptorSetLayout sc_dsl;
    VkPipelineLayout sc_layout;
    VkShaderModule sc_fs;
    VkPipeline sc_pipe;
    int scene_done;                 /* ran this frame (before the HUD, vkr_scene_run) */
    VkSampler sc_near, sc_lin;
    VkImage sc_in;
    VkDeviceMemory sc_in_mem;
    VkImageView sc_in_view;

    /* Switch: frames straight to the screen (RECOMP_VK_PRESENT, default on).
     * A swapchain on the default NWindow (NVK's VK_NN_vi_surface) instead of
     * reading every frame back for the CPU presenter (fb_present_nx.c): no
     * 3.7 MB full-resolution copy to host memory, no CPU copy of it, no blit.
     * present.frag draws the frame into the swapchain image with the gamma
     * ramp and the FPS overlay. The window passes from the CPU presenter to
     * the swapchain at the first flip (xbox_FramebufferHandOver); a failure
     * hands it back and the read-back path carries on. */
    int present;                    /* wanted, and the surface exists */
    volatile int pr_on;             /* the swapchain is live */
    int pr_stop;                    /* failing: back to the CPU presenter at the next flip */
    int pr_recreate;                /* out of date: rebuild the swapchain */
    unsigned pr_fails, pr_frames;
    VkSurfaceKHR surf;
    VkSwapchainKHR swap;
    VkFormat swap_fmt;
    VkExtent2D swap_ext;
    uint32_t swap_n;
    VkImage swap_img[8];
    VkImageView swap_view[8];
    VkSemaphore swap_done[8];       /* rendered: presentation may start (per image) */
    VkSemaphore acq[VKR_FRAMES];    /* image acquired (per frame slot) */
    VkDescriptorSetLayout pr_dsl;
    VkPipelineLayout pr_layout;
    VkPipeline pr_pipe;
    VkFormat pr_pipe_fmt;
    VkSampler pr_lin, pr_near;
    VkImage pr_ramp, pr_ov;         /* 256 x 1 gamma ramp, the overlay's corner */
    VkDeviceMemory pr_ramp_mem, pr_ov_mem;
    VkImageView pr_ramp_view, pr_ov_view;
    unsigned pr_ramp_serial, pr_ov_serial;
    int pr_ramp_valid, pr_ov_valid, pr_ov_on;
    int pr_hold;                    /* this flip is not shown: the screen keeps the last one */

    /* SSX3NX_BOOST_LINES: speed lines at the screen edges while boosting
     * (speedlines.frag, vkr_speedlines), before the HUD. */
    VkPipelineLayout sl_layout;
    VkPipeline sl_pipe;
    VkDescriptorSetLayout sl_dsl;   /* binding 0: the frame copy (sc_in), for the edge blur */
    VkSampler sl_lin;
    int sl_tried, sl_want, sl_noblur;
    int sl_half_ok;                 /* BGRA8 blits with a linear filter (SSX3NX_BOOST_BLUR_HALF) */
    VkImage sl_half;                /* the frame at half size, for the edge blur */
    VkDeviceMemory sl_half_mem;
    VkImageView sl_half_view;
};

/* ── Pipeline cache (Nintendo Switch) ───────────────────────────────
 *
 * NVK compiles every pipeline from NIR on the CPU, and the uber fragment
 * shader takes seconds on the console's A57 (2.7 s and 5.9 s for the first
 * two in SSX3NX.log), every boot -- Mesa's own disk cache does not run
 * there. The driver's VkPipelineCache is kept on the SD card instead:
 * SSX3NX_VK_PIPECACHE=<path>, default vk_pipeline.cache beside the .nro
 * (RECOMP_SYSTEM_DIR), "0" for none. Written after each of the first eight
 * new pipelines, then every fourth, and at exit (there may be no clean exit:
 * HOME, power). After OpenTrickyNX's d3d8_vk_core.c pcache_load/save. */
#if defined(__SWITCH__)
static char s_pcache_path[512];
static pthread_mutex_t s_pcache_lock = PTHREAD_MUTEX_INITIALIZER;

static void pcache_load(VkrContext *c)
{
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    const char *e = getenv("SSX3NX_VK_PIPECACHE");
    void *data = NULL;
    long n = 0;
    FILE *f;
    if (e && !strcmp(e, "0"))
        s_pcache_path[0] = 0;
    else if (e && *e)
        snprintf(s_pcache_path, sizeof s_pcache_path, "%s", e);
    else {
        const char *d = getenv("RECOMP_SYSTEM_DIR");
        snprintf(s_pcache_path, sizeof s_pcache_path, "%s/vk_pipeline.cache",
                 d && *d ? d : "sdmc:/switch/ssx3nx");
    }
    if (s_pcache_path[0] && (f = fopen(s_pcache_path, "rb"))) {
        if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            data = NULL;
        }
        fclose(f);
    }
    ci.initialDataSize = data ? (size_t)n : 0;
    ci.pInitialData = data;
    if (vkCreatePipelineCache(c->dev, &ci, NULL, &c->pcache) != VK_SUCCESS) {
        /* a cache from another driver build: start empty */
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(c->dev, &ci, NULL, &c->pcache) != VK_SUCCESS)
            c->pcache = VK_NULL_HANDLE;
    }
    fprintf(stderr, "[VKR] pipeline cache %s: %ld bytes loaded%s\n",
            s_pcache_path[0] ? s_pcache_path : "(off)", data ? n : 0L,
            c->pcache ? "" : " -- no cache");
    free(data);
}

static void pcache_save(VkrContext *c, int force)
{
    size_t n = 0;
    void *data;
    char tmp[600];
    FILE *f;
    unsigned now = __atomic_load_n(&c->pcache_new, __ATOMIC_RELAXED);
    if (!c->pcache || !s_pcache_path[0])
        return;
    if (!force && now == c->pcache_saved)
        return;
    if (!force && now > 8 && now < c->pcache_saved + 4)
        return;
    pthread_mutex_lock(&s_pcache_lock);
    c->pcache_saved = now;
    if (vkGetPipelineCacheData(c->dev, c->pcache, &n, NULL) == VK_SUCCESS && n && (data = malloc(n))) {
        if (vkGetPipelineCacheData(c->dev, c->pcache, &n, data) == VK_SUCCESS) {
            snprintf(tmp, sizeof tmp, "%s.part", s_pcache_path);
            if ((f = fopen(tmp, "wb"))) {
                int ok = fwrite(data, 1, n, f) == n;
                if (fclose(f) != 0) ok = 0;
                if (ok) { remove(s_pcache_path); ok = rename(tmp, s_pcache_path) == 0; }
                if (!ok) remove(tmp);
            }
        }
        free(data);
    }
    pthread_mutex_unlock(&s_pcache_lock);
}

static VkrContext *s_pcache_ctx;
static void pcache_at_exit(void)
{
    if (s_pcache_ctx)
        pcache_save(s_pcache_ctx, 1);
}
#else
#define pcache_load(c)    ((void)0)
#define pcache_save(c, f) ((void)0)
#endif


static VkImageAspectFlags ds_aspect(const VkrContext *c)
{
    return VK_IMAGE_ASPECT_DEPTH_BIT | (c->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static void use_frame(VkrContext *c, int i)
{
    c->cur = i;
    c->cmd = c->fr[i].cmd;
    c->fence = c->fr[i].fence;
    c->buf = c->fr[i].buf;
    c->buf_map = c->fr[i].buf_map;
}

/* Until no frame is in flight or waiting to be read. */
static void wait_all_free(VkrContext *c)
{
    int i;
    pthread_mutex_lock(&c->lock);
    for (i = 0; i < VKR_FRAMES; i++)
        while (c->fr[i].busy)
            pthread_cond_wait(&c->freed, &c->lock);
    pthread_mutex_unlock(&c->lock);
}

/* A Vulkan call that failed: ERROR, CRITICAL when the device is lost (the
 * driver reset or crashed; nothing renders after it). Once per call site
 * and result. */
static void vk_failed(const char *call, VkResult r, const char *file, int line)
{
    log_msg(r == VK_ERROR_DEVICE_LOST ? LOG_CRITICAL : LOG_ERROR, "VKR", "%s failed: %d%s (%s:%d)", call,
            (int)r, r == VK_ERROR_DEVICE_LOST ? ", device lost (GPU driver reset or crashed)"
                    : r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? ", out of GPU memory"
                    : r == VK_ERROR_OUT_OF_HOST_MEMORY ? ", out of memory" : "", file, line);
}

/* "upload buffer full": what filled this frame's staging, once a frame */
static void up_full(VkrContext *c, uint32_t want)
{
    if (c->up_full_said)
        return;
    c->up_full_said = 1;
    log_msg(LOG_ERROR, "VKR", "upload buffer full: %u KB game textures, %u KB HD textures, %u KB draws this "
            "frame; a request of %u KB did not fit", c->up_tex >> 10, c->up_hd >> 10, c->up_draw >> 10,
            want >> 10);
}

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    vk_failed(#x, r_, __FILE__, __LINE__); \
    return 0; } } while (0)

/* the same for the per-frame calls: the first failure, then every 600th */
#define CK_FRAME(x, fail) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    static unsigned n_; if (n_++ % 600 == 0) vk_failed(#x, r_, __FILE__, __LINE__); \
    fail; } } while (0)

static int find_mem(VkrContext *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < c->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* readback: the CPU reads it back. Host-visible coherent memory is write-
 * combined (or the GPU's own BAR) on discrete GPUs -- fine for uploads, but
 * reading 1.2 MB of it took ~10 ms per flip (240 ms/s in a race). A readback
 * buffer takes HOST_CACHED memory, coherent if there is such a type, else
 * non-coherent (*noncoherent = 1: invalidate before reading). */
static int make_buffer_ex(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer *b, VkDeviceMemory *m, void **map, int readback,
                          int *noncoherent);

static int make_buffer(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                       VkBuffer *b, VkDeviceMemory *m, void **map)
{
    return make_buffer_ex(c, size, usage, b, m, map, 0, NULL);
}

static int make_buffer_ex(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer *b, VkDeviceMemory *m, void **map, int readback,
                          int *noncoherent)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int t;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CK(vkCreateBuffer(c->dev, &bi, NULL, b));
    vkGetBufferMemoryRequirements(c->dev, *b, &mr);
    t = -1;
    if (noncoherent) *noncoherent = 0;
    if (readback) {
        t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) {
            t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                               | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (t >= 0 && noncoherent) *noncoherent = 1;
        }
    }
    if (t < 0)
        t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) { fprintf(stderr, "[VKR] no host-visible memory\n"); return 0; }
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    CK(vkAllocateMemory(c->dev, &ai, NULL, m));
    CK(vkBindBufferMemory(c->dev, *b, *m, 0));
    CK(vkMapMemory(c->dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image_ms(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                         VkImageView *view, uint32_t width, uint32_t height, uint32_t levels,
                         VkSampleCountFlagBits samples)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    int t;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = width; ii.extent.height = height; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1; ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CK(vkCreateImage(c->dev, &ii, NULL, img));
    vkGetImageMemoryRequirements(c->dev, *img, &mr);
    t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) return 0;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    CK(vkAllocateMemory(c->dev, &ai, NULL, mem));
    CK(vkBindImageMemory(c->dev, *img, *mem, 0));
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = levels; vi.subresourceRange.layerCount = 1;
    CK(vkCreateImageView(c->dev, &vi, NULL, view));
    return 1;
}

static int make_image_ex(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                         VkImageView *view, uint32_t width, uint32_t height, uint32_t levels)
{
    return make_image_ms(c, fmt, usage, aspect, img, mem, view, width, height, levels,
                         VK_SAMPLE_COUNT_1_BIT);
}

static int make_image(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                      VkImageView *view, uint32_t width, uint32_t height)
{
    return make_image_ex(c, fmt, usage, aspect, img, mem, view, width, height, 1);
}

/* Colour mip levels [base, base + n) of img. */
static void barrier_mips(VkrContext *c, VkImage img, uint32_t base, uint32_t n,
                         VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                         VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = base;
    b.subresourceRange.levelCount = n; b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c->cmd, &d);
}

static void barrier(VkrContext *c, VkImage img, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c->cmd, &d);
}

static VkShaderModule shader(VkrContext *c, const unsigned char *code, size_t n)
{
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule m = VK_NULL_HANDLE;
    si.codeSize = n; si.pCode = (const uint32_t *)code;
    if (vkCreateShaderModule(c->dev, &si, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

/* Rider tags: the alpha the game blends with is the second source's */
static VkBlendFactor src1_factor(VkBlendFactor f)
{
    return f == VK_BLEND_FACTOR_SRC_ALPHA ? VK_BLEND_FACTOR_SRC1_ALPHA
         : f == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
         : f;
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static int make_layout(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        /* the upload buffer, for the GPU vertex program (xbox_vsh.vert) */
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 64},
                                  {VK_SHADER_STAGE_FRAGMENT_BIT, 64, 20}};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    c->vs = shader(c, xbox_vert_spv, xbox_vert_spv_len);
    {
        /* RECOMP_VKR_SPEC=0: one fragment pipeline per blend state that
         * interprets the combiners (spec_words) */
        const char *e = getenv("RECOMP_VKR_SPEC");
        c->spec = !(e && *e == '0');
        /* RECOMP_VKR_SPEC_SYNC=1: compile them on the spot (the draw waits) */
        c->spec_sync = getenv("RECOMP_VKR_SPEC_SYNC") != NULL;
    }
    c->fs = shader(c, xbox_frag_spv, xbox_frag_spv_len);
    c->fs_wdepth = shader(c, xbox_frag_wdepth_spv, xbox_frag_wdepth_spv_len);
    c->vs_vsh = shader(c, xbox_vsh_vert_spv, xbox_vsh_vert_spv_len);
    if (!c->vs || !c->fs || !c->fs_wdepth || !c->vs_vsh) { fprintf(stderr, "[VKR] shader module failed\n"); return 0; }
    if (c->dual_src) {
        c->fs_tags[0] = shader(c, xbox_frag_tags_spv, xbox_frag_tags_spv_len);
        c->fs_tags[1] = shader(c, xbox_frag_wdepth_tags_spv, xbox_frag_wdepth_tags_spv_len);
        if (!c->fs_tags[0] || !c->fs_tags[1]) c->dual_src = 0;
    }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 3; di.pBindings = b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->dsl;
    li.pushConstantRangeCount = 2; li.pPushConstantRanges = pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->layout));
    return 1;
}

/* RECOMP_CRT=1 [RECOMP_CRT_PARAMS=name=value,...]: the CRT post-pass. */
static const char *const crt_names[10] = {
    "curv", "scan", "mask", "vig", "ab", "glow", "bright", "mpitch", "flicker", "noise"};

static void crt_config(VkrContext *c)
{
    static const float def[10] = {0.18f, 0.6f, 0.35f, 0.35f, 0.8f, 0.4f, 1.1f, 0.0f, 0.0f, 0.25f};
    const char *e = getenv("RECOMP_CRT");
    char buf[512], *tok, *save = NULL;
    int i;
    c->crt = e && *e && *e != '0';
    memcpy(c->crt_p, def, sizeof def);
    e = getenv("RECOMP_CRT_PARAMS");
    if (!c->crt || !e)
        return;
    snprintf(buf, sizeof buf, "%s", e);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = 0; i < 10; i++)
            if (!strcmp(tok, crt_names[i]))
                c->crt_p[i] = (float)atof(eq + 1);
    }
}

static int make_crt(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 72};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    c->crt_vs = shader(c, crt_vert_spv, crt_vert_spv_len);
    c->crt_fs = shader(c, crt_frag_spv, crt_frag_spv_len);
    if (!c->crt_vs || !c->crt_fs) { fprintf(stderr, "[VKR] CRT shader module failed\n"); return 0; }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 1; di.pBindings = &b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->crt_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->crt_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->crt_layout));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->crt_samp));

    stg[0].module = c->crt_vs; stg[1].module = c->crt_fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->crt_layout;
    CK(vkCreateGraphicsPipelines(c->dev, c->pcache, 1, &pi, NULL, &c->crt_pipe));
    fprintf(stderr, "[VKR] CRT on: curv %.2f scan %.2f mask %.2f vig %.2f ab %.2f glow %.2f "
            "bright %.2f mpitch %.1f (0 = auto) flicker %.3f noise %.2f\n",
            c->crt_p[0], c->crt_p[1], c->crt_p[2], c->crt_p[3], c->crt_p[4],
            c->crt_p[5], c->crt_p[6], c->crt_p[7], c->crt_p[8], c->crt_p[9]);
    return 1;
}

/* The scene pass's knobs: RECOMP_MBLUR=<strength, 1 = the full
 * frame-to-frame motion, 0 = off>; RECOMP_SCENE_PARAMS=name=value,...: near
 * (motion blur starts, world units, 0) mindist (nearer = HUD models, 120)
 * edge (extra blur towards the screen edges: x (1 + edge) in the corners, 1)
 * cap (longest blur, pixels at 1080 lines, 160) speed (radial speed blur at
 * full speed: this fraction of the distance from the focus of expansion,
 * 0.05) s0 s1 (camera speed where it starts / is full, world units per
 * frame: 15 / 45, ~30 / ~100 km/h at 60 Hz) reach (the speed blur is full up
 * to this distance and falls off as reach / distance beyond it, to 15% on
 * the backdrop: 2500). World units are SSX 3's (cm). */
static void scene_config(VkrContext *c)
{
    static const char *const names[8] = {"near", "mindist", "edge", "cap", "speed", "s0", "s1", "reach"};
    const char *e = getenv("RECOMP_MBLUR");
    char buf[512], *tok, *save = NULL;
    int i;
    c->sc_p[0] = e && *e ? (float)atof(e) : 0.0f;
    c->sc_p[1] = 0.0f; c->sc_p[2] = 120.0f; c->sc_p[3] = 1.0f; c->sc_p[4] = 160.0f;
    c->sc_p[5] = 0.05f; c->sc_p[6] = 15.0f; c->sc_p[7] = 45.0f; c->sc_p[8] = 2500.0f;
    e = getenv("RECOMP_SCENE_PARAMS");
    if (e) {
        snprintf(buf, sizeof buf, "%s", e);
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            for (i = 0; i < 8; i++)
                if (!strcmp(tok, names[i]))
                    c->sc_p[1 + i] = (float)atof(eq + 1);
        }
    }
    c->scene = c->sc_p[0] > 0.0f;
}

/* scene.frag as a fullscreen triangle (crt.vert) into the colour target. */
static int make_scene(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs = shader(c, crt_vert_spv, crt_vert_spv_len);

    c->sc_fs = c->samples != VK_SAMPLE_COUNT_1_BIT
              ? shader(c, scene_frag_ms_spv, scene_frag_ms_spv_len)   /* MSAA depth */
              : shader(c, scene_frag_spv, scene_frag_spv_len);
    if (!vs || !c->sc_fs) { fprintf(stderr, "[VKR] scene shader modules failed\n"); return 0; }
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->sc_near));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->sc_lin));
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 3; di.pBindings = b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->sc_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->sc_dsl;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->sc_layout));
    stg[0].module = vs; stg[1].module = c->sc_fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = c->samples;   /* draws into the (MSAA) colour target */
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->sc_layout;
    CK(vkCreateGraphicsPipelines(c->dev, c->pcache, 1, &pi, NULL, &c->sc_pipe));
    vkDestroyShaderModule(c->dev, vs, NULL);
    fprintf(stderr, "[VKR] scene pass on: mblur %.2f (near %.0f, mindist %.0f, edge %.2f, cap %.0f,"
            " speed %.3f from %.0f to %.0f, reach %.0f)\n", c->sc_p[0], c->sc_p[1], c->sc_p[2], c->sc_p[3],
            c->sc_p[4], c->sc_p[5], c->sc_p[6], c->sc_p[7], c->sc_p[8]);
    return 1;
}

void vkr_scene_set(VkrContext *c, const VkrScene *s)
{
    c->cam = *s;
    c->cam_valid = 1;
}

/* The fragment shader's specialization constants (xbox.frag SPEC, S0..S37):
 * the combiner words that shape the program, so the compiler folds the
 * interpreter away. Words the shader does not read for this state are 0, so
 * they do not make extra pipelines. All 0 = the interpreter (RECOMP_VKR_SPEC=0). */
static void spec_words(const VkrContext *c, const VkrDrawState *st, uint32_t *w)
{
    const VkrUniforms *u = &st->u;
    uint32_t n = u->control & 255u, s;
    memset(w, 0, VKR_SPEC_WORDS * sizeof *w);
    if (!c->spec)
        return;
    w[0] = 1;
    w[38] = st->alpha_func;
    if (!u->rc_seen) {
        w[36] = u->stage_program & 31u;
        return;
    }
    if (n > 8) n = 8;
    for (s = 0; s < n; s++) {
        w[1 + s] = u->color_icw[s];  w[9 + s] = u->alpha_icw[s];
        w[17 + s] = u->color_ocw[s]; w[25 + s] = u->alpha_ocw[s];
    }
    w[33] = u->final0; w[34] = u->final1;
    w[35] = u->control; w[36] = u->stage_program; w[37] = u->rc_seen;
}

static uint32_t pipe_hash(const uint32_t *key)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < 8 + VKR_SPEC_WORDS; i++)
        h = (h ^ key[i]) * 16777619u;
    return h;
}

/* The pipeline for a key (get_pipeline). */
static VkPipeline create_pipeline(VkrContext *c, const uint32_t *key)
{
    VkSpecializationMapEntry sme[VKR_SPEC_WORDS];
    VkSpecializationInfo spi;
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkVertexInputBindingDescription bind = {0, sizeof(VkrVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription at[8];
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipeline pipe;
    int blend = (int)key[0], gpu_vsh = (int)key[5], w_buffer = (int)(key[7] & 1u), i;
    uint32_t topology = key[6], write_mask = key[4];

    stg[0].module = gpu_vsh ? c->vs_vsh : c->vs; stg[1].module = w_buffer ? c->fs_wdepth : c->fs;
    if (key[7] & 16u)
        stg[1].module = c->fs_tags[w_buffer];
    for (i = 0; i < VKR_SPEC_WORDS; i++) {
        sme[i].constantID = (uint32_t)i;
        sme[i].offset = (uint32_t)(i * 4);
        sme[i].size = 4;
    }
    spi.mapEntryCount = VKR_SPEC_WORDS; spi.pMapEntries = sme;
    spi.dataSize = VKR_SPEC_WORDS * sizeof key[0]; spi.pData = key + 8;
    stg[1].pSpecializationInfo = &spi;
    for (i = 0; i < 8; i++) {
        at[i].location = (uint32_t)i; at[i].binding = 0;
        at[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        at[i].offset = (uint32_t)i * 16;
    }
    if (!gpu_vsh) {             /* the GPU program pulls its inputs from binding 5 */
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = at;
    }
    ia.topology = topology == VKR_TOPO_LINES  ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : topology == VKR_TOPO_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = c->wide_lines ? (c->scale < c->max_line ? c->scale : c->max_line) : 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(key[7] >> 8 ? key[7] >> 8 : 1);  /* MSAA (get_pipeline) */
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    cba.colorWriteMask = write_mask & 0xF;
    if (key[7] & 16u) {
        /* rider tags: colour blends with the real alpha (second source),
         * alpha takes the tag; a draw that writes no colour leaves both alone */
        cba.blendEnable = VK_TRUE;
        if (blend) {
            cba.srcColorBlendFactor = src1_factor(blend_factor(key[1]));
            cba.dstColorBlendFactor = src1_factor(blend_factor(key[2]));
            cba.colorBlendOp = blend_op(key[3]);
        } else {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
        }
        cba.srcAlphaBlendFactor = (key[7] & 32u) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = (key[7] & 32u) ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        if (cba.colorWriteMask & 7u)
            cba.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    } else if (blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = blend_factor(key[1]);
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = blend_factor(key[2]);
        cba.colorBlendOp = cba.alphaBlendOp = blend_op(key[3]);
    }
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]); dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    ri.depthAttachmentFormat = c->depth_fmt;
    if (c->has_stencil)
        ri.stencilAttachmentFormat = c->depth_fmt;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->layout;
    {
        double t0 = vkr_now_ms();
        if (vkCreateGraphicsPipelines(c->dev, c->pcache, 1, &pi, NULL, &pipe) != VK_SUCCESS) {
            fprintf(stderr, "[VKR] pipeline creation failed\n");
            return VK_NULL_HANDLE;
        }
        fprintf(stderr, "[VKR] pipeline: %.1f ms (blend %u %u/%u eq %u mask %X vsh %u topo %u wdepth %u"
                " rc %u stages %u)\n", vkr_now_ms() - t0,
                key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                key[8], key[8 + 35] & 255u);
#if defined(__SWITCH__)
        __atomic_add_fetch(&c->pcache_new, 1, __ATOMIC_RELAXED);
        pcache_save(c, 0);
#endif
    }
    return pipe;
}

/* Compiles specialized pipelines in the background: one took ~90 ms on
 * Honeykrisp (once; Mesa's disk cache has it after that). */
static void *pipe_worker(void *arg)
{
    VkrContext *c = (VkrContext *)arg;
    /* Switch: any of cores 0-2, core 2 preferred, background priority -- on
     * core 0 alone it compiled only while the game thread slept (2.7 s for
     * one pipeline in SSX3NX.log). */
    vkr_thread_place(2, 0);
#if defined(__SWITCH__)
    {
        extern void xbox_nx_thread_name(const char *name);   /* SSX3NX_CPU_STATS */
        xbox_nx_thread_name("VKCOMPILE");
    }
#endif
    pthread_mutex_lock(&c->pj_lock);
    for (;;) {
        struct PipeEntry *e;
        VkPipeline p;
        while (!c->pj_head && !c->pj_quit)
            pthread_cond_wait(&c->pj_cond, &c->pj_lock);
        if (c->pj_quit)
            break;
        e = c->pj_head;
        c->pj_head = e->next_job;
        if (!c->pj_head)
            c->pj_tail = NULL;
        pthread_mutex_unlock(&c->pj_lock);
        p = create_pipeline(c, e->key);
        pthread_mutex_lock(&c->pj_lock);
        e->built = p;
        e->next_job = e;                /* done, even if it failed */
    }
    c->pj_done = 1;
    pthread_cond_broadcast(&c->pj_cond);
    pthread_mutex_unlock(&c->pj_lock);
    return NULL;
}

/* The entry for a key, made (and its pipeline created or queued) on a miss. */
static struct PipeEntry *pipe_entry(VkrContext *c, const uint32_t *key)
{
    struct PipeEntry *e;
    uint32_t slot;
    for (slot = pipe_hash(key) % VKR_PIPE_INDEX; c->pipe_index[slot]; slot = (slot + 1) % VKR_PIPE_INDEX)
        if (!memcmp(c->pipes[c->pipe_index[slot] - 1]->key, key, sizeof e->key))
            return c->pipes[c->pipe_index[slot] - 1];
    if (c->npipes >= VKR_PIPE_INDEX / 2) {
        fprintf(stderr, "[VKR] pipeline cache full\n");
        return NULL;
    }
    if (c->npipes == c->cappipes) {
        struct PipeEntry **np;
        int cap = c->cappipes ? c->cappipes * 2 : 256;
        if (!(np = (struct PipeEntry **)realloc(c->pipes, (size_t)cap * sizeof *np)))
            return NULL;
        c->pipes = np;
        c->cappipes = cap;
    }
    if (!(e = (struct PipeEntry *)calloc(1, sizeof *e)))
        return NULL;
    memcpy(e->key, key, sizeof e->key);
    if (!key[8] || c->spec_sync) {
        e->pipe = create_pipeline(c, key);
    } else {
        if (!c->pj_started) {
            pthread_t th;
            pthread_mutex_init(&c->pj_lock, NULL);
            pthread_cond_init(&c->pj_cond, NULL);
            if (pthread_create(&th, NULL, pipe_worker, c) != 0) {
                c->spec_sync = 1;
                e->pipe = create_pipeline(c, key);
                goto add;
            }
            pthread_detach(th);
            c->pj_started = 1;
        }
        pthread_mutex_lock(&c->pj_lock);
        if (c->pj_tail)
            c->pj_tail->next_job = e;
        else
            c->pj_head = e;
        c->pj_tail = e;
        e->pending = 1;
        pthread_cond_signal(&c->pj_cond);
        pthread_mutex_unlock(&c->pj_lock);
    }
add:
    c->pipes[c->npipes] = e;
    c->pipe_index[slot] = ++c->npipes;
    return e;
}

/* Pipeline for one blend/write-mask/combiner combination (depth state is
 * dynamic): the specialized one once it is compiled, until then (or if it
 * failed) the interpreting one. */
static VkPipeline get_pipeline(VkrContext *c, const VkrDrawState *st, int gpu_vsh)
{
    uint32_t key[8 + VKR_SPEC_WORDS];
    struct PipeEntry *e;
    VkPipeline p;

    key[0] = (uint32_t)st->blend_enable; key[1] = st->blend_src; key[2] = st->blend_dst;
    key[3] = st->blend_eq; key[4] = st->write_mask;
    if (!st->blend_enable) key[1] = key[2] = key[3] = 0;
    key[5] = (uint32_t)gpu_vsh;
    key[6] = st->topology;
    key[7] = (uint32_t)(st->w_buffer != 0) | (uint32_t)c->samples << 8;  /* + MSAA vs offscreen */
    if (c->tags) {
        /* tagged: the tag goes to alpha where the draw writes depth, the
         * alpha underneath stays where it does not */
        key[7] |= 16u;
        if (st->depth_test && st->depth_write)
            key[7] |= 32u;
    }
    spec_words(c, st, key + 8);
    if (!(e = pipe_entry(c, key)))
        return VK_NULL_HANDLE;
    if (e->pending) {
        pthread_mutex_lock(&c->pj_lock);
        if (e->next_job == e) {
            e->pipe = e->built;
            e->pending = 0;
        }
        pthread_mutex_unlock(&c->pj_lock);
    }
    if ((p = e->pipe) || !key[8])
        return p;
    memset(key + 8, 0, VKR_SPEC_WORDS * sizeof key[0]);
    e = pipe_entry(c, key);
    return e ? e->pipe : VK_NULL_HANDLE;
}

static VkSampler get_sampler(VkrContext *c, uint32_t au, uint32_t av, int linear)
{
    uint32_t key = (au & 7) | (av & 7) << 3 | (uint32_t)(linear != 0) << 6;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerAddressMode m[2];
    uint32_t a[2] = {au, av}, k;
    int i;

    for (i = 0; i < c->nsamplers; i++)
        if (c->samplers[i].key == key)
            return c->samplers[i].s;
    if (c->nsamplers == 16)
        return c->samplers[0].s;
    for (k = 0; k < 2; k++)
        switch (a[k]) {
        case 2:  m[k] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case 3: case 5: m[k] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 4:  m[k] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
        default: m[k] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        }
    si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    /* Mip levels where a texture has them (the HD replacements, vkr_hdtex.c);
     * the game's own textures are one level, so this changes nothing there. */
    si.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (linear && c->aniso > 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = c->aniso;
    }
    si.addressModeU = m[0]; si.addressModeV = m[1]; si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(c->dev, &si, NULL, &c->samplers[c->nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    c->samplers[c->nsamplers].key = key;
    return c->samplers[c->nsamplers++].s;
}

VkrContext *vkr_create(void)
{
    VkrContext *c = (VkrContext *)calloc(1, sizeof *c);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDevice devs[8];
    uint32_t n = 8, i, nq = 0;
    VkQueueFamilyProperties qp[16];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features f14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    void *map;
    int f;

    if (!c) return NULL;
    c->bc7 = -1;
    c->tex_mips = -1;
    c->dedup = -1;
    c->cur_off = -1;
    app.pApplicationName = "ssx3"; app.apiVersion = VK_API_VERSION_1_4;
    ii.pApplicationInfo = &app;
#if defined(__SWITCH__)
    {
        static const char *const iext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
        const char *e = getenv("RECOMP_VK_PRESENT");
        c->present = !(e && *e == '0');
        if (c->present) {
            ii.enabledExtensionCount = 2;
            ii.ppEnabledExtensionNames = iext;
        }
    }
    if (vkCreateInstance(&ii, NULL, &c->inst) != VK_SUCCESS) {
        if (!c->present) goto fail;
        fprintf(stderr, "[VKR] no instance with VK_NN_vi_surface: frames read back for the CPU presenter\n");
        c->present = 0;
        ii.enabledExtensionCount = 0;
        ii.ppEnabledExtensionNames = NULL;
        if (vkCreateInstance(&ii, NULL, &c->inst) != VK_SUCCESS) goto fail;
    }
    if (c->present) {
        VkViSurfaceCreateInfoNN si = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
        si.window = nwindowGetDefault();
        if (vkCreateViSurfaceNN(c->inst, &si, NULL, &c->surf) != VK_SUCCESS) {
            fprintf(stderr, "[VKR] vkCreateViSurfaceNN failed: frames read back for the CPU presenter\n");
            c->present = 0;
        }
    }
#else
    if (vkCreateInstance(&ii, NULL, &c->inst) != VK_SUCCESS) goto fail;
#endif
    if (vkEnumeratePhysicalDevices(c->inst, &n, devs) < 0 || !n) goto fail;
    /* Prefer a real GPU over llvmpipe. */
    c->phys = devs[0];
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
            || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            c->phys = devs[i];
            break;
        }
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(c->phys, &p);
        /* NVIDIA packs the driver version 10.8.8.6 bits, others like Vulkan */
        if (p.vendorID == 0x10DE)
            fprintf(stderr, "[VKR] device: %s (Vulkan %u.%u, driver %u.%u)\n", p.deviceName,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                    p.driverVersion >> 22, (p.driverVersion >> 14) & 0xFF);
        else
            fprintf(stderr, "[VKR] device: %s (Vulkan %u.%u, driver %u.%u.%u, vendor %04X)\n", p.deviceName,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                    VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion),
                    VK_VERSION_PATCH(p.driverVersion), p.vendorID);
    }
    vkGetPhysicalDeviceMemoryProperties(c->phys, &c->mem);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    for (i = 0; i < nq && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT); i++)
        ;
    if (i == nq) goto fail;
    c->qfam = i;
    qi.queueFamilyIndex = i; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    {
        /* RECOMP_RES: the internal resolution, as a frame height (1080 =
         * x2.25 of 480) or a factor (2 = x2). Default x1. */
        const char *r = getenv("RECOMP_RES");
        double v = r ? atof(r) : 0;
        c->scale = v >= 240 ? (float)(v / 480.0) : (v >= 1.0 && v <= 8.0) ? (float)v : 1.0f;
        /* RECOMP_ASPECT (main.c, from RECOMP_WIDESCREEN): a target that
         * wide (16:9, 32:9, ...); the game renders that view squeezed into
         * 640x480 (ssx3_overrides.c sub_0024EA13). */
        r = getenv("RECOMP_ASPECT");
        vkr_set_aspect(c, r ? (float)atof(r) : 0.0f);
    }
    {
        VkPhysicalDeviceFeatures pf;
        VkPhysicalDeviceProperties pp;
        static VkPhysicalDeviceFeatures en;
        vkGetPhysicalDeviceFeatures(c->phys, &pf);
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        if (pf.dualSrcBlend) {          /* RECOMP_MBLUR rider tags */
            en.dualSrcBlend = VK_TRUE;
            di.pEnabledFeatures = &en;
            c->dual_src = 1;
        }
        if (pf.wideLines) {             /* lines as wide as the scale */
            en.wideLines = VK_TRUE;
            c->wide_lines = 1;
            c->max_line = pp.limits.lineWidthRange[1];
        }
        en.textureCompressionBC = pf.textureCompressionBC;   /* HD textures */
        {
            /* RECOMP_ANISO (default 16; 0 or 1 = off): anisotropic filtering
             * on every linear-filtered sampler -- the slopes are seen at
             * grazing angles, where plain filtering blurs a few metres ahead. */
            const char *e = getenv("RECOMP_ANISO");
            float want = e && *e ? (float)atof(e) : 16.0f;
            if (pf.samplerAnisotropy && want > 1.0f) {
                en.samplerAnisotropy = VK_TRUE;
                c->aniso = want < pp.limits.maxSamplerAnisotropy ? want
                                                                 : pp.limits.maxSamplerAnisotropy;
            }
            fprintf(stderr, "[VKR] anisotropic filtering %s (%.0fx)\n",
                    c->aniso > 1.0f ? "on" : "off", c->aniso > 1.0f ? c->aniso : 1.0f);
        }
        if (!pf.textureCompressionBC)
            c->bc7 = 0;
        di.pEnabledFeatures = &en;
    }
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    f14.pushDescriptor = VK_TRUE;
    f13.pNext = &f14;
    di.pNext = &f13; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
#if defined(__SWITCH__)
    {
        static const char *const dext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkBool32 can = VK_FALSE;
        if (c->present && (vkGetPhysicalDeviceSurfaceSupportKHR(c->phys, c->qfam, c->surf, &can) != VK_SUCCESS || !can)) {
            fprintf(stderr, "[VKR] the queue cannot present to the window: frames read back for the CPU presenter\n");
            c->present = 0;
        }
        if (c->present) {
            di.enabledExtensionCount = 1;
            di.ppEnabledExtensionNames = dext;
        }
        if (vkCreateDevice(c->phys, &di, NULL, &c->dev) != VK_SUCCESS) {
            if (!c->present) goto fail;
            fprintf(stderr, "[VKR] no device with VK_KHR_swapchain: frames read back for the CPU presenter\n");
            c->present = 0;
            di.enabledExtensionCount = 0;
            di.ppEnabledExtensionNames = NULL;
            if (vkCreateDevice(c->phys, &di, NULL, &c->dev) != VK_SUCCESS) goto fail;
        }
    }
#else
    if (vkCreateDevice(c->phys, &di, NULL, &c->dev) != VK_SUCCESS) goto fail;
#endif
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);
    pcache_load(c);
#if defined(__SWITCH__)
    if (c->pcache && !s_pcache_ctx) {
        s_pcache_ctx = c;
        atexit(pcache_at_exit);
    }
#endif
    pci.queueFamilyIndex = c->qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(c->dev, &pci, NULL, &c->pool) != VK_SUCCESS) goto fail;
    cai.commandPool = c->pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for (f = 0; f < VKR_FRAMES; f++) {
        VkrFrame *fr = &c->fr[f];
        if (vkAllocateCommandBuffers(c->dev, &cai, &fr->cmd) != VK_SUCCESS) goto fail;
        if (vkCreateFence(c->dev, &fi, NULL, &fr->fence) != VK_SUCCESS) goto fail;
        if (!make_buffer(c, VKR_BUF_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                         | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &fr->buf, &fr->buf_mem, &map))
            goto fail;
        fr->buf_map = (uint8_t *)map;
    }
    {
        /* RECOMP_VKR_GPUPROF=<ms>: GPU time per draw from timestamps; a
         * frame whose GPU work took longer than <ms> is logged with its most
         * expensive draws ([GPUPROF], vkr_frame_pixels). */
        const char *e = getenv("RECOMP_VKR_GPUPROF");
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        if (e && atof(e) > 0 && qp[c->qfam].timestampValidBits && pp.limits.timestampPeriod > 0) {
            VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = VKR_PROF_MAX;
            c->prof_thr = atof(e);
            c->prof_frame = getenv("RECOMP_VKR_GPUPROF_FRAME") != NULL;
            c->ts_period = pp.limits.timestampPeriod;
            for (f = 0; f < VKR_FRAMES; f++) {
                if (vkCreateQueryPool(c->dev, &qpi, NULL, &c->fr[f].qp) != VK_SUCCESS
                    || !(c->fr[f].meta = (VkrProfDraw *)calloc(VKR_PROF_MAX, sizeof(VkrProfDraw)))) {
                    c->prof_thr = 0;
                    break;
                }
            }
            fprintf(stderr, "[VKR] GPU profiler %s: frames over %.1f ms\n",
                    c->prof_thr > 0 ? "on" : "failed", c->prof_thr);
        }
    }
    {
        /* The NV2A's Z24S8 has a stencil buffer that SSX 3 uses (the snow
         * trail surfaces fix up depth through it); a format with stencil. */
        static const VkFormat cand[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
        int k;
        c->depth_fmt = VK_FORMAT_D32_SFLOAT;
        for (k = 0; k < 2; k++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(c->phys, cand[k], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                c->depth_fmt = cand[k];
                c->has_stencil = 1;
                break;
            }
        }
        if (!c->has_stencil)
            fprintf(stderr, "[VKR] no depth-stencil format: stencil ignored\n");
    }
    {
        /* RECOMP_MSAA=0/2/4/8 (default 4): multisampled edges, at most what
         * the GPU offers for colour, depth and stencil together. */
        VkPhysicalDeviceProperties pp;
        const char *e = getenv("RECOMP_MSAA");
        int want = e && *e ? atoi(e) : 4;
        VkSampleCountFlags ok;
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        ok = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts
           & (c->has_stencil ? pp.limits.framebufferStencilSampleCounts : ~0u);
        c->samples = VK_SAMPLE_COUNT_1_BIT;
        for (int n = 8; n > 1; n >>= 1)
            if (want >= n && (ok & (VkSampleCountFlags)n)) {
                c->samples = (VkSampleCountFlagBits)n;
                break;
            }
        fprintf(stderr, "[VKR] MSAA %ux%s\n", (unsigned)c->samples,
                c->samples == VK_SAMPLE_COUNT_1_BIT ? " (off)" : "");
    }
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->freed, NULL);
    use_frame(c, 0);
    if (!make_layout(c)) goto fail;
    crt_config(c);
    if (c->crt && !make_crt(c))
        c->crt = 0;
    scene_config(c);
    if (c->scene && !make_scene(c))
        c->scene = 0;
    {
        /* the boost pass's edge blur reads a copy of the frame (sc_in) */
        const char *e = getenv("SSX3NX_BOOST_LINES");
        c->sl_want = e && *e && *e != '0';
        c->sl_noblur = e && *e == '2';      /* 2 = speed lines only: no frame copy, no blur */
        /* SSX3NX_BOOST_BLUR_HALF (default 1): the blur reads a half-size
         * copy made by one filtered blit -- a quarter of the copy's bytes and
         * of the taps' reads, and a blur hides the resolution. 0 = full size. */
        e = getenv("SSX3NX_BOOST_BLUR_HALF");
        if (!(e && *e == '0')) {
            VkFormatProperties fp;
            VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                                      | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_B8G8R8A8_UNORM, &fp);
            c->sl_half_ok = (fp.optimalTilingFeatures & need) == need;
        }
    }
    /* Rider tags: the motion blur leaves riders sharp by them;
     * RECOMP_MBLUR_TAGS=0 off (then only the near distance does) */
    {
        const char *e = getenv("RECOMP_MBLUR_TAGS");
        c->tags = c->scene && c->dual_src && !(e && *e == '0');
        if (c->scene)
            fprintf(stderr, "[VKR] rider tags %s\n", c->tags ? "on" : c->dual_src ? "off" : "off (no dualSrcBlend)");
    }
#if defined(__SWITCH__)
    fprintf(stderr, "[VKR] presentation: %s\n", c->present
            ? "Vulkan swapchain from the first frame (RECOMP_VK_PRESENT=0: read back for the CPU presenter)"
            : "frames read back for the CPU presenter");
#endif
    return c;
fail:
    fprintf(stderr, "[VKR] init failed\n");
    free(c);
    return NULL;
}

static void free_target(VkrContext *c)
{
    int f;
    if (!c->color) return;
    wait_all_free(c);
    vkDeviceWaitIdle(c->dev);
    vkDestroyImageView(c->dev, c->color_view, NULL);
    vkDestroyImageView(c->dev, c->depth_view, NULL);
    vkDestroyImageView(c->dev, c->depth_sview, NULL);
    vkDestroyImage(c->dev, c->color, NULL);
    vkDestroyImage(c->dev, c->depth, NULL);
    vkFreeMemory(c->dev, c->color_mem, NULL);
    vkFreeMemory(c->dev, c->depth_mem, NULL);
    for (f = 0; f < VKR_FRAMES; f++) {
        vkDestroyBuffer(c->dev, c->fr[f].rb, NULL);
        vkFreeMemory(c->dev, c->fr[f].rb_mem, NULL);
        if (c->fr[f].rbh) {
            vkDestroyBuffer(c->dev, c->fr[f].rbh, NULL);
            vkFreeMemory(c->dev, c->fr[f].rbh_mem, NULL);
            c->fr[f].rbh = VK_NULL_HANDLE;
        }
    }
    if (c->down) {
        vkDestroyImageView(c->dev, c->down_view, NULL);
        vkDestroyImage(c->dev, c->down, NULL);
        vkFreeMemory(c->dev, c->down_mem, NULL);
        c->down = VK_NULL_HANDLE;
    }
    if (c->crt_in) {
        vkDestroyImageView(c->dev, c->crt_in_view, NULL);
        vkDestroyImage(c->dev, c->crt_in, NULL);
        vkFreeMemory(c->dev, c->crt_in_mem, NULL);
        vkDestroyImageView(c->dev, c->crt_out_view, NULL);
        vkDestroyImage(c->dev, c->crt_out, NULL);
        vkFreeMemory(c->dev, c->crt_out_mem, NULL);
        c->crt_in = c->crt_out = VK_NULL_HANDLE;
    }
    if (c->resolved) {
        vkDestroyImageView(c->dev, c->resolved_view, NULL);
        vkDestroyImage(c->dev, c->resolved, NULL);
        vkFreeMemory(c->dev, c->resolved_mem, NULL);
        c->resolved = VK_NULL_HANDLE;
    }
    if (c->sl_half) {
        vkDestroyImageView(c->dev, c->sl_half_view, NULL);
        vkDestroyImage(c->dev, c->sl_half, NULL);
        vkFreeMemory(c->dev, c->sl_half_mem, NULL);
        c->sl_half = VK_NULL_HANDLE;
    }
    if (c->sc_in) {
        vkDestroyImageView(c->dev, c->sc_in_view, NULL);
        vkDestroyImage(c->dev, c->sc_in, NULL);
        vkFreeMemory(c->dev, c->sc_in_mem, NULL);
        c->sc_in = VK_NULL_HANDLE;
    }
    c->color = c->depth = VK_NULL_HANDLE;
}

void vkr_info(VkrContext *c, uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes)
{
    *w = c->sw;
    *h = c->sh;
    *msaa = (uint32_t)c->main.samples;
    *tex_bytes = c->tex_bytes;
}

void vkr_tex_purge(VkrContext *c)
{
    c->purge_req = 1;
}

uint32_t vkr_tex_generation(VkrContext *c)
{
    return c->tex_gen;
}

static void tex_free_all(VkrContext *c)
{
    c->tex_gen++;
    uint32_t i;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        vkDestroyImageView(c->dev, t->view, NULL);
        vkDestroyImage(c->dev, t->img, NULL);
        vkFreeMemory(c->dev, t->mem, NULL);
        free(t);
    }
    free(c->texs);
    c->texs = NULL; c->ntexs = c->captexs = 0; c->tex_bytes = 0; c->white = NULL;
    free(c->hidx);
    c->hidx = NULL; c->hcap = 0;
}

static uint32_t tex_slot(uint64_t key, uint32_t cap)
{
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
    return (uint32_t)key & (cap - 1);
}

/* (Re)build the key index for the current texs. */
static int tex_rehash(VkrContext *c)
{
    uint32_t cap = 1024, i;
    while (cap < c->ntexs * 2 + 2)
        cap <<= 1;
    free(c->hidx);
    c->hidx = (int32_t *)malloc(cap * sizeof *c->hidx);
    if (!c->hidx) { c->hcap = 0; return 0; }
    c->hcap = cap;
    for (i = 0; i < cap; i++)
        c->hidx[i] = -1;
    for (i = 0; i < c->ntexs; i++) {
        uint32_t h = tex_slot(c->texs[i]->key, cap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (cap - 1);
        c->hidx[h] = (int32_t)i;
    }
    return 1;
}

static void tex_free_one(VkrContext *c, VkrTex *t)
{
    vkDestroyImageView(c->dev, t->view, NULL);
    vkDestroyImage(c->dev, t->img, NULL);
    vkFreeMemory(c->dev, t->mem, NULL);
    c->tex_bytes -= t->bytes;
    free(t);
}

/* Textures not used for `age` flips: certainly not in flight (two frames
 * at most), and very likely dead -- a texture whose content changes every
 * frame gets a new key each time. Freed, the index rebuilt, and the
 * generation bumped so the backend drops the pointers it keeps. */
static void tex_evict_old(VkrContext *c, uint32_t age)
{
    uint32_t i, k = 0, freed = 0;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        if (t != c->white && c->flip_serial - t->last_use > age) {
            tex_free_one(c, t);
            freed++;
        } else
            c->texs[k++] = t;
    }
    if (!freed)
        return;
    c->ntexs = k;
    c->tex_gen++;
    tex_rehash(c);
}

void vkr_destroy(VkrContext *c)
{
    if (!c) return;
    free_target(c);
    vkDeviceWaitIdle(c->dev);
    tex_free_all(c);
    if (c->pj_started) {
        pthread_mutex_lock(&c->pj_lock);
        c->pj_quit = 1;
        pthread_cond_broadcast(&c->pj_cond);
        while (!c->pj_done)
            pthread_cond_wait(&c->pj_cond, &c->pj_lock);
        pthread_mutex_unlock(&c->pj_lock);
    }
#if defined(__SWITCH__)
    pcache_save(c, 1);
    if (s_pcache_ctx == c)
        s_pcache_ctx = NULL;
    if (c->pcache)
        vkDestroyPipelineCache(c->dev, c->pcache, NULL);
#endif
    for (int i = 0; i < c->npipes; i++) {
        if (c->pipes[i]->pipe)
            vkDestroyPipeline(c->dev, c->pipes[i]->pipe, NULL);
        free(c->pipes[i]);
    }
    free(c->pipes);
    for (int i = 0; i < c->nsamplers; i++)
        vkDestroySampler(c->dev, c->samplers[i].s, NULL);
    vkDestroyShaderModule(c->dev, c->vs, NULL);
    vkDestroyShaderModule(c->dev, c->fs, NULL);
    vkDestroyShaderModule(c->dev, c->fs_wdepth, NULL);
    vkDestroyShaderModule(c->dev, c->vs_vsh, NULL);
    vkDestroyPipelineLayout(c->dev, c->layout, NULL);
    vkDestroyDescriptorSetLayout(c->dev, c->dsl, NULL);
    if (c->crt_pipe) vkDestroyPipeline(c->dev, c->crt_pipe, NULL);
    if (c->crt_samp) vkDestroySampler(c->dev, c->crt_samp, NULL);
    if (c->crt_layout) vkDestroyPipelineLayout(c->dev, c->crt_layout, NULL);
    if (c->crt_dsl) vkDestroyDescriptorSetLayout(c->dev, c->crt_dsl, NULL);
    if (c->crt_vs) vkDestroyShaderModule(c->dev, c->crt_vs, NULL);
    if (c->crt_fs) vkDestroyShaderModule(c->dev, c->crt_fs, NULL);
    for (int i = 0; i < 2; i++)
        if (c->fs_tags[i]) vkDestroyShaderModule(c->dev, c->fs_tags[i], NULL);
    if (c->sc_pipe) vkDestroyPipeline(c->dev, c->sc_pipe, NULL);
    if (c->sc_layout) vkDestroyPipelineLayout(c->dev, c->sc_layout, NULL);
    if (c->sc_dsl) vkDestroyDescriptorSetLayout(c->dev, c->sc_dsl, NULL);
    if (c->sc_near) vkDestroySampler(c->dev, c->sc_near, NULL);
    if (c->sc_lin) vkDestroySampler(c->dev, c->sc_lin, NULL);
    if (c->sc_fs) vkDestroyShaderModule(c->dev, c->sc_fs, NULL);
    if (c->sl_pipe) vkDestroyPipeline(c->dev, c->sl_pipe, NULL);
    if (c->sl_layout) vkDestroyPipelineLayout(c->dev, c->sl_layout, NULL);
    if (c->sl_dsl) vkDestroyDescriptorSetLayout(c->dev, c->sl_dsl, NULL);
    if (c->sl_lin) vkDestroySampler(c->dev, c->sl_lin, NULL);
    for (int i = 0; i < VKR_FRAMES; i++) {
        vkDestroyBuffer(c->dev, c->fr[i].buf, NULL);
        vkFreeMemory(c->dev, c->fr[i].buf_mem, NULL);
        vkDestroyFence(c->dev, c->fr[i].fence, NULL);
    }
    vkDestroyCommandPool(c->dev, c->pool, NULL);
    vkDestroyDevice(c->dev, NULL);
    vkDestroyInstance(c->inst, NULL);
    free(c);
}

static void end_pass(VkrContext *c);

/* Submit what has been recorded and wait for it. Texture uploads are in
 * there: dropping the commands left those textures without content, and
 * they stayed cached that way (magenta HUD in races after a resize). */
static int flush_wait(VkrContext *c)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!c->recording) return 1;
    end_pass(c);
    CK(vkEndCommandBuffer(c->cmd));
    c->recording = 0;
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
    CK(vkQueueSubmit(c->queue, 1, &si, c->fence));
    CK(vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, UINT64_MAX));
    c->buf_used = 0;
    c->up_tex = c->up_hd = c->up_draw = 0;
    c->up_full_said = 0;
    c->upload_serial++;
    return 1;
}

void vkr_offscreen_done(VkrContext *c);

int vkr_set_target(VkrContext *c, uint32_t w, uint32_t h, float zmax)
{
    void *map;
    int f;
    vkr_offscreen_done(c);
    if (c->color && !flush_wait(c))
        return 0;
    free_target(c);
    c->w = w; c->h = h; c->zmax = zmax;
    c->sw = (uint32_t)((float)w * c->scale_x + 0.5f);
    c->sh = (uint32_t)((float)h * c->scale + 0.5f);
    c->sc.offset.x = c->sc.offset.y = 0; c->sc.extent.width = c->sw; c->sc.extent.height = c->sh;
    c->gsc[0] = c->gsc[1] = 0; c->gsc[2] = w; c->gsc[3] = h;
    /* present.frag samples the finished frame (the swapchain path) */
    VkImageUsageFlags pr_use = c->present ? VK_IMAGE_USAGE_SAMPLED_BIT : 0;
    if (!make_image_ms(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | (c->samples == VK_SAMPLE_COUNT_1_BIT ? pr_use : 0),
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->color, &c->color_mem, &c->color_view,
                       c->sw, c->sh, 1, c->samples))
        return 0;
    if (!make_image_ms(c, c->depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT, ds_aspect(c), &c->depth, &c->depth_mem,
                       &c->depth_view, c->sw, c->sh, 1, c->samples))
        return 0;
    {
        /* the scene pass samples depth: a depth-only view (a depth+stencil
         * view cannot be sampled); multisampled with MSAA (scene_ms.frag) */
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = c->depth;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = c->depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        CK(vkCreateImageView(c->dev, &vi, NULL, &c->depth_sview));
    }
    if (c->samples != VK_SAMPLE_COUNT_1_BIT
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | pr_use,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->resolved, &c->resolved_mem,
                       &c->resolved_view, c->sw, c->sh))
        return 0;
    if ((c->scene || c->sl_want)
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->sc_in, &c->sc_in_mem, &c->sc_in_view,
                       c->sw, c->sh))
        return 0;
    /* the edge blur's half-size copy of the frame (vkr_speedlines) */
    if (c->sl_want && !c->sl_noblur && c->sl_half_ok && c->sw >= 4 && c->sh >= 4
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->sl_half, &c->sl_half_mem, &c->sl_half_view,
                       c->sw / 2, c->sh / 2))
        c->sl_half = VK_NULL_HANDLE;       /* the full-size copy, as before */
    if ((c->scale != 1.0f || c->scale_x != 1.0f)
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->down, &c->down_mem, &c->down_view, w, h))
        return 0;
    if (c->crt && (c->scale != 1.0f || c->scale_x != 1.0f)) {
        uint32_t m = c->sw > c->sh ? c->sw : c->sh;
        for (c->crt_levels = 1; m > 1; m >>= 1)
            c->crt_levels++;
        if (!make_image_ex(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           &c->crt_in, &c->crt_in_mem, &c->crt_in_view, c->sw, c->sh, c->crt_levels)
            || !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | pr_use,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->crt_out, &c->crt_out_mem,
                           &c->crt_out_view, c->sw, c->sh))
            return 0;
    } else if (c->crt) {
        static int said;
        if (!said++)
            fprintf(stderr, "[VKR] CRT needs an internal resolution above the guest's (RECOMP_RES)\n");
    }
    for (f = 0; f < VKR_FRAMES; f++) {
        if (!make_buffer_ex(c, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            &c->fr[f].rb, &c->fr[f].rb_mem, &map, 1, &c->fr[f].rb_noncoherent))
            return 0;
        c->fr[f].rb_map = (uint8_t *)map;
        if (c->scale != 1.0f || c->scale_x != 1.0f) {
            if (!make_buffer_ex(c, (VkDeviceSize)c->sw * c->sh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                &c->fr[f].rbh, &c->fr[f].rbh_mem, &map, 1, &c->fr[f].rbh_noncoherent))
                return 0;
            c->fr[f].rbh_map = (uint8_t *)map;
        }
    }
    if (c->scale != 1.0f || c->scale_x != 1.0f)
        fprintf(stderr, "[VKR] internal resolution %ux%u (x%.2f/x%.2f of %ux%u)%s\n", c->sw, c->sh,
                c->scale_x, c->scale, w, h, c->squeeze2d != 1.0f ? ", widescreen" : "");
    c->recording = c->in_pass = c->color_valid = 0;
    return 1;
}

/* RECOMP_VKR_GPUPROF: a timestamp once everything recorded so far has run;
 * the difference to the previous one is (about) that draw's GPU time. */
static void prof_mark(VkrContext *c, const VkrDrawState *st, uint32_t nidx, int vsh)
{
    VkrFrame *fr = &c->fr[c->cur];
    VkrProfDraw *m;
    if (c->prof_thr <= 0 || !fr->qreset || fr->nq >= VKR_PROF_MAX || (st && c->prof_frame))
        return;
    m = &fr->meta[fr->nq];
    memset(m, 0, sizeof *m);
    if (st) {
        int k;
        m->nidx = nidx; m->topo = st->topology; m->vsh = (uint32_t)vsh;
        m->blend = (uint32_t)st->blend_enable; m->src = st->blend_src; m->dst = st->blend_dst;
        m->eq = st->blend_eq; m->mask = st->write_mask; m->stages = st->u.stage_program;
        m->final0 = st->u.final0; m->alpha = st->alpha_func;
        m->psize = st->point_params ? -1.0f : st->point_size;
        for (k = 0; k < 2; k++)
            if (st->tex[k]) { m->tw[k] = st->tex[k]->w; m->th[k] = st->tex[k]->h; }
        m->scw = c->sc.extent.width; m->sch = c->sc.extent.height;
    }
    vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fr->qp, fr->nq);
    fr->nq++;
}

static void prof_report(VkrContext *c, int slot)
{
    VkrFrame *fr = &c->fr[slot];
    static uint64_t ts[VKR_PROF_MAX];
    int top[8], k;
    uint32_t i, n = fr->nq;
    double total;
    if (c->prof_thr <= 0 || n < 2)
        return;
    if (vkGetQueryPoolResults(c->dev, fr->qp, 0, n, n * sizeof ts[0], ts, sizeof ts[0],
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    total = (double)(ts[n - 1] - ts[0]) * c->ts_period / 1e6;
    {
        /* every 120 frames: the GPU frame time's average and maximum */
        static double sum, mx;
        static unsigned nf, passes;
        sum += total; nf++;
        passes += fr->npass;
        if (total > mx) mx = total;
        if (nf == 120) {
            fprintf(stderr, "[GPUPROF] 120 frames: GPU avg %.2f ms, max %.2f ms, %.1f passes\n",
                    sum / nf, mx, (double)passes / nf);
            sum = mx = 0; nf = passes = 0;
        }
    }
    if (total < c->prof_thr || c->prof_said >= 60)
        return;
    c->prof_said++;
    for (k = 0; k < 8; k++) top[k] = -1;
    for (i = 1; i < n; i++) {
        uint64_t d = ts[i] - ts[i - 1];
        for (k = 0; k < 8; k++) {
            if (top[k] < 0 || d > ts[top[k]] - ts[top[k] - 1]) {
                int j;
                for (j = 7; j > k; j--) top[j] = top[j - 1];
                top[k] = (int)i;
                break;
            }
        }
    }
    fprintf(stderr, "[GPUPROF] frame GPU %.1f ms, %u draws, target %ux%u; top draws:\n",
            total, n - 2, c->sw, c->sh);
    for (k = 0; k < 8 && top[k] > 0; k++) {
        const VkrProfDraw *m = &fr->meta[top[k]];
        fprintf(stderr, "[GPUPROF]   #%d %.2f ms %s n %u %s blend %u %X/%X eq %X mask %X"
                " stages %05X final0 %08X alpha %X tex %ux%u %ux%u clip %ux%u psize %.1f\n",
                top[k], (double)(ts[top[k]] - ts[top[k] - 1]) * c->ts_period / 1e6,
                m->topo == 2 ? "points" : m->topo == 1 ? "lines" : "tris", m->nidx,
                !m->nidx ? "-" : m->vsh ? "vsh" : "cpu", m->blend, m->src, m->dst, m->eq, m->mask,
                m->stages, m->final0, m->alpha, m->tw[0], m->th[0], m->tw[1], m->th[1],
                m->scw, m->sch, m->psize);
    }
}

static int begin_cmd(VkrContext *c)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (c->recording) return 1;
    /* The frame's previous use must be rendered and read back. */
    pthread_mutex_lock(&c->lock);
    while (c->fr[c->cur].busy)
        pthread_cond_wait(&c->freed, &c->lock);
    pthread_mutex_unlock(&c->lock);
    CK(vkResetFences(c->dev, 1, &c->fence));
    CK(vkResetCommandBuffer(c->cmd, 0));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CK(vkBeginCommandBuffer(c->cmd, &bi));
    c->recording = 1;
    if (c->prof_thr > 0 && !c->fr[c->cur].qreset) {
        vkCmdResetQueryPool(c->cmd, c->fr[c->cur].qp, 0, VKR_PROF_MAX);
        c->fr[c->cur].qreset = 1;
        c->fr[c->cur].nq = 0;
        c->fr[c->cur].npass = 0;
        prof_mark(c, NULL, 0, -1);
    }
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            c->color_valid ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(c, c->depth, ds_aspect(c),
            c->color_valid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    return 1;
}

static void begin_pass(VkrContext *c)
{
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};   /* render area: the whole target */
    if (c->in_pass) return;
    c->fr[c->cur].npass++;
    ca.imageView = c->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = c->color_valid ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = c->depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = c->color_valid ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    if (c->has_stencil)
        ri.pStencilAttachment = &da;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
    c->in_pass = 1;
    c->color_valid = 1;
    /* A new pass (maybe a new command buffer, after another pipeline layout
     * in the CRT/scene passes): nothing is known to be bound. */
    c->bnd.valid = 0;
}

static void end_pass(VkrContext *c)
{
    if (c->in_pass) { vkCmdEndRendering(c->cmd); c->in_pass = 0; }
}

/* Draws and clears stay inside the NV2A clip rectangle; the target stays the
 * whole surface (a smaller clip must not recreate it mid-frame). */
static void apply_scissor(VkrContext *c)
{
    uint32_t x = c->gsc[0], y = c->gsc[1], w = c->gsc[2], h = c->gsc[3];
    if (x >= c->w || y >= c->h) { x = y = 0; w = h = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (c->scale != 1.0f || c->scale_x != 1.0f) {   /* guest pixels -> target pixels */
        /* Pillarbox: the guest frame is the 4:3 middle of the target. */
        float sx = c->squeeze_all > 0.5f ? c->scale : c->scale_x;
        float ox = c->squeeze_all > 0.5f ? ((float)c->sw - (float)c->w * c->scale) * 0.5f : 0.0f;
        uint32_t x1 = (uint32_t)(ox + (float)(x + w) * sx + 0.5f);
        uint32_t y1 = (uint32_t)((float)(y + h) * c->scale + 0.5f);
        x = (uint32_t)(ox + (float)x * sx + 0.5f);
        y = (uint32_t)((float)y * c->scale + 0.5f);
        if (x1 > c->sw) x1 = c->sw;
        if (y1 > c->sh) y1 = c->sh;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    if (c->sc.offset.x == (int32_t)x && c->sc.offset.y == (int32_t)y
        && c->sc.extent.width == w && c->sc.extent.height == h)
        return;
    c->sc.offset.x = (int32_t)x; c->sc.offset.y = (int32_t)y;
    c->sc.extent.width = w; c->sc.extent.height = h;
    if (c->in_pass)
        vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
}

void vkr_set_scissor(VkrContext *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c->gsc[0] = x; c->gsc[1] = y; c->gsc[2] = w; c->gsc[3] = h;
    apply_scissor(c);
}

void vkr_clear(VkrContext *c, const float rgba[4], int clear_depth, float depth)
{
    VkClearAttachment ca[2];
    VkClearRect cr = {c->sc, 0, 1};
    uint32_t n = 1;
    if (!begin_cmd(c)) return;
    begin_pass(c);
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[0].colorAttachment = 0;
    memcpy(ca[0].clearValue.color.float32, rgba, 16);
    if (c->tags)
        ca[0].clearValue.color.float32[3] = 0.0f;   /* the rider tag: none */
    if (clear_depth) {
        ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca[1].colorAttachment = 0;
        ca[1].clearValue.depthStencil.depth = depth; ca[1].clearValue.depthStencil.stencil = 0;
        n = 2;
    }
    vkCmdClearAttachments(c->cmd, n, ca, 1, &cr);
}

void vkr_clear_ds(VkrContext *c, int clear_depth, float depth, int clear_stencil, uint32_t stencil)
{
    VkClearAttachment ca = {0};
    VkClearRect cr = {c->sc, 0, 1};
    if (clear_depth)
        ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (clear_stencil && c->has_stencil)
        ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!ca.aspectMask) return;
    if (!begin_cmd(c)) return;
    begin_pass(c);
    ca.clearValue.depthStencil.depth = depth;
    ca.clearValue.depthStencil.stencil = stencil;
    vkCmdClearAttachments(c->cmd, 1, &ca, 1, &cr);
}

/* ---- offscreen targets ---------------------------------------------------
 * A guest render target the title later samples as a texture (SSX 3's rider
 * shadow maps, 128x128), drawn on the GPU at `scale` times its size instead
 * of in software: vkr_offscreen_use(key) makes it the current target (draws,
 * clears and the scissor go there, in its guest pixels), vkr_offscreen_done
 * goes back to the main target, and vkr_offscreen_tex hands its image to a
 * draw that samples it. Nothing is written back to guest memory. */
static void off_to_read(VkrContext *c, struct VkrOff *o)
{
    if (!o->in_rt)
        return;
    barrier(c, o->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    o->in_rt = 0;
}

void vkr_offscreen_done(VkrContext *c)
{
    struct VkrOff *o;
    if (c->cur_off < 0)
        return;
    o = &c->off[c->cur_off];
    end_pass(c);
    o->valid = c->color_valid;
    off_to_read(c, o);
    c->color = c->main.color; c->depth = c->main.depth;
    c->color_view = c->main.color_view; c->depth_view = c->main.depth_view;
    c->w = c->main.w; c->h = c->main.h; c->sw = c->main.sw; c->sh = c->main.sh;
    c->scale = c->main.scale; c->scale_x = c->main.scale_x;
    c->squeeze2d = c->main.squeeze2d; c->squeeze_all = c->main.squeeze_all;
    c->zmax = c->main.zmax; c->sc = c->main.sc; memcpy(c->gsc, c->main.gsc, sizeof c->gsc);
    c->color_valid = c->main.color_valid; c->samples = c->main.samples;
    c->cur_off = -1;
}

int vkr_offscreen_use(VkrContext *c, uint32_t key, uint32_t w, uint32_t h, float scale, float zmax)
{
    struct VkrOff *o = NULL;
    int i;
    if (!c->color || !begin_cmd(c))
        return 0;
    for (i = 0; i < c->noff; i++)
        if (c->off[i].key == key && c->off[i].w == w && c->off[i].h == h)
            o = &c->off[i];
    if (c->cur_off >= 0 && o == &c->off[c->cur_off])
        return 1;
    vkr_offscreen_done(c);
    if (!o) {
        int slot = c->noff;
        if (c->noff == 8) {
            /* SSX3NX: all 8 taken. The game allocates each event's shadow
             * maps at new addresses (no memory reuse), so the second event
             * found no room and drew its shadow maps in software: 13-20 FPS
             * (2026-11-25). The least recently used target, unused for
             * longer than any frame in flight, is freed and reused. */
            uint32_t oldest = 0xFFFFFFFFu;
            slot = -1;
            for (i = 0; i < 8; i++) {
                uint32_t age = c->flip_serial - c->off[i].last_use;
                if (age > VKR_FRAMES + 2 && c->off[i].last_use < oldest) {
                    oldest = c->off[i].last_use;
                    slot = i;
                }
            }
            if (slot < 0)
                return 0;
            end_pass(c);
            o = &c->off[slot];
            fprintf(stderr, "[VKR] offscreen target %08X (unused for %u frames) freed for %08X\n",
                    o->key, c->flip_serial - o->last_use, key);
            vkDestroyImageView(c->dev, o->color_view, NULL);
            vkDestroyImage(c->dev, o->color, NULL);
            vkFreeMemory(c->dev, o->color_mem, NULL);
            vkDestroyImageView(c->dev, o->depth_view, NULL);
            vkDestroyImage(c->dev, o->depth, NULL);
            vkFreeMemory(c->dev, o->depth_mem, NULL);
            free(o->tex);
        }
        o = &c->off[slot];
        memset(o, 0, sizeof *o);
        o->key = key; o->w = w; o->h = h; o->scale = scale;
        o->sw = (uint32_t)((float)w * scale + 0.5f); o->sh = (uint32_t)((float)h * scale + 0.5f);
        end_pass(c);
        if (!make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &o->color, &o->color_mem, &o->color_view,
                        o->sw, o->sh)
            || !make_image(c, c->depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           ds_aspect(c), &o->depth, &o->depth_mem, &o->depth_view, o->sw, o->sh))
            return 0;
        o->tex = (VkrTex *)calloc(1, sizeof *o->tex);
        o->tex->img = o->color; o->tex->view = o->color_view;
        o->tex->w = o->sw; o->tex->h = o->sh; o->tex->key = 0x0FF5C0000000000ull | key;
        barrier(c, o->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(c, o->depth, ds_aspect(c), VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        fprintf(stderr, "[VKR] offscreen target %08X %ux%u on the GPU at %ux%u\n", key, w, h, o->sw, o->sh);
        if (slot == c->noff)
            c->noff++;
    }
    o->last_use = c->flip_serial;
    end_pass(c);
    c->main.color = c->color; c->main.depth = c->depth;
    c->main.color_view = c->color_view; c->main.depth_view = c->depth_view;
    c->main.w = c->w; c->main.h = c->h; c->main.sw = c->sw; c->main.sh = c->sh;
    c->main.scale = c->scale; c->main.scale_x = c->scale_x;
    c->main.squeeze2d = c->squeeze2d; c->main.squeeze_all = c->squeeze_all;
    c->main.zmax = c->zmax; c->main.sc = c->sc; memcpy(c->main.gsc, c->gsc, sizeof c->gsc);
    c->main.color_valid = c->color_valid; c->main.samples = c->samples;
    barrier(c, o->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    o->in_rt = 1;
    c->color = o->color; c->depth = o->depth;
    c->color_view = o->color_view; c->depth_view = o->depth_view;
    c->w = o->w; c->h = o->h; c->sw = o->sw; c->sh = o->sh;
    c->scale = c->scale_x = o->scale;
    c->squeeze2d = 1.0f; c->squeeze_all = 0.0f;
    if (zmax > 0.0f)
        c->zmax = zmax;
    c->color_valid = o->valid;
    c->samples = VK_SAMPLE_COUNT_1_BIT;
    c->cur_off = (int)(o - c->off);
    c->sc.offset.x = c->sc.offset.y = 0; c->sc.extent.width = o->sw; c->sc.extent.height = o->sh;
    c->gsc[0] = c->gsc[1] = 0; c->gsc[2] = w; c->gsc[3] = h;
    return 1;
}

/* 1 and its guest size if key is an offscreen target already. */
int vkr_offscreen_size(VkrContext *c, uint32_t key, uint32_t *w, uint32_t *h)
{
    int i;
    for (i = 0; i < c->noff; i++)
        if (c->off[i].key == key) {
            *w = c->off[i].w; *h = c->off[i].h;
            return 1;
        }
    return 0;
}

VkrTex *vkr_offscreen_tex(VkrContext *c, uint32_t key)
{
    int i;
    for (i = 0; i < c->noff; i++)
        if (c->off[i].key == key && c->off[i].valid) {
            if (c->cur_off == i)
                return NULL;            /* sampling the target being drawn: not supported */
            c->off[i].last_use = c->flip_serial;
            return c->off[i].tex;
        }
    return NULL;
}

VkrTex *vkr_tex_find(VkrContext *c, uint64_t key)
{
    uint32_t h;
    if (!c->hcap)
        return NULL;
    for (h = tex_slot(key, c->hcap); c->hidx[h] >= 0; h = (h + 1) & (c->hcap - 1)) {
        VkrTex *t = c->texs[c->hidx[h]];
        if (t->key == key) {
            t->last_use = c->flip_serial;
            return t;
        }
    }
    return NULL;
}

/* Into texs and the key index. */
static void tex_index_add(VkrContext *c, VkrTex *t)
{
    if (c->ntexs == c->captexs) {
        c->captexs = c->captexs ? c->captexs * 2 : 256;
        c->texs = (VkrTex **)realloc(c->texs, c->captexs * sizeof *c->texs);
    }
    c->texs[c->ntexs++] = t;
    if (c->ntexs * 2 + 2 > c->hcap)
        tex_rehash(c);
    else {
        uint32_t h = tex_slot(t->key, c->hcap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (c->hcap - 1);
        c->hidx[h] = (int32_t)(c->ntexs - 1);
    }
}

/* New contents for an existing texture of the same size (a render target
 * the title redraws every frame: its shadow maps). Draws recorded before
 * this still read the old contents -- the copy is ordered after them. */
int vkr_tex_update(VkrContext *c, VkrTex *t, uint64_t key, const uint32_t *pixels)
{
    VkBufferImageCopy cp = {0};
    uint32_t off = (c->buf_used + 15) & ~15u, bytes = t->w * t->h * 4;
    if (off + bytes > VKR_BUF_BYTES || !begin_cmd(c))
        return 0;
    end_pass(c);
    memcpy(c->buf_map + off, pixels, bytes);
    c->up_tex += off + bytes - c->buf_used;
    c->buf_used = off + bytes;
    barrier(c, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = t->w; cp.imageExtent.height = t->h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c->cmd, c->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(c, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->last_use = c->flip_serial;
    if (t->key != key) {
        t->key = key;
        tex_rehash(c);                  /* the index is by key */
    }
    return 1;
}

VkrTex *vkr_tex_create(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                       const uint32_t *pixels)
{
    return vkr_tex_create_ex(c, key, w, h, pixels, 0);
}

/* mips: also build the mip chain on the GPU (linear blits, level by level).
 * The backend decodes only a texture's base level, and without smaller
 * levels distant surfaces shimmer and anisotropic filtering has nothing to
 * blend. Not for textures later rewritten with vkr_tex_update. Off with
 * RECOMP_TEX_MIPS=0. */
VkrTex *vkr_tex_create_ex(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                          const uint32_t *pixels, int mips)
{
    VkrTex *t = (VkrTex *)calloc(1, sizeof *t);
    uint32_t levels = 1, l;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkBufferImageCopy cp = {0};
    uint32_t off = (c->buf_used + 15) & ~15u, bytes = w * h * 4;
    int m;

    if (!t || !w || !h || off + bytes > VKR_BUF_BYTES) { free(t); return NULL; }
    if (c->tex_mips < 0) {
        const char *e = getenv("RECOMP_TEX_MIPS");
        c->tex_mips = !(e && *e == '0');
    }
    if (mips && c->tex_mips)
        for (l = w > h ? w : h; l > 1; l >>= 1)
            levels++;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c->dev, &ii, NULL, &t->img) != VK_SUCCESS) { free(t); return NULL; }
    vkGetImageMemoryRequirements(c->dev, t->img, &mr);
    m = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)(m < 0 ? 0 : m);
    if (vkAllocateMemory(c->dev, &ai, NULL, &t->mem) != VK_SUCCESS) {
        vkDestroyImage(c->dev, t->img, NULL); free(t); return NULL;
    }
    vkBindImageMemory(c->dev, t->img, t->mem, 0);
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels; vi.subresourceRange.layerCount = 1;
    vkCreateImageView(c->dev, &vi, NULL, &t->view);
    t->key = key; t->w = w; t->h = h;

    if (!begin_cmd(c)) return NULL;
    end_pass(c);                         /* copies are not allowed inside a pass */
    memcpy(c->buf_map + off, pixels, bytes);
    c->up_tex += off + bytes - c->buf_used;
    c->buf_used = off + bytes;
    barrier_mips(c, t->img, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c->cmd, c->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    {
        int32_t mw = (int32_t)w, mh = (int32_t)h;
        for (l = 1; l < levels; l++) {
            VkImageBlit bl = {0};
            barrier_mips(c, t->img, l - 1, 1,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
            bl.srcSubresource.mipLevel = l - 1;
            bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = l;
            bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
            mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
            bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(c->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        if (levels > 1)
            barrier_mips(c, t->img, 0, levels - 1,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barrier_mips(c, t->img, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    t->last_use = c->flip_serial;
    if (levels > 1)
        bytes += bytes / 3;              /* the chain: a third more */
    c->tex_bytes += bytes;
    t->bytes = bytes;
    tex_index_add(c, t);
    return t;
}

int vkr_has_bc7(VkrContext *c)
{
    if (c->bc7 < 0) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
        c->bc7 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        if (!c->bc7)
            fprintf(stderr, "[VKR] this GPU cannot sample BC7: no HD textures\n");
    }
    return c->bc7;
}

/* A BC7 texture with its mip chain (HD replacements, vkr_hdtex.c): data
 * holds the levels one after another, as in a DDS file. NULL when the
 * device cannot sample BC7 or the frame's staging buffer has no room left
 * (the caller tries again on a later frame). */
uint32_t vkr_tex_flags(const VkrTex *t)
{
    return t ? t->flags : 0;
}

/* Can the device sample this block-compressed format? (cached per format) */
static int bc_ok(VkrContext *c, VkFormat f)
{
    static int8_t known[4] = {-1, -1, -1, -1};   /* BC1 RGBA, BC2, BC3 */
    int i = f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 0 : f == VK_FORMAT_BC2_UNORM_BLOCK ? 1
          : f == VK_FORMAT_BC3_UNORM_BLOCK ? 2 : -1;
    VkFormatProperties fp;
    if (f == VK_FORMAT_BC7_UNORM_BLOCK)
        return vkr_has_bc7(c);
    if (i < 0)
        return 0;
    if (known[i] < 0) {
        vkGetPhysicalDeviceFormatProperties(c->phys, f, &fp);
        known[i] = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        fprintf(stderr, "[VKR] BC%d textures %s\n", i + 1,
                known[i] ? "sampled natively (the game's DXT blocks uploaded as they are)"
                         : "not supported: decoded on the CPU");
    }
    return known[i];
}

static VkrTex *tex_create_bc(VkrContext *c, uint64_t key, uint32_t w, uint32_t h, uint32_t mips,
                             const void *data, size_t size, uint32_t flags, VkFormat fmt);

VkrTex *vkr_tex_create_bc7(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                           uint32_t mips, const void *data, size_t size, uint32_t flags)
{
    return tex_create_bc(c, key, w, h, mips, data, size, flags, VK_FORMAT_BC7_UNORM_BLOCK);
}

/* The game's own DXT1 / DXT3 / DXT5 texture (format code 1 / 2 / 3) with
 * its mip chain, uploaded as BC1 / BC2 / BC3: no CPU decode, an eighth
 * (DXT1) or a quarter of the bytes to copy and keep. NULL when the device
 * cannot sample the format or the staging buffer is full this frame. */
VkrTex *vkr_tex_create_dxt(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                           uint32_t mips, const void *data, size_t size, int dxt)
{
    VkFormat f = dxt == 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : dxt == 2 ? VK_FORMAT_BC2_UNORM_BLOCK
               : dxt == 3 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    if (f == VK_FORMAT_UNDEFINED || !bc_ok(c, f))
        return NULL;
    return tex_create_bc(c, key, w, h, mips, data, size, 0, f);
}

static VkrTex *tex_create_bc(VkrContext *c, uint64_t key, uint32_t w, uint32_t h, uint32_t mips,
                             const void *data, size_t size, uint32_t flags, VkFormat fmt)
{
    const uint32_t bb = fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;   /* bytes per block */
    const int hd = fmt == VK_FORMAT_BC7_UNORM_BLOCK;
    VkrTex *t;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkBufferImageCopy cp[16];
    uint32_t off = (c->buf_used + 15) & ~15u, l, lw, lh;
    size_t pos = 0;
    int m;

    if (!bc_ok(c, fmt) || !w || !h || !mips || mips > 16 || off + size > VKR_BUF_BYTES - VKR_DRAW_RESERVE)
        return NULL;
    for (l = 0, lw = w, lh = h; l < mips; l++) {       /* the levels fit in data */
        pos += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * bb;
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    if (pos > size || !(t = (VkrTex *)calloc(1, sizeof *t)))
        return NULL;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = mips; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c->dev, &ii, NULL, &t->img) != VK_SUCCESS) { free(t); return NULL; }
    vkGetImageMemoryRequirements(c->dev, t->img, &mr);
    m = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)(m < 0 ? 0 : m);
    if (vkAllocateMemory(c->dev, &ai, NULL, &t->mem) != VK_SUCCESS) {
        vkDestroyImage(c->dev, t->img, NULL); free(t); return NULL;
    }
    vkBindImageMemory(c->dev, t->img, t->mem, 0);
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mips; vi.subresourceRange.layerCount = 1;
    vkCreateImageView(c->dev, &vi, NULL, &t->view);
    t->key = key; t->w = w; t->h = h; t->bytes = pos; t->flags = flags;

    if (!begin_cmd(c)) { tex_free_one(c, t); return NULL; }
    end_pass(c);
    memcpy(c->buf_map + off, data, pos);
    if (hd)
        c->up_hd += off + (uint32_t)pos - c->buf_used;
    else
        c->up_tex += off + (uint32_t)pos - c->buf_used;
    c->buf_used = off + (uint32_t)pos;
    memset(cp, 0, sizeof cp);
    for (l = 0, lw = w, lh = h, pos = 0; l < mips; l++) {
        cp[l].bufferOffset = off + pos;
        cp[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp[l].imageSubresource.mipLevel = l;
        cp[l].imageSubresource.layerCount = 1;
        cp[l].imageExtent.width = lw; cp[l].imageExtent.height = lh; cp[l].imageExtent.depth = 1;
        pos += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * bb;
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    barrier_mips(c, t->img, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(c->cmd, c->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, cp);
    barrier_mips(c, t->img, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->last_use = c->flip_serial;
    c->tex_bytes += t->bytes;
    tex_index_add(c, t);
    return t;
}

/* NV2A stencil state -> dynamic Vulkan stencil state (both faces). */
static VkStencilOp stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static void set_stencil(VkrContext *c, const VkrDrawState *st)
{
    VkStencilFaceFlags f = VK_STENCIL_FACE_FRONT_AND_BACK;
    int on = c->has_stencil && st->stencil_test;
    vkCmdSetStencilTestEnable(c->cmd, on ? VK_TRUE : VK_FALSE);
    if (!c->has_stencil)
        return;
    vkCmdSetStencilOp(c->cmd, f, stencil_op(st->stencil_fail), stencil_op(st->stencil_zpass),
                      stencil_op(st->stencil_zfail),
                      st->stencil_func >= 0x200 && st->stencil_func <= 0x207
                          ? (VkCompareOp)(st->stencil_func - 0x200) : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(c->cmd, f, st->stencil_read_mask);
    vkCmdSetStencilWriteMask(c->cmd, f, on ? st->stencil_write_mask : 0);
    vkCmdSetStencilReference(c->cmd, f, st->stencil_ref);
}

/* ── Redundant state filter ─────────────────────────────────────────
 *
 * A draw used to record ~17 commands whatever the previous draw had set:
 * pipeline, three or four push-constant ranges, the push descriptor set
 * (4 samplers + the combiner uniforms), depth x3, stencil x5, blend
 * constants and both buffer binds. On the Switch NVK records all of that on
 * the A57, and a race has ~1,000 draws a frame. Now each piece is recorded
 * only when it differs from what this rendering pass last set (c->bnd, reset
 * in begin_pass); the buffers are bound once per pass at offset 0 and each
 * draw addresses its data with firstIndex / vertexOffset; an unchanged
 * uniform block is reused rather than uploaded again (which also keeps the
 * descriptor set the same). RECOMP_VKR_NO_DEDUP=1: every command, as before. */
static int vkr_dedup(VkrContext *c)
{
    if (c->dedup < 0) {
        c->dedup = getenv("RECOMP_VKR_NO_DEDUP") == NULL;
        fprintf(stderr, "[VKR] redundant draw-state filter %s\n", c->dedup ? "on" : "off");
    }
    return c->dedup;
}

/* Offset of st->u in the upload buffer: the last upload if the block is the
 * same, else a new one at *end (advanced). */
static uint32_t ubo_place(VkrContext *c, const VkrDrawState *st, uint32_t *end, int *fresh)
{
    uint32_t ub;
    if (vkr_dedup(c) && c->ubo_serial == c->upload_serial
        && !memcmp(&c->ubo_last, &st->u, sizeof st->u)) {
        *fresh = 0;
        return c->ubo_off;
    }
    ub = (*end + 255) & ~255u;
    *end = ub + (uint32_t)sizeof st->u;
    *fresh = 1;
    return ub;
}

static void ubo_commit(VkrContext *c, const VkrDrawState *st, uint32_t ub, int fresh)
{
    if (!fresh)
        return;
    memcpy(c->buf_map + ub, &st->u, sizeof st->u);
    c->ubo_last = st->u;
    c->ubo_off = ub;
    c->ubo_serial = c->upload_serial;
}

/* Everything a draw sets before vkCmdDrawIndexed, filtered. pcu: the GPU
 * vertex program's bases (vkr_draw_vsh), NULL for CPU-transformed vertices. */
static void bind_draw_state(VkrContext *c, const VkrDrawState *st, VkPipeline pipe,
                            uint32_t ub, const uint32_t *pcu)
{
    struct VkrBound *b = &c->bnd;
    float pcv[8], sq[4];
    uint32_t pcf[5], sten[8], k, nds = pcu ? 3u : 2u;
    VkDescriptorImageInfo di[4];
    VkCompareOp dop;
    int ds_same;

    if (!vkr_dedup(c) || !b->valid) {
        memset(b, 0, sizeof *b);
        b->valid = 1;
    }
    if (b->pipe != pipe) {
        vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        b->pipe = pipe;
    }
    pcv[0] = 1.0f / (float)c->w; pcv[1] = 1.0f / (float)c->h;
    pcv[2] = 1.0f / c->zmax;
    pcv[3] = st->u.fog_enable ? st->u.fog_param[2] : 0.0f;
    pcv[4] = st->u.fog_param[0]; pcv[5] = st->u.fog_param[1];
    pcv[6] = st->point_size * c->scale; pcv[7] = st->point_params ? c->scale : 0.0f;
    if (!b->have_pcv || memcmp(b->pcv, pcv, sizeof pcv)) {
        vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pcv);
        memcpy(b->pcv, pcv, sizeof pcv); b->have_pcv = 1;
    }
    if (pcu && (!b->have_pcu || memcmp(b->pcu, pcu, sizeof b->pcu))) {
        vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 32, 16, pcu);
        memcpy(b->pcu, pcu, sizeof b->pcu); b->have_pcu = 1;
    }
    sq[0] = c->squeeze2d; sq[1] = c->squeeze_all;
    sq[2] = c->squeeze_all > 0.5f ? 0.0f : st->hud_shift;
    sq[3] = (float)st->keep_edges;      /* 0, 1 (edge vertices stay), 2 (stretched whole) */
    if (!b->have_sq || memcmp(b->sq, sq, sizeof sq)) {
        vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 48, 16, sq);
        memcpy(b->sq, sq, sizeof sq); b->have_sq = 1;
    }
    pcf[0] = st->alpha_func; pcf[1] = st->alpha_ref;
    {
        /* W-buffering: depth is the interpolated clip w in depth-buffer
         * units, scaled into 0..1 like z. */
        float ws = st->w_buffer ? 1.0f / c->zmax : 0.0f;
        memcpy(&pcf[2], &ws, 4);
    }
    /* bit 0: point sprite; bits 8+: MSAA samples (alpha-test coverage) */
    pcf[3] = (uint32_t)(st->topology == VKR_TOPO_POINTS && st->point_sprite)
           | (uint32_t)(st->clip2d != 0) << 1 | (uint32_t)c->samples << 8;
    pcf[4] = (uint32_t)st->rider;
    if (!b->have_pcf || memcmp(b->pcf, pcf, sizeof pcf)) {
        vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 20, pcf);
        memcpy(b->pcf, pcf, sizeof pcf); b->have_pcf = 1;
    }

    ds_same = b->have_ds && b->ds_n == nds && b->ub == ub;
    for (k = 0; k < 4; k++) {
        VkrTex *t = st->tex[k] ? st->tex[k] : c->white;
        di[k].sampler = get_sampler(c, st->addr_u[k], st->addr_v[k], st->linear[k]);
        di[k].imageView = t->view;
        di[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (di[k].sampler != b->di[k].sampler || di[k].imageView != b->di[k].imageView)
            ds_same = 0;
    }
    if (!ds_same) {
        VkDescriptorBufferInfo bi, sb;
        VkWriteDescriptorSet w[3];
        bi.buffer = c->buf; bi.offset = ub; bi.range = sizeof st->u;
        sb.buffer = c->buf; sb.offset = 0; sb.range = VK_WHOLE_SIZE;
        memset(w, 0, sizeof w);
        w[0].sType = w[1].sType = w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstBinding = 0; w[0].descriptorCount = 4;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = di;
        w[1].dstBinding = 4; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
        w[2].dstBinding = 5; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &sb;
        vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, nds, w);
        memcpy(b->di, di, sizeof di);
        b->ub = ub; b->ds_n = nds; b->have_ds = 1;
    }

    dop = st->depth_func >= 0x200 && st->depth_func <= 0x207
              ? (VkCompareOp)(st->depth_func - 0x200) : VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!b->have_depth || b->depth_test != (st->depth_test != 0)) {
        vkCmdSetDepthTestEnable(c->cmd, st->depth_test ? VK_TRUE : VK_FALSE);
        b->depth_test = st->depth_test != 0;
    }
    if (!b->have_depth || b->depth_write != (st->depth_write != 0)) {
        vkCmdSetDepthWriteEnable(c->cmd, st->depth_write ? VK_TRUE : VK_FALSE);
        b->depth_write = st->depth_write != 0;
    }
    if (!b->have_depth || b->depth_op != dop) {
        vkCmdSetDepthCompareOp(c->cmd, dop);
        b->depth_op = dop;
    }
    b->have_depth = 1;

    memset(sten, 0, sizeof sten);
    sten[0] = (uint32_t)(c->has_stencil && st->stencil_test);
    if (c->has_stencil) {
        sten[1] = st->stencil_fail; sten[2] = st->stencil_zpass; sten[3] = st->stencil_zfail;
        sten[4] = st->stencil_func; sten[5] = st->stencil_read_mask;
        sten[6] = sten[0] ? st->stencil_write_mask : 0; sten[7] = st->stencil_ref;
    }
    if (!b->have_stencil || memcmp(b->stencil, sten, sizeof sten)) {
        set_stencil(c, st);
        memcpy(b->stencil, sten, sizeof sten); b->have_stencil = 1;
    }
    if (!b->have_blend || memcmp(b->blend, st->blend_color, sizeof b->blend)) {
        vkCmdSetBlendConstants(c->cmd, st->blend_color);
        memcpy(b->blend, st->blend_color, sizeof b->blend); b->have_blend = 1;
    }
    /* The upload buffer bound once at offset 0 (bit 0 vertices, bit 1 indices) */
    if (!pcu && !(b->bufs & 1)) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(c->cmd, 0, 1, &c->buf, &zero);
        b->bufs |= 1;
    }
    if (!(b->bufs & 2)) {
        vkCmdBindIndexBuffer(c->cmd, c->buf, 0, VK_INDEX_TYPE_UINT32);
        b->bufs |= 2;
    }
}

int vkr_draw(VkrContext *c, const VkrVertex *v, uint32_t nverts,
             const uint32_t *idx, uint32_t nidx, const VkrDrawState *st)
{
    uint32_t vb, ib, ub, end;
    VkPipeline pipe;
    int fresh;

    pipe = get_pipeline(c, st, 0);
    if (!pipe) return 0;
    if (!c->white) {
        static const uint32_t white = 0xFFFFFFFFu;
        c->white = vkr_tex_create(c, 0x77686974ull, 1, 1, &white);
        if (!c->white) return 0;
    }
    if (!begin_cmd(c)) return 0;
    begin_pass(c);
    /* Vertices at a multiple of their size: the buffer stays bound at 0 and
     * the draw starts at vertexOffset = vb / sizeof *v. */
    vb = ((c->buf_used + (uint32_t)sizeof *v - 1) / (uint32_t)sizeof *v) * (uint32_t)sizeof *v;
    ib = vb + nverts * (uint32_t)sizeof *v;
    end = ib + nidx * 4;
    ub = ubo_place(c, st, &end, &fresh);
    if (end > VKR_BUF_BYTES) {
        up_full(c, end - c->buf_used);
        return 0;
    }
    memcpy(c->buf_map + vb, v, nverts * sizeof *v);
    memcpy(c->buf_map + ib, idx, nidx * 4);
    ubo_commit(c, st, ub, fresh);
    c->up_draw += end - c->buf_used;
    c->buf_used = end;

    bind_draw_state(c, st, pipe, ub, NULL);
    vkCmdDrawIndexed(c->cmd, nidx, 1, ib / 4, (int32_t)(vb / (uint32_t)sizeof *v), 0);
    prof_mark(c, st, nidx, 0);
    return 1;
}

/* The same draw with the vertex program run on the GPU (xbox_vsh.vert): in is
 * nverts x nin float4 inputs; prog is nins x 8 words (nv2a_vsh_export);
 * consts the 192 constants. The program and constants are uploaded once per
 * recording while they stay the same. */
int vkr_draw_vsh(VkrContext *c, const float *in, uint32_t nverts, uint32_t nin,
                 const uint32_t *prog, uint32_t nins, uint32_t prog_hash,
                 const float (*consts)[4], const uint32_t *idx, uint32_t nidx,
                 const VkrDrawState *st)
{
    uint32_t vb, ib, ub, end, po, co;
    uint32_t in_bytes = nverts * (nin ? nin : 1) * 16, prog_bytes = nins * 32;
    VkPipeline pipe;
    uint32_t pcu[4];
    int fresh;

    pipe = get_pipeline(c, st, 1);
    if (!pipe) return 0;
    if (!c->white) {
        static const uint32_t white = 0xFFFFFFFFu;
        c->white = vkr_tex_create(c, 0x77686974ull, 1, 1, &white);
        if (!c->white) return 0;
    }
    if (!begin_cmd(c)) return 0;
    begin_pass(c);
    if (c->vsh_serial != c->upload_serial) {
        c->vsh_serial = c->upload_serial;
        c->vsh_const_off = c->vsh_prog_off = 0xFFFFFFFFu;
    }
    end = (c->buf_used + 15) & ~15u;
    co = c->vsh_const_off;
    if (co == 0xFFFFFFFFu || memcmp(c->vsh_consts, consts, sizeof c->vsh_consts)) {
        co = end;
        end += (uint32_t)sizeof c->vsh_consts;
    }
    po = c->vsh_prog_off;
    if (po == 0xFFFFFFFFu || c->vsh_prog_hash != prog_hash || c->vsh_prog_n != nins) {
        po = end;
        end += prog_bytes;
    }
    vb = end;
    ib = vb + in_bytes;
    end = ib + nidx * 4;
    ub = ubo_place(c, st, &end, &fresh);
    if (end > VKR_BUF_BYTES) {
        up_full(c, end - c->buf_used);
        return 0;
    }
    if (co != c->vsh_const_off) {
        memcpy(c->buf_map + co, consts, sizeof c->vsh_consts);
        memcpy(c->vsh_consts, consts, sizeof c->vsh_consts);
        c->vsh_const_off = co;
    }
    if (po != c->vsh_prog_off) {
        memcpy(c->buf_map + po, prog, prog_bytes);
        c->vsh_prog_off = po;
        c->vsh_prog_hash = prog_hash;
        c->vsh_prog_n = nins;
    }
    memcpy(c->buf_map + vb, in, in_bytes);
    memcpy(c->buf_map + ib, idx, nidx * 4);
    ubo_commit(c, st, ub, fresh);
    c->up_draw += end - c->buf_used;
    c->buf_used = end;

    pcu[0] = vb / 16; pcu[1] = nin ? nin : 1; pcu[2] = po / 16; pcu[3] = co / 16;
    bind_draw_state(c, st, pipe, ub, pcu);
    /* gl_VertexIndex stays the index itself (vertexOffset 0): the program
     * fetches its inputs from pcu[0] in the storage buffer. */
    vkCmdDrawIndexed(c->cmd, nidx, 1, ib / 4, 0, 0);
    prof_mark(c, st, nidx, 1);
    return 1;
}

/* Widescreen target: 1 = pillarbox (narrow every vertex to the 4:3 middle),
 * 0 = widescreen (only 2D ones). */
void vkr_set_pillarbox(VkrContext *c, int on)
{
    float v = on && c->squeeze2d != 1.0f ? 1.0f : 0.0f;
    if (v != c->squeeze_all) {
        c->squeeze_all = v;
        apply_scissor(c);
    }
}

/* The target's shape (width / height): wider than 4:3 widens the target
 * and narrows 2D back to 4:3; anything else is the plain 4:3 target. Takes
 * effect at the next vkr_set_target. */
void vkr_set_aspect(VkrContext *c, float aspect)
{
    c->scale_x = c->scale;
    c->squeeze2d = 1.0f;
    if (aspect > 4.0f / 3.0f + 0.01f) {
        float f = aspect * 0.75f;
        c->scale_x = c->scale * f;
        c->squeeze2d = 1.0f / f;
    }
}

/* Docked / handheld (vkr_backend.c, between frames): the internal
 * resolution as a scale of the guest's 480 lines, and on the Switch the
 * window / swapchain size. A new scale rebuilds the targets (the frame after
 * starts blank); a new window size destroys the swapchain, sets the window's
 * dimensions and lets the next present build it again at that size. 1 if
 * anything changed. */
int vkr_set_display(VkrContext *c, uint32_t win_w, uint32_t win_h, float scale)
{
    int changed = 0;
#if defined(__SWITCH__)
    if (!c->pr_on)          /* the CPU presenter: a fixed 1280x720 framebuffer */
        return 0;
#endif
    if (scale >= 0.5f && scale <= 8.0f && fabsf(scale - c->scale) > 0.001f) {
        float f = c->scale > 0.0f ? c->scale_x / c->scale : 1.0f;   /* the widescreen factor */
        c->scale = scale;
        c->scale_x = scale * f;
        if (c->color && c->w && c->h && !vkr_set_target(c, c->w, c->h, c->zmax))
            fprintf(stderr, "[VKR] display: the %.2fx target could not be made\n", scale);
        changed = 1;
    }
#if defined(__SWITCH__)
    if (win_w && win_h && c->pr_on && c->swap
        && (win_w != c->swap_ext.width || win_h != c->swap_ext.height)) {
        uint32_t i, rc;
        vkDeviceWaitIdle(c->dev);
        for (i = 0; i < c->swap_n; i++) {
            if (c->swap_view[i]) vkDestroyImageView(c->dev, c->swap_view[i], NULL);
            c->swap_view[i] = VK_NULL_HANDLE;
        }
        c->swap_n = 0;
        vkDestroySwapchainKHR(c->dev, c->swap, NULL);
        c->swap = VK_NULL_HANDLE;
        rc = nwindowSetDimensions(nwindowGetDefault(), win_w, win_h);
        fprintf(stderr, "[VKR] display: window %ux%u%s\n", win_w, win_h,
                rc ? " (refused: the swapchain keeps its size)" : "");
        c->pr_recreate = 1;
        changed = 1;
    }
#else
    (void)win_w; (void)win_h;
#endif
    return changed;
}

uint32_t vkr_width(const VkrContext *c) { return c->w; }
uint32_t vkr_height(const VkrContext *c) { return c->h; }

/* At a flip, c->color in TRANSFER_SRC: crt_in = c->color with mips, then
 * crt.frag into crt_out, left in TRANSFER_SRC for the read-back. */
static void crt_pass(VkrContext *c, VkImage src)
{
    VkImageCopy ic = {0};
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkDescriptorImageInfo dii = {c->crt_samp, c->crt_in_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkViewport vp;
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};
    struct { float rect[4], tsize[2], curv, scan, mask, vig, ab, glow, bright, time,
             mpitch, lines, flicker, noise; } pc;
    int32_t mw = (int32_t)c->sw, mh = (int32_t)c->sh;
    uint32_t i;

    barrier_mips(c, c->crt_in, 0, c->crt_levels,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    vkCmdCopyImage(c->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    for (i = 1; i < c->crt_levels; i++) {
        VkImageBlit bl = {0};
        barrier_mips(c, c->crt_in, i - 1, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.srcSubresource.mipLevel = i - 1;
        bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = i;
        bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier_mips(c, c->crt_in, c->crt_levels - 1, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, c->crt_in, 0, c->crt_levels,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->crt_out, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* The picture: the 4:3 middle when pillarboxed, else the whole target. */
    pc.rect[2] = c->squeeze_all > 0.5f ? (float)c->w * c->scale : (float)c->sw;
    pc.rect[3] = (float)c->sh;
    pc.rect[0] = ((float)c->sw - pc.rect[2]) * 0.5f;
    pc.rect[1] = 0.0f;
    pc.tsize[0] = (float)c->sw; pc.tsize[1] = (float)c->sh;
    pc.curv = c->crt_p[0];
    pc.scan = c->crt_p[1]; pc.mask = c->crt_p[2]; pc.vig = c->crt_p[3];
    pc.ab = c->crt_p[4]; pc.glow = c->crt_p[5]; pc.bright = c->crt_p[6];
    pc.time = (float)(vkr_now_ms() * 1e-3);
    /* One RGB triad per guest pixel (auto), at least 3 target pixels: a
     * slot under one pixel wide only aliases. */
    pc.mpitch = c->crt_p[7] > 0.0f ? c->crt_p[7] : pc.rect[2] / (float)c->w;
    if (pc.mpitch < 3.0f) pc.mpitch = 3.0f;
    pc.lines = (float)c->h;          /* the guest's scanlines */
    pc.flicker = c->crt_p[8]; pc.noise = c->crt_p[9];

    ca.imageView = c->crt_out_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vp.x = pc.rect[0]; vp.y = 0.0f; vp.width = pc.rect[2]; vp.height = pc.rect[3];
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->crt_pipe);
    w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->crt_layout, 0, 1, &w);
    vkCmdPushConstants(c->cmd, c->crt_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
    barrier(c, c->crt_out, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
}

/* RECOMP_MBLUR: scene.frag into c->color, on a frame with a camera; it reads
 * a copy of the frame (sc_in) and the depth. c->color in COLOR_ATTACHMENT
 * and depth in DEPTH_ATTACHMENT before and after. */
static void scene_pass(VkrContext *c)
{
    static const uint32_t bind[3] = {0, 1, 4};   /* img, dep, S */
    VkDescriptorImageInfo ii[2];
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet wr[3];
    VkImageCopy ic = {0};
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};
    float u[68];
    uint32_t off = (c->buf_used + 255) & ~255u, k;
    if (off + sizeof u > VKR_BUF_BYTES)
        return;
    memcpy(u, c->cam.inv_m, 64);
    memcpy(u + 16, c->cam.prev_m, 64);
    u[32] = c->sc_p[0]; u[33] = c->sc_p[1]; u[34] = c->sc_p[2]; u[35] = c->tags ? 1.0f : 0.0f;
    u[36] = (float)c->w; u[37] = (float)c->h; u[38] = 1.0f / c->scale_x; u[39] = 1.0f / c->scale;
    u[40] = c->zmax; u[41] = (float)c->sw; u[42] = (float)c->sh;
    u[43] = (float)((long long)vkr_now_ms() % 100000) * 1e-3f;
    u[44] = c->sc_p[3]; u[45] = c->sc_p[4]; u[46] = u[47] = 0.0f;
    {
        /* speed blur: none below s0, full from s1 (smoothstep) */
        float t = c->sc_p[7] > c->sc_p[6] ? (c->cam.speed - c->sc_p[6]) / (c->sc_p[7] - c->sc_p[6]) : 0.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        u[48] = c->cam.foe[0]; u[49] = c->cam.foe[1];
        u[50] = c->cam.speed > 0.0f ? c->sc_p[5] * t * t * (3.0f - 2.0f * t) : 0.0f;
        u[51] = c->sc_p[8];
    }
    for (k = 0; k < 8; k++) {           /* the riders' own motion (tags 1..7) */
        u[52 + 2 * k] = c->cam.rider_v[k][0];
        u[53 + 2 * k] = c->cam.rider_v[k][1];
    }
    memcpy(c->buf_map + off, u, sizeof u);
    c->buf_used = off + (uint32_t)sizeof u;

    /* the frame so far, copied for the pass to read */
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, c->sc_in, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    if (c->samples != VK_SAMPLE_COUNT_1_BIT) {
        /* MSAA: the pass reads the resolved frame (a multisampled image
         * cannot be copied into a plain one) */
        VkImageResolve rv = {0};
        rv.srcSubresource = ic.srcSubresource; rv.dstSubresource = ic.dstSubresource;
        rv.extent = ic.extent;
        vkCmdResolveImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          c->sc_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
    } else
        vkCmdCopyImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->sc_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    barrier(c, c->sc_in, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(c, c->depth, ds_aspect(c),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    ii[0].sampler = c->sc_lin; ii[0].imageView = c->sc_in_view;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = c->sc_near; ii[1].imageView = c->depth_sview;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    bi.buffer = c->buf; bi.offset = off; bi.range = sizeof u;
    memset(wr, 0, sizeof wr);
    for (k = 0; k < 3; k++) {
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstBinding = bind[k]; wr[k].descriptorCount = 1;
        wr[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (k < 2) wr[k].pImageInfo = &ii[k]; else wr[k].pBufferInfo = &bi;
    }
    ca.imageView = c->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->sc_pipe);
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->sc_layout, 0, 3, wr);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);

    barrier(c, c->depth, ds_aspect(c),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

/* Before the first 2D draw after the world (vkr_backend.c): the scene pass
 * on what is drawn so far, so the HUD is drawn on top of it untouched.
 * Once per frame; a frame that never gets here has it at the flip. */
void vkr_scene_run(VkrContext *c)
{
    if (!c->scene || !c->cam_valid || c->cam.off || c->scene_done || !c->sc_in || !c->recording)
        return;
    if (c->cur_off >= 0)
        return;                          /* drawing a shadow map: the frame comes later */
    end_pass(c);
    scene_pass(c);
    c->scene_done = 1;
}

/* ── Speed lines (SSX3NX_BOOST_LINES) ──────────────────────────────────
 * speedlines.frag as a fullscreen triangle (crt.vert), blended over the
 * frame (premultiplied white): thin rays around the screen edges. No
 * descriptors, a 32-byte push constant. */
static int make_speedlines(VkrContext *c)
{
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs = shader(c, crt_vert_spv, crt_vert_spv_len);
    VkShaderModule fs = shader(c, speedlines_frag_spv, speedlines_frag_spv_len);
    VkResult r;
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(c->dev, vs, NULL);
        if (fs) vkDestroyShaderModule(c->dev, fs, NULL);
        return 0;
    }
    {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        di.bindingCount = 1; di.pBindings = &b;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->sl_dsl) != VK_SUCCESS
            || vkCreateSampler(c->dev, &si, NULL, &c->sl_lin) != VK_SUCCESS) {
            vkDestroyShaderModule(c->dev, vs, NULL); vkDestroyShaderModule(c->dev, fs, NULL);
            return 0;
        }
    }
    li.setLayoutCount = 1; li.pSetLayouts = &c->sl_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(c->dev, &li, NULL, &c->sl_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(c->dev, vs, NULL); vkDestroyShaderModule(c->dev, fs, NULL);
        return 0;
    }
    stg[0].module = vs; stg[1].module = fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    ms.rasterizationSamples = c->samples;   /* into the (MSAA) colour target */
    cba.blendEnable = VK_TRUE;               /* premultiplied: rgb*1 + dst*(1-a) */
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   /* keep the frame's alpha (tags) */
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->sl_layout;
    r = vkCreateGraphicsPipelines(c->dev, c->pcache, 1, &pi, NULL, &c->sl_pipe);
    vkDestroyShaderModule(c->dev, vs, NULL);
    vkDestroyShaderModule(c->dev, fs, NULL);
    if (r != VK_SUCCESS) {
        c->sl_pipe = VK_NULL_HANDLE;
        return 0;
    }
    fprintf(stderr, "[VKR] boost speed lines ready (SSX3NX_BOOST_LINES)\n");
    return 1;
}

/* Speed lines over the frame drawn so far: strength 0..1 (0 = nothing),
 * time in seconds (they flicker and run outwards), the focus (where the
 * rays come from) as a fraction of the frame. Before the HUD (vkr_backend.c
 * scene_before_2d). */
void vkr_speedlines(VkrContext *c, float strength, float t, float fx, float fy)
{
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};
    VkImageCopy ic = {0};
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkImageView src_view = c->sc_in_view;   /* or the half-size copy */
    float pc[8];
    if (strength <= 0.001f || !c->recording || c->cur_off >= 0 || !c->color_valid || !c->sc_in)
        return;
    if (!c->sl_tried) {
        c->sl_tried = 1;
        if (!make_speedlines(c))
            fprintf(stderr, "[VKR] boost speed lines: pipeline failed, off\n");
    }
    if (!c->sl_pipe)
        return;
    end_pass(c);
    if (c->sl_noblur) {
        /* lines only: the frame copy is never read, just a valid binding */
        barrier(c, c->sc_in, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        goto bind;
    }
    /* the frame so far, copied for the edge blur to read (as scene_pass) */
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    if (c->sl_half && c->samples == VK_SAMPLE_COUNT_1_BIT) {
        /* half size: one filtered blit straight from the frame */
        VkImageBlit bl = {0};
        bl.srcSubresource = ic.srcSubresource; bl.dstSubresource = ic.dstSubresource;
        bl.srcOffsets[1].x = (int32_t)c->sw; bl.srcOffsets[1].y = (int32_t)c->sh; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int32_t)(c->sw / 2); bl.dstOffsets[1].y = (int32_t)(c->sh / 2);
        bl.dstOffsets[1].z = 1;
        barrier(c, c->sl_half, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdBlitImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->sl_half, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrier(c, c->sl_half, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        src_view = c->sl_half_view;
    } else {
        barrier(c, c->sc_in, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (c->samples != VK_SAMPLE_COUNT_1_BIT) {
            VkImageResolve rv = {0};
            rv.srcSubresource = ic.srcSubresource; rv.dstSubresource = ic.dstSubresource;
            rv.extent = ic.extent;
            vkCmdResolveImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              c->sc_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
        } else
            vkCmdCopyImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c->sc_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        barrier(c, c->sc_in, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
bind:
    ii.sampler = c->sl_lin; ii.imageView = src_view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    pc[0] = strength; pc[1] = t; pc[2] = fx * (float)c->sw; pc[3] = fy * (float)c->sh;
    pc[4] = (float)c->sw; pc[5] = (float)c->sh; pc[6] = (float)c->sh / 720.0f;
    pc[7] = c->sl_noblur ? 0.0f : 1.0f;     /* the edge blur on */
    ca.imageView = c->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->sl_pipe);
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->sl_layout, 0, 1, &wr);
    vkCmdPushConstants(c->cmd, c->sl_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
    /* the next draw begins a pass again (begin_pass: bindings unknown) */
}

/* 1: the next flip is rendered but not shown (vkr_flip_submit): the screen
 * keeps the last frame (a transition's black frame). Swapchain path only. */
void vkr_set_hold(VkrContext *c, int hold)
{
    c->pr_hold = hold;
}

/* ── Switch: the swapchain (see VkrContext.present) ─────────────────── */
#if defined(__SWITCH__)
extern int xbox_FramebufferHandOver(void);                      /* fb_present_nx.c */
extern void xbox_FramebufferTakeBack(void);
extern unsigned xbox_FramebufferOverlayPixels(uint32_t *rgba, int w, int h);
extern unsigned xbox_FramebufferGammaTexels(uint32_t *out256);  /* fb_gamma.c */
extern unsigned xbox_FramebufferGammaSerial(void);
extern const uint8_t *xbox_FramebufferGammaLut(void);
#define PR_OV_W 640                     /* the overlay's corner: 3 lines of 2x text */
#define PR_OV_H 80

static void pr_destroy_swap(VkrContext *c)
{
    uint32_t i;
    for (i = 0; i < c->swap_n; i++) {
        if (c->swap_view[i]) vkDestroyImageView(c->dev, c->swap_view[i], NULL);
        c->swap_view[i] = VK_NULL_HANDLE;
    }
    if (c->swap) vkDestroySwapchainKHR(c->dev, c->swap, NULL);
    c->swap = VK_NULL_HANDLE;
    c->swap_n = 0;
}

static int pr_make_swap(VkrContext *c)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[32];
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR old = c->swap;
    uint32_t nf = 32, i, n;
    VkResult r;
    CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, c->surf, &caps));
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, c->surf, &nf, fmts) < 0 || !nf) return 0;
    c->swap_fmt = VK_FORMAT_UNDEFINED;
    for (i = 0; i < nf && !c->swap_fmt; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { c->swap_fmt = fmts[i].format; ci.imageColorSpace = fmts[i].colorSpace; }
    for (i = 0; i < nf && !c->swap_fmt; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { c->swap_fmt = fmts[i].format; ci.imageColorSpace = fmts[i].colorSpace; }
    if (!c->swap_fmt) { c->swap_fmt = fmts[0].format; ci.imageColorSpace = fmts[0].colorSpace; }
    c->swap_ext = caps.currentExtent;
    if (c->swap_ext.width == 0xFFFFFFFFu || !c->swap_ext.width || !c->swap_ext.height) {
        c->swap_ext.width = 1280; c->swap_ext.height = 720;
#if defined(__SWITCH__)
        {
            /* the window's own size (vkr_set_display: 1920x1080 docked) */
            uint32_t ww = 0, wh = 0;
            if (!nwindowGetDimensions(nwindowGetDefault(), &ww, &wh) && ww && wh) {
                c->swap_ext.width = ww; c->swap_ext.height = wh;
            }
        }
#endif
    }
    /* three: a 30 FPS race never waits for one, and the executor does not
     * stall on the display */
    n = 3;
    if (n < caps.minImageCount) n = caps.minImageCount;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    ci.surface = c->surf;
    ci.minImageCount = n;
    ci.imageFormat = c->swap_fmt;
    ci.imageExtent = c->swap_ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  /* vsync, as the CPU presenter */
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(c->dev, &ci, NULL, &c->swap);
    for (i = 0; i < c->swap_n; i++) {
        if (c->swap_view[i]) vkDestroyImageView(c->dev, c->swap_view[i], NULL);
        c->swap_view[i] = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(c->dev, old, NULL);
    c->swap_n = 0;
    if (r != VK_SUCCESS) { vk_failed("vkCreateSwapchainKHR", r, __FILE__, __LINE__); c->swap = VK_NULL_HANDLE; return 0; }
    c->swap_n = 8;
    CK(vkGetSwapchainImagesKHR(c->dev, c->swap, &c->swap_n, c->swap_img));
    for (i = 0; i < c->swap_n; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vi.image = c->swap_img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = c->swap_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        CK(vkCreateImageView(c->dev, &vi, NULL, &c->swap_view[i]));
        if (!c->swap_done[i])
            CK(vkCreateSemaphore(c->dev, &si, NULL, &c->swap_done[i]));
    }
    c->pr_recreate = 0;
    return 1;
}

static int pr_make_pipe(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs, fs;
    VkResult r;

    if (c->pr_pipe && c->pr_pipe_fmt == c->swap_fmt)
        return 1;
    if (c->pr_pipe) { vkDestroyPipeline(c->dev, c->pr_pipe, NULL); c->pr_pipe = VK_NULL_HANDLE; }
    if (!c->pr_layout) {
        di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        di.bindingCount = 3; di.pBindings = b;
        CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->pr_dsl));
        li.setLayoutCount = 1; li.pSetLayouts = &c->pr_dsl;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
        CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->pr_layout));
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        CK(vkCreateSampler(c->dev, &si, NULL, &c->pr_lin));
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        CK(vkCreateSampler(c->dev, &si, NULL, &c->pr_near));
    }
    vs = shader(c, crt_vert_spv, crt_vert_spv_len);
    fs = shader(c, present_frag_spv, present_frag_spv_len);
    if (!vs || !fs) {
        fprintf(stderr, "[VKR] present shader modules failed\n");
        if (vs) vkDestroyShaderModule(c->dev, vs, NULL);
        if (fs) vkDestroyShaderModule(c->dev, fs, NULL);
        return 0;
    }
    stg[0].module = vs; stg[1].module = fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &c->swap_fmt;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->pr_layout;
    r = vkCreateGraphicsPipelines(c->dev, c->pcache, 1, &pi, NULL, &c->pr_pipe);
    vkDestroyShaderModule(c->dev, vs, NULL);
    vkDestroyShaderModule(c->dev, fs, NULL);
    if (r != VK_SUCCESS) { vk_failed("vkCreateGraphicsPipelines (present)", r, __FILE__, __LINE__); return 0; }
    c->pr_pipe_fmt = c->swap_fmt;
    return 1;
}

static int pr_make_res(VkrContext *c)
{
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    int f;
    if (!c->pr_ramp
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->pr_ramp, &c->pr_ramp_mem, &c->pr_ramp_view, 256, 1))
        return 0;
    if (!c->pr_ov
        && !make_image(c, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->pr_ov, &c->pr_ov_mem, &c->pr_ov_view, PR_OV_W, PR_OV_H))
        return 0;
    for (f = 0; f < VKR_FRAMES; f++)
        if (!c->acq[f])
            CK(vkCreateSemaphore(c->dev, &si, NULL, &c->acq[f]));
    return 1;
}

/* The first flip: the window from the CPU presenter, then the swapchain. */
static int pr_start(VkrContext *c)
{
    if (!xbox_FramebufferHandOver()) {
        fprintf(stderr, "[VKR] the CPU presenter did not release the window: frames read back\n");
        c->present = 0;
        return 0;
    }
    if (!pr_make_res(c) || !pr_make_swap(c) || !pr_make_pipe(c)) {
        pr_destroy_swap(c);
        xbox_FramebufferTakeBack();
        c->present = 0;
        fprintf(stderr, "[VKR] swapchain setup failed: back to the CPU presenter (frames read back)\n");
        return 0;
    }
    c->pr_on = 1;
    fprintf(stderr, "[VKR] frames go straight to the screen: swapchain %ux%u, %u images, format %d, FIFO\n",
            c->swap_ext.width, c->swap_ext.height, c->swap_n, (int)c->swap_fmt);
    return 1;
}

/* Failing presents: the window back to the CPU presenter (before the next
 * frame is recorded, nothing of the swapchain in flight). */
static void pr_stop(VkrContext *c)
{
    vkDeviceWaitIdle(c->dev);
    pr_destroy_swap(c);
    c->pr_on = 0;
    c->present = 0;
    c->pr_stop = 0;
    xbox_FramebufferTakeBack();
    fprintf(stderr, "[VKR] presents keep failing: back to the CPU presenter (frames read back)\n");
}

/* Staging in the frame's upload buffer, then into img (layout was
 * SHADER_READ_ONLY once valid, else UNDEFINED) -- outside a render pass. */
static int pr_upload(VkrContext *c, VkImage img, int valid, const void *px, uint32_t w, uint32_t h)
{
    VkBufferImageCopy cp = {0};
    uint32_t off = (c->buf_used + 15) & ~15u, bytes = w * h * 4;
    if (off + bytes > VKR_BUF_BYTES)
        return 0;
    memcpy(c->buf_map + off, px, bytes);
    c->buf_used = off + bytes;
    barrier(c, img, VK_IMAGE_ASPECT_COLOR_BIT,
            valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c->cmd, c->buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(c, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return 1;
}

/* src (sw x sh, in TRANSFER_SRC; left in SHADER_READ_ONLY) onto swapchain
 * image ii, which ends in PRESENT_SRC. */
static void pr_record(VkrContext *c, VkImage src, VkImageView srcv, uint32_t sw, uint32_t sh, uint32_t ii)
{
    static uint32_t ov_px[PR_OV_W * PR_OV_H], ramp[256];
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkDescriptorImageInfo dii[3];
    VkWriteDescriptorSet w[3];
    uint32_t W = c->swap_ext.width, H = c->swap_ext.height;
    VkViewport vp = {0, 0, (float)W, (float)H, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {W, H}};
    struct { float rect[4], ov_size[2], gamma_on, ov_on; } pc;
    unsigned s;
    int k;

    /* the ramp: when the title sets a new one */
    s = xbox_FramebufferGammaSerial();
    if (!c->pr_ramp_valid || s != c->pr_ramp_serial) {
        xbox_FramebufferGammaTexels(ramp);
        if (pr_upload(c, c->pr_ramp, c->pr_ramp_valid, ramp, 256, 1)) {
            c->pr_ramp_valid = 1;
            c->pr_ramp_serial = s;
        }
    }
    /* the overlay: redrawn once a second (0 = none) */
    s = xbox_FramebufferOverlayPixels(NULL, PR_OV_W, PR_OV_H);
    if (!c->pr_ov_valid || s != c->pr_ov_serial) {
        if (s)
            xbox_FramebufferOverlayPixels(ov_px, PR_OV_W, PR_OV_H);
        else
            memset(ov_px, 0, sizeof ov_px);
        if (pr_upload(c, c->pr_ov, c->pr_ov_valid, ov_px, PR_OV_W, PR_OV_H)) {
            c->pr_ov_valid = 1;
            c->pr_ov_serial = s;
            c->pr_ov_on = s != 0;
        }
    }

    barrier(c, src, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    /* the acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT */
    barrier(c, c->swap_img[ii], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* the frame fitted with its aspect kept (a 16:9 frame fills 1280x720) */
    if (sw == W && sh == H) {
        pc.rect[0] = pc.rect[1] = 0.0f; pc.rect[2] = (float)W; pc.rect[3] = (float)H;
    } else {
        float s1 = (float)W / (float)sw, s2 = (float)H / (float)sh, f = s1 < s2 ? s1 : s2;
        pc.rect[2] = (float)sw * f; pc.rect[3] = (float)sh * f;
        pc.rect[0] = ((float)W - pc.rect[2]) * 0.5f; pc.rect[1] = ((float)H - pc.rect[3]) * 0.5f;
    }
    pc.ov_size[0] = (float)PR_OV_W; pc.ov_size[1] = (float)PR_OV_H;
    pc.gamma_on = xbox_FramebufferGammaLut() ? 1.0f : 0.0f;
    pc.ov_on = c->pr_ov_on ? 1.0f : 0.0f;

    dii[0].sampler = c->pr_lin;  dii[0].imageView = srcv;
    dii[1].sampler = c->pr_near; dii[1].imageView = c->pr_ramp_view;
    dii[2].sampler = c->pr_near; dii[2].imageView = c->pr_ov_view;
    for (k = 0; k < 3; k++) {
        dii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&w[k], 0, sizeof w[k]);
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[k].pImageInfo = &dii[k];
    }
    ca.imageView = c->swap_view[ii]; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  /* every pixel is written */
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pr_pipe);
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pr_layout, 0, 3, w);
    vkCmdPushConstants(c->cmd, c->pr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
    barrier(c, c->swap_img[ii], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0);
}

/* A swapchain image for this frame: 1 and *ii, or 0 (none in 100 ms, or the
 * swapchain is broken: the frame is not shown). */
static int pr_acquire(VkrContext *c, int slot, uint32_t *ii)
{
    VkResult r;
    int tries;
    for (tries = 0; tries < 2; tries++) {
        if (c->pr_recreate) {
            vkDeviceWaitIdle(c->dev);
            if (!pr_make_swap(c) || !pr_make_pipe(c)) { c->pr_stop = 1; return 0; }
        }
        r = vkAcquireNextImageKHR(c->dev, c->swap, 100000000ull, c->acq[slot], VK_NULL_HANDLE, ii);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            return 1;
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { c->pr_recreate = 1; continue; }
        if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            vk_failed("vkAcquireNextImageKHR", r, __FILE__, __LINE__);
            if (++c->pr_fails >= 30) c->pr_stop = 1;
        }
        return 0;
    }
    return 0;
}
#endif /* __SWITCH__ */

int vkr_flip_submit(VkrContext *c)
{
    VkBufferImageCopy cp = {0};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    int slot = c->cur;
    VkImage frame;                       /* the finished, single-sample image */
    /* the swapchain path: what goes onto the screen, and c->color's layout
     * after it (present.frag leaves its source in SHADER_READ_ONLY) */
    VkImage pr_src = VK_NULL_HANDLE;
    VkImageView pr_srcv = VK_NULL_HANDLE;
    VkImageLayout color_after = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    int pr = 0;
    uint32_t pr_ii = 0;
#if defined(__SWITCH__)
    if (c->pr_stop)
        pr_stop(c);
    if (c->present && !c->pr_on)
        pr_start(c);
#endif
    vkr_offscreen_done(c);               /* the frame is the main target's */
    if (!begin_cmd(c)) return -1;
    frame = c->resolved ? c->resolved : c->color;
    if (c->squeeze_all > 0.5f) {
        /* Pillarboxed frame: the bars are black (nothing draws there). */
        VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0, 0, 0, 1}}}};
        uint32_t bw = (uint32_t)(((float)c->sw - (float)c->w * c->scale) * 0.5f);
        VkClearRect cr[2] = {{{{0, 0}, {bw, c->sh}}, 0, 1},
                             {{{(int32_t)(c->sw - bw), 0}, {bw, c->sh}}, 0, 1}};
        if (bw) {
            begin_pass(c);
            vkCmdClearAttachments(c->cmd, 1, &ca, 2, cr);
        }
    }
    end_pass(c);
    if (c->scene && c->cam_valid && !c->cam.off && c->sc_in && !c->scene_done)
        scene_pass(c);
    c->scene_done = 0;
    c->cam_valid = 0;
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (c->resolved) {
        /* MSAA: the samples averaged once per frame, here; draws keep
         * accumulating into the multisampled target until then. */
        VkImageResolve rs = {0};
        barrier(c, c->resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        rs.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rs.srcSubresource.layerCount = 1;
        rs.dstSubresource = rs.srcSubresource;
        rs.extent.width = c->sw; rs.extent.height = c->sh; rs.extent.depth = 1;
        vkCmdResolveImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          c->resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
        barrier(c, c->resolved, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.depth = 1;
    pr_src = frame;
    pr_srcv = c->resolved ? c->resolved_view : c->color_view;
    if (c->scale == 1.0f && c->scale_x == 1.0f) {
        cp.imageExtent.width = c->w; cp.imageExtent.height = c->h;
        vkCmdCopyImageToBuffer(c->cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c->fr[slot].rb, 1, &cp);
    } else {
        /* The full image for the presenter, and a filtered guest-size copy
         * for guest memory (what the title and the screenshots read). With
         * the swapchain the full image goes to the screen on the GPU: no
         * read-back of it. */
        VkImageBlit bl = {0};
        cp.imageExtent.width = c->sw; cp.imageExtent.height = c->sh;
        if (c->crt_in) {
            crt_pass(c, frame);
            pr_src = c->crt_out;
            pr_srcv = c->crt_out_view;
            if (!c->pr_on)
                vkCmdCopyImageToBuffer(c->cmd, c->crt_out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       c->fr[slot].rbh, 1, &cp);
        } else if (!c->pr_on)
            vkCmdCopyImageToBuffer(c->cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   c->fr[slot].rbh, 1, &cp);
        barrier(c, c->down, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.srcOffsets[1].x = (int32_t)c->sw; bl.srcOffsets[1].y = (int32_t)c->sh; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int32_t)c->w;  bl.dstOffsets[1].y = (int32_t)c->h;  bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->down, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrier(c, c->down, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        cp.imageExtent.width = c->w; cp.imageExtent.height = c->h;
        vkCmdCopyImageToBuffer(c->cmd, c->down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c->fr[slot].rb, 1, &cp);
    }
#if defined(__SWITCH__)
    if (c->pr_on && !c->pr_hold && (pr = pr_acquire(c, slot, &pr_ii)) != 0) {
        pr_record(c, pr_src, pr_srcv, c->sw, c->sh, pr_ii);   /* frame / crt_out: sw x sh */
        if (pr_src == c->color)
            color_after = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
#else
    (void)pr_src; (void)pr_srcv; (void)pr_ii; (void)pr;
#endif
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            color_after, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    prof_mark(c, NULL, 0, -1);
    CK_FRAME(vkEndCommandBuffer(c->cmd), return -1);
    c->recording = 0;
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
#if defined(__SWITCH__)
    VkPipelineStageFlags pr_ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (pr) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &c->acq[slot]; si.pWaitDstStageMask = &pr_ws;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &c->swap_done[pr_ii];
    }
#endif
    CK_FRAME(vkQueueSubmit(c->queue, 1, &si, c->fence), return -1);
#if defined(__SWITCH__)
    if (pr) {
        /* NVK's Switch WSI hands the GPU's fence to the display with the
         * buffer (nwindowQueueBuffer): no CPU wait for the frame here */
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkResult r;
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &c->swap_done[pr_ii];
        pi.swapchainCount = 1; pi.pSwapchains = &c->swap; pi.pImageIndices = &pr_ii;
        r = vkQueuePresentKHR(c->queue, &pi);
        if (r == VK_SUCCESS) {
            c->pr_fails = 0;
            c->pr_frames++;
        } else if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
            c->pr_recreate = 1;
        } else {
            vk_failed("vkQueuePresentKHR", r, __FILE__, __LINE__);
            if (++c->pr_fails >= 30)
                c->pr_stop = 1;
        }
    }
#endif
    pthread_mutex_lock(&c->lock);
    c->fr[slot].busy = 1;
    pthread_mutex_unlock(&c->lock);
    use_frame(c, (slot + 1) % VKR_FRAMES);
    c->fr[c->cur].qreset = 0;   /* its queries are reset when it is next begun */
    c->buf_used = 0;
    c->up_tex = c->up_hd = c->up_draw = 0;
    c->up_full_said = 0;
    c->upload_serial++;
    if (++c->flip_serial % 120 == 0)     /* every ~2 s: textures unused for 5 s */
        tex_evict_old(c, 300);
#if defined(__SWITCH__)
    /* SSX3NX: the GPU's memory is the application's own (shared with the
     * guest image, the lifted code and Mesa), so the safety net sits lower */
    if (c->tex_bytes > (384ull << 20) || c->purge_req) {
#else
    if (c->tex_bytes > (768ull << 20) || c->purge_req) {  /* drop the cache once nothing is in flight */
#endif
        c->purge_req = 0;
        vkDeviceWaitIdle(c->dev);
        tex_free_all(c);
    }
    return slot;
}

const uint8_t *vkr_frame_pixels(VkrContext *c, int slot)
{
    CK_FRAME(vkWaitForFences(c->dev, 1, &c->fr[slot].fence, VK_TRUE, UINT64_MAX), return NULL);
    prof_report(c, slot);
    if (c->fr[slot].rb_noncoherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mr.memory = c->fr[slot].rb_mem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(c->dev, 1, &mr);
    }
    return c->fr[slot].rb_map;
}

/* After vkr_frame_pixels: the full-resolution image of the same frame
 * (*w x *h), or NULL when the internal resolution is the guest's. */
const uint8_t *vkr_frame_pixels_full(VkrContext *c, int slot, uint32_t *w, uint32_t *h)
{
    /* the swapchain shows it: no full-resolution read-back to hand on */
    if ((c->scale == 1.0f && c->scale_x == 1.0f) || !c->fr[slot].rbh_map || c->pr_on)
        return NULL;
    if (c->fr[slot].rbh_noncoherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mr.memory = c->fr[slot].rbh_mem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(c->dev, 1, &mr);
    }
    *w = c->sw; *h = c->sh;
    return c->fr[slot].rbh_map;
}

void vkr_frame_release(VkrContext *c, int slot)
{
    pthread_mutex_lock(&c->lock);
    c->fr[slot].busy = 0;
    pthread_cond_broadcast(&c->freed);
    pthread_mutex_unlock(&c->lock);
}

int vkr_read_back(VkrContext *c, uint8_t *out)
{
    int slot = vkr_flip_submit(c);
    const uint8_t *px = slot < 0 ? NULL : vkr_frame_pixels(c, slot);
    if (px)
        memcpy(out, px, (size_t)c->w * c->h * 4);
    if (slot >= 0)
        vkr_frame_release(c, slot);
    return px != NULL;
}
