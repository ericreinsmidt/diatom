/* Loading a core, and its lifecycle.
 *
 * ADR-0006: every core is dlopen'd once and NEVER unloaded. dlclose does not
 * appear in this file, or anywhere else in Diatom.
 * ADR-0010: RTLD_LOCAL always. It is the only thing preventing symbol collision
 * between resident cores - measured, picodrive exports 1069 non-retro_* symbols
 * including a complete static zlib. RTLD_GLOBAL would let its crc32 answer for
 * everyone, silently.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diatom.h"

static bool bind_sym(void *h, void *slot, const char *name)
{
	void *p = dlsym(h, name);
	if (!p) {
		fprintf(stderr, "diatom: core is missing %s\n", name);
		return false;
	}
	memcpy(slot, &p, sizeof p);
	return true;
}

#define BIND(field, name) \
	if (!bind_sym(c->handle, &c->field, name)) return false

bool diatom_core_open(diatom_core *c, const char *path)
{
	memset(c, 0, sizeof *c);
	c->path = path;

	c->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);   /* ADR-0010 */
	if (!c->handle) {
		fprintf(stderr, "diatom: dlopen %s: %s\n", path, dlerror());
		return false;
	}

	BIND(set_environment,            "retro_set_environment");
	BIND(set_video_refresh,          "retro_set_video_refresh");
	BIND(set_audio_sample,           "retro_set_audio_sample");
	BIND(set_audio_sample_batch,     "retro_set_audio_sample_batch");
	BIND(set_input_poll,             "retro_set_input_poll");
	BIND(set_input_state,            "retro_set_input_state");
	BIND(set_controller_port_device, "retro_set_controller_port_device");
	BIND(init,                       "retro_init");
	BIND(deinit,                     "retro_deinit");
	BIND(get_system_info,            "retro_get_system_info");
	BIND(get_system_av_info,         "retro_get_system_av_info");
	BIND(load_game,                  "retro_load_game");
	BIND(unload_game,                "retro_unload_game");
	BIND(run,                        "retro_run");
	BIND(serialize_size,             "retro_serialize_size");
	BIND(serialize,                  "retro_serialize");
	BIND(unserialize,                "retro_unserialize");

	return true;
}

/* Cores that set need_fullpath want a path and read the file themselves;
 * the rest want the bytes. Getting this backwards is a silent load failure. */
static bool read_file(const char *path, void **out, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *buf;

	*out = NULL; *len = 0;
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	n = ftell(f);
	if (n <= 0) { fclose(f); return false; }
	rewind(f);

	buf = malloc((size_t)n);
	if (!buf) { fclose(f); return false; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf); fclose(f); return false;
	}
	fclose(f);
	*out = buf; *len = (size_t)n;
	return true;
}

bool diatom_core_start(diatom_core *c, const char *rom_path)
{
	struct retro_system_info si;
	struct retro_game_info gi;
	void *data = NULL;
	size_t len = 0;
	bool ok;

	memset(&si, 0, sizeof si);
	c->get_system_info(&si);

	if (!c->initialised) { c->init(); c->initialised = true; }

	/* A core that declared SET_SUPPORT_NO_GAME expects NULL, not a path. */
	if (!rom_path) {
		if (!c->load_game(NULL)) {
			fprintf(stderr, "diatom: core refused to start without content\n");
			return false;
		}
		c->game_loaded = true;
		return true;
	}

	memset(&gi, 0, sizeof gi);
	gi.path = rom_path;
	if (!si.need_fullpath) {
		if (!read_file(rom_path, &data, &len)) {
			fprintf(stderr, "diatom: cannot read %s\n", rom_path);
			return false;
		}
		gi.data = data;
		gi.size = len;
	}

	ok = c->load_game(&gi);

	/* The core has copied whatever it needs by now; libretro does not promise
	 * the buffer stays valid, and holding it would waste anonymous memory on a
	 * device with no swap. */
	free(data);

	if (!ok) {
		fprintf(stderr, "diatom: core refused %s\n", rom_path);
		return false;
	}
	c->game_loaded = true;
	return true;
}

/* Stops the GAME, not the core. The core stays initialised and stays mapped -
 * that is the whole of ADR-0006. */
void diatom_core_stop(diatom_core *c)
{
	if (c->game_loaded) {
		c->unload_game();
		c->game_loaded = false;
	}
}
