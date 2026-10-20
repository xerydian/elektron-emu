// DistrhoPluginInfo.h -- la Syntakt come VST3 con DPF.
// Stesso motore (stcore, dt_rt) e stessa faccia dell'AU (au/): 3 uscite stereo
// (Main, Main senza Analog FX, mandata Analog FX), MIDI in/out, Main Volume e
// Sync DAW. Il firmware NON e' nel plugin: sta nella cartella dell'utente
// (vst/README.md). Vista ridimensionabile a proporzioni fisse e tasti di zoom
// che chiedono all'host una nuova misura (IPlugFrame::resizeView in VST3).
#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND "Elektron Emu"
#define DISTRHO_PLUGIN_NAME  "Syntakt"
#define DISTRHO_PLUGIN_URI   "urn:simo:syntakt"
#define DISTRHO_PLUGIN_CLAP_ID "simo.syntakt"   // serve anche all'Info.plist dell'AU

#define DISTRHO_PLUGIN_BRAND_ID  Simo
#define DISTRHO_PLUGIN_UNIQUE_ID sntk
#define DISTRHO_PLUGIN_AU_TYPE   aumu

#define DISTRHO_PLUGIN_HAS_UI          1
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     6
// solo AU: oltre alle uscite separate, lo stereo del Main (Logic, GarageBand)
#define DISTRHO_PLUGIN_EXTRA_IO        { 0, 2 },
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 1
#define DISTRHO_PLUGIN_WANT_LATENCY    1
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
#define DISTRHO_PLUGIN_WANT_STATE      1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1
#define DISTRHO_PLUGIN_WANT_TIMEPOS    1

#define DISTRHO_UI_USE_NANOVG      1
#define DISTRHO_UI_USER_RESIZABLE  1
#define DISTRHO_UI_FILE_BROWSER    1
#define DISTRHO_UI_DEFAULT_WIDTH   712
#define DISTRHO_UI_DEFAULT_HEIGHT  605

#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"

#endif
