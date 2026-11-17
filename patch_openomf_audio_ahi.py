#!/usr/bin/env python3
"""Sound for the OpenOMF AGA build: the AHI back end and the real sources.

Run after apply_aga_profile.py and patch_openomf_amigaos3.py on the work tree:
- amiga-audio/ahi_backend.[ch] go to src/audio/backends/ahi/, and the AGA
  build enables that back end (ENABLE_AHI_AUDIO_BACKEND), ahead of the null
  one, which stays as the fallback when ahi.device cannot be opened;
- amiga-audio/audio_sources_amiga.c replaces the profile's first-light one,
  so sound effects come from SOUNDS.DAT and music from the PSM files;
- psm_source.c is built again (in place of the stub), against the pinned
  libxmp that build-amigaos3.sh builds (OPENOMF_XMP_ROOT).

MIT, Copyright (c) 2026 AmiCradle Team.
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent


def replace_once(text: str, old: str, new: str, what: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"{what}: expected one match, found {text.count(old)}")
    return text.replace(old, new, 1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("work", type=Path)
    ns = ap.parse_args()
    w = ns.work

    ahi = w / "src/audio/backends/ahi"
    ahi.mkdir(parents=True, exist_ok=True)
    for name in ("ahi_backend.c", "ahi_backend.h"):
        shutil.copy2(HERE / "amiga-audio" / name, ahi / name)
    shutil.copy2(HERE / "amiga-audio/audio_sources_amiga.c", w / "src/game/audio/audio_sources_amiga.c")
    # the real psm_source.c is built, so the first-light stub must not be in
    # the tree (CMake's source glob would pick it up too)
    (w / "src/audio/music_sources/psm_source_amiga_stub.c").unlink(missing_ok=True)

    cmake = w / "CMakeLists.txt"
    t = cmake.read_text(encoding="utf-8")
    t = replace_once(t, '''        "src/audio/music_sources/opus_source.c"
        "src/audio/music_sources/psm_source.c"
        "src/resources/modmanager.c"
    )
    list(APPEND OPENOMF_SRC
        "src/resources/modmanager_amiga_stub.c"
        "src/audio/music_sources/psm_source_amiga_stub.c"
    )''', '''        "src/audio/music_sources/opus_source.c"
        "src/resources/modmanager.c"
    )
    list(APPEND OPENOMF_SRC
        "src/resources/modmanager_amiga_stub.c"
    )''', "psm source")
    t = replace_once(t, '''    list(APPEND AUDIO_C_DEFINES ENABLE_NULL_AUDIO_BACKEND)
    set(ENABLED_AUDIO_BACKEND_PLUGINS)''', '''    list(APPEND AUDIO_C_DEFINES ENABLE_NULL_AUDIO_BACKEND)
    set(ENABLED_AUDIO_BACKEND_PLUGINS ahi)''', "AHI back end")
    t = replace_once(t, '''    target_link_libraries(openomf_core PRIVATE
        openomf::enet
        openomf::SDL2
        acgame
    )''', '''    target_link_libraries(openomf_core PRIVATE
        openomf::enet
        openomf::SDL2
        acgame
        "${OPENOMF_XMP_ROOT}/lib/libxmp.a"
    )
    target_include_directories(openomf_core PRIVATE "${OPENOMF_XMP_ROOT}/include")
    target_compile_definitions(openomf_core PRIVATE LIBXMP_STATIC)''', "libxmp link")
    cmake.write_text(t, encoding="utf-8")

    audio = w / "src/audio/audio.c"
    a = audio.read_text(encoding="utf-8")
    a = replace_once(a, '''#ifdef ENABLE_SDL_AUDIO_BACKEND
#include "audio/backends/sdl/sdl_backend.h"''', '''#ifdef ENABLE_AHI_AUDIO_BACKEND
#include "audio/backends/ahi/ahi_backend.h"
#endif
#ifdef ENABLE_SDL_AUDIO_BACKEND
#include "audio/backends/sdl/sdl_backend.h"''', "audio.c include")
    a = replace_once(a, '''static audio_backend_init all_backends[] = {
#ifdef ENABLE_SDL_AUDIO_BACKEND''', '''static audio_backend_init all_backends[] = {
#ifdef ENABLE_AHI_AUDIO_BACKEND
    ahi_audio_backend_set_callbacks,
#endif
#ifdef ENABLE_SDL_AUDIO_BACKEND''', "audio.c registry")
    audio.write_text(a, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
