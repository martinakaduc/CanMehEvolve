# CanMehEvolve

LLM-driven patch search over the Garmin FIT SDK decoder, for CS6219.

**The question:** does an LLM search loop improve code beyond what a compiler,
a human, or the same number of independent model calls can achieve?

You test this on FIT SDK **21.201.0**, using the NUS cluster. Each candidate is
a unified diff against the stock SDK. It must preserve decoded values and
profile lookups, and it must not use threads. One timed decode reads all four
activity files, about 900 kB in total.

## Where to start

| I need to… | Read |
|---|---|
| Connect, transfer code, configure the model, or submit a job | [Cluster setup and operation](docs/cluster.md) |
| Understand the phases, controls, report, or marking | [Assignment requirements](docs/assignment.md) |
| Look up the architecture, timing JSON, defaults, or a term | [Technical reference](docs/reference.md) |

The workflow below is the short path through the project. The linked guides
retain the detailed requirements and measurement explanations.

## Workflow

| Step | What you do | What you keep |
|---|---|---|
| Setup | Build, reserve a Xeon node, and initialize a work directory | Build settings, node identity, and null check |
| C2: human control | Spend one timed hour optimizing the decoder **before viewing search output** | Your patch and elapsed time |
| Phase 0: calibration | Evaluate your decision rule on pairs with known work counts | False-positive rate, detection curve, and bias |
| Phase 1: search | Run the loop with the calibrated rule | Patches, raw timings, decisions, and token costs |
| Controls C1, C3, C4 | Compare compiler settings, best-of-N, and at least five seeds per configuration | Comparable results for every control |
| Phase 2: report | Explain what the evidence supports | Report, patch series, logs, and demo |

### 1. Build on the login node

You need CMake 3.20+, a C++17 compiler, libcurl, network access, and `jq`.
Follow the [connection and transfer instructions](docs/cluster.md#connecting),
then run from the repository root:

```sh
cmake -S . -B build
cmake --build build -j
```

CMake fetches the pinned dependencies and SDK, and generates the input files in
`build/data`. This checkout includes an implementation of `src/Search.cpp`,
so the normal build includes `CanMehSearch`. The
[original starter instructions](docs/cluster.md#building-the-tools) explain
how to build the provided tools while that file is still incomplete.

Configure your [model endpoint and API key](docs/cluster.md#model-endpoint)
before running a search. Record the model id that actually answers.

### 2. Reserve a compute node

Run **all timing commands**, including `CanMehHarness init`, on an allocated
24-core Xeon node. The login node is for building and submitting jobs.

Run this command by itself:

```sh
salloc --partition=long --constraint="xcnc|xcnd" --nodes=1 \
  --mem=0 --cpus-per-task=24 --time=2:00:00
```

It opens a shell on the allocated node. Run the remaining timing commands in
that shell, from the repository root. Size the allocation for the work you plan
to run; calibration and search may need separate allocations. Check the actual
partition and time limit with `scontrol show job <jobid>`.

### 3. Initialize a work directory and check the null run

On the allocated compute node:

```sh
W="$PWD/build/work-$SLURM_JOB_ID"
OUT="runs/exp-$SLURM_JOB_ID"
mkdir -p "$OUT"
./build/CanMehHarness init --work "$W" --repeat 10 --repetitions 9 \
  > "$OUT/init.json"
jq -e '.ok and .null_ok' "$OUT/init.json"
```

Continue only if the last command prints `true`. A failed null check requires
a quieter node and a fresh work directory. Keep the rejected run's log.

### 4. Complete the human control

Before looking at evolutionary or best-of-N search output, spend **one timed
hour** hand-optimizing the decoder. Preserve its behavior, measure your patch,
and report the patch and elapsed time even if it produces no improvement.

See [C2: the human control](docs/assignment.md#c2--the-human-control) and the
[manual patch commands](docs/assignment.md#phase-1-search-loop).

### 5. Calibrate the decision rule

First check all twelve supplied calibration pairs:

```sh
./build/CanMehCalibrate --repeat 10 --repetitions 9
```

This writes `build/calibration/calibration.json`, including each pair's chosen
`min_time`. **It checks the calibration pairs; it does not test your decision
rule.**

Then collect repeated trials using the rule implemented in this checkout:

```sh
./build/CanMehSearch --calibration-trials 10 \
  --calibration-root build/calibration \
  --repeat 10 --repetitions 9 --min-effect 0.02 \
  --out "runs/phase0-$SLURM_JOB_ID"
```

This mode makes no model calls. It saves raw trials in
`calibration_trials.jsonl` and rates, confidence intervals, and bias in
`calibration_summary.json` under the output directory.

Ten trials are a starting point. Report what the trial count supports; zero
false positives in ten trials does not establish a one-percent false-positive
rate. Read [Phase 0](docs/assignment.md#phase-0-calibration) for the required
intervals, detection curve, failure checks, and limits of calibration.

Use the same `--repeat` and `--repetitions` in calibration and search.
Calibration uses each pair's recorded `min_time`; search uses the SDK work
directory's setting. Do not force them to match.

### 6. Run the search

Before spending a search budget, also evaluate an **inert patch** by hand, such
as adding a comment. This checks the measurement path on the actual SDK;
calibration pairs alone do not replace it. The
[manual workflow](docs/assignment.md#phase-1-search-loop) shows `regions`,
`show`, `splice`, and `measure-patch`.

After calibration and the manual checks, use the work and output directories
from step 3:

```sh
./build/CanMehSearch --work "$W" --out "$OUT" --generations 40 \
  --sample-seconds 20 --min-effect 0.02 --seed 1 \
  --init-result "$OUT/init.json" \
  --llm-api-key-file ~/.config/canmeh/api-key \
  > "$OUT/console.log" 2>&1
```

The search profiles the baseline before the first generation, proposes region
rewrites, and measures candidates with `measure-patch --no-decision`. Your
rule computes the statistics and decisions from `timings`. Passing or unstable
candidates receive a second measurement.

For unattended runs, use the [batch submission example](docs/cluster.md#submitting-a-search).
Measure a generation's duration and add time for initialization, profiling,
confirmation measurements, and headroom before choosing a job limit.

### 7. Run every control and report the evidence

| Control | Required comparison |
|---|---|
| **C1: compiler** | Baselines at `-O2`, `-O3`, and with `-march=native`; use a separate work directory for each setting |
| **C2: human** | Your timed hour of optimization, completed before viewing search output |
| **C3: best-of-N** | Same model, prompt, and total API-call budget; every proposal starts from stock, with no feedback, population, or memory |
| **C4: variance** | At least five seeds for every configuration; report the median and full range |

The current search supports `--best-of-n` and `--call-budget` for C3. Count
actual API calls, including retries, when matching budgets. See the
[full control requirements](docs/assignment.md#controls).

A clear negative result can earn all **100 base marks**. Confirmed speedup per
token earns up to **10 bonus marks**. Put a negative result plainly in the
abstract, and keep every claim within what the measurements support.

## What to implement

The assignment's implementation file is [`src/Search.cpp`](src/Search.cpp).
The original starter leaves seven functions empty; this checkout contains their
implementations, including the decision rule, search loop, calibration-trial
mode, and saved-measurement judging mode (`--judge`).

| Tool | Purpose |
|---|---|
| `CanMehSearch` | Propose candidates, apply your decision rule, and log the search |
| `CanMehHarness` | Manage SDK work trees, splice patches, build libraries, and invoke measurements |
| `CanMehCalibrate` | Check the twelve supplied calibration pairs |
| `CanMehBench` | Check correctness and time reference/candidate decodes |
| `CanMehDecodeForSampler` | Decode repeatedly while a profiler records source-line samples |
| `CanMehDataGenerator` | Generate the four deterministic activity files; run by the `CanMehData` build target |

The other five tools and the utilities in `src/utilities/` are provided. See the
[architecture and call diagrams](docs/reference.md#architecture) for the
function responsibilities.

## Where results go

Each search writes to its `--out` directory:

| File | Contents |
|---|---|
| `results.jsonl` | One record per candidate, including rejected candidates |
| `credits.csv` | API calls, tokens, elapsed time, and best confirmed/retained speedups |
| `regions_tried.json` | Per-region attempts, acceptances, and improvement over parents |
| `patches/gen_N.patch` and `patches/gen_N.repl` | Candidate diffs and replacement text |
| `best.patch` | Best confirmed patch, written when the run finishes |

Read the [output field reference](docs/cluster.md#monitoring-a-run) before
parsing logs: rejected candidates can have fewer fields. Count
`improved_on_parent` when attributing discoveries; a descendant can inherit an
earlier patch's speedup. `best_retained_speedup` may include candidates your
rule did not accept.

## Measurement rules to keep in view

- **Compute decisions from raw `timings`.** The harness's `ratio` and
  `significant` fields do not substitute for your calibrated rule.
- **Times are already per decode.** Do not divide `candidate_ns` or
  `reference_ns` by the iteration counts.
- **Give each concurrent search its own work directory.** Keep other timing
  jobs and profilers off that node, and do not rebuild while jobs run.
- **Keep region indexing consistent.** Pass the same `--max-lines` to
  `regions`, `show`, and `splice`; changing it renumbers ids.
- **Report ratios and provenance.** Record the node, compiler, model version,
  seeds, settings, calls, tokens, wall-clock time, and failed runs.
- **Back up run artifacts.** `runs/` is ignored by Git, so a clone will not
  contain them. `best.patch` may be absent if a job hits its time limit.

See the [timing reference](docs/reference.md#measurement) and
[cluster traps](docs/cluster.md#traps) for the explanations.

## Submission

Submit a repository that builds from a clean checkout and includes your rule,
loop, and run logs; a cleanly applicable patch series with explanatory commit
messages; a report of at most **four pages plus references, with one figure**;
and a **five-minute demonstration with one figure**. Deadlines and submission
instructions are announced in class.

Coding agents are allowed, but you must explain every submitted line and trace
every reported number to a real run. The FIT SDK is licensed: fetch it under its
terms, and do not commit or redistribute it. Frame results around what your
modification changed.

Read the [report checklist](docs/assignment.md#phase-2-reporting),
[deliverables](docs/assignment.md#deliverables), and
[marking scheme](docs/assignment.md#marking) before writing up the results.
