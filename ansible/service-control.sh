#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INVENTORY="${SPECTRAL_LINE_INVENTORY:-${SCRIPT_DIR}/inventory.yml}"
HOST_GROUP="${SPECTRAL_LINE_HOST_GROUP:-spectral_line_servers}"
FORKS="${SPECTRAL_LINE_FORKS:-10}"

usage() {
  cat <<EOF
Usage: $0 {start|stop|restart|status} [inventory]

Control spectral-line.service on every receiver in the Ansible inventory.
The optional inventory argument defaults to:
  ${INVENTORY}

Environment overrides:
  SPECTRAL_LINE_INVENTORY   inventory path
  SPECTRAL_LINE_HOST_GROUP  target group (default: spectral_line_servers)
  SPECTRAL_LINE_FORKS       parallel hosts (default: 10)
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage >&2
  exit 2
fi

ACTION="$1"
if [[ $# -eq 2 ]]; then
  INVENTORY="$2"
fi

if [[ ! -f "$INVENTORY" ]]; then
  echo "Inventory not found: $INVENTORY" >&2
  exit 1
fi

if ! command -v ansible >/dev/null 2>&1; then
  echo "ansible command not found" >&2
  exit 1
fi

COMMON_ARGS=(
  --inventory "$INVENTORY"
  "$HOST_GROUP"
  --become
  --forks "$FORKS"
)

case "$ACTION" in
  start)
    ansible "${COMMON_ARGS[@]}" \
      --module-name systemd \
      --args "name=spectral-line.service state=started enabled=false"
    ;;
  stop)
    ansible "${COMMON_ARGS[@]}" \
      --module-name systemd \
      --args "name=spectral-line.service state=stopped enabled=false"
    ;;
  restart)
    ansible "${COMMON_ARGS[@]}" \
      --module-name systemd \
      --args "name=spectral-line.service state=restarted enabled=false"
    ;;
  status)
    ansible "${COMMON_ARGS[@]}" --one-line \
      --module-name command \
      --args "systemctl show spectral-line.service --property=ActiveState,SubState,MainPID --no-pager"
    ;;
  *)
    echo "Unsupported action: $ACTION" >&2
    usage >&2
    exit 2
    ;;
esac
