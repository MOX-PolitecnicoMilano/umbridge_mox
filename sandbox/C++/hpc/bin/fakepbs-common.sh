# Shared helpers for the fake PBS commands qsub, qstat and qdel (source only).
#
# State lives in $FAKEPBS_DIR (default: ~/.fakepbs), so the three commands see
# the same jobs regardless of the directory they are run from.
# MAX_JOBS (default: number of cores) bounds how many jobs run concurrently;
# the others stay queued (state Q), exactly as on a busy cluster.

FAKEPBS_DIR=${FAKEPBS_DIR:-$HOME/.fakepbs}
FAKEPBS_SERVER=${FAKEPBS_SERVER:-fakepbs}
FAKEPBS_JOBS="$FAKEPBS_DIR/jobs"

fp_init() {
	mkdir -p -- "$FAKEPBS_JOBS" || { echo "Cannot create $FAKEPBS_JOBS" >&2; exit 1; }
}

# Strip an optional ".server" suffix and validate the numeric part.
fp_normalize_id() {
	local id=${1%%.*}
	[[ $id =~ ^[0-9]+$ ]] || return 1
	printf '%s\n' "$id"
}

# fp_get <num> <field>: print a job field (empty if missing).
fp_get() {
	local file="$FAKEPBS_JOBS/$1/$2"
	[[ -r $file ]] && cat -- "$file"
	return 0
}

fp_set() { printf '%s\n' "$3" > "$FAKEPBS_JOBS/$1/$2"; }

# A job in state Q or R whose supervisor has vanished (e.g. killed by hand or
# after a reboot) is reported as finished.
fp_state() {
	local num=$1 state pid
	state=$(fp_get "$num" state)
	if [[ $state == Q || $state == R ]]; then
		pid=$(fp_get "$num" pid)
		if [[ -z $pid ]] || ! kill -0 "$pid" 2>/dev/null; then
			state=F
			fp_set "$num" state F
		fi
	fi
	printf '%s\n' "$state"
}

fp_list_ids() {
	local d
	for d in "$FAKEPBS_JOBS"/*/; do
		[[ -d $d ]] || continue
		d=${d%/}
		printf '%s\n' "${d##*/}"
	done | sort -n
}

# Convert [[HH:]MM:]SS to seconds.
fp_walltime_seconds() {
	local IFS=: parts
	read -ra parts <<< "$1"
	local s=0 p
	for p in "${parts[@]}"; do
		[[ $p =~ ^[0-9]+$ ]] || return 1
		s=$((s * 60 + 10#$p))
	done
	printf '%s\n' "$s"
}

# Supervisor of one job; runs detached in its own session (see qsub).
fp_run_job() {
	local num=$1 dir="$FAKEPBS_JOBS/$1"
	local max_jobs=${MAX_JOBS:-$(nproc)}
	local running d

	echo $$ > "$dir/pid"
	exec 8> "$FAKEPBS_DIR/lock"

	# Wait for a free slot; the lock makes counting and claiming atomic.
	# Jobs start in submission order because older ids check first.
	while :; do
		flock -x 8
		running=0
		local ahead=0
		for d in "$FAKEPBS_JOBS"/*/; do
			d=${d%/}
			local n=${d##*/}
			[[ $n == "$num" ]] && continue
			case $(fp_state "$n") in
				R) ((running += 1)) ;;
				Q) ((n < num)) && ((ahead += 1)) ;;
			esac
		done
		if ((running < max_jobs && ahead == 0)); then
			fp_set "$num" state R
			date +%s > "$dir/stime"
			flock -u 8
			break
		fi
		flock -u 8
		sleep 0.2
	done

	local -a env_args=(
		"PBS_JOBID=$num.$FAKEPBS_SERVER"
		"PBS_JOBNAME=$(fp_get "$num" name)"
		"PBS_O_WORKDIR=$(fp_get "$num" workdir)"
		"PBS_O_HOST=$(hostname)"
		"PBS_O_QUEUE=$(fp_get "$num" queue)"
		"PBS_QUEUE=$(fp_get "$num" queue)"
		"PBS_ENVIRONMENT=PBS_BATCH"
		"PBS_NODEFILE=$dir/nodefile"
		"PBS_O_HOME=$HOME"
		"TMPDIR=${TMPDIR:-/tmp}"
	)
	local line
	while IFS= read -r line; do
		[[ -n $line ]] && env_args+=("$line")
	done < "$dir/vars"

	local -a runner=()
	local wall
	wall=$(fp_get "$num" walltime_s)
	[[ -n $wall && $wall -gt 0 ]] && runner=(timeout -k 10 "$wall")

	local status=0
	# Like PBS, run from the home directory, with separate stdout/stderr files.
	(cd -- "$HOME" && exec env "${env_args[@]}" "${runner[@]}" bash "$dir/script") \
		> "$(fp_get "$num" stdout)" 2> "$(fp_get "$num" stderr)" || status=$?
	fp_set "$num" exit "$status"
	date +%s > "$dir/etime"
	fp_set "$num" state F
}
