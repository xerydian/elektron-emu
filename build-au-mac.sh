#!/usr/bin/env bash
# Gli Audio Unit (AUv2, .component) delle cinque macchine, per Logic Pro e
# GarageBand. Solo macOS su Apple Silicon, come i VST3.
#
#   ./build-au-mac.sh [MACCHINA ...]
#
# MACCHINA: Digitakt-II Digitone-II Syntakt Analog-Rytm-MKII Model-Cycles
# (predefinite: tutte). Per ognuna:
#   1. lancia <macchina>/source/vst/scripts/build-mac.sh, che costruisce il .vst3
#      e ora anche il .component (cmake: WITH_AU, attivo di default su macOS);
#   2. mette nel .component la foto del pannello presa dal .vst3 appena fatto;
#   3. lo firma ad hoc e lo installa in ~/Library/Audio/Plug-Ins/Components;
#   4. lo verifica con auval (come fa Logic al primo avvio).
#
# Variabili:
#   WORK_ROOT   cartella di lavoro comune (predefinita: quelle di build-mac.sh,
#               ~/.cache/<sigla>-vst, cosi' Unicorn e DPF gia' scaricati si riusano)
#   NO_INSTALL=1  costruisce senza installare
#   NO_AUVAL=1    non lancia auval
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[[ $(uname -s) == Darwin ]] || { echo "build-au-mac: serve macOS" >&2; exit 2; }

# gli strumenti: senza pkg-config il configure di Unicorn fallisce e lascia una build rotta
missing=()
for t in cmake ninja pkg-config python3 git; do command -v "$t" >/dev/null || missing+=("$t"); done
xcrun --find clang >/dev/null 2>&1 || missing+=("Xcode command line tools (xcode-select --install)")
if [[ ${#missing[@]} -gt 0 ]]; then
  echo "build-au-mac: mancano: ${missing[*]}" >&2
  echo "  brew install cmake ninja pkg-config python3" >&2
  exit 2
fi

# sigla della cartella di lavoro di build-mac.sh e sottotipo AU
# (DISTRHO_PLUGIN_UNIQUE_ID in source/vst/DistrhoPluginInfo.h; il produttore e' Simo)
info() {
  case $1 in
    Digitakt-II)      echo "dt2 dk2v" ;;
    Digitone-II)      echo "dn  dtn2" ;;
    Syntakt)          echo "st  sntk" ;;
    Analog-Rytm-MKII) echo "ar  aryv" ;;
    Model-Cycles)     echo "mc  mcyv" ;;
    *) return 1 ;;
  esac
}

machines=("$@")
[[ ${#machines[@]} -gt 0 ]] || machines=(Digitakt-II Digitone-II Syntakt Analog-Rytm-MKII Model-Cycles)

components=$HOME/Library/Audio/Plug-Ins/Components
ok=()
ko=()

for m in "${machines[@]}"; do
  m=${m%/}
  read -r tag sub < <(info "$m") || { echo "build-au-mac: macchina sconosciuta: $m" >&2; exit 2; }
  if [[ -n ${WORK_ROOT:-} ]]; then work=$WORK_ROOT/$m; else work=$HOME/.cache/$tag-vst; fi
  echo "=== $m"

  # una build di Unicorn fallita a meta' (configure senza pkg-config) va rifatta da capo
  if [[ -d $work/unicorn-work/build && ! -f $work/unicorn-mac/lib/libunicorn.a \
        && ! -f $work/unicorn-m68k-st/lib/libunicorn.a ]]; then
    rm -rf "$work/unicorn-work/build"
  fi

  INSTALL=0 NO_INSTALL=1 "$here/$m/source/vst/scripts/build-mac.sh" "$work"

  bin=$work/mac/vst-build/bin
  comp=$(find "$bin" -maxdepth 1 -name '*.component' -type d | head -1)
  if [[ -z $comp ]]; then
    echo "build-au-mac: nessun .component in $bin (cmake -DWITH_AU=OFF nella cache?)" >&2
    ko+=("$m"); continue
  fi

  # la foto del pannello: la stessa del .vst3 (il nome del .vst3 cambia per il Digitone II)
  vst=$(find "$bin" -maxdepth 1 -name '*.vst3' -type d | head -1)
  mkdir -p "$comp/Contents/Resources"
  rm -f "$comp/Contents/Resources/"*_panel.png
  if [[ -n $vst ]]; then
    for img in "$vst/Contents/Resources/"*_panel.png; do
      [[ -f $img ]] && cp "$img" "$comp/Contents/Resources/"
    done
  fi
  codesign -f -s - "$comp" 2>/dev/null

  if [[ ${NO_INSTALL:-0} == 1 ]]; then
    echo "pronto: $comp (non installato)"
    ok+=("$m"); continue
  fi
  name=$(basename "$comp")
  mkdir -p "$components"
  rm -rf "${components:?}/$name"
  cp -R "$comp" "$components/$name"
  echo "installato $components/$name"

  if [[ ${NO_AUVAL:-0} == 1 ]]; then ok+=("$m"); continue; fi
  # macOS tiene una cache dei componenti: si fa ripartire chi la tiene
  killall -9 AudioComponentRegistrar 2>/dev/null || true
  log=$work/auval-$sub.log
  if auval -v aumu "$sub" Simo >"$log" 2>&1 && grep -q 'AU VALIDATION SUCCEEDED' "$log"; then
    echo "auval: ok (aumu $sub Simo)"
    ok+=("$m")
  else
    echo "auval: NON passato per aumu $sub Simo, vedi $log" >&2
    tail -20 "$log" >&2 || true
    ko+=("$m")
  fi
done

echo
[[ ${#ok[@]} -gt 0 ]] && echo "ok: ${ok[*]}"
if [[ ${#ko[@]} -gt 0 ]]; then echo "problemi: ${ko[*]}" >&2; exit 1; fi
[[ ${NO_INSTALL:-0} == 1 ]] || echo "In Logic Pro: Impostazioni > Plug-in Manager > Reimposta e riesamina la selezione, se non li vede."
