/*
 * The game's audio sources on AmigaOS 3: sound effects from SOUNDS.DAT
 * (dat_source) and the original PSM music through libxmp (psm_source).
 * Remixed music mods (opus) are not built: no opusfile on this target.
 * Replaces the AGA profile's first-light version, which loaded only the null
 * sources. MIT, Copyright (c) 2026 AmiCradle Team.
 */
#include "game/audio/audio_sources.h"

#include "audio/music_sources/null_music_source.h"
#include "audio/music_sources/psm_source.h"
#include "audio/sound_sources/dat_source.h"
#include "audio/sound_sources/null_sound_source.h"
#include "resources/ids.h"
#include "resources/resource_files.h"
#include "utils/log.h"
#include "utils/path.h"

#include <assert.h>

static audio_sources_mode current_mode = AUDIO_SOURCES_REAL;

void audio_sources_set_mode(const audio_sources_mode mode) {
    current_mode = mode;
}

audio_sources_mode audio_sources_get_mode(void) {
    return current_mode;
}

bool sound_source_pick(sound_source *src, const int sound_id) {
    if(current_mode == AUDIO_SOURCES_NULL) {
        return null_sound_source_load(src, sound_id);
    }
    return dat_source_load(src, sound_id);
}

bool music_source_pick(music_source *src, const resource_id id, const unsigned channels, const unsigned sample_rate,
                       const unsigned resampler) {
    assert(is_music(id));
    if(current_mode == AUDIO_SOURCES_NULL) {
        return null_music_source_load(src);
    }
    path music = get_resource_filename(get_resource_file(id));
    log_debug("Found original music file %s", path_c(&music));
    return psm_load(src, channels, sample_rate, resampler, path_c(&music));
}
