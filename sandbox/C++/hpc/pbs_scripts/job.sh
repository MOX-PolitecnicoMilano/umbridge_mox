#!/bin/bash
#PBS -S /bin/bash
#PBS -N testmodel
#PBS -l select=1:ncpus=1:mpiprocs=1:mem=1gb
#PBS -l walltime=00:05:00
##PBS -q CHANGE_ME_QUEUE
##PBS -A CHANGE_ME_PROJECT

# PBS Professional/OpenPBS example for a LoadBalancer running on a login node.
# CHANGE ME: adapt queue/account, resources, walltime, modules and model command.
# Uncomment an optional directive above by changing ##PBS to #PBS.
# PBS keeps separate job-specific stdout/stderr files unless site defaults or
# qsub options override them. Shell variables are not expanded in #PBS lines.

set -euo pipefail

fail() {
    printf 'job.sh: %s\n' "$*" >&2
    exit 1
}

# These values are supplied by real PBS and by PBSSubmitter, respectively.
# Relative paths must resolve as they do on the login node, not in PBS's spool.
: "${PBS_O_WORKDIR:?PBS_O_WORKDIR is required; submit this script through PBS}"
: "${PBS_JOBID:?PBS_JOBID is required; submit this script through PBS}"
: "${UMBRIDGE_LOADBALANCER_COMM_FILEDIR:?LoadBalancer must export the URL directory}"
cd -- "$PBS_O_WORKDIR" || fail "Cannot enter submission directory: $PBS_O_WORKDIR"

# Match the ID format understood by this checkout's PBSJob parser. Arrays and
# dotted server FQDNs need matching C++ parser changes before they can be used.
if [[ ! $PBS_JOBID =~ ^([0-9]+)(\.[A-Za-z0-9_-]+)?$ ]]; then
    fail "Unsupported PBS_JOBID '$PBS_JOBID'; expected digits or digits.server"
fi
job_id=${BASH_REMATCH[1]}
comm_dir=$UMBRIDGE_LOADBALANCER_COMM_FILEDIR
mkdir -p -- "$comm_dir"
[[ -d $comm_dir && -w $comm_dir ]] || fail "URL directory is not writable: $comm_dir"

# CHANGE ME: initialize the site's compiler/MPI/container environment here.
# LoadBalancer exports the communication directory with qsub -v; it does not
# export every variable from the login-node shell or reproduce module settings.
# module load ...
model_command=(./testmodel) # CHANGE ME: executable plus any required arguments.
startup_timeout_seconds=120 # Starts after PBS starts this job, not at submission.
port_min=1024
port_max=65535
port_attempts=100

for utility in shuf curl hostname mktemp mv; do
    command -v "$utility" >/dev/null 2>&1 || fail "Required command not found: $utility"
done
[[ -x ./is_port_free ]] || fail "Build ./is_port_free in the submission directory first"
command -v "${model_command[0]}" >/dev/null 2>&1 || fail "Model executable not found: ${model_command[0]}"

# The model must listen on this exported port on a non-loopback interface.
# Checking a free port does not reserve it; if another process wins the race,
# the startup/liveness checks below report the model's failed startup.
port=
for ((attempt = 0; attempt < port_attempts; ++attempt)); do
    candidate=$(shuf -i "${port_min}-${port_max}" -n 1)
    if ./is_port_free "$candidate"; then
        port=$candidate
        break
    fi
done
[[ -n $port ]] || fail "Could not find a free TCP port after $port_attempts attempts"
export PORT=$port

# CHANGE ME if this name does not resolve from the login node to the compute
# network. Do not advertise localhost or 127.0.0.1 to the login-node balancer.
advertise_host=$(hostname -f)
[[ -n $advertise_host && $advertise_host != *[[:space:]/]* ]] || fail "Invalid compute-node hostname"
model_url="http://${advertise_host}:${port}"

model_pid=
url_tmp=
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n $model_pid ]]; then
        if kill -0 "$model_pid" 2>/dev/null; then
            kill "$model_pid" 2>/dev/null || true
        fi
        wait "$model_pid" 2>/dev/null || true
    fi
    [[ -z $url_tmp ]] || rm -f -- "$url_tmp"
    # FilesystemCommunicator owns the final URL file and removes it after use.
    # PBS remains responsible for terminating the complete allocated job tree.
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"${model_command[@]}" &
model_pid=$!
printf 'PBS job %s: waiting for model server at %s\n' "$PBS_JOBID" "$model_url"

# Require a successful HTTP status at /Info, not merely an open TCP port.
# HTTP body/protocol validation is performed by UM-Bridge after URL discovery.
# Bypass inherited web proxies for this internal compute-node connection.
deadline=$((SECONDS + startup_timeout_seconds))
while :; do
    kill -0 "$model_pid" 2>/dev/null || fail "Model server exited before becoming ready"
    # Capture only the most recent connection error instead of flooding PBS's
    # stderr file during startup; report it if the readiness deadline expires.
    if curl_error=$(curl --fail --silent --show-error --noproxy '*' \
            --connect-timeout 2 --max-time 5 "${model_url}/Info" 2>&1 >/dev/null); then
        kill -0 "$model_pid" 2>/dev/null || fail "Model server exited during its readiness check"
        break
    fi
    ((SECONDS < deadline)) || fail "Model server did not become ready within ${startup_timeout_seconds}s at $model_url. Last curl error: $curl_error"
    sleep 1
done

# Write a complete announcement and atomically rename it on the same shared
# filesystem, so the polling balancer never reads a partially written line.
url_file="${comm_dir}/url-${job_id}.txt"
url_tmp=$(mktemp "${comm_dir}/.url-${job_id}.txt.XXXXXX")
printf '%s\n' "$model_url" > "$url_tmp"
mv -f -- "$url_tmp" "$url_file"
url_tmp=
printf 'Published model URL in %s\n' "$url_file"

# Keep the allocation alive for the server lifetime. The balancer normally
# calls qdel when the proxied operation finishes; an early server exit also ends
# this batch job and preserves its exit status, instead of sleeping forever.
wait "$model_pid"
