#!/usr/bin/env bash
set -euo pipefail
# Launch parameters may contain credentials.
set +x

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_id=com.truebest.lgnome.native
ipk=""
mode=""
value=""
install_app=1
launch_app=1

fail() {
  echo "deploy-native-webos: $*" >&2
  exit 2
}

usage() {
  echo "Usage: ARES_DEVICE=<tv-device> $0 --ipk PATH [--no-launch]"
  echo "       $0 [--ipk PATH | --no-install] [--connect-slot COLOR | --config FILE | --with-defaults | --camera-preview]"
  echo "Default launch uses saved profiles. Configuration overrides are explicit."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ipk|--config|--connect-slot)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "$1 requires a value"
      if [[ "$1" == --ipk ]]; then
        [[ -z "$ipk" ]] || fail "--ipk specified twice"
        ipk="$2"
      else
        [[ -z "$mode" ]] || fail "launch modes are mutually exclusive"
        mode="$1"
        value="$2"
      fi
      shift 2
      ;;
    --with-defaults|--camera-preview)
      [[ -z "$mode" ]] || fail "launch modes are mutually exclusive"
      mode="$1"
      shift
      ;;
    --no-install) install_app=0; shift ;;
    --no-launch) launch_app=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown argument: $1" ;;
  esac
done

[[ "$install_app" == 1 || "$launch_app" == 1 ]] || fail "nothing to do"
[[ "$launch_app" == 1 || -z "$mode" ]] || fail "launch mode requires launching"
if [[ "$install_app" == 1 ]]; then
  [[ -n "$ipk" && -f "$ipk" ]] || fail "installation requires --ipk PATH to an existing package"
  command -v ares-install >/dev/null || fail "ares-install not found"
else
  [[ -z "$ipk" ]] || fail "--ipk conflicts with --no-install"
fi
if [[ "$launch_app" == 1 ]]; then
  command -v ares-launch >/dev/null || fail "ares-launch not found"
fi

launch_args=()
case "$mode" in
  --connect-slot)
    case "$value" in red|green|yellow|blue) ;; *) fail "invalid profile color" ;; esac
    launch_args=(--params "{\"connectSlot\":\"$value\"}")
    ;;
  --with-defaults) launch_args=(--params '{"ignoreSavedConfig":true}') ;;
  --camera-preview) launch_args=(--params '{"cameraPreview":true,"ignoreSavedConfig":true}') ;;
  --config)
    launch_params="$(python3 - "$value" <<'PY'
import json
import sys

def settings_object(fields):
    if fields.keys() & {"connectSlot", "cameraPreview", "ignoreSavedConfig", "params", "launchParams"}:
        raise ValueError()
    return fields

try:
    with open(sys.argv[1], "rb") as file:
        raw = file.read(16384)
    if len(raw) >= 16384:
        raise ValueError()
    config = json.loads(raw, object_hook=settings_object)
    if not isinstance(config, dict):
        raise ValueError()
    params = json.dumps(config, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    # ares-launch rewrites literal apostrophes before JSON parsing.
    params = params.replace("'", r"\u0027")
    if len(params.encode("utf-8")) >= 16384:
        raise ValueError()
except (OSError, ValueError, RecursionError):
    raise SystemExit("config must be a JSON object below 16 KiB without launch-control keys")
print(params)
PY
)" || fail "invalid explicit config"
    launch_args=(--params "$launch_params")
    ;;
esac

device_args=()
[[ -z "${ARES_DEVICE:-}" ]] || device_args=(-d "$ARES_DEVICE")
if [[ "$install_app" == 1 ]]; then
  "$repo_root/tools/build-native-webos.sh" --verify-ipk "$ipk"
  ares-install "${device_args[@]}" "$ipk"
fi
if [[ "$launch_app" == 1 ]]; then
  if [[ -n "$mode" ]]; then
    running="$(ares-launch "${device_args[@]}" --running)"
    if awk -v id="$app_id" '$1 == id { found = 1 } END { exit !found }' <<<"$running"; then
      ares-launch "${device_args[@]}" --close "$app_id"
    fi
  fi
  ares-launch "${device_args[@]}" "$app_id" "${launch_args[@]}"
fi
