# Cluster setup and operation

[Back to README](../README.md) · [Assignment requirements](assignment.md) · [Technical reference](reference.md)

- [Prerequisites](#setup)
- [Connect](#connecting)
- [Transfer the code](#transferring-the-code)
- [Build](#building-the-tools)
- [Configure the model endpoint](#model-endpoint)
- [Reserve a node](#node-family)
- [Submit a search](#submitting-a-search)
- [Monitor a run and read its output](#monitoring-a-run)
- [Cluster traps](#traps)

Cluster behavior and runtime figures below are observations from earlier runs.
Check the returned allocation and model response for your own run.

## Setup

You need:

- NUS VPN or an on-campus connection, SSH, and access to the cluster.
- CMake 3.20+, a C++17 compiler, libcurl, and network access during configuration
  and dependency downloads.
- `jq` for the JSON checks, including those inside batch jobs.

Build on the login node. Run **all measurements**, including `CanMehHarness init`,
on an allocated 24-core Xeon node (`xcnc` or `xcnd`).

### Connecting

`xlogin`, the login node, answers only through a jump host and only from inside
the NUS network, so start the NUS VPN client or work from a machine on campus. A
direct connection to `xlogin.comp.nus.edu.sg:22` does not complete even with the
VPN running.

So put the two hops in your `~/.ssh/config` as one host entry, which also keeps
one authenticated connection open for eight hours so that later commands reuse
it rather than authenticating again. Substitute your own NUS username for
`<user>` in both places:

```
Host xlogin
User <user>
HostName xlogin.comp.nus.edu.sg
ProxyJump <user>@stfjump.comp.nus.edu.sg
ControlMaster auto
ControlPath /tmp/ssh-%r@%h:%p
ControlPersist 8h
```

With that entry in place, `ssh xlogin` reaches the login node.

`sshd` at each hop prompts for a password, so no program running unattended can
authenticate. Authenticate once, open a shared connection, and let later `ssh`
invocations reuse it:

```sh
ssh xlogin                                        # authenticate once, by hand
ssh -o BatchMode=yes xlogin 'squeue -u $USER'     # reuses that connection
```

Pass `BatchMode=yes`: without it `ssh` prompts for a password once the shared
connection has died, and a program running unattended hangs on that prompt
instead of failing. The connection does die -- suspending a laptop or changing
networks ends it, and `ServerAliveInterval` does not keep it alive -- so re-run
`ssh xlogin`. A running job is unaffected, because `slurmd` runs it on a
compute node.

### Transferring the code

Login and compute nodes reach `github.com` only over HTTPS on port 443. A
connection to `github.com:22` or `ssh.github.com:443` does not complete.

So clone over HTTPS instead, into your home directory on the login node:

```sh
git clone https://github.com/NUS-CS6219-2026/CanMehEvolve.git ~/Projects/CanMehEvolve
```

For a private repository, and for your own edits once you start writing the
search, push to a bare repository on the cluster over the shared connection.
Pushing to a bare repository stores no GitHub credential on a shared
filesystem, where a personal access token would be readable:

```sh
ssh xlogin 'git init --bare ~/Projects/canmeh.git'
git remote add cluster xlogin:Projects/canmeh.git
git push cluster HEAD:main
```

Build on the login node as described below, and create no work
directory there: `CanMehHarness init` ends with a measurement, so a work
directory created on the login node records a null run from a machine you will
never search on. Build once before you submit anything: several jobs may share
one checkout, because a job names its run and work directories after
`$SLURM_JOB_ID`, but two `cmake --build` processes in one checkout write the
same object files and corrupt each other's output.

### Building the tools

Run this on the login node, from the repository root:

```sh
cd ~/Projects/CanMehEvolve
cmake -S . -B build
cmake --build build -j
```

This checkout includes the search implementation, so a normal build includes
`CanMehSearch`. In the original starter, build only the five provided targets
until the seven functions in `src/Search.cpp` are implemented:

```sh
cmake --build build -j --target CanMehHarness CanMehCalibrate \
  CanMehBench CanMehDecodeForSampler CanMehData
```

CMake fetches pinned versions of Google Benchmark, nlohmann/json, and CLI11 into
`build/deps`, plus FIT SDK 21.201.0. The `CanMehData` target runs
`CanMehDataGenerator` to create the four input files in `build/data`.

Build once before submitting jobs. Concurrent builds in one checkout can corrupt
object files, and rebuilding while jobs run can replace the harness mid-run.

### Model endpoint

Use an endpoint compatible with the OpenAI chat-completions API. The
[SoC LLM portal](https://soclaas-portal.comp.nus.edu.sg) explains how to obtain
a key for the default endpoint. You may use another provider or a local model.

Put your API key in a file and pass `--llm-api-key-file` that path.
`CanMehSearch` reads the file itself, so the key never reaches a command line,
where `ps` shows it to every other user on the machine:

```sh
mkdir -p ~/.config/canmeh
printf '%s' 'clsk_...' > ~/.config/canmeh/api-key
chmod 600 ~/.config/canmeh/api-key
```

`--llm-base-url` defaults to `https://soclaas-api.comp.nus.edu.sg/v1` and
`--llm-model` to `qwen3-coder-next`, so a run needs only your key. Before the
first one, check that the endpoint returns a completion rather than merely
listing models, because an id that appears in the model list does not always
answer:

```sh
{
  printf 'header = "Authorization: Bearer %s"\n' "$(cat ~/.config/canmeh/api-key)"
} | curl --config - --silent --show-error --fail --max-time 60 \
  https://soclaas-api.comp.nus.edu.sg/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3-coder-next","max_tokens":16,
       "messages":[{"role":"user","content":"reply with the single word ok"}]}' \
  | jq -r '.choices[0].message.content, .model'
```

For another model, replace `qwen3-coder-next` in the request. Read the second
output line, which is the id of the model that answered. An id on this
endpoint may be an alias, in which case the endpoint serves a version other
than the one the id names. Measured on 2026-10-05: `qwen3-coder-next` answered
`ok` and reported `qwen3-coder-next`, `coder` reported `qwen3.8:27b` and
returned a null message content. `qwen3-coder-next` is the default for that
reason, and if SoC retires it you will find out here rather than at generation
one.
State in your report which id answered, which
the [Search](assignment.md#search) group lists, because `utilities::requestModelReply` keeps
the reply's message text and its token counts, so the endpoint's `model` field
reaches no log.

The endpoint operators state that it serves four concurrent requests at full
speed and queues any request beyond the fourth, releasing queued requests four
at a time. `CanMehSearch` makes one call at a time, so four parallel searches
use all four request slots and a fifth only makes all five slower.

### Node family

Initialize the work directory on the reserved compute node. `init` includes
a [null measurement](reference.md#null-run), so it must run on the same node
as the search.

A timing describes the machine that produced it, so produce every number you
report on an `xcnc` or `xcnd` node. `salloc` reserves one whole Xeon node for
two hours and drops you into a shell on it, and `CanMehCalibrate` then measures
the twelve calibration pairs there for
[Phase 0](assignment.md#phase-0-calibration):

```sh
salloc --partition=long --constraint="xcnc|xcnd" --nodes=1 \
       --mem=0 --cpus-per-task=24 --time=2:00:00

# salloc opens a shell on the node it reserved, so type the next line in that
# shell rather than pasting both lines at once.
./build/CanMehCalibrate --repeat 10
```

`CanMehCalibrate` prints how long measuring every pair again takes at the
settings it used, and you size `--time` from that duration. Use the same
`--repeat` and `--repetitions` for the search as for the calibration pairs, and
do not match `--min-time`, for the reason [Phase 0: Calibration](assignment.md#phase-0-calibration) gives.

## Cluster

### Submitting a search

One `sbatch` command runs a whole search unattended. It reserves a Xeon node for
two hours and forty-five minutes, creates a work directory and an output
directory named after the job id, runs `CanMehHarness init` and stops if
`null_ok` came back false, then runs `CanMehSearch` for 40 generations with its
console output captured:

```sh
mkdir -p runs
sbatch --partition=long --constraint="xcnc|xcnd" \
       --nodes=1 --ntasks=1 --cpus-per-task=24 --mem=0 \
       --time=2:45:00 --job-name=canmeh --output=runs/slurm-%j.log \
       --mail-type=FAIL,TIME_LIMIT --mail-user=<you>@u.nus.edu \
       --wrap='set -e
  W=$PWD/build/work-$SLURM_JOB_ID
  OUT=runs/exp-$SLURM_JOB_ID
  mkdir -p "$OUT"
  ./build/CanMehHarness init --work "$W" --repeat 10 > "$OUT/init.json"
  jq -e ".ok and .null_ok" "$OUT/init.json" > /dev/null
  ./build/CanMehSearch --work "$W" --out "$OUT" --generations 40 \
    --sample-seconds 20 --init-result "$OUT/init.json" \
    --llm-model qwen3-coder-next --llm-api-key-file ~/.config/canmeh/api-key \
    > "$OUT/console.log" 2>&1'
```

Why each `sbatch` flag in that submission is there. With `--constraint="xcnc|xcnd"` Slurm runs the job only
on the node family you calibrated on, and the shell must not see the `|`
unquoted. `--mem=0 --cpus-per-task=24` takes a whole `xcnc` node, which is what
`--exclusive` would do on a cluster that allowed it. Naming `$W` and `$OUT`
after `$SLURM_JOB_ID` keeps concurrent jobs apart, because two `measure-patch` processes
sharing a [work directory](reference.md#work-directory) overwrite each other's scratch directory and object
tree. `--sample-seconds 20` rather than `--region-files` is what narrows the
search to a measured profile, for the reason [Phase 1](assignment.md#phase-1-search-loop) gives.

`--sample-seconds` works on these nodes. `CanMehSearch` runs `perf record -F
997` there and `sample` on macOS, and we confirmed on an `xcnc` node that
`perf` resolves source lines in the work tree: 316 distinct lines of
`tree/src`, and `CanMehDecodeForSampler` decoded to the same checksum under
the sampler that `CanMehBench` reports without one. `perf` warns there that a
restricted `kptr_restrict` stops it resolving kernel addresses, and you can
ignore that warning, because `weighRanges` counts only lines in the work tree
toward a region's share.
Expect a sizable unattributed share here, and note that more than half of what
we measured named `tree/src/SdkDecode.cpp` or
`tree/src/utilities/Portability.hpp`, two files that fall outside the region
index, so a larger `--max-lines` leaves that part of the share where it is.

Check `ok` and `null_ok` in the saved `init` output before starting a search.
The current search implementation also checks the saved result when you pass
`--init-result`; `meta.json` alone does not record the null spread. A null
p-value is not a substitute for these checks. `CanMehHarness init` decides
`null_ok` from effect size instead, and a work directory where `CanMehHarness
init` reported `null_ok` false makes every later number from that directory
worthless.

One [generation](reference.md#generation) is one model call, then one measurement, then a second
measurement of anything that passed. Measured on an `xcnc` node at
`--repeat 10`: 40 generations took 1 hour 47 minutes and spent 57,090 tokens,
about 160 seconds and 1,430 tokens per generation. Size `--time` from that. An allocation submitted with `--partition=long` has
been coming back in the `normal` partition, which Slurm limits to 3:00:00, so
read the partition trap below before you rely on more than three hours. For
more runs, submit that `sbatch` command again with a different `--seed` on the
`CanMehSearch` line, or use
`--array` and pass `--seed $SLURM_ARRAY_TASK_ID`. Submit at most four, because
the endpoint serves four at full speed.

### Monitoring a run

`squeue` and `scontrol` report the job's state, and the files `CanMehSearch`
wrote under `--out` report the search itself, one generation at a
time.

```sh
squeue -u $USER -o '%.10i %.13j %.2t %.9M %.10L %.6D %R'
scontrol show job <jobid>     # detail, including a job that has just ended
tail -f runs/exp-<jobid>/console.log
```

**`sacct` does not work on this cluster.** `sacct` fails with `No space left on
device` from the accounting database, and the same full database makes `seff`
fail too. Use `scontrol show job` while `slurmctld` still holds the job record;
afterwards only the run's own files remain.

`CanMehSearch` writes these files into the directory `--out` names:

- **`results.jsonl`**, one record per candidate: `generation`, `id`, `parent`,
  `region`, `file`, `label`, `ok`, [`score_ms`](reference.md#score_ms), [`significant`](reference.md#significant),
  `significant_at_min_effect`, `min_effect`, [`improved_on_parent`](reference.md#improved_on_parent), `p_value`,
  [`confirmed_p`](reference.md#confirmed_p), `unstable`, [`ratio`](reference.md#ratio), `speedup`, [`ratio_spread`](reference.md#ratio_spread),
  [`candidate_cv`](reference.md#candidate_cv), `swapped_processes`, `stage` and `note`. **Not every record
  holds every field.** A failed model call or unusable reply records its error
  without timing statistics. Check `ok` and `stage` before reading timing
  fields. The current implementation also records selection, parent ratio,
  raw-output archive paths, and confirmation details when available. An empty `stage` means
  `CanMehBench` timed both libraries' decodes and rejected nothing. Otherwise
  `stage` names the step that
  rejected it. `CanMehHarness` reports `patch` when the diff does not apply,
  `compile`, `link`, and `crash` naming the signal that killed
  the benchmark process. `CanMehBench` reports `data` when it cannot read the
  activity directory, `baseline` when the reference library fails to [decode](reference.md#decode) at
  all, `load` for both a missing symbol (usually a changed signature) and two
  libraries that resolved to one image, `correctness` for the decoded-value
  checksum, `profile` for the sweep over the lookup domain, and `threading` when
  processor time over one decode exceeds 1.15 times wall-clock time.
  `CanMehSearch` reports `llm` for a reply it could not use at all. Handle an
  unknown value too: `CanMehHarness` also emits `args`, `init`, `build`,
  `region`, `run` and [`oracle`](reference.md#oracle) for its own failures, and a loop that switches on
  this field should treat anything it does not recognize as a rejection rather
  than as a measurement. One column needs a warning:
  `significant` here is not `measure-patch`'s field of that name, because
  `CanMehSearch` computes its own decisions from raw timings. The column
  means both measurements met its ratio-below-1.0 and p-below-0.01 checks.
  Of the three decision columns, count `improved_on_parent`: the
  [Glossary](reference.md#lineage) entry for lineage says why the other two credit
  a whole lineage for one discovery.

- **`credits.csv`**, one row per generation:
  `generation,calls,prompt_tokens,completion_tokens,total_tokens,`
  `best_confirmed_speedup,best_retained_speedup,elapsed_s`. Plot the two
  speedup columns against `total_tokens`. They disagree when the best ratio
  among the candidates `CanMehSearch` [retained](reference.md#retained) comes from one that did not
  clear `--min-effect` twice: `best_confirmed_speedup` counts only candidates
  that did, and `best_retained_speedup` counts every candidate kept.

- **`regions_tried.json`**, how many candidates `CanMehSearch` produced for
  each region, how many of them it accepted, and the largest improvement any
  rewrite of that region made over its parent. This is the file that separates
  a real discovery from its descendants.
- **`best.patch`** and `patches/gen_N.patch`, the unified diffs, plus
  `patches/gen_N.repl` holding the extracted replacement. The current
  implementation saves the full reply in `patches/gen_N.reply.txt`.
  `CanMehSearch` writes `best.patch` at the end of a run.

### Traps

**Size `--time` from a measured generation, and add headroom.** Slurm kills
the job at the limit with no warning the search can act on, and `CanMehSearch`
writes `best.patch` only after its last generation, so a run Slurm kills loses
`best.patch` entirely. What survives is `results.jsonl` and
`patches/gen_N.patch`, which `CanMehSearch` writes as it goes, so you can
recover the best patch by reading the ratio column of `results.jsonl` and
taking the matching `gen_N.patch`. Time one generation first, multiply by
`--generations`, add the sampling pass and the build, then add a margin:
`--repeat 10` means ten benchmark processes per `measure-patch`, and `CanMehSearch`
runs a second `measure-patch` over every candidate the first one called significant or
unstable, so a generation that reaches a measurement takes roughly twice as
long as one that fails to compile. Pass `--mail-type=FAIL,TIME_LIMIT` so Slurm
tells you which of the two ended the job.

**Pin the node family.** This cluster contains both 24-core Xeon E5-2620 v3
machines and 80-, 128- and 512-core AMD EPYC machines, and we calibrated the
measurement only on the Xeons. On an EPYC node, `measure-patch` reported a difference of
more than two percent between two libraries linked from identical object files,
which is larger than the difference many real patches make. On an `xcnc` node
`measure-patch` reports well under one percent instead. We measured that on one EPYC node
and did not establish the cause, so run on the Xeons.

The administrators configured a Slurm feature named after each node family, so
`--constraint="xcnc|xcnd"` selects the Xeons. **Quote the constraint.**
On a shell command line, an unquoted `|` becomes a pipe and prevents
`sbatch` from receiving the intended constraint. `scontrol show job` printing `Features=xcnc|xcnd` confirms Slurm
received it.

**This cluster refuses `--exclusive`**, and `--exclusive=user` too, both with
`salloc: error: Exclusive node access is not permited.` Request
`--mem=0 --cpus-per-task=24` instead, which allocates all 63,000 MB and all 24
CPUs of an `xcnc` node, so `slurmctld` leaves no room for a second job.
Measured on xcnc1: `--mem=0` alone reports `NumCPUs=8`, so the job gets 8 of
the node's 24 CPUs, although a second job pinned to that node stayed
`PENDING (Resources)` because `DefMemPerCPU=3072` means every job requests
memory and none fits beside one holding all of it. `--cpus-per-task=24` without
`--mem=0` is refused with `Requested node configuration is not available`,
because 3,072 MB per CPU times 24 asks for 73,728 MB on a 63,000 MB node.

**`--partition=long` may not give you the `long` partition.** Every allocation
submitted with `--partition=long` on 2026-10-05 and 2026-10-06 came back with
`Partition=normal` in both `scontrol show job` and `squeue`. Both partitions
contain `xcnc` and `xcnd` nodes, `normal` limits a job to 3:00:00 and `long` to
3-00:00:00, and nobody has established what reroutes these submissions. Read
`Partition=` out of `scontrol show job` before you rely on more than three
hours of `--time`.

**`--repeat` between 2 and 7 makes a result unreachable.** At `--repeat 2` or
more, `measure-patch` decides by a [sign-flip test](reference.md#sign-flip-test) over N
per-process log-ratios, whose smallest attainable p-value is 2/2^N: 0.0625 at
`--repeat 5`, against the 0.01 `measure-patch` requires. Ten processes provide finer p-value resolution; whether a measurement
passes also depends on its observed log-ratios. `CanMehHarness init --repeat` fixes the count in `meta.json` and every
later `measure-patch` inherits it.

**Nothing else should run on an allocated node.** A profiler, a quick timing
check or a second search all enlarge the variance inside each benchmark
process, and `measure-patch` then calls nothing significant, while the job still uses
the whole allocation. Nobody has measured whether a busy node also biases the
ratio. For the same reason, do not run `measure-patch` by hand against a work directory
a job is using, and do not rebuild a checkout while its jobs run:
`CanMehSearch` spawns `./build/CanMehHarness` three or four times in every
generation, and three more times before the first one, so
replacing that binary mid-run splits the results across two builds.

**A baseline in milliseconds describes one node.** A ratio measured
inside one process on one node divides out that node's absolute speed, which is
why you report a ratio rather than milliseconds. A ratio
still depends on the node family: read **Pin the node family** above, where
`measure-patch` reported more than two percent between two libraries linked from
identical object files on one node family and well under one percent on the
other. So read your own baseline from `CanMehHarness init` and report which node
you measured it on rather than quoting a figure from another machine.

**`.gitignore` lists `runs/`.** `git clone` does not copy a run directory, so
copy `results.jsonl`, `credits.csv`, `regions_tried.json` and the patches out
yourself before you need them.

**A clean build once ran the login node out of `/tmp`.** The account has a
10,240 KB quota there, where gcc writes its assembler temporaries, and a clean
`-j 8` build exhausted it and died naming a file you did not touch.
`CMakeLists.txt` passes `-pipe`, so gcc streams between its stages and writes
no `.s` file at all, and a clean build now leaves `/tmp` at its 60 KB baseline.
If you ever see `Disk quota exceeded` from the compiler, `quota -s` reports the
limit and `df -h /tmp` the free space, so check both before concluding that the
failure is in the code.
