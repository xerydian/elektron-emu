// SyntaktPlugin.hpp -- la parte audio del VST3: il motore dt_rt (rt.c del Digitakt
// sopra stcore), come au/Extension/SyntaktAudioUnit.mm. La vista (SyntaktUI.cpp)
// la raggiunge con l'accesso diretto di DPF (stesso processo) per schermo, LED,
// pannello e stato del firmware.
//
// Il firmware NON e' nel bundle: il plugin lo cerca in una cartella dell'utente
// (SyntaktStore.hpp). Senza, resta muto e la vista dice dove metterlo.
#pragma once

#include "DistrhoPlugin.hpp"
#include "SyntaktStore.hpp"
#include "dtcore.h"
#include "stcore.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

START_NAMESPACE_DISTRHO

// cosa mostra la vista oltre al pannello
struct EngineStatus {
    enum Kind { Starting, NoFirmware, Booting, Busy, Running, Error } kind = Starting;
    std::string text;               // la riga di stato
    std::string firmwareDir;        // dove mettere il firmware (percorso intero)
    std::string firmwareLabel;      // "Syntakt OS 1.41" se c'e'
    std::string warning;            // firmware sconosciuto, file scartati
    double bootSeconds = 0;         // da quanto dura il primo avvio
};

class SyntaktPlugin : public Plugin {
public:
    enum Params { kParamVolume, kParamSync, kParamCount };
    static const int kBuses = 3;               // Main, Main senza Analog FX, mandata Analog FX
    static const int kChannels = 2 * kBuses;   // i canali di dt_rt (stcore.h: 0/1, 2/3, 4/5)

    SyntaktPlugin();
    ~SyntaktPlugin() override;

    // dal thread della vista
    uint64_t copyScreen(uint8_t screen[128 * 64], uint8_t leds[50]);
    void panel(int kind, int code, int arg);
    double masterVolume() const { return fMaster.load(); }
    void setMasterFromUI(double v);
    EngineStatus status();
    // "Scegli firmware...": controlla il file, lo copia nella cartella e avvia
    // -> "" se tutto bene, altrimenti il motivo
    std::string installFirmware(const std::string& srcUtf8);

protected:
    const char* getLabel() const override { return "Syntakt"; }
    const char* getDescription() const override { return "Syntakt emulator (runs the user's own firmware)"; }
    const char* getMaker() const override { return "Elektron Emu"; }
    const char* getHomePage() const override { return "https://github.com/"; }
    const char* getLicense() const override { return "GPL-2.0-or-later"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override;
    void initPortGroup(uint32_t groupId, PortGroup& group) override;
    void initParameter(uint32_t index, Parameter& p) override;
    void initState(uint32_t index, State& s) override;
    float getParameterValue(uint32_t index) const override;
    void setParameterValue(uint32_t index, float value) override;
    String getState(const char* key) const override;
    void setState(const char* key, const char* value) override;

    void ioChanged(uint16_t numInputs, uint16_t numOutputs) override;
    void activate() override;
    void run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount) override;

private:
    // sincronia col trasporto della DAW: Start/Stop/SPP/clock nel MIDI IN emulato
    struct SyncOut {                           // gli eventi del blocco, in ordine di tempo
        struct Ev { uint32_t off; uint8_t d[3]; uint8_t n; };
        Ev ev[256];
        uint32_t n = 0;
        void add(uint32_t off, const uint8_t* d, size_t len)
        {
            if (n < 256 && len <= 3) { ev[n].off = off; ev[n].n = uint8_t(len); for (size_t i = 0; i < len; ++i) ev[n].d[i] = d[i]; ++n; }
        }
    };
    struct HostSync {
        bool started = false, wasMoving = false;
        int64_t nextTick = 0;
        double expectedBeat = 0;
        void run(SyncOut& out, const TimePosition& t, uint32_t frames, double rate);
    };

    bool startEngine(double rate, uint32_t frames);
    void swapEngine(dt_rt* rt);
    void coldBoot(double rate, uint32_t frames);
    bool loadFirmware();                       // cerca nella cartella (a meno che sia invariata)
    void setStatus(EngineStatus::Kind k, const std::string& text);
    stplug::fs::path machinePath() const;
    stplug::fs::path coldBootCachePath() const;

    std::atomic<dt_rt*> fRt{nullptr};
    std::atomic<int> fInRun{0};
    std::atomic<double> fMaster{0.8};
    std::atomic<bool> fSync{true};
    HostSync fHostSync;
    SyncOut fSyncOut;
    std::vector<float> fBuf;
    float* fCh[kChannels] = {};
    uint32_t fNumOutputs = DISTRHO_PLUGIN_NUM_OUTPUTS;     // l'AU in stereo ne usa 2 (ioChanged)
    uint32_t fMaxFrames = 0;
    double fRate = 0;
    uint32_t fRtFrames = 0;

    stplug::FirmwareInfo fFw;                // il firmware scelto (fFw.data: il .syx)
    std::string fFwSignature;                // com'era la cartella all'ultima ricerca
    bool fScanned = false;
    std::vector<uint8_t> fPendingState;      // stato del progetto prima che il motore parta
    std::mutex fLock;                        // fPendingState, fStatus, fFw
    std::recursive_mutex fStartLock;         // startEngine: host, setState, autosave e thread del primo avvio
    EngineStatus fStatus;
    std::thread fBoot;
    std::atomic<int> fBootCancel{0};
    std::atomic<bool> fBooting{false};
    std::atomic<int64_t> fBootStartMs{0};

    // la macchina unica (come l'hardware): RAM ed eMMC in un file sul disco,
    // condiviso da tutti i progetti. Una sola istanza alla volta (lucchetto sul
    // file): le altre restano mute e partono da sole quando quella si chiude.
    struct MachineLock {
#if defined(_WIN32)
        void* h = nullptr;
#else
        int fd = -1;
#endif
        bool acquire(const stplug::fs::path& path);
        void release();
    };
    bool tryOwn();
    bool saveMachine();
    MachineLock fMachineLock;
    std::atomic<bool> fOwner{false};
    double fWantRate = 0;                    // per partire quando la macchina si libera o arriva il firmware
    uint32_t fWantFrames = 0;
    std::mutex fSaveLock;
    std::thread fAutosave;
    std::mutex fAutosaveMx;
    std::condition_variable fAutosaveCv;
    bool fAutosaveStop = false;
    std::atomic<int64_t> fLastActivityMs{0};  // trasporto in moto o MIDI ricevuto (thread audio)
    std::atomic<bool> fSaveRequested{false};  // getState chiede, il thread salva
    std::atomic<bool> fRescan{false};         // "Scegli firmware": riguarda la cartella subito
    void requestSave();

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntaktPlugin)
};

END_NAMESPACE_DISTRHO
