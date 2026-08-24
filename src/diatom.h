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
} diatom_core;

/* core.c */
bool diatom_core_open(diatom_core *c, const char *path);
bool diatom_core_start(diatom_core *c, const char *rom_path);
void diatom_core_stop(diatom_core *c);

/* env.c */
void diatom_env_bind(diatom_core *c, diatom_policy *p, diatom_port_caps *caps);
bool diatom_env_geometry_changed(void);   /* consumes the flag */

/* Buttons the host is using for itself this frame and the core must not see.
 * Diatom owns MENU outright (it never appears in the retropad map); this is
 * for keys that are normally the core's but are currently part of a host
 * chord. */
void diatom_env_suppress(uint32_t mask);

/* scale.c - geometry is arithmetic and lives here, once, so every port agrees.
 * Performing the blit is hardware and belongs to the port.
 *
 * Which mode should be default is OPEN (register §5). The set exists so the
 * question can be answered by looking at a panel rather than by argument. */
typedef enum {
	DIATOM_SCALE_NATIVE,         /* 1x, centred                               */
	DIATOM_SCALE_INTEGER,        /* largest whole factor that fits, boxed     */
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

/* osd.c - transient on-screen text naming what Diatom is doing. A debug
 * affordance, not a user interface: overlays and menus belong to the host
 * application (ADR-0009). Drawn host-side into the frame the host already
 * owns, so the port seam does not grow and every backend gets it free. */
void diatom_osd_show(const char *text, int frames);
bool diatom_osd_active(void);
void diatom_osd_tick(void);
void diatom_osd_draw(void *frame, int w, int h, size_t pitch,
                     diatom_pixfmt fmt, int ox, int oy);

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
