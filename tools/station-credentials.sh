#!/usr/bin/env bash
# Station credentials for AWS IoT Core. Files go to certs/ (gitignored).
#
#   tools/station-credentials.sh create station-001   # key pair + CSR, before deploying
#   tools/station-credentials.sh fetch  station-001   # signed certificate + endpoint, after deploying
#
# The private key is generated locally and never uploaded: AWS signs the CSR. On real hardware the
# key would be generated inside a secure element, and only the CSR would leave the station.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CERTS="$ROOT/certs"
OUTPUTS="$ROOT/app/amplify_outputs.json"
PROFILE="${AWS_PROFILE:-fs}"
ROOT_CA_URL="https://www.amazontrust.com/repository/AmazonRootCA1.pem"

usage() { echo "usage: $0 create|fetch <station-id>" >&2; exit 2; }
[[ $# -eq 2 ]] || usage
action="$1" station="$2"
[[ "$station" =~ ^[a-z0-9-]{3,64}$ ]] || { echo "station id must match [a-z0-9-]{3,64}" >&2; exit 2; }
mkdir -p "$CERTS"
chmod 700 "$CERTS"

case "$action" in
create)
  [[ -e "$CERTS/$station.key" ]] && { echo "$CERTS/$station.key already exists" >&2; exit 1; }
  # ECDSA P-256: smaller and faster than RSA on a Cortex-M3, and supported by AWS IoT Core.
  (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$CERTS/$station.key")
  openssl req -new -key "$CERTS/$station.key" -subj "/CN=$station" -out "$CERTS/$station.csr"
  echo "created $CERTS/$station.key and $CERTS/$station.csr; now deploy the backend"
  ;;
fetch)
  [[ -f "$OUTPUTS" ]] || { echo "$OUTPUTS not found: deploy the backend first" >&2; exit 1; }
  cert_id="$(node -e '
    const o = require(process.argv[1]);
    const s = (o.custom?.iot?.stations ?? []).find((x) => x.stationId === process.argv[2]);
    if (!s) process.exit(1);
    console.log(s.certificateId);' "$OUTPUTS" "$station")" \
    || { echo "$station is not in $OUTPUTS: was its CSR present when you deployed?" >&2; exit 1; }

  aws iot describe-certificate --profile "$PROFILE" --certificate-id "$cert_id" \
    --query certificateDescription.certificatePem --output text > "$CERTS/$station.crt"
  aws iot describe-endpoint --profile "$PROFILE" --endpoint-type iot:Data-ATS \
    --query endpointAddress --output text > "$CERTS/endpoint.txt"
  [[ -f "$CERTS/AmazonRootCA1.pem" ]] || curl -fsS "$ROOT_CA_URL" -o "$CERTS/AmazonRootCA1.pem"

  # The certificate must belong to the key we generated.
  [[ "$(openssl x509 -in "$CERTS/$station.crt" -noout -pubkey)" == \
     "$(openssl pkey -in "$CERTS/$station.key" -pubout)" ]] \
    || { echo "certificate does not match $station.key" >&2; exit 1; }
  echo "fetched $CERTS/$station.crt; endpoint $(cat "$CERTS/endpoint.txt")"
  ;;
*) usage ;;
esac
