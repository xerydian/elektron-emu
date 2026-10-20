// DistrhoPluginInfo.h -- l'Analog Rytm MKII come VST3 con DPF.
// Stesso motore (arcore, dt_rt) e stessa faccia dell'AU (au/): 9 uscite stereo
// (Mix + le 8 voci BD, BT, SD, LT, CH/OH, MT/HT, CY/CB, RS/CP prima degli
// effetti), MIDI in/out, Volume e Sync DAW. Vista ridimensionabile a proporzioni
// fisse e tasti di zoom che chiedono all'host una nuova misura.
#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND "Elektron Emu"
#define DISTRHO_PLUGIN_NAME  "Analog Rytm MKII"
#define DISTRHO_PLUGIN_URI   "urn:simo:analogrytm"
#define DISTRHO_PLUGIN_CLAP_ID "simo.analogrytm"

#define DISTRHO_PLUGIN_BRAND_ID  Simo
#define DISTRHO_PLUGIN_UNIQUE_ID aryv
#define DISTRHO_PLUGIN_AU_TYPE   aumu

#define DISTRHO_PLUGIN_HAS_UI          1
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     18
// solo AU: oltre alle uscite separate, lo stereo del Mix (Logic, GarageBand)
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
// pannello 1242 x 708 al 75% + linguetta del +DRIVE (24) e riga di stato (22)
#define DISTRHO_UI_DEFAULT_WIDTH   956
#define DISTRHO_UI_DEFAULT_HEIGHT  553

#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth|Drum"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument", "synthesizer", "drum-machine", "stereo"

#endif
