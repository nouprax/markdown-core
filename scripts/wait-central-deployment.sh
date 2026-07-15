#!/usr/bin/env bash
set -euo pipefail

deployment=${1:?usage: wait-central-deployment.sh DEPLOYMENT_ID EXPECTED_STATE}
expected=${2:?usage: wait-central-deployment.sh DEPLOYMENT_ID EXPECTED_STATE}
attempt=0

while [ "$attempt" -lt 80 ]; do
    response=$(scripts/central-portal.sh status "$deployment")
    state=$(printf '%s' "$response" | node -e \
        'let value=""; process.stdin.on("data", chunk => value += chunk); process.stdin.on("end", () => console.log(JSON.parse(value).deploymentState));')
    case "$state" in
        "$expected")
            echo "Central deployment $deployment reached $expected"
            exit 0
            ;;
        FAILED)
            echo "$response" >&2
            echo "Central deployment $deployment failed validation" >&2
            exit 1
            ;;
        PENDING|VALIDATING|VALIDATED|PUBLISHING)
            ;;
        *)
            echo "Central deployment $deployment returned unexpected state: $state" >&2
            exit 1
            ;;
    esac
    attempt=$((attempt + 1))
    sleep 15
done

echo "Central deployment $deployment did not reach $expected before timeout" >&2
exit 1
