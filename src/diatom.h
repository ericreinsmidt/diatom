/* Diatom internals.
 *
 * One module organised by file, not by layer. An earlier proposal split "core
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
	bool        initialised;     /* retro_init has run */
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
	size_t (*serialize_size)(void);
	bool   (*serialize)(void *, size_t);
	bool   (*unserialize)(const void *, size_t);
	void  *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} diatom_core;

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
bool diatom_env_geometry_changed(void);   /* consumes the flag */

/* Buttons the host is using for itself this frame and the core must not see.
 * Diatom owns MENU outright (it never appears in the retropad map); this is
 * for keys that are normally the core's but are currently part of a host
 * chord. */
void diatom_env_suppress(uint32_t mask);

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
	DIATOM_MSG_HANGUP      /* launcher went away; the game keeps running */
} diatom_msg_kind;

typedef struct {
	diatom_msg_kind kind;
	char core[1024];
	char rom[1024];
	char tag[64];
	char slot[64];
} diatom_msg;

bool diatom_proto_listen(const char *path);
void diatom_proto_close(void);
bool diatom_proto_active(void);
bool diatom_proto_connected(void);
void diatom_proto_send(const char *fmt, ...);

/* timeout_ms < 0 blocks. `running` is reported to a launcher that connects
 * mid-game, so a restarted launcher does not draw over live output. */
diatom_msg_kind diatom_proto_poll(diatom_msg *out, int timeout_ms, bool running);

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
	DIATOM_SCALE_NATIVE,         /* 1x, centred                               */
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
 * were briefly modelled as a flat list of (mode, filter) presets on the
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

#endif /* DIATOM_H */
