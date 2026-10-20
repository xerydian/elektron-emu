// SyntaktPlugin.cpp -- vedi SyntaktPlugin.hpp. La logica e' quella dell'AU
// (au/Extension/SyntaktAudioUnit.mm) e del VST3 del Digitakt II
// (digitakt/emulator/vst): stato intero nel progetto, Sync DAW, MIDI in/out,
// macchina unica sul disco con salvataggio in due fasi, una sola istanza.
//
// Differenza decisiva: il firmware NON e' nel bundle. Si cerca in
// <dati>/Firmware/ (SyntaktStore.hpp); lo stato iniziale si genera al primo
// avvio dal boot freddo del firmware (st_coldboot_session) e si tiene in cache.
#include "SyntaktPlugin.hpp"
#include "DistrhoPluginUtils.hpp"
#include "extra/Base64.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifndef ST_COLDBOOT_REV
#define ST_COLDBOOT_REV 1       // da alzare se cambia lo stato che il primo avvio produce
#endif

// il boot freddo in C (core/src/st_boot.inc.c); finche' manca, ColdBootStub.cpp
extern "C" int st_coldboot_session(const dt_firmware* fw, int compress, uint8_t** out, size_t* len,
                                   volatile int* cancel, char* err, size_t errlen);

START_NAMESPACE_DISTRHO

namespace {

using namespace stplug;

bool logOn() { return std::getenv("STCORE_LOG") != nullptr; }

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// rt.c parte con il volume gia' "applicato" a 0,25: 0,25 esatto non arriverebbe al
// motore. Il successivo double va bene: il motore lo usa in float32.
double rtVolume(double v) { return v == 0.25 ? std::nextafter(0.25, 1.0) : v; }

} // namespace

// diagnostica: STCORE_MIDI_TRACE=1 stampa ogni messaggio mandato al MIDI IN emulato
// (blocco, scarto, byte); la variabile si legge una volta sola
static bool midiTraceOn()
{
    static const bool on = std::getenv("STCORE_MIDI_TRACE") != nullptr;
    return on;
}
static uint64_t gTraceBlock = 0;
static int tracedMidi(dt_rt* rt, uint32_t off, const uint8_t* d, size_t n)
{
    if (midiTraceOn()) {
        std::fprintf(stderr, "midi blocco %llu +%u:", (unsigned long long)gTraceBlock, off);
        for (size_t i = 0; i < n; ++i) std::fprintf(stderr, " %02x", d[i]);
        std::fprintf(stderr, "\n");
    }
    return ::dt_rt_midi(rt, off, d, n);
}
#define dt_rt_midi tracedMidi

// ------------------------------------------------------------------ HostSync
// Come HostSync dell'AU: gli host non mandano Start/Stop/clock ai plugin, li
// generiamo dal trasporto e li mettiamo nel MIDI IN emulato all'istante giusto.
// Il clock a 24 ppq si ricalcola a ogni blocco dalla posizione in beat.
void SyntaktPlugin::HostSync::run(SyncOut& out, const TimePosition& t, uint32_t frames, double rate)
{
    if (!t.bbt.valid || rate <= 0 || t.bbt.ticksPerBeat <= 0) return;
    const double q = t.bbt.beatType > 0 ? 4.0 / t.bbt.beatType : 1.0;          // beat -> semiminime
    const double beat = ((t.bbt.bar - 1) * (double)t.bbt.beatsPerBar + (t.bbt.beat - 1)
                         + t.bbt.tick / t.bbt.ticksPerBeat) * q;
    double tempo = t.bbt.beatsPerMinute > 0 ? t.bbt.beatsPerMinute * q : 120;
    const bool moving = t.playing;
    const double spb = tempo / 60.0 / rate;                                     // semiminime per campione
    uint8_t b[3];
    auto sppContinue = [&](uint32_t off) {
        const int spp = static_cast<int>(beat * 4 + 1e-9);                       // in sedicesimi
        b[0] = 0xF2; b[1] = spp & 0x7F; b[2] = (spp >> 7) & 0x7F;
        out.add(off, b, 3);
        b[0] = 0xFB;                                                            // Continue
        out.add(off, b, 1);
    };
    if (!started) {
        started = true;
        if (!moving) { b[0] = 0xFC; out.add(0, b, 1); }   // la DAW e' ferma: Stop
    }
    if (moving && !wasMoving) {
        if (beat < 1e-6) { b[0] = 0xFA; out.add(0, b, 1); }               // Start
        else sppContinue(0);
        nextTick = static_cast<int64_t>(std::ceil(beat * 24 - 1e-9));
    } else if (moving && std::fabs(beat - expectedBeat) > 1.0 / 48) {
        b[0] = 0xFC;                                                            // loop o salto
        out.add(0, b, 1);
        sppContinue(0);
        nextTick = static_cast<int64_t>(std::ceil(beat * 24 - 1e-9));
    }
    if (moving) {
        const double end = beat + frames * spb;
        b[0] = 0xF8;                                                            // clock
        for (;; ++nextTick) {
            double off = (nextTick / 24.0 - beat) / spb;
            if (off < 0) off = 0;
            const uint32_t o = static_cast<uint32_t>(off + 0.5);                // arrotonda: niente 199 per 199,9999
            if (o >= frames) break;                                             // e' del blocco dopo
            out.add(o, b, 1);
        }
        expectedBeat = end;
    } else if (wasMoving) {
        b[0] = 0xFC;                                                            // Stop
        out.add(0, b, 1);
    }
    wasMoving = moving;
}

// ------------------------------------------------------------------ plugin
SyntaktPlugin::SyntaktPlugin()
    : Plugin(kParamCount, 0, 1)
{
    fStatus.kind = EngineStatus::Starting;
    fStatus.text = "engine not started yet";
    if (isDummyInstance()) return;
    // ogni secondo: la macchina si e' liberata? e' arrivato il firmware? Ogni minuto
    // salva, ma solo a Syntakt ferma (trasporto fermo e niente MIDI da 5 s): la
    // fotografia ferma il motore per qualche decina di ms
    fAutosave = std::thread([this] {
        std::unique_lock<std::mutex> g(fAutosaveMx);
        int since = 0;
        int64_t tick = nowMs();
        while (!fAutosaveStop) {
            fAutosaveCv.wait_for(g, std::chrono::seconds(1),
                                 [this] { return fAutosaveStop || fSaveRequested.load() || fRescan.load(); });
            if (fAutosaveStop) break;
            g.unlock();
            if (nowMs() - tick >= 1000) { tick = nowMs(); ++since; }
            fRescan = false;
            if (!fRt.load() && fWantRate > 0 && !fBooting.load()) {
                startEngine(fWantRate, fWantFrames);        // firmware nuovo o macchina libera
            } else if (fSaveRequested.exchange(false)) {    // chiesto (getState): subito
                saveMachine();
                since = 0;
            } else if (since >= 60 && nowMs() - fLastActivityMs.load() > 5000) {
                saveMachine();
                since = 0;
            }
            g.lock();
        }
    });
}

SyntaktPlugin::~SyntaktPlugin()
{
    {
        std::lock_guard<std::mutex> g(fAutosaveMx);
        fAutosaveStop = true;
    }
    fAutosaveCv.notify_all();
    if (fAutosave.joinable()) fAutosave.join();
    fBootCancel = 1;
    if (fBoot.joinable()) fBoot.join();
    saveMachine();                               // si spegne: la macchina resta com'era
    if (logOn())
        if (dt_rt* rt = fRt.load())
            std::fprintf(stderr, "stcore: buchi %lld, ritardo massimo %lld campioni\n", (long long)dt_rt_underruns(rt),
                         (long long)dt_rt_max_lag(rt));
    swapEngine(nullptr);
    fMachineLock.release();
}

void SyntaktPlugin::setStatus(EngineStatus::Kind k, const std::string& text)
{
    if (logOn()) std::fprintf(stderr, "stcore: stato %d: %s\n", int(k), text.c_str());
    std::lock_guard<std::mutex> g(fLock);
    fStatus.kind = k;
    fStatus.text = text;
}

EngineStatus SyntaktPlugin::status()
{
    EngineStatus s;
    {
        std::lock_guard<std::mutex> g(fLock);
        s = fStatus;
        s.firmwareLabel = fFw.kind >= FirmwareInfo::Unknown ? fFw.label : std::string();
        s.warning = fFw.note;
    }
    s.firmwareDir = utf8(firmwareDir());
    if (fBooting.load()) s.bootSeconds = (nowMs() - fBootStartMs.load()) / 1000.0;
    if (dt_rt* rt = fRt.load()) {
        const char* r = dt_rt_status(rt);
        if (std::strcmp(r, "running")) { s.kind = EngineStatus::Error; s.text = r; }
        else if (s.kind != EngineStatus::Running) { s.kind = EngineStatus::Running; s.text = "running"; }
    }
    return s;
}

// ------------------------------------------------------------------ la macchina unica
bool SyntaktPlugin::MachineLock::acquire(const fs::path& path)
{
#if defined(_WIN32)
    // aperto in esclusiva finche' l'istanza vive: Windows lo libera anche se il processo muore
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    h = f;
    return true;
#else
    const int f = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (f < 0) return false;
    if (::flock(f, LOCK_EX | LOCK_NB) != 0) { ::close(f); return false; }   // anche fra istanze dello stesso processo
    fd = f;
    return true;
#endif
}

void SyntaktPlugin::MachineLock::release()
{
#if defined(_WIN32)
    if (h) CloseHandle(static_cast<HANDLE>(h));
    h = nullptr;
#else
    if (fd >= 0) ::close(fd);
    fd = -1;
#endif
}

fs::path SyntaktPlugin::machinePath() const
{
    const fs::path dir = dataDir();
    if (dir.empty() || fFw.data.empty()) return fs::path();
    // una macchina per firmware ("-g1": generazione del formato)
    return dir / ("syntakt-macchina-" + shaPrefix(fFw.data.data(), fFw.data.size()) + "-g1.stsz");
}

bool SyntaktPlugin::tryOwn()
{
    if (fOwner.load()) return true;
    if (isDummyInstance()) return false;
    const fs::path m = machinePath();
    if (m.empty()) return false;
    makeDirs(dataDir());
    fs::path lock = m;
    lock += ".lock";
    fOwner = fMachineLock.acquire(lock);
    if (logOn()) std::fprintf(stderr, "stcore: macchina %s (%s)\n", utf8(m).c_str(), fOwner ? "presa" : "occupata");
    return fOwner.load();
}

void SyntaktPlugin::requestSave()
{
    if (!fOwner.load()) return;
    {
        std::lock_guard<std::mutex> g(fAutosaveMx);
        fSaveRequested = true;
    }
    fAutosaveCv.notify_all();
}

bool SyntaktPlugin::saveMachine()
{
    if (!fOwner.load()) return false;
    std::lock_guard<std::mutex> g(fSaveLock);
    dt_rt* rt = fRt.load();
    const fs::path m = machinePath();
    if (!rt || m.empty()) return false;
    uint8_t* buf = nullptr;
    size_t len = 0;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    if (dt_rt_save(rt, &buf, &len) == 0 && buf) ok = writeFile(m, buf, len);
    std::free(buf);
    if (logOn())
        std::fprintf(stderr, "stcore: macchina salvata (%zu byte, %.0f ms) %s\n", len,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), ok ? "ok" : "ERRORE");
    return ok;
}

void SyntaktPlugin::initAudioPort(bool input, uint32_t index, AudioPort& port)
{
    if (input) return;
    const uint32_t bus = index / 2;
    static const char* names[] = {"Main", "Main (no Analog FX)", "Analog FX send"};
    static const char* syms[] = {"main", "dry", "fxsend"};
    port.groupId = bus;
    port.name = String(names[bus]) + (index % 2 ? " R" : " L");
    port.symbol = String(syms[bus]) + (index % 2 ? "_r" : "_l");
}

void SyntaktPlugin::initPortGroup(uint32_t groupId, PortGroup& group)
{
    static const char* names[] = {"Main", "Main (no Analog FX)", "Analog FX send"};
    static const char* syms[] = {"main", "dry", "fxsend"};
    group.name = names[groupId];
    group.symbol = syms[groupId];
}

void SyntaktPlugin::initParameter(uint32_t index, Parameter& p)
{
    if (index == kParamVolume) {
        p.hints = kParameterIsAutomatable;
        p.name = "Main Volume";
        p.symbol = "volume";
        p.ranges.min = 0.0f; p.ranges.max = 1.0f; p.ranges.def = 0.8f;       // staudio.VOLUME
    } else if (index == kParamSync) {
        p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
        p.name = "Sync DAW";
        p.symbol = "syncdaw";
        p.ranges.min = 0.0f; p.ranges.max = 1.0f; p.ranges.def = 1.0f;
    }
}

void SyntaktPlugin::initState(uint32_t, State& s)
{
    // come l'hardware la macchina e' una sola, sul disco (machinePath): il progetto
    // della DAW non la contiene. La chiave resta per i progetti con lo stato dentro:
    // se la macchina non c'e' ancora, il loro stato diventa la macchina.
    s.hints = kStateIsOnlyForDSP | kStateIsBase64Blob;
    s.key = "stcore_state";
    s.defaultValue = "";
    s.label = "Syntakt state";
}

float SyntaktPlugin::getParameterValue(uint32_t index) const
{
    if (index == kParamVolume) return static_cast<float>(fMaster.load());
    if (index == kParamSync) return fSync.load() ? 1.0f : 0.0f;
    return 0.0f;
}

void SyntaktPlugin::setParameterValue(uint32_t index, float value)
{
    if (index == kParamVolume) {
        const double v = value < 0 ? 0 : (value > 1.0f ? 1.0 : value);
        fMaster = v;
        if (dt_rt* rt = fRt.load()) dt_rt_set_master(rt, rtVolume(v));
    } else if (index == kParamSync) {
        fSync = value >= 0.5f;
    }
}

void SyntaktPlugin::setMasterFromUI(double v)
{
    fMaster = v < 0 ? 0 : (v > 1.0 ? 1.0 : v);
    if (dt_rt* rt = fRt.load()) dt_rt_set_master(rt, rtVolume(fMaster.load()));
}

String SyntaktPlugin::getState(const char* key) const
{
    // la DAW salva il progetto (o chiede lo stato per altro, anche spesso): la
    // macchina si salva nel thread di salvataggio, mai qui
    if (!std::strcmp(key, "stcore_state")) const_cast<SyntaktPlugin*>(this)->requestSave();
    return String();
}

void SyntaktPlugin::setState(const char* key, const char* value)
{
    if (std::strcmp(key, "stcore_state") || !value || !*value) return;
    // progetto con lo stato dentro: conta solo se la macchina unica non c'e' ancora
    {
        const fs::path m = machinePath();
        std::error_code ec;
        if (!m.empty() && fs::exists(m, ec)) return;
    }
    std::vector<uint8_t> st = d_getChunkFromBase64String(value);
    if (st.size() < 16) return;
    {
        std::lock_guard<std::mutex> g(fLock);
        fPendingState = std::move(st);
    }
    std::lock_guard<std::recursive_mutex> sg(fStartLock);
    if (fRt.load() && fRate > 0) {                 // gia' in moto: si riparte da quello stato
        const double rate = fRate;
        fRate = 0;
        startEngine(rate, fRtFrames);
    }
}

fs::path SyntaktPlugin::coldBootCachePath() const
{
    const fs::path dir = cacheDir();
    if (dir.empty() || fFw.data.empty()) return fs::path();
    char name[160];
    std::snprintf(name, sizeof name, "primo-avvio-%s-%s-r%d.stsz", shaPrefix(fFw.data.data(), fFw.data.size()).c_str(),
                  STCORE_VERSION, ST_COLDBOOT_REV);
    return dir / name;
}

void SyntaktPlugin::swapEngine(dt_rt* rt)
{
    dt_rt* old = fRt.exchange(rt);
    // run() ha caricato il vecchio puntatore prima dello scambio: si aspetta che finisca
    while (fInRun.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (old) dt_rt_free(old);
}

// la cartella del firmware: la crea se manca e sceglie il .syx (a meno che non sia
// cambiato nulla dall'ultima volta)
bool SyntaktPlugin::loadFirmware()
{
    const fs::path dir = firmwareDir();
    if (dir.empty()) {
        setStatus(EngineStatus::Error, "data folder not available");
        return false;
    }
    makeDirs(dir);
    const std::string sig = dirSignature(dir);
    {
        std::lock_guard<std::mutex> g(fLock);
        if (fScanned && fFwSignature == sig) {              // nulla di nuovo nella cartella
            if (fFw.data.empty()) { fStatus.kind = EngineStatus::NoFirmware; fStatus.text = "firmware missing"; }
            return !fFw.data.empty();
        }
    }
    FirmwareInfo fi = findFirmware(dir);
    std::lock_guard<std::mutex> g(fLock);
    fScanned = true;
    fFwSignature = fi.signature;
    if (fi.kind >= FirmwareInfo::Unknown) {
        fFw = std::move(fi);
        return true;
    }
    const std::string note = fi.note;
    fFw = FirmwareInfo();
    fFw.note = note;                                // il motivo, se c'era un file ma non valido
    fStatus.kind = EngineStatus::NoFirmware;
    fStatus.text = "firmware missing";
    if (logOn()) std::fprintf(stderr, "stcore: firmware missing in %s %s\n", utf8(dir).c_str(), note.c_str());
    return false;
}

std::string SyntaktPlugin::installFirmware(const std::string& srcUtf8)
{
    const fs::path src = fromUtf8(srcUtf8);
    std::vector<uint8_t> bytes;
    if (!readFile(src, bytes)) return "cannot read file: " + srcUtf8;
    FirmwareInfo fi = checkFirmware(src, std::move(bytes));
    if (fi.kind == FirmwareInfo::Invalid) return fi.note;
    const fs::path dir = firmwareDir();
    if (!makeDirs(dir)) return "cannot create " + utf8(dir);
    const fs::path dest = dir / src.filename();
    std::error_code ec;
    if (!(fs::exists(dest, ec) && fs::equivalent(src, dest, ec))) {
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) return "copy failed: " + ec.message();
    }
    fRescan = true;
    fAutosaveCv.notify_all();
    return std::string();
}

bool SyntaktPlugin::startEngine(double rate, uint32_t frames)
{
    std::lock_guard<std::recursive_mutex> sg(fStartLock);
    if (fRt.load() && rate == fRate && frames <= fRtFrames) return true;
    fWantRate = rate;
    fWantFrames = frames;
    std::vector<uint8_t> src;
    if (dt_rt* cur = fRt.load()) {                 // cambio di frequenza: si riparte dallo stato attuale
        uint8_t* buf = nullptr;
        size_t len = 0;
        if (dt_rt_save(cur, &buf, &len) == 0 && buf) src.assign(buf, buf + len);
        std::free(buf);
    } else if (!loadFirmware()) {
        return false;
    }
    if (!tryOwn()) {                               // una sola Syntakt: questa istanza aspetta
        setStatus(EngineStatus::Busy, "Syntakt is already open in another instance: close that one and this one starts by itself");
        return false;
    }
    bool fromMachine = false;
    if (src.empty()) {                             // la macchina unica, se c'e'
        const fs::path m = machinePath();
        fromMachine = !m.empty() && readFile(m, src);
    }
    if (src.empty()) {
        std::lock_guard<std::mutex> g(fLock);
        src = fPendingState;                       // un progetto con lo stato dentro, o il primo avvio appena fatto
    }
    if (src.empty()) {
        // senza stato: la prima sessione dal firmware, una volta sola, poi dalla cache
        std::vector<uint8_t> boot;
        const fs::path cache = coldBootCachePath();
        if (cache.empty() || !readFile(cache, boot)) {
            coldBoot(rate, frames);
            return true;
        }
        src = std::move(boot);
    }
    char err[256] = "";
    dt_rt* rt = dt_rt_new_mem(fFw.data.data(), fFw.data.size(), src.data(), src.size(), rate, frames, err, sizeof err);
    if (!rt) {
        setStatus(EngineStatus::Error, std::string("engine: ") + err);
        return false;
    }
    dt_rt_set_master(rt, rtVolume(fMaster.load()));
    swapEngine(rt);
    fRate = rate;
    fRtFrames = frames;
    setLatency(dt_rt_latency(rt));
    if (!fromMachine) requestSave();             // la prima volta lo stato diventa la macchina
    setStatus(EngineStatus::Running, "running");
    return true;
}

void SyntaktPlugin::coldBoot(double rate, uint32_t frames)
{
    if (fBooting.exchange(true)) return;
    if (fBoot.joinable()) fBoot.join();
    fBootStartMs = nowMs();
    setStatus(EngineStatus::Booting, "First start: booting the firmware (once only)");
    setLatency(dt_rt_latency_for(rate, frames));
    fBoot = std::thread([this, rate, frames] {
        dt_firmware fwd;
        char err[256] = "";
        uint8_t* buf = nullptr;
        size_t len = 0;
        int r = -1;
        std::vector<uint8_t> dev;
        if (const char* dp = std::getenv("ST_DEV_STATE")) {      // prove: uno stato fatto fuori (tools/snap2dtst.py)
            if (readFile(fromUtf8(dp), dev)) {
                buf = static_cast<uint8_t*>(std::malloc(dev.size()));
                if (buf) { std::memcpy(buf, dev.data(), dev.size()); len = dev.size(); r = 0; }
            }
        } else if (dt_fw_extract(fFw.data.data(), fFw.data.size(), &fwd, err, sizeof err) == 0) {
            r = st_coldboot_session(&fwd, 1, &buf, &len, reinterpret_cast<volatile int*>(&fBootCancel), err, sizeof err);
            dt_fw_free(&fwd);
        }
        if (r == 0 && buf && !fBootCancel.load()) {
            const fs::path cache = coldBootCachePath();
            if (!cache.empty() && makeDirs(cache.parent_path())) writeFile(cache, buf, len);
            {
                std::lock_guard<std::mutex> g(fLock);
                if (fPendingState.empty()) fPendingState.assign(buf, buf + len);
            }
            std::free(buf);
            fBooting = false;                    // startEngine non deve ripartire da coldBoot() e fare join di se' stesso
            startEngine(rate, frames);
            return;
        }
        std::free(buf);
        if (!fBootCancel.load()) setStatus(EngineStatus::Error, std::string("cold boot: ") + err);
        fBooting = false;
    });
}

void SyntaktPlugin::ioChanged(uint16_t, uint16_t numOutputs)
{
    fNumOutputs = numOutputs < DISTRHO_PLUGIN_NUM_OUTPUTS ? numOutputs : DISTRHO_PLUGIN_NUM_OUTPUTS;
}

void SyntaktPlugin::activate()
{
    const uint32_t frames = getBufferSize();
    fMaxFrames = frames;
    fBuf.assign(static_cast<size_t>(kChannels) * frames, 0.0f);
    for (int i = 0; i < kChannels; ++i) fCh[i] = fBuf.data() + static_cast<size_t>(i) * frames;
    fHostSync = HostSync();
    if (!fBooting.load()) startEngine(getSampleRate(), frames);
}

void SyntaktPlugin::run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount)
{
    ++gTraceBlock;
    fInRun.store(1);
    dt_rt* rt = fRt.load();
    if (rt && frames <= fMaxFrames) {
        fSyncOut.n = 0;
        if (fSync.load()) fHostSync.run(fSyncOut, getTimePosition(), frames, getSampleRate());
        if (midiCount || getTimePosition().playing) fLastActivityMs.store(nowMs());
        // gli eventi del trasporto e quelli dell'host in ordine di tempo (rt.c li
        // consegna nell'ordine in cui arrivano: un clock a +200 prima di una nota a
        // +0 la ritarderebbe a +200); a pari istante prima il trasporto
        uint32_t is = 0, ih = 0;
        while (is < fSyncOut.n || ih < midiCount) {
            const bool takeSync = is < fSyncOut.n &&
                (ih >= midiCount || fSyncOut.ev[is].off <= (midi[ih].frame < frames ? midi[ih].frame : frames - 1));
            if (takeSync) {
                const SyncOut::Ev& e = fSyncOut.ev[is++];
                dt_rt_midi(rt, e.off, e.d, e.n);
            } else {
                const MidiEvent& ev = midi[ih++];
                const uint8_t* d = ev.size > MidiEvent::kDataSize ? ev.dataExt : ev.data;
                if (d && ev.size) dt_rt_midi(rt, ev.frame < frames ? ev.frame : frames - 1, d, ev.size);
            }
        }
        // export della DAW (VST3 V3_OFFLINE): si aspetta il motore invece di perdere campioni
        dt_rt_render(rt, frames, fCh, kChannels, isRenderingOffline() ? 1 : 0);
        uint8_t mb[256];
        uint32_t off = 0;
        size_t n;
        while ((n = dt_rt_midi_out(rt, mb, sizeof mb, &off, frames)) > 0) {
            MidiEvent ev;
            ev.frame = off < frames ? off : frames - 1;
            ev.size = static_cast<uint32_t>(n);
            if (n <= MidiEvent::kDataSize) {
                std::memcpy(ev.data, mb, n);
                ev.dataExt = nullptr;
            } else {
                ev.dataExt = mb;                 // SysEx: l'host la copia dentro writeMidiEvent
            }
            if (!writeMidiEvent(ev)) break;
        }
        fInRun.store(0);
        for (uint32_t i = 0; i < fNumOutputs; ++i)
            std::memcpy(outputs[i], fCh[i], frames * sizeof(float));
        return;
    }
    fInRun.store(0);
    for (uint32_t i = 0; i < fNumOutputs; ++i) std::memset(outputs[i], 0, frames * sizeof(float));
}

// ------------------------------------------------------------------ per la vista
uint64_t SyntaktPlugin::copyScreen(uint8_t screen[128 * 64], uint8_t leds[50])
{
    dt_rt* rt = fRt.load();
    return rt ? dt_rt_screen(rt, screen, leds) : 0;
}

void SyntaktPlugin::panel(int kind, int code, int arg)
{
    if (dt_rt* rt = fRt.load()) dt_rt_panel(rt, kind, code, arg);
}

Plugin* createPlugin()
{
    return new SyntaktPlugin();
}

END_NAMESPACE_DISTRHO
