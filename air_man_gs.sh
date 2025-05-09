#!/usr/bin/env bash
set -euo pipefail

PORT=12355
VERBOSE=0

print_help() {
  cat <<EOF
Usage:
  $0 [--verbose] <server_ip> "<command>"
  $0 --help

Options:
  -v, --verbose   Enable debug output
  -h, --help      Show this help message

Commands (use quotes for multiple words):
  start_alink
  stop_alink
  restart_majestic
  "change_channel <n>"
  confirm_channel_change
  "set_alink_power <0–10>"
  "set_video_mode <size> <fps> <exposure> '<crop>'"
  restart_wfb
  restart_msposd
  (and any air_man_cmd.sh commands)
EOF
}

# Parse flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    -v|--verbose) VERBOSE=1; shift ;;
    -h|--help)     print_help; exit 0 ;;
    --)            shift; break ;;
    -*)            echo "Unknown option: $1" >&2; print_help; exit 1 ;;
    *)             break ;;
  esac
done

if [[ $# -lt 2 ]]; then
  print_help; exit 1
fi

SERVER_IP=$1; shift
CMD="$1"

[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Command → $CMD"

# Translate legacy alias
if [[ $CMD =~ ^set\ air\ wfbng\ air_channel\ ([0-9]+)$ ]]; then
  CMD="change_channel ${BASH_REMATCH[1]}"
  [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Alias → $CMD"
fi

#
# 1) Non-change_channel: try up to 3×, then exit
#
if [[ ! $CMD =~ ^change_channel\ ([0-9]+)$ ]]; then
  MAX=3
  for i in $(seq 1 $MAX); do
    [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Simple cmd attempt $i/$MAX"
    set +e
    RESPONSE=$(printf '%s\n' "$CMD" | nc -w2 "$SERVER_IP" $PORT)
    STAT=$?
    set -e
    if [[ $STAT -eq 0 && -n $RESPONSE ]]; then
      echo "$RESPONSE"
      exit 0
    fi
    sleep 0.5
  done
  echo "No response from VTX after $MAX attempts"
  exit 1
fi

#
# 2) change_channel logic (with 3× retries on no reply)
#
CH=${BASH_REMATCH[1]}
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Sending change_channel $CH"

MAX_TRIES=3
SLEEP_BETWEEN=0.5
RESPONSE_LINES=()

for (( try=1; try<=MAX_TRIES; try++ )); do
  [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] change_channel attempt $try/$MAX_TRIES"

  # send the command and keep the socket open 2s
  mapfile -t RESPONSE_LINES < <(
    {
      printf 'change_channel %d\n' "$CH"
      sleep 2
    } | nc -w3 "$SERVER_IP" $PORT
  )

  if (( ${#RESPONSE_LINES[@]} > 0 )); then
    [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Received reply on attempt $try"
    break
  fi

  [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] No reply yet; sleeping ${SLEEP_BETWEEN}s"
  sleep "$SLEEP_BETWEEN"
done

if (( ${#RESPONSE_LINES[@]} == 0 )); then
  echo "No reply from VTX on change_channel after $MAX_TRIES attempts"
  exit 1
fi

# Print every line the server sent
for line in "${RESPONSE_LINES[@]}"; do
  echo "$line"
done

# Abort on any “Failed”
for line in "${RESPONSE_LINES[@]}"; do
  if [[ "$line" == *Failed* ]]; then
    [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Detected failure; aborting"
    exit 1
  fi
done

[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Server change_channel succeeded → proceeding with local reconfigure"


#
# 3) Local NIC reconfiguration (use first WFB_NICS for ORIG)
#

# 3.1) Grab the raw WFB_NICS value (strip quotes)
. /etc/default/wifibroadcast
RAW_NICS=$WFB_NICS
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Raw WFB_NICS string: '$RAW_NICS'"

# 3.2) Split on whitespace into an array
read -ra NICS <<< "$RAW_NICS"
[[ $VERBOSE -eq 1 ]] && printf "[DEBUG] Parsed NICS: %s\n" "${NICS[@]}"


# 3.2) Split on whitespace into an array
read -ra NICS <<< "$RAW_NICS"
[[ $VERBOSE -eq 1 ]] && printf "[DEBUG] Parsed NICS: %s\n" "${NICS[@]}"

# 3.3) Use the first NIC for the original-channel check
FIRST_NIC="${NICS[0]}"
ORIG=$(
  iw dev "$FIRST_NIC" info 2>/dev/null \
    | awk '/channel/ {print $2; exit}' \
    || echo "unknown"
)
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Original local channel (on $FIRST_NIC): $ORIG"

# 3.4) Read bandwidth, strip non-digits, and choose MODE
RAW_BW_LINE=$(grep -E '^\s*bandwidth' /etc/wifibroadcast.cfg || true)
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Raw bandwidth line: '$RAW_BW_LINE'"

BANDWIDTH=$(
  awk -F= '/^\s*bandwidth/ {
    gsub(/[^0-9]/, "", $2)
    print $2
    exit
  }' /etc/wifibroadcast.cfg || echo
)
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Parsed BANDWIDTH='$BANDWIDTH'"

case "$BANDWIDTH" in
  10) MODE="10MHz" ;;
  40) MODE="HT40+" ;;
  80) MODE="80MHz" ;;
  *)  MODE="" ;;
esac
[[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Will use channel mode: '$MODE'"


# 3.5) Apply the new channel on each NIC in the list
for nic in "${NICS[@]}"; do
  if [[ -n "$MODE" ]]; then
    [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] iw dev $nic set channel $CH $MODE"
    iw dev "$nic" set channel "$CH" $MODE
  else
    [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] iw dev $nic set channel $CH"
    iw dev "$nic" set channel "$CH"
  fi
done


#
# 4) Confirm channel change rapidly (10x @ 250ms) until anything replies
#
SUCCESS=0
for i in {1..10}; do
  [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Sending confirm_channel_change attempt $i"
  set +e
  CONFIRM=$(printf 'confirm_channel_change\n' | nc -w1 "$SERVER_IP" $PORT)
  RC=$?
  set -e

  if [[ $RC -eq 0 && -n "$CONFIRM" ]]; then
    echo "$CONFIRM"
    SUCCESS=1
    break
  fi

  sleep 0.25
done

if [[ $SUCCESS -eq 1 ]]; then
  [[ $VERBOSE -eq 1 ]] && echo "[DEBUG] Got confirmation. Persisting wifi_channel $CH into config"
  sed -i -E "s|^\s*wifi_channel\s*=.*|wifi_channel = $CH|" /etc/wifibroadcast.cfg
else
  echo "No confirmation received. Reverting local NICs to $ORIG"
  for nic in "${NICS[@]}"; do
    iw dev "$nic" set channel "$ORIG" $MODE
  done
fi



exit 0
