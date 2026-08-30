/* Diatom internals.
 *
 * One module organized by file, not by layer. An earlier proposal split "core
 * host" (ABI conformance) from "session" (lifecycle, policy) as separate
 * layers; that seam had one implementation and almost nothing a core asks via
 * RETRO_ENVIRONMENT_* can be answered from ABI knowledge alone. See register §2.
 *
 * The boundary that does matter here is temporal, not architectural:
 * per-game versus per-frame. Everything policy-ish resolves once at load into
 * diatom_policy, which the frame loop reads as plain fields. Cores are
 * permitted to call GET_VARIABLE every frame; a naive implementation would do
 * string comparisons at 60Hz.
 */
#ifndef DIATOM_H
#define DIATOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diatom_port.h"
#include "libretro.h"

/* ---- resolved once at game load; the frame loop only reads ---------------- */
typedef struct {
	const char   *system_dir;
	const char   *save_dir;
	unsigned      controller_type;   /* retro_set_controller_port_device */
	diatom_pixfmt pixfmt;
	bool          pixfmt_set;
	bool          supports_no_game; /* core declared SET_SUPPORT_NO_GAME */
} diatom_policy;

/* ---- a loaded core ------------------------------------------------------- */
typedef struct {
	void       *handle;          /* dlopen'd RTLD_NOW|RTLD_LOCAL - ADR-0010 */
	const char *path;
	bool        initialized;     /* retro_init has run */
	bool        game_loaded;

	void   (*set_environment)(retro_environment_t);
	void   (*set_video_refresh)(retro_video_refresh_t);
	void   (*set_audio_sample)(retro_audio_sample_t);
	void   (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void   (*set_input_poll)(retro_input_poll_t);
	void   (*set_input_state)(retro_input_state_t);
	void   (*set_controller_port_device)(unsigned, unsigned);
	void   (*init)(void);
	void   (*deinit)(void);
	void   (*get_system_info)(struct retro_system_info *);
	void   (*get_system_av_info)(struct retro_system_av_info *);
	bool   (*load_game)(const struct retro_game_info *);
	void   (*unload_game)(void);
	void   (*run)(void);
	void   (*reset)(void);
	size_t (*serialize_size)(void);
	bool   (*serialize)(void *, size_t);
	bool   (*unserialize)(const void *, size_t);
	void  *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} diatom_core;

/* zip.c - launcher libraries arrive zipped, one ROM per archive */
bool diatom_zip_is(const char *path);
bool diatom_zip_load(const char *path, void **out, size_t *out_len,
                     char *name, size_t name_n);

/* core.c */
bool diatom_core_open(diatom_core *c, const char *path);
bool diatom_core_start(diatom_core *c, const char *rom_path);
void diatom_core_stop(diatom_core *c);

/* Open once and keep - ADR-0006. Returns the same core for the same path, so a
 * repeat launch skips dlopen and retro_init. Never unloaded. */
diatom_core *diatom_core_resident(const char *path);
int          diatom_core_resident_count(void);

/* env.c */
void diatom_env_bind(diatom_core *c, diatom_policy *p, diatom_port_caps *caps);
bool diatom_env_geometry_changed(void);
bool diatom_env_new_geometry(int *w, int *h, double *aspect);   /* consumes the flag */

/* Buttons the host is using for itself this frame and the core must not see.
 * Diatom owns MENU outright (it never appears in the retropad map); this is
 * for keys that are normally the core's but are currently part of a host
 * chord. */
void diatom_env_suppress(uint32_t mask);

/* Silence a core's DEBUG/INFO chatter once a game is running. See core_log in
 * env.c for the measurement that made this necessary. */
void diatom_env_core_log_quiet(bool quiet);

/* options.c - what a core can be configured with, and what it currently is.
 *
 * Diatom holds the definitions and the values; the LAUNCHER decides what the
 * values should be. Diatom has no opinion about whether a Genesis should be PAL
 * and no way to ask, having no UI (ADR-0009). */
void        diatom_options_bind(const void *owner);
void        diatom_options_define_v2(const struct retro_core_options_v2 *v2);
void        diatom_options_define_vars(const struct retro_variable *vars);
const char *diatom_options_get(const char *key);
bool        diatom_options_set(const char *key, const char *value);
bool        diatom_options_take_update(void);   /* consumes the flag */
int         diatom_options_count(void);
void        diatom_options_list(void);
void        diatom_options_emit(void);   /* over the protocol, for a menu */
/* Drop values the launcher set for a game that is now over - see options.c. */
void        diatom_options_clear_pending(void);

/* proto.c - the launcher protocol (ADR-0009). One Unix socket, line-based,
 * tab-separated key=value. Diatom is a component the launcher drives; this is
 * the only place it talks back.
 *
 * The governing rule is about the DISPLAY: ERROR means the game never started,
 * EXIT means it ran and stopped. A launcher that hears RUNNING stops drawing. */
typedef enum {
	DIATOM_MSG_NONE = 0,
	DIATOM_MSG_RUN,
	DIATOM_MSG_STOP,
	DIATOM_MSG_QUIT,
	DIATOM_MSG_RESUME,     /* leave the menu, Diatom takes the display back */
	DIATOM_MSG_RESET,      /* retro_reset: the menu's Reset row, and nothing else */
	DIATOM_MSG_SAVE,       /* write a state to `path` */
	DIATOM_MSG_LOAD,       /* read a state from `path` */
	DIATOM_MSG_OPTIONS,    /* enumerate this core's options */
	DIATOM_MSG_SETOPT,     /* set `key` to `value` */
	/* The state plane - ADR-0020. Each is query / write, and the query verb
	 * doubles as the unsolicited change event so a launcher parses one shape
	 * per state rather than two. */
	DIATOM_MSG_INPUTS,     /* enumerate this game's button labels */
	DIATOM_MSG_MAP,        /* report the active remap table */
	DIATOM_MSG_SETMAP,     /* replace it whole: `map` = "a:b,x:none" */
	DIATOM_MSG_LEVELS,     /* report volume and brightness */
	DIATOM_MSG_SETLEVEL,   /* set `lkind` to `index` of `count` positions */
	DIATOM_MSG_DISPLAY,    /* report the display mode and filter */
	DIATOM_MSG_SETDISPLAY, /* set them: `dmode`, `dfilter` */
	DIATOM_MSG_OVERLAY,    /* composite `path` over the game for `count` ms */
	DIATOM_MSG_CHEEVOS,    /* report the achievement set and what has fired */
	DIATOM_MSG_SETCHEEVOS, /* load a set from `path`; empty unloads */
	DIATOM_MSG_HANGUP      /* launcher went away; the game keeps running */
} diatom_msg_kind;

typedef struct {
	diatom_msg_kind kind;
	char core[1024];
	char rom[1024];
	char firmware[128];    /* RUN: what this content needs in the system dir */
	char tag[64];
	char slot[64];
	/* Session persistence, ADR-0024: the launcher passes explicit paths
	 * (ADR-0016), and these are how they arrive. All optional. */
	char resume[1024];     /* RUN: state to load at start, if it exists */
	char exit_state[1024]; /* RUN: state written on every way out */
	char preview[1024];    /* RUN: BMP of the frame, on pause and on exit */
	/* Achievements, ADR-0026. `console` is a RetroAchievements console id and
	 * is not optional for them: an RA address is an offset into a per-console
	 * space, so 0x06f3 means nothing until the launcher says which console. */
	char cheevos[1024];    /* RUN: a set to watch from the first frame */
	int  console;          /* RUN / SETCHEEVOS */
	char path[1024];       /* SAVE / LOAD / SETCHEEVOS */
	char key[80];          /* SETOPT */
	char value[128];       /* SETOPT */
	char map[512];         /* SETMAP */
	char lkind[32];        /* SETLEVEL: volume | brightness */
	char dmode[32];        /* SETDISPLAY: a name from diatom_modes[] */
	char dfilter[16];      /* SETDISPLAY: nearest | sharp */
	int  index, count;     /* SETLEVEL: `count` is POSITIONS, not a max index.
	                        * OVERLAY: `count` is the duration in ms, sent as
	                        * `ms=`. One slot, two verbs, no second name. */
} diatom_msg;

/* Input mapping and labels live in env.c, the one layer a remap touches. */
void diatom_input_reset_map(void);
bool diatom_input_set_map(const char *spec);
void diatom_input_emit_map(void);
void diatom_input_emit_labels(void);

bool diatom_proto_listen(const char *path);
void diatom_proto_close(void);
bool diatom_proto_active(void);
bool diatom_proto_connected(void);
void diatom_proto_send(const char *fmt, ...);

/* timeout_ms < 0 blocks. `running` is reported to a launcher that connects
 * mid-game, so a restarted launcher does not draw over live output. */
diatom_msg_kind diatom_proto_poll(diatom_msg *out, int timeout_ms, bool running);

/* Async-signal-safe, for the crash handler. `line` must be a complete constant
 * with its own newline: nothing that formats a string is callable from there. */
void diatom_proto_emit_fatal(const char *line);

/* save.c - persistence. Host-side entirely: the port deals in pixels, samples,
 * buttons and time, and a file is none of those.
 *
 * SRAM is automatic because it is the game's own data. Save states are opt-in
 * and take explicit PATHS, never slot numbers - Diatom has no concept of a
 * slot, a game or a system, and the launcher owns all three (ADR-0016). */
bool diatom_save_init(diatom_core *c, const char *save_dir, const char *rom_path);
void diatom_save_tick(void);       /* once per frame; may schedule a write */
void diatom_save_flush(void);      /* synchronous; exit and signal paths */
void diatom_save_shutdown(void);
bool diatom_state_save(diatom_core *c, const char *path);
bool diatom_state_load(diatom_core *c, const char *path);

/* scale.c - geometry is arithmetic and lives here, once, so every port agrees.
 * Performing the blit is hardware and belongs to the port.
 *
 * Which mode should be default is OPEN (register §5). The set exists so the
 * question can be answered by looking at a panel rather than by argument. */
typedef enum {
	DIATOM_SCALE_NATIVE,         /* 1x, centered                               */
	DIATOM_SCALE_INTEGER,        /* largest whole factor that fits, boxed     */
	DIATOM_SCALE_INTEGER_VERT,   /* whole factor down, shape-correct across   */
	DIATOM_SCALE_INTEGER_OVER,   /* smallest whole factor that covers, cropped*/
	DIATOM_SCALE_ASPECT_FIT,     /* fractional, shape kept, boxed             */
	DIATOM_SCALE_ASPECT_FILL,    /* fractional, shape kept, cropped           */
	DIATOM_SCALE_STRETCH         /* fills both axes, shape ignored            */
} diatom_scale_mode;

diatom_rect diatom_scale_rect(diatom_scale_mode mode, int src_w, int src_h,
                              double aspect, int surf_w, int surf_h);

/* Mode and filter are independent axes, cycled independently on device. They
 * were briefly modeled as a flat list of (mode, filter) presets on the
 * assumption that most combinations collapse; measurement killed that. FCEUmm
 * reports an 8:7 pixel aspect, about 1.219, not 4:3, so on the Brick's 4:3
 * panel fit, fill and stretch are three different pictures. */
typedef struct {
	const char       *name;
	diatom_scale_mode mode;
	const char       *note;
} diatom_display_mode_info;

extern const diatom_display_mode_info diatom_modes[];
extern const int                      diatom_mode_count;

/* audio.c - cores emit 32040..65536 Hz; the device runs at whatever it runs at.
 * Linear interpolation, with dynamic rate control holding the port's buffer near
 * half full. The resampler itself is still a placeholder; the control loop is
 * not, because a fixed ratio drifts until the buffer empties or overflows. */
void   diatom_audio_configure(double src_rate, int dst_rate, int capacity_frames);
size_t diatom_audio_push(const int16_t *in, size_t frames);
void   diatom_audio_prime(void);           /* fill to target before frame one */
void   diatom_audio_sync(void);            /* once per frame, after pushing */
double diatom_audio_ratio_drift(void);     /* current DRC correction, for reporting */
/* Frames the port refused, this session. Nonzero means rate control is not
 * keeping up - the buffer is hitting a wall rather than being steered. */
uint64_t diatom_audio_dropped(void);

/* Peak absolute sample, and how many of the samples written were non-zero.
 * The first thing to check when nothing is audible. */
void     diatom_audio_note_input(const int16_t *f, size_t n);
/* Raw S16 stereo taps either side of the resampler, for offline analysis. */
void     diatom_audio_tap(const char *in_path, const char *out_path);
void     diatom_audio_tap_close(void);
int      diatom_audio_in_peak(void);
uint64_t diatom_audio_in_nonzero(void);
uint64_t diatom_audio_in_samples(void);
int      diatom_audio_peak(void);
double   diatom_audio_rms(void);
double   diatom_audio_in_rms(void);
uint64_t diatom_audio_nonzero(void);
uint64_t diatom_audio_samples(void);

#endif /* DIATOM_H */
