//
// Sound backend for the ESP32-S3-RLCD-4.2.
//
// Phase 0: silent. Every hook is present and the game-side sound path runs for
// real -- S_StartSound allocates channels, computes distance attenuation and
// stereo separation, and calls in here -- but nothing reaches the ES8311 yet.
//
// Keeping the module live rather than compiling sound out (which is what
// ESP_DOOM did) means the audio work later is confined to this one file, and
// means we find out now if the sound path costs us memory or frame time.
//
// TODO(audio): back these with the ES8311 codec over I2S. Pins are fixed by the
// schematic: MCLK 16, SCLK 9, LRCK 45, DSDIN 8, PA_CTRL 46, with codec setup
// over the shared I2C bus (SDA 13, SCL 14).
//

#include <stddef.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "sounds.h"
#include "w_wad.h"

#include "esp_log.h"

static const char *TAG = "doom.snd";

static boolean use_sfx_prefix;

// Referenced by I_BindSoundVariables under FEATURE_SOUND. Upstream these live in
// i_sdlsound.c behind HAVE_LIBSAMPLERATE; we have no libsamplerate, but the
// config binding still needs somewhere to point.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// ---------------------------------------------------------------------------
// Sound effects
// ---------------------------------------------------------------------------

static boolean I_RLCD_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    ESP_LOGW(TAG, "sound backend is a STUB -- channels are allocated but silent");
    // Returning true deliberately: it keeps S_StartSound and the channel
    // allocator on the live path so we exercise them. Nothing is audible.
    return true;
}

static void I_RLCD_ShutdownSound(void)
{
}

// This one is NOT a stub. The S_ layer uses the returned lump number for
// precaching and for its own bookkeeping, so handing back a wrong or negative
// value would misbehave well before any audio hardware is involved.
static int I_RLCD_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    // Linked sfx lumps point at the sound they alias.
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }

    // Doom prefixes sound lumps with "ds"; Heretic and Hexen do not.
    if (use_sfx_prefix) {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(namebuf, DEH_String(sfx->name), sizeof(namebuf));
    }

    return W_GetNumForName(namebuf);
}

static void I_RLCD_UpdateSound(void)
{
}

static void I_RLCD_UpdateSoundParams(int channel, int vol, int sep)
{
    // `vol` is distance attenuation, `sep` is stereo separation (0..254, 128
    // centre). The board's speaker connector is mono, so `sep` has nowhere to go
    // on the built-in output -- distance still reads correctly through `vol`.
    (void)channel; (void)vol; (void)sep;
}

static int I_RLCD_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    (void)sfxinfo; (void)vol; (void)sep;
    return channel;
}

static void I_RLCD_StopSound(int channel)
{
    (void)channel;
}

static boolean I_RLCD_SoundIsPlaying(int channel)
{
    (void)channel;
    // Report finished immediately. If we claimed sounds were still playing the
    // channel allocator would never reclaim them and would starve.
    return false;
}

static void I_RLCD_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;
}

static snddevice_t sound_rlcd_devices[] =
{
    // snd_sfxdevice defaults to SNDDEVICE_SB, so this list must contain it or
    // I_InitSound will not select this module at all.
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_rlcd_devices,
    arrlen(sound_rlcd_devices),
    I_RLCD_InitSound,
    I_RLCD_ShutdownSound,
    I_RLCD_GetSfxLumpNum,
    I_RLCD_UpdateSound,
    I_RLCD_UpdateSoundParams,
    I_RLCD_StartSound,
    I_RLCD_StopSound,
    I_RLCD_SoundIsPlaying,
    I_RLCD_PrecacheSounds,
};

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------
//
// Doom stores music as MUS lumps. A real backend converts MUS to MIDI with
// mus2mid (already vendored) and then needs something to synthesise it -- an OPL
// emulator or a wavetable. That is a materially bigger job than sfx, so it stays
// stubbed even after the ES8311 work.

static boolean I_RLCD_InitMusic(void)
{
    return true;
}

static void I_RLCD_ShutdownMusic(void) { }
static void I_RLCD_SetMusicVolume(int volume) { (void)volume; }
static void I_RLCD_PauseSong(void) { }
static void I_RLCD_ResumeSong(void) { }

static void *I_RLCD_RegisterSong(void *data, int len)
{
    (void)data; (void)len;
    return NULL;
}

static void I_RLCD_UnRegisterSong(void *handle) { (void)handle; }
static void I_RLCD_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
static void I_RLCD_StopSong(void) { }

static boolean I_RLCD_MusicIsPlaying(void)
{
    return false;
}

static void I_RLCD_PollMusic(void) { }

static snddevice_t music_rlcd_devices[] =
{
    // SNDDEVICE_SB is here even though doomgeneric's InitMusicModule assigns
    // music_module unconditionally without consulting this list. Relying on
    // that would make correctness depend on an upstream shortcut; snd_musicdevice
    // defaults to SNDDEVICE_SB, so listing it is what actually matches.
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_rlcd_devices,
    arrlen(music_rlcd_devices),
    I_RLCD_InitMusic,
    I_RLCD_ShutdownMusic,
    I_RLCD_SetMusicVolume,
    I_RLCD_PauseSong,
    I_RLCD_ResumeSong,
    I_RLCD_RegisterSong,
    I_RLCD_UnRegisterSong,
    I_RLCD_PlaySong,
    I_RLCD_StopSong,
    I_RLCD_MusicIsPlaying,
    I_RLCD_PollMusic,
};
