# Assignment requirements

[Back to README](../README.md) · [Cluster setup](cluster.md) · [Technical reference](reference.md)

Follow Phase 0, Phase 1, and Phase 2 in order. Complete C2 before viewing any
search output. These requirements apply even when the search finds no speedup.

- [Phase 0: Calibration](#phase-0-calibration)
- [Phase 1: Search loop](#phase-1-search-loop)
- [Phase 2: Reporting](#phase-2-reporting)
- [Controls C1–C4](#controls)
- [Deliverables](#deliverables)
- [Marking](#marking)
- [Measurement discipline](#measurement-discipline), [AI use](#use-of-ai), and [licensing](#licensing)

## Assignment

### Phase 0: Calibration

Calibrate your decision rule on library pairs with known work counts. Measure
how often the rule reports a false speedup, the smallest effect it detects, and
the bias in its reported ratio. Include all three in your report.

You write the rule that turns [timings](reference.md#timings) into a claim, so you have to establish
what that rule does before you spend a model budget on it. An [inert patch](reference.md#inert-patch) cannot
establish what that rule does: an inert patch tests one point, a [true ratio](reference.md#true-ratio) of
exactly 1.000, and leaves unmeasured both the smallest effect your rule detects
and the distance between the number your rule reports and the true ratio.

CMake builds **calibration pairs** so that you can. CMake compiles each pair
from one source file into two shared libraries that differ only in how many
**[work units](reference.md#work-unit)** each library performs, so the quotient of those two counts is the
true ratio. CMake writes every pair into its own directory under
`build/calibration`, and records the two work counts in each `meta.json` and
never their quotient: dividing them is your job. Each of those directories is a
[work directory](reference.md#work-directory) that `CanMehHarness measure-pair` accepts.

CMake builds twelve pairs under `build/calibration`, each in a work directory
`CanMehHarness measure-pair` accepts, and `./build/CanMehCalibrate` measures all
twelve and prints one table:

```sh
./build/CanMehCalibrate --repeat 10
```

The true ratio is the candidate's work count divided by the reference's, so a
[ratio](reference.md#ratio) below 1.000 means the candidate library decoded faster:

| pair | candidate units | true ratio | what it is for |
|---|---|---|---|
| `null` | 20000 | 1.0000 | the [false-positive rate](reference.md#false-positive-rate) |
| `faster-1-percent` | 19801 | 0.9901 | [detection threshold](reference.md#detection-threshold) |
| `faster-2-percent` | 19607 | 0.9804 | detection threshold |
| `faster-5-percent` | 19047 | 0.9524 | detection threshold |
| `faster-10-percent` | 18181 | 0.9091 | detection threshold |
| `slower-10-percent` | 22000 | 1.1000 | a one-sided rule must never call this a speedup |
| `sweep-105` | 21000 | 1.0500 | [bias](reference.md#bias) |
| `sweep-110` | 22000 | 1.1000 | bias |
| `sweep-200` | 40000 | 2.0000 | bias, and required |
| `sweep-400` | 80000 | 4.0000 | bias, and required |
| `broken-digest` | 18181 | — | `CanMehBench` refuses it at stage `correctness` |
| `shared-install-name` | 18181 | — | `CanMehBench` refuses it at stage `load` |

At the default 20,000 reference units the counts are what the table shows, and
they scale with `-DCANMEH_CALIBRATION_REFERENCE_UNITS`. Note that
`slower-10-percent` and `sweep-110` are the same pair twice: both compile 22,000
candidate units, so the ten measured pairs supply nine distinct true ratios. Measuring
both is still worth the time, because two pairs built from one configuration are
a free check that the rig repeats.

The last two rows are not decoration. `CanMehBench` refuses the `broken-digest` pair
because the two libraries fold different checksums, and refuses the shared
install name because `dlopen` answers with one image twice. A loop that reports a
number when `CanMehBench` refused the pair has reported a number about nothing,
so measure both.

For the two pairs CMake built to be refused, `CanMehCalibrate` reports the stage
`CanMehBench` refused the pair at. For every other pair, `CanMehCalibrate`
reports the true ratio, the measured ratio, the error and [`ratio_spread`](reference.md#ratio_spread), and
requires five things of the twelve rows: `CanMehBench` must refuse
`broken-digest` at stage `correctness` and `shared-install-name` at stage
`load`, and the measured ratio must land within two percent of 1.000 for
`null` and within three percent of the true ratio for `sweep-200` and
`sweep-400`. A required row that misses ends the run with a non-zero exit
status. `CanMehCalibrate` prints the other seven rows for you to read, because
near the smallest effect `measure-patch` resolves, a miss is evidence about
`measure-patch` rather than about the calibration pairs.

Report three numbers, each with the machine, the compiler, `--repeat`,
`--repetitions`, `--min-time` and how many trials you ran:

1. The **false-positive rate** of your rule: how often your rule reports a
   speedup for the `null` pair, whose two libraries perform equal work, with a
   95 percent Clopper-Pearson interval, which is the exact binomial one. Use
   Clopper-Pearson or Wilson rather than a normal approximation, because the
   normal approximation gives an interval of zero width when your rule reports
   no speedup in any trial, and no speedup in any trial is the outcome to
   expect. Ten trials with no false positive give 0 to 0.31, which is why ten
   trials cannot distinguish one percent from fifteen, so say what your trial
   count supports.
2. The **detection threshold**: the smallest true effect your rule finds in at
   least nine trials out of ten. The four faster pairs give you four known
   effect sizes; report the rate at each one instead of one number,
   because a threshold with no curve behind it hides whether the rule
   degrades gradually or drops abruptly below some effect size.
3. The **bias** of the ratio your rule reports: the reported ratio minus
   the quotient of the compiled work counts, at each effect size.

A rate and a threshold both need many trials, and `CanMehCalibrate` measures
each pair once per invocation, so you produce trials by repeating one
measurement rather than by invoking `CanMehCalibrate` again. A shell loop around
`./build/CanMehHarness measure-pair --work build/calibration/null --no-decision`
gives one trial of your rule per iteration, and the same loop over
`build/calibration/faster-5-percent` and its three siblings gives the detection
trials. Record how many you ran, because the count is what decides which rate
you can claim.

Choosing your rule's thresholds by looking at the pairs is allowed. Say which
numbers you chose that way and which pairs you chose them on, because a rate
measured on the same pairs that set a threshold describes those pairs rather
than your next measurement, and a reader who knows which is which reads your
false-positive rate correctly. Calibration here means
fixing your rule and then measuring what the fixed rule does against an answer
known by construction, so nothing inside `CanMehHarness` is adjusted.

`CanMehCalibrate` measures the four sweep pairs, whose candidates perform more work
than the reference in the same run and prints them in the same table, and you
read those four rows to check the calibration pairs rather than to check your
rule. For those four pairs `CanMehCalibrate` divides the measured
ratio by the quotient of the counts CMake compiled in and reports how
far that quotient falls from 1.000. That comparison is there to
rule out one thing. Suppose one [decode](reference.md#decode) of `N` work units takes `a + b*N`. The
measured ratio is then `(a + b*M) / (a + b*N)`, which equals `M/N` only when `a`
is negligible. A measured ratio of 2.000 for the 2.00 pair therefore shows that
`a` is small enough for the quotient of the compiled counts to be the true
ratio. When `CanMehCalibrate` reports a required row outside its bound, raise
the work count rather than widening the bound. Raising it means reconfiguring
with a larger `-DCANMEH_CALIBRATION_REFERENCE_UNITS` and rebuilding, which
recompiles all twelve pairs:

```sh
cmake -S . -B build -DCANMEH_CALIBRATION_REFERENCE_UNITS=<units>
cmake --build build -j
```

Then report the settings you measured at, the work count included, because a
rate or a threshold holds only for the pairs it was measured on.

Two rules for applying what Phase 0 established when you run Phase 1. Use the
same `--repeat` and `--repetitions` for the search as for the calibration
pairs, because a false-positive rate you establish at one setting is not the
rate at another setting. **`--min-time` is the exception, and matching it would
be wrong.** `CanMehCalibrate` sizes `--min-time` per pair from a probe, at about
twice one decode of that pair, and records the value it used in each row of
`build/calibration/calibration.json`. The reason is in the opposite direction
from what you would guess: a `min_time` far above one decode makes Google
Benchmark average many decodes into each repetition, which hides the work
loop's variance and makes your rule look better on the pairs than it performs
on the SDK decode. So leave `--min-time` alone in both phases, and when you run
`CanMehHarness measure-pair` over a pair by hand, pass that pair's recorded
`min_time` yourself, which `CanMehCalibrate` recorded in `calibration.json`,
because `measure-pair` otherwise falls back to the 0.25s default. And run your loop against `measure-patch --no-decision`, so
`CanMehHarness` hands you timings and your code decides. One warning about
running your loop against `measure-patch --no-decision`: a rule that reads
`result["ratio"]` without first checking that the field is present throws on
the first `--no-decision` run, and `ratio` is the field such a rule reaches for
first. If you know that, the diagnosis takes a
minute; if you do not, it takes an afternoon.

What the calibration pairs cannot establish, and what to say about each in your
report:

- **The calibration work loop's timings are stationary and the SDK decoder's are
  not.** Stationary here means the distribution each repetition's time is drawn
  from stays the same across the series. The SDK decoder allocates inside the
  timed region and branches on the data, so the SDK decoder's
  timings may have a heavier tail and may correlate from one
  repetition to the next, and nobody here has measured either. A rule that
  assumes independent repetitions can reach its nominal false-positive rate on a
  calibration pair and exceed it on the SDK decoder. Report the lag-1 sample
  autocorrelation of each process's `candidate_ns` series: the sum of
  `(x[i] - mean) * (x[i+1] - mean)` over the adjacent pairs, divided by the sum
  of `(x[i] - mean)^2` over the whole series, taking that series' own mean. Nine
  repetitions give one such number a standard error near 1/sqrt(9), about 0.33,
  so report the median and the range of it across the processes of one
  measurement rather than one series' value, for the calibration work loop and
  for the SDK decoder, and state whether your rule assumes independent
  repetitions.
  - **A calibration pair scales the whole workload uniformly.** A real patch
  usually changes the time of one part of a decode and leaves the rest
  unchanged, so a detection threshold you establish on a pair that scales the
  whole decode does not transfer to a patch that speeds up one part of it.
- **A pair bounds only the part of a bias that survives the division.** A
  constant difference between the two [slots](reference.md#slot), or an error in the clock's scale,
  divides out. Measuring the `null` pair bounds only the part that does not
  cancel, which is why we still require both the inert patch on the
  SDK decoder and the null run from `CanMehHarness init`.
- **The twelve pairs reach three outcomes.** `CanMehCalibrate` calls
  `measure-pair`, which times two libraries CMake already built, so a pair
  reaches stage `load`, stage `correctness` or a measurement. Every calibration
  library decodes on one thread, so `CanMehBench`'s threading check accepts all
  twelve. Say which of your loop's failure branches a calibration pair reached.

### Phase 1: Search loop

This phase is the search itself. You write the loop and the rule, run them
against the harness with every statistic withheld, and produce the separation
between a real effect and no effect yourself.

You write two programs: the rule that decides whether a candidate is faster,
and the search loop that spends a budget on candidates. `CanMehHarness`
compiles the tree, splices your replacement into one region and times the two
libraries' decodes, and `measure-patch --no-decision` hands you every timing and
withholds every statistic. In Phase 0 you calibrate the first program.

The original starter declares five structs and seven empty functions in
`src/Search.cpp`. This checkout has implementations of those functions. The
requirements below describe what the submitted loop must do. [Defaults](reference.md#defaults) lists the main option defaults. The four
harness subcommands your loop calls appear below. We expect C++, so
that you can include the four headers in `src/utilities/`, and any other
language except Python works if you implement the chat-completions request
yourself.

The command that runs a finished loop names the work directory `init` created,
the output directory for the run's logs and patches, and the number of
generations. `--llm-base-url` and `--llm-model` already default to the SoC
endpoint and a model it serves, so the only flag you have to add is
`--llm-api-key-file`, which [Model endpoint](cluster.md#model-endpoint) covers:

```sh
./build/CanMehSearch --work build/work --out runs/exp1 --generations 40 \
  --llm-api-key-file ~/.config/canmeh/api-key
```

Your measurement rule has no prescribed shape. It reads the `timings` object
out of `measure-patch --no-decision` and emits one decision, and whether it is a
function inside your loop, a separate program or a shell pipeline is yours to
choose. What the marking scheme asks for is that the rule is identifiable, that
Phase 0 calibrated the rule you actually ran, and that a reader can see what it
computes.

Your loop asks `CanMehHarness regions` which regions your loop may edit, picks a
region, asks `show` for that region's current text under the [parent patch](reference.md#parent-patch), builds
a prompt, calls the model, writes the replacement text out of the reply to a
file and names that file to `splice`, then has `measure-patch --no-decision`
compile the patched tree and time its decodes, applies your own rule to the timings, feeds compile errors
and correctness failures into the next prompt, and logs every candidate so anyone
can reconstruct the run.

Those four subcommands, with real arguments. Every one of them takes `--work`,
and every one that resolves a region id takes `--max-lines`, because the id is
an index into the list at that setting and a different setting renumbers it:

```sh
H=./build/CanMehHarness
W=build/work  # replace with your initialized work directory
R=0  # replace with an id from the regions listing below

# splice writes its patch with no check that the directory exists, and reports
# ok either way, so create the directory first or splice reports success and
# leaves no file. CanMehSearch creates it itself; you create it when you call
# splice by hand.
mkdir -p runs/exp1/patches

# Which regions exist. One JSON object holding `count`, `total` and a `regions`
# array of {id, file, label, lines, start, end, ordinal}. 5,514 at --max-lines 250.
# Two regions can share a label, so label plus ordinal identifies an overload.
$H regions --work $W --max-lines 250
$H regions --work "$W" --max-lines 250 --file fit_decode.cpp   # filtered, ids unchanged

# The current text of one region, as it reads after a parent patch applies.
# Prints {ok, text, ...}; feed `text` to the model.
$H show --work "$W" --max-lines 250 --region "$R" --patch runs/exp1/patches/gen_7.patch

# Replace that region. --replacement is a FILE holding the new text, not the
# text itself, and --out is the patch file to write. --includes names a second
# file of #include lines, which splice merges into the file's include block.
$H splice --work "$W" --max-lines 250 --region "$R" \
          --patch runs/exp1/patches/gen_7.patch \
          --replacement /tmp/replacement.cpp \
          --includes /tmp/includes.txt \
          --out runs/exp1/patches/gen_8.patch

# Time it, and withhold every statistic so your rule decides.
$H measure-patch --work $W --patch runs/exp1/patches/gen_8.patch --no-decision
```

A **region** is one function body, defined in the [region definition](reference.md#region).
`CanMehHarness regions` indexes the 5,514 bodies of at most `--max-lines` lines,
and raising that bound renumbers every id, so an id from one listing means a
different function at another setting.

Three options decide how a measurement runs, and `CanMehHarness init` writes all
three into `meta.json` so that every later `measure-patch` inherits them: `--repeat`, the
number of independent processes; `--repetitions`, the repetitions inside one
process; and `--min-time`, the seconds Google Benchmark spends per repetition.
Pass them to `measure-patch` only to override what `init` recorded, and if you do, say so
in your report, because Phase 0 calibrated the settings `init` stored.

Before giving your loop a budget, check the manual workflow and profile the decoder.

Evaluate one patch by hand first. Use `show`, write a replacement in an editor,
`splice` that replacement and `measure-patch` the result, starting with a change you know
is inert, such as adding a comment, until `measure-patch` reports [`significant`](reference.md#significant)
false. Then try something you believe is real and compare the two p values. A
loop built before you have evaluated one patch by hand automates a measurement
you have never operated.

Choose regions from a measurement, not a guess. `CanMehHarness regions` indexes
more than five thousand editable regions, and in a measured decode a
few dozen of them take every profiler sample, with a handful of those taking
half of all samples. A loop that draws regions uniformly therefore spends almost
every [generation](reference.md#generation) on one of the generated message accessors outside
that few dozen. Pass `--sample-seconds` and `CanMehSearch` profiles one
decode first and then draws regions in proportion to the samples each
one took. `--uniform-share` sets how many draws ignore
that profile, because a one-line accessor the compiler inlined has no samples of
its own and rewriting that accessor can still make the decode faster.

### Phase 2: Reporting

A result is only as good as the evidence that the measurement behind it works.
This phase is the write-up of that evidence, and every requirement the report
has to meet is collected below.

#### Evidence

- The [null run](reference.md#null-run) from `CanMehHarness init`, and which node it ran on.
- Phase 0's three numbers: the false-positive rate with
  a confidence interval, the detection threshold with a
  rate at each effect size, and the bias. Give the trial count, and the
  `--repeat`, `--repetitions`, `--min-time` and work count each was produced at.
- The rule itself: what your code reads out of `timings`, what it
  computes and the threshold at which it calls a candidate faster.
- An inert patch spliced into the SDK and evaluated by hand.
- The data's four identifying numbers: the checksum, the byte count, the message
  count and the field count that `CanMehBench` wrote to `scratch/bench.err`.

#### Search

- All four controls, C1 through C4.
- Which model id answered, which is not always the id you asked for.
- The tokens you spent, from `credits.csv`, beside the speedup you claim.

#### Run

- The machine, the compiler version, the model and its version string, the
  seeds, the number of API calls and the wall-clock time.
- The runs you abandoned, and why you abandoned them.
- Which node every baseline came from, because a baseline in milliseconds
  describes one node only.

#### Document

- At most four pages, with one figure.
- A negative result stated plainly, in the abstract rather than buried.
- A result worded as what your modification changed, for the reason
  [Licensing](assignment.md#licensing) gives.

If your report contains a speedup no larger than the one your rule reports for
an inert patch, that speedup is not evidence of anything, and the four controls
below are what show whether something other than your search produced the number
you are quoting.

The design [Measurement](reference.md#measurement) describes, which loads both libraries into one
process and decides from a permutation test or a sign-flip test over the
per-repetition times, is one answer, and in Phase 1 you write the decision.
What we require is evidence of how often your rule reports a speedup where the
true ratio is 1.000, which is two things: the false-positive rate on the `null` pair from
Phase 0, and an inert patch spliced into the SDK and evaluated by hand.

## Controls

Run all four controls. Together they distinguish the search's contribution
from compiler settings, human optimization, repeated sampling, and run variance.

### C1 — The compiler control

Report the baseline at `-O2`, at `-O3` and with
`-march=native`. Say how much of your
search's gain a single compiler flag reproduces, deterministically, and note
that a compiler preserves the program's observable behavior by construction,
while your search can change that behavior.

**For control C1 you need to rebuild the SDK at other optimization settings.**
`CanMehHarness` takes `--opt` and `--cxxstd` before the subcommand, and compiles
every translation unit of every build in that run with them, so
`--opt -O3` or `--opt "-O3 -march=native"` gives you a baseline at that setting:

```sh
./build/CanMehHarness --opt -O3 init --work build/work-O3 --repeat 10
```

Each setting needs its own work directory, because `init` records the compiler in
`meta.json`, and the compiler has to compile every patched translation unit with
the flags that built the reference library.

### C2 — The human control

Spend one hour hand-optimizing the SDK decoder
yourself, and time yourself, before you look at any search output. Report the
patch and the wall-clock time. If an hour of your attention beats forty
generations of search, that is the most interesting number in your report.

### C3 — The no-search control

Run best-of-N: the same model, the same prompt,
the same total number of API calls, but every call starts from the unmodified
baseline with no feedback, no [population](reference.md#population) and no memory. Take the best result. If
your evolutionary search does not beat best-of-N at an equal call budget, the
selection and the feedback in your loop contributed nothing, and what you have
is a sampling result.

The mechanism is one flag you leave out. `show` and `splice` both take
`--patch` optionally: given it, `CanMehHarness` reads and rewrites the region
as the parent patch left it, and left out, `CanMehHarness` reads and rewrites
the region as the unmodified baseline holds it. So a best-of-N run leaves
`--patch` off every `show` and `splice` call and sends the same prompt every
time, which is C3 as stated above. Count calls
rather than generations when you compare the two runs, because a generation of
your search may make more than one call.

### C4 — The variance control

Run every configuration with at least five seeds.
Report the median and the full range of the speedups those runs produced. A
single best run is not a result.

## Deliverables

Deadlines and submission instructions are announced in class.

1. A repository with your measurement rule, your search loop and every run log,
   building from a clean checkout. It must not contain the FIT SDK.
2. A patch series: your best patches, each one applying cleanly in order, each
   with a commit message in which you state what the patch changes and why the
   patched code is faster.
3. A report of at most four pages plus references. Structure it like a workshop
   paper: what you built, what you measured, what each control measured when you
   ran it, what you believe and what you do not.
4. A five-minute demonstration in the final session, with one figure.

## Marking

| component | marks |
|---|---|
| Experimental design and execution of the controls | 35 |
| Analysis, and whether each claim in the report matches the evidence behind it | 35 |
| Measurement: the calibration pairs, the three numbers you produced from them, what your loop does with a rejected candidate, reproducibility | 30 |

Those 35 marks read one thing: whether every number in your report is claimed
at the strength your own evidence supports. A speedup reported beside the
false-positive rate you measured earns them, and the
same speedup stated flat does not. So does a negative result stated plainly, so
does naming a control you did not run, and so does saying which of your numbers
your trial count cannot support.

The calibration evidence is the larger part of those 30 marks. Phase 0 is what
produces the evidence behind a measurement rule.

**The speedup earns up to 10 bonus marks on top of the 100.**
Those 100 marks above are reachable with a search that found nothing, as long as
you measured carefully and reported what your measurement supports. On top of them we award up to 10 marks on
this quotient, read from the last row of your `credits.csv`:

    (best_confirmed_speedup - 1.0) / total_tokens

We rank the submissions on that quotient and award the bonus on the ranking. A
submission with no confirmed speedup scores zero bonus marks and keeps all 100
of the marks above.

Two reasons the denominator is tokens rather than nothing at all. A speedup
found in 400 model calls and one found in 15 are not the same result, and the
quotient says so. And a rule that admits a false positive early records a ratio
no later candidate beats, so that run spends every remaining token chasing a
number that is not there; the quotient reports that run as inefficient without
us having to find the false positive ourselves.

`CanMehSearch` writes
`prompt_tokens`, `completion_tokens` and `total_tokens` into `credits.csv`
every generation, and the bonus quotient above divides your confirmed speedup by
those tokens, so measure your own spending from that file from the first run
rather than at the end.

We keep the speedup out of the 100 on purpose. Anyone can get a number. The
skill we are teaching is knowing what a number means, and a student who
measures carefully and finds no speedup has done the assignment.

## Measurement discipline

You must be able to trace every figure to a logged run. Report the machine, the
compiler version, the model and its version string, the seeds, the number of API
calls and the wall-clock time of each run. Report the failed runs too, including
the ones you abandoned.

If your search underperforms a control, say so plainly in the abstract. We award
full marks for analysis to a clear negative result with the four controls run
and reported. We award none to a positive result that a control you did not run
would have explained.

## Use of AI

Coding agents are allowed.

Two conditions. You must be able to explain every line you submit, and we will
ask you to at the demonstration. And every number in your report must come from
a run that happened on your machine. A fabricated measurement is academic
misconduct here as anywhere, and it is especially damaging in a course about a
literature with a measurement problem.

## Licensing

The FIT SDK is licensed, not open source. Do not commit it, do not redistribute
it, and fetch it yourself under its terms. We are modifying it to explore what
LLM-driven patch search can do, and a good patch is one we would offer back to
Garmin. Write your report in those terms: what your modification changed and how
much faster the decode became, not a comparison against a vendor product.
