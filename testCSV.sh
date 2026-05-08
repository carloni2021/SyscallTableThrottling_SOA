#!/bin/bash
# testCSV.sh — campagna di test sistematica, output CSV
#
# Esegue throttleTest su una matrice (threads × MAX) e raccoglie i risultati
# in un file CSV per l'analisi delle prestazioni del modulo.
#
# Uso:
#   sudo ./testCSV.sh [durata_sec] [output.csv]
#
# Default:
#   durata_sec = 6
#   output.csv = risultati_$(date).csv
#
# Esempio:
#   sudo ./testCSV.sh 6 campagna.csv

set -e

THROTTLE_TEST="./throttleTest"
DURATA="${1:-6}"
OUTPUT="${2:-risultati_$(date +%Y%m%d_%H%M%S).csv}"

# Matrice di test — modifica qui per estendere la campagna
THREADS=(1 2 4 8)
MAX_VALUES=(10 50 100)

# ----------------------------------------------------------------
#  Prerequisiti
# ----------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    echo "Errore: richiede root." >&2
    exit 1
fi
if [ ! -x "$THROTTLE_TEST" ]; then
    echo "Errore: $THROTTLE_TEST non trovato. Esegui 'make' prima." >&2
    exit 1
fi
if [ ! -c "/dev/throttleDriver" ]; then
    echo "Errore: /dev/throttleDriver non trovato. Esegui 'make load' prima." >&2
    exit 1
fi

# ----------------------------------------------------------------
#  Header CSV
# ----------------------------------------------------------------
echo "threads,MAX,avg_calls_finestra,peak_calls_finestra,avg_delay_ns,peak_delay_ns,avg_bloccati,peak_bloccati,esito" > "$OUTPUT"

TOTALE=$((${#THREADS[@]} * ${#MAX_VALUES[@]}))
N=0

echo "========================================================"
echo "  testCSV — campagna throttleDriver"
echo "========================================================"
echo "  Matrice  : ${#THREADS[@]} thread × ${#MAX_VALUES[@]} MAX = $TOTALE run"
echo "  Durata   : ${DURATA}s per run"
echo "  Output   : $OUTPUT"
echo "========================================================"
echo ""

# ----------------------------------------------------------------
#  Loop campagna
# ----------------------------------------------------------------
for t in "${THREADS[@]}"; do
    for m in "${MAX_VALUES[@]}"; do
        N=$((N + 1))
        printf "[%d/%d] threads=%-2d  MAX=%-5d  durata=%ds ... " "$N" "$TOTALE" "$t" "$m" "$DURATA"

        # Esegui throttleTest e cattura stdout+stderr
        out=$("$THROTTLE_TEST" "$t" "$DURATA" "$m" 2>&1) || true

        # Estrai metriche dal testo stampato da throttleTest
        avg_calls=$(echo  "$out" | awk '/Avg  calls\/finestra/{print $NF}')
        peak_calls=$(echo "$out" | awk '/Peak calls\/finestra/{print $NF}')
        # "Peak delay" può comparire anche in "Peak delay prog" e "Peak delay uid":
        # si filtra con !/prog/ && !/uid/
        peak_delay=$(echo "$out" | awk '/Peak delay/ && !/prog/ && !/uid/ {print $4}')
        avg_delay=$(echo  "$out" | awk '/Avg  delay/  && !/prog/ && !/uid/ {print $4}')
        peak_blocc=$(echo "$out" | awk '/Peak bloccati/{print $4}')
        avg_blocc=$(echo  "$out" | awk '/Avg  bloccati/{print $4}')
        esito=$(echo      "$out" | grep -E "^\s+(PASS|FAIL)$" | tr -d ' ')

        # Fallback se parsing fallisce (es. driver non ha restituito stats)
        avg_calls="${avg_calls:-N/A}"
        peak_calls="${peak_calls:-N/A}"
        peak_delay="${peak_delay:-N/A}"
        avg_delay="${avg_delay:-N/A}"
        peak_blocc="${peak_blocc:-N/A}"
        avg_blocc="${avg_blocc:-N/A}"
        esito="${esito:-N/A}"

        echo "$t,$m,$avg_calls,$peak_calls,$avg_delay,$peak_delay,$avg_blocc,$peak_blocc,$esito" >> "$OUTPUT"
        echo "$esito  (avg=$avg_calls  peak_delay=${peak_delay}ns  peak_blocc=$peak_blocc)"

        # Pausa breve per lasciare il modulo in stato pulito tra un run e l'altro
        sleep 1
    done
done

echo ""
echo "========================================================"
echo "  Campagna completata — $TOTALE run"
echo "  Risultati in: $OUTPUT"
echo "========================================================"
echo ""
cat "$OUTPUT"
