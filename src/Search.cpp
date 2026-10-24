/// @file
/// CanMehSearch -- the search loop.
///
/// A single-population evolutionary search. Each individual is one patch: a
/// unified diff against the stock FIT SDK holding one region rewrite for each
/// ancestor of that patch. When the model answers and CanMehHarness splices the
/// answer, CanMehSearch adds exactly one region rewrite to the parent's patch;
/// when `show`, the model call or `splice` fails, CanMehSearch adds none.
///
/// In each generation CanMehSearch picks a parent patch and a region of source,
/// asks the model to rewrite that one region, and hands the reply to
/// CanMehHarness, which splices the rewrite into a new patch, builds the patched
/// tree and times the decode. CanMehHarness decides how to build and how to
/// measure; CanMehSearch decides only what to try next.
///
/// CLI11 generates the usage text from the option definitions in main. The
/// --llm-* options overwrite the matching fields of ChatClientConfig, which
/// utilities/ChatClient.hpp documents.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

#include <CLI11.hpp>
#include <nlohmann/json.hpp>

#include "utilities/ChatClient.hpp"
#include "utilities/LineSamples.hpp"
#include "utilities/Process.hpp"

using utilities::ChatClientConfig;
using utilities::ModelReply;
using utilities::readFile;
using utilities::writeFile;
using utilities::truncate;
using utilities::runForJsonReply;


using nlohmann::json;

namespace {

#ifndef CANMEH_DEFAULT_HARNESS
#define CANMEH_DEFAULT_HARNESS "./CanMehHarness"
#endif
#ifndef CANMEH_DEFAULT_DECODE_FOR_SAMPLER
#define CANMEH_DEFAULT_DECODE_FOR_SAMPLER "./CanMehDecodeForSampler"
#endif

constexpr auto invalidScore = double{1e18};

struct Options {
  std::string work;
  std::string out = "runs/latest";
  std::string harnessPath = CANMEH_DEFAULT_HARNESS;
  std::string decodeForSamplerPath = CANMEH_DEFAULT_DECODE_FOR_SAMPLER;
  /// Zero means CanMehSearch runs no sampler, so the sample-weighted draw
  /// never happens and every draw the bestGain branch does not take is
  /// uniform. Above zero, CanMehSearch builds a library with debug line
  /// tables, samples a decode of it for this long, and draws regions in
  /// proportion to the samples each one took.
  double sampleSeconds = 0.0;
  /// The share of the draws the bestGain branch did not take that CanMehSearch
  /// makes uniformly over every region rather than in proportion to samples.
  /// Not zero by default: a one-line accessor the compiler inlined has no
  /// samples of its own, and rewriting it can still make the decode faster, so
  /// no region is unreachable. At 1.0 CanMehSearch never takes the
  /// sample-weighted draw; the bestGain draw still runs.
  double uniformShare = 0.1;
  /// Adds three sentences to the prompt naming the kinds of work to look
  /// for. Default false: those sentences name kinds of code worth rewriting,
  /// so a run with them answers a different question from a run without them,
  /// and your report has to say which arm produced the result you quote.
  bool promptHints = false;
  int generations = 40;
  int population = 6;
  int maxLines = 250;
  unsigned seed = 1;
  /// Empty means every region CanMehHarness indexes. There is no default list,
  /// because a hand-written one is the experimenter's guess about where the win
  /// is, and a narrowed region set has to come from a measured profile.
  std::string regionFileNames;
};

struct Region {
  int id = 0;
  std::string file;
  std::string label;
  int lines = 0;
  /// 1-based and inclusive, as `CanMehHarness regions` prints them.
  /// weighRanges compares a profiler's line against these two to decide which
  /// region that line is in.
  int start = 0;
  int end = 0;
  /// Ending many profiler samples name a line inside this region. Zero when
  /// --sample-seconds was zero, so CanMehSearch ran no profiler.
  long long samples = 0;
};

/// One patch the search produced, together with what measuring that patch
/// reported. The patch itself is the diff in the file at patchPath; this struct
/// is what CanMehSearch records about that diff.
struct EvaluatedPatch {
  int id = 0;
  int parent = -1;
  int generation = 0;
  int region = -1;
  std::string patchPath;  // empty means the unmodified baseline
  double score = invalidScore;
  bool valid = false;  std::string note;
  /// Our rule found p < .01 and a ratio below one in both measurements.
  bool significant = false;
  /// Both measurements also cleared our effect floor and noise check. These
  /// compare the accumulated patch with stock; improved_on_parent separately
  /// records whether this generation improved its parent's retained ratio.
  bool significantAtMinEffect = false;
};

struct RegionStats {
  int attempts = 0;
  int accepted = 0;
  // Largest ratio improvement over a parent, counting only candidates which
  // our own rule accepted in both measurements.
  // A difference of two ratios, not a duration.
  double bestGain = 0.0;
};

json runHarnessCommand(const Options& options, std::vector<std::string> args,
                        double timeoutSeconds = 900) {
  if (args.empty()) throw std::runtime_error("missing harness subcommand");
  const auto command = args.front();
  args.insert(args.end(), {"--work", options.work});
  if (command == "regions" || command == "show" || command == "splice")
    args.insert(args.end(), {"--max-lines", std::to_string(options.maxLines)});

  // Keep every invocation, including both measurements of a candidate. The
  // harness reuses scratch/gb, so paths in timings alone are not an archive.
  static unsigned serial = 0;
  auto name = std::ostringstream{};
  name << std::setw(6) << std::setfill('0') << ++serial << '-' << command;
  const auto directory = std::filesystem::path(options.out) / "harness" / name.str();
  std::filesystem::create_directories(directory);
  if (command == "measure-patch" || command == "measure-pair") {
    // A compile failure never reaches the harness's benchmark-directory reset.
    // Remove earlier reports so a failure cannot archive another candidate's data.
    const auto scratch = std::filesystem::path(options.work) / "scratch";
    std::filesystem::remove_all(scratch / "gb");
    for (const auto* file : {"bench.err", "bench.out", "cc.err", "cc.out",
                              "ld.err", "ld.out", "patch.err", "patch.out"})
      std::filesystem::remove(scratch / file);
  }
  writeFile(directory / "command.json", json{{"program", options.harnessPath},
                                             {"args", args}}.dump(2) + "\n");
  auto reply = runForJsonReply(options.harnessPath, args, directory / "stdout.json",
                               directory / "stderr.log", timeoutSeconds);
  if (command == "measure-patch" || command == "measure-pair") {
    const auto scratch = std::filesystem::path(options.work) / "scratch";
    auto error = std::error_code{};
    if (std::filesystem::exists(scratch / "gb"))
      std::filesystem::copy(scratch / "gb", directory / "gb",
                            std::filesystem::copy_options::recursive, error);
    if (error) throw std::runtime_error("cannot archive benchmark reports: " + error.message());
    for (const auto* file : {"bench.err", "bench.out", "cc.err", "cc.out",
                              "ld.err", "ld.out", "patch.err", "patch.out"}) {
      if (std::filesystem::exists(scratch / file)) {
        std::filesystem::copy_file(scratch / file, directory / file,
                                  std::filesystem::copy_options::overwrite_existing, error);
        if (error) throw std::runtime_error("cannot archive benchmark output: " + error.message());
      }
    }
  }
  // This is provenance, never an input to the decision rule.
  reply["archive"] = directory.string();
  writeFile(directory / "reply.json", reply.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
  return reply;
}

double median(std::vector<double> values) {
  if (values.empty()) throw std::runtime_error("empty timing series");
  std::sort(values.begin(), values.end());
  const auto n = values.size();
  return n % 2 ? values[n / 2] : values[n / 2 - 1] / 2 + values[n / 2] / 2;
}

double coefficientOfVariation(const std::vector<double>& values) {
  const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  auto squares = 0.0;
  for (double x : values) squares += (x - mean) * (x - mean);
  return std::sqrt(squares / values.size()) / mean;
}

double lagOne(const std::vector<double>& values) {
  const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  auto denominator = 0.0;
  auto numerator = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    denominator += (values[i] - mean) * (values[i] - mean);
    if (i + 1 < values.size()) numerator += (values[i] - mean) * (values[i + 1] - mean);
  }
  return denominator == 0 ? 0.0 : numerator / denominator;
}

json rangeSummary(const std::vector<double>& values) {
  return {{"median", median(values)},
          {"min", *std::min_element(values.begin(), values.end())},
          {"max", *std::max_element(values.begin(), values.end())}};
}

// An exact, two-sided randomization test on process log-ratios. Processes are
// the units of inference; repetitions averaged by Google Benchmark are not
// treated as additional independent observations. Sign exchangeability of
// independent process effects under the null is still an assumption.
double processPValue(const std::vector<double>& logs) {
  const auto observed = std::abs(std::accumulate(logs.begin(), logs.end(), 0.0));
  auto tolerance = 0.0;
  for (double x : logs) tolerance += std::abs(x);
  tolerance *= 1e-12;  // Count floating-point ties conservatively.
  const auto assignments = std::uint64_t{1} << logs.size();
  auto extreme = std::uint64_t{0};
  for (std::uint64_t mask = 0; mask < assignments; ++mask) {
    auto sum = 0.0;
    for (std::size_t i = 0; i < logs.size(); ++i)
      sum += (mask & (std::uint64_t{1} << i)) ? logs[i] : -logs[i];
    if (std::abs(sum) + tolerance >= observed) ++extreme;
  }
  return static_cast<double>(extreme) / assignments;
}

// All statistics and decisions below depend ONLY on the timings argument.
// A conventional median is used, including averaging the middle two values.
json decisionFromTimings(const json& timings, double minEffect) {
  try {
    if (!std::isfinite(minEffect) || minEffect < 0 || minEffect >= 1)
      throw std::runtime_error("min-effect must be in [0, 1)");
    if (timings.at("unit") != "ns" || timings.at("clock") != "real_time")
      throw std::runtime_error("expected real_time in nanoseconds");
    if (timings.at("ragged").get<bool>()) throw std::runtime_error("ragged timing series");
    const auto& processes = timings.at("process");
    const auto repetitions = timings.at("repetitions_requested").get<int>();
    if (!processes.is_array() || processes.empty() || processes.size() > 20 || repetitions < 3)
      throw std::runtime_error("need 1 to 20 processes and at least 3 repetitions");

    auto candidates = std::vector<double>{};
    auto references = std::vector<double>{};
    auto ratios = std::vector<double>{};
    auto logs = std::vector<double>{};
    auto candidateLag = std::vector<double>{};
    auto referenceLag = std::vector<double>{};
    auto candidateIterations = std::vector<double>{};
    auto referenceIterations = std::vector<double>{};
    auto indices = std::set<int>{};
    auto candidateCv = 0.0;
    auto referenceCv = 0.0;
    auto swapped = 0;
    for (const auto& process : processes) {
      if (!indices.insert(process.at("index").get<int>()).second)
        throw std::runtime_error("duplicate process index");
      auto series = [&](const char* name, bool allowZero = false) {
        const auto& array = process.at(name);
        if (!array.is_array() || array.size() != static_cast<std::size_t>(repetitions))
          throw std::runtime_error(std::string("missing repetitions in ") + name);
        auto values = std::vector<double>{};
        for (const auto& item : array) {
          if (!item.is_number()) throw std::runtime_error("non-numeric timing");
          const auto x = item.get<double>();
          if (!std::isfinite(x) || x < 0 || (!allowZero && x == 0))
            throw std::runtime_error("non-finite or non-positive timing/count");
          values.push_back(x);
        }
        return values;
      };
      const auto candidate = series("candidate_ns");
      const auto reference = series("reference_ns");
      series("candidate_cpu_ns", true);
      series("reference_cpu_ns", true);
      const auto ci = series("candidate_iterations");
      const auto ri = series("reference_iterations");
      candidateIterations.insert(candidateIterations.end(), ci.begin(), ci.end());
      referenceIterations.insert(referenceIterations.end(), ri.begin(), ri.end());
      candidates.push_back(median(candidate));
      references.push_back(median(reference));
      ratios.push_back(candidates.back() / references.back());
      logs.push_back(std::log(ratios.back()));
      candidateCv = std::max(candidateCv, coefficientOfVariation(candidate));
      referenceCv = std::max(referenceCv, coefficientOfVariation(reference));
      candidateLag.push_back(lagOne(candidate));
      referenceLag.push_back(lagOne(reference));
      if (process.at("swapped").get<bool>()) ++swapped;
    }
    // Times are already per decode. Iteration counts describe the averaging
    // depth and are logged; they are neither divisors nor extra sample sizes.
    const auto ratio = median(candidates) / median(references);
    const auto spread = *std::max_element(ratios.begin(), ratios.end()) -
                        *std::min_element(ratios.begin(), ratios.end());
    const auto p = processPValue(logs);
    const auto unstable = processes.size() > 1 ? spread > 0.25
                                               : std::max(candidateCv, referenceCv) > 0.04;
    const auto significant = ratio < 1 && p < 0.01;
    const auto accepted = significant && 1 - ratio >= minEffect && !unstable;
    return {{"ok", true}, {"ratio", ratio}, {"speedup", 1 / ratio},
            {"score_ms", median(candidates) / 1e6},
            {"reference_ms", median(references) / 1e6},
            {"p_value", p}, {"significant", significant},
            {"significant_at_min_effect", accepted}, {"accepted", accepted},
            {"min_effect", minEffect}, {"unstable", unstable},
            {"ratio_spread", spread}, {"candidate_cv", candidateCv},
            {"reference_cv", referenceCv}, {"processes", processes.size()},
            {"repetitions", repetitions}, {"swapped_processes", swapped},
            {"candidate_lag1", rangeSummary(candidateLag)},
            {"reference_lag1", rangeSummary(referenceLag)},
            {"candidate_iterations", rangeSummary(candidateIterations)},
            {"reference_iterations", rangeSummary(referenceIterations)},
            {"min_time", timings.at("min_time")},
            {"test", "exact two-sided process log-ratio sign-flip"}};
  } catch (const std::exception& error) {
    return {{"ok", false}, {"stage", "timings"}, {"error", error.what()},
            {"accepted", false}, {"significant", false},
            {"significant_at_min_effect", false}};
  }
}

/// The decision: did this accumulated candidate patch decode faster than stock?
/// Comparison with its parent's ratio is a separate, descriptive calculation.
///
/// Phase 1 of README.md requires you to run measure-patch with --no-decision,
/// and CanMehHarness then sends neither `significant` nor `ratio`, so your
/// version computes the decision from reply["timings"] alone. That computation
/// is the assignment, which is why the rule is one function rather than an
/// expression inside the loop.
bool candidateIsFaster(const json& reply, double minEffect) {
  if (!reply.contains("timings")) return false;
  return decisionFromTimings(reply.at("timings"), minEffect).value("accepted", false);
}

struct CodeBlock {
  std::string tag;
  std::string text;
};

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? std::string{} :
      text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Reasoning models sometimes put their analysis before the final answer.
// Never select a code example from that analysis instead of the answer.
std::string answerText(std::string text) {
  text = trim(text);
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text = trim(text.substr(3));
  for (const auto* tag : {"think", "analysis"}) {
    const auto opening = std::string{"<"} + tag + ">";
    const auto closing = std::string{"</"} + tag + ">";
    while (text.rfind(opening, 0) == 0) {
      const auto end = text.find(closing, opening.size());
      if (end == std::string::npos) return {};
      text = trim(text.substr(end + closing.size()));
    }
  }
  if (text.rfind("<final>", 0) == 0) {
    const auto end = text.find("</final>", 7);
    if (end == std::string::npos) return {};
    text = trim(text.substr(7, end - 7));
  }
  return text;
}

// A cheap truncation check, not a C++ parser. Ignore comments and literals so
// braces in either cannot make an incomplete function appear complete.
bool completeFunction(const std::string& text) {
  auto delimiters = std::vector<char>{};
  auto skeleton = std::string{};
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = text[i];
    if (text.compare(i, 2, "//") == 0) {
      const auto end = text.find('\n', i + 2);
      if (end == std::string::npos) break;
      i = end;
      skeleton += '\n';
    } else if (text.compare(i, 2, "/*") == 0) {
      const auto end = text.find("*/", i + 2);
      if (end == std::string::npos) return false;
      i = end + 1;
      skeleton += ' ';
    } else if (text.compare(i, 2, "R\"") == 0) {
      const auto start = text.find('(', i + 2);
      if (start == std::string::npos || start - i > 18) return false;
      const auto closing = ")" + text.substr(i + 2, start - i - 2) + "\"";
      const auto end = text.find(closing, start + 1);
      if (end == std::string::npos) return false;
      i = end + closing.size() - 1;
      skeleton += ' ';
    } else if (c == '\"' || (c == '\'' && !(i > 0 &&
               std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
               i + 1 < text.size() && std::isalnum(static_cast<unsigned char>(text[i + 1]))))) {
      auto closed = false;
      while (++i < text.size()) {
        if (text[i] == '\\') ++i;
        else if (text[i] == c) { closed = true; break; }
      }
      if (!closed) return false;
      skeleton += ' ';
    } else {
      skeleton += c;
      if (c == '(' || c == '[' || c == '{') delimiters.push_back(c);
      if (c == ')' || c == ']' || c == '}') {
        const auto opening = c == ')' ? '(' : c == ']' ? '[' : '{';
        if (delimiters.empty() || delimiters.back() != opening) return false;
        delimiters.pop_back();
      }
    }
  }
  skeleton = trim(skeleton);
  const auto body = skeleton.find('{');
  return delimiters.empty() && body != std::string::npos &&
      skeleton.find('(') < body && !skeleton.empty() && skeleton.back() == '}';
}

bool includeLine(const std::string& line) {
  const auto text = trim(line);
  if (text.empty() || text.front() != '#') return false;
  const auto directive = trim(text.substr(1));
  return directive.rfind("include", 0) == 0 && directive.size() > 7 &&
      (std::isspace(static_cast<unsigned char>(directive[7])) ||
       directive[7] == '<' || directive[7] == '\"');
}

std::string takeIncludes(const std::string& text, std::string* includes) {
  auto lines = std::istringstream(text);
  auto line = std::string{};
  auto code = std::string{};
  while (std::getline(lines, line)) {
    if (includeLine(line)) {
      // Harness splice recognizes the canonical #include spelling only.
      *includes += "#" + trim(trim(line).substr(1)) + "\n";
    }
    else code += line + "\n";
  }
  return code;
}

std::vector<CodeBlock> codeBlocks(const std::string& reply) {
  auto blocks = std::vector<CodeBlock>{};
  auto lines = std::istringstream(reply);
  auto line = std::string{};
  auto current = CodeBlock{};
  char marker = 0;
  std::size_t fenceLength = 0;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto start = line.find_first_not_of(" \t");
    const auto trimmed = start == std::string::npos ? std::string{} : line.substr(start);
    auto count = std::size_t{0};
    if (!trimmed.empty() && (trimmed.front() == '`' || trimmed.front() == '~'))
      while (count < trimmed.size() && trimmed[count] == trimmed.front()) ++count;
    if (!marker && count >= 3) {
      marker = trimmed.front();
      fenceLength = count;
      current = {trimmed.substr(count), ""};
    } else if (marker && !trimmed.empty() && trimmed.front() == marker &&
               count >= fenceLength && trimmed.find_first_not_of(" \t", count) == std::string::npos) {
      blocks.push_back(current);
      marker = 0;
    } else if (marker) {
      current.text += line + "\n";
    }
  }
  // An unclosed block is usually a token-truncated answer; do not compile it.
  return blocks;
}

/// Reads the replacement text and the include lines out of one model reply.
///
/// Accept C++ or untagged fences, bare functions, and JSON objects containing
/// code/replacement and includes. Skip explanation/include-only blocks when
/// looking for the function. An unclosed fence or function is not a candidate.
void splitReply(const std::string& reply, std::string* code, std::string* includes) {
  code->clear();
  includes->clear();
  const auto answer = answerText(reply);
  auto blocks = codeBlocks(answer);
  if (blocks.empty() && answer.find("```") == std::string::npos &&
      answer.find("~~~") == std::string::npos) blocks.push_back({"", answer});
  // Prefer explicitly tagged C++ over untagged examples elsewhere in a reply.
  auto cpp = [](const std::string& tag) {
    return tag == "cpp" || tag == "c++" || tag == "cxx" || tag == "cc" ||
        tag == "c" || tag == "cplusplus" || tag == "cpp17" || tag == "c++17";
  };
  for (auto& block : blocks) {
    block.tag = trim(block.tag);
    std::transform(block.tag.begin(), block.tag.end(), block.tag.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    block.tag = block.tag.substr(0, block.tag.find_first_of(" \t"));
    if (block.tag.size() > 3 && block.tag.rfind("{.", 0) == 0 && block.tag.back() == '}')
      block.tag = block.tag.substr(2, block.tag.size() - 3);
  }
  std::stable_sort(blocks.begin(), blocks.end(), [&](const CodeBlock& a, const CodeBlock& b) {
    return cpp(a.tag) && !cpp(b.tag);
  });
  for (const auto& block : blocks) {
    auto text = block.text;
    auto extraIncludes = std::string{};
    if (block.tag == "json" || (!trim(text).empty() && trim(text).front() == '{')) {
      const auto object = json::parse(text, nullptr, false);
      if (object.is_object()) {
        text.clear();
        for (const auto* key : {"replacement", "code", "function"})
          if (object.contains(key) && object.at(key).is_string()) {
            text = object.at(key).get<std::string>();
            break;
          }
        if (object.contains("includes")) {
          const auto& value = object.at("includes");
          if (value.is_string()) takeIncludes(value.get<std::string>(), &extraIncludes);
          else if (value.is_array())
            for (const auto& line : value)
              if (line.is_string()) takeIncludes(line.get<std::string>(), &extraIncludes);
        }
      } else if (block.tag == "json") continue;
    }
    if (!block.tag.empty() && !cpp(block.tag) && block.tag != "json" &&
        block.tag.find("include") == std::string::npos) continue;
    const auto replacement = takeIncludes(text, &extraIncludes);
    // Includes accompanying a rejected alternative must not leak into the
    // selected function. Standalone include blocks apply to the selected code.
    if (trim(replacement).empty() || block.tag.find("include") != std::string::npos)
      *includes += extraIncludes;
    else if (code->empty() && completeFunction(replacement)) {
      *code = replacement;
      *includes += extraIncludes;
    }
  }
}

struct SearchModelReply : ModelReply {
  std::string raw;
  long httpStatus = 0;
  std::string finishReason;
  std::string contentSource;
};

// Text content can be a string or an array of text parts. Ignore non-text
// parts (images, reasoning, refusals), rather than dumping JSON into C++.
std::string responseText(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  auto result = std::string{};
  if (value.is_array()) {
    for (const auto& part : value) {
      if (part.is_string()) result += part.get<std::string>();
      else if (part.is_object() && part.contains("text") &&
               (!part.contains("type") || part.at("type") == "text" || part.at("type") == "output_text"))
        result += responseText(part.at("text"));
    }
  } else if (value.is_object()) {
    if (value.contains("text") && (!value.contains("type") || value.at("type") == "text" ||
                                  value.at("type") == "output_text"))
      result = responseText(value.at("text"));
    else if (value.contains("value") && value.at("value").is_string())
      result = value.at("value").get<std::string>();
  }
  return result;
}

SearchModelReply parseModelResponse(const std::string& raw) {
  auto reply = SearchModelReply{};
  reply.raw = raw;
  const auto parsed = json::parse(raw, nullptr, false);
  if (!parsed.is_object()) {
    reply.error = "malformed response: expected a JSON object";
    return reply;
  }
  // Preserve billed usage even when no usable final answer was returned.
  if (parsed.contains("usage") && parsed.at("usage").is_object()) {
    const auto& usage = parsed.at("usage");
    auto count = [&](const char* key) -> long long {
      if (!usage.contains(key) || !usage.at(key).is_number_integer()) return 0;
      const auto& value = usage.at(key);
      if (value.is_number_unsigned() && value.get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) return 0;
      return std::max(0LL, value.get<long long>());
    };
    reply.promptTokens = count("prompt_tokens");
    reply.completionTokens = count("completion_tokens");
    reply.totalTokens = count("total_tokens");
  }
  if (!parsed.contains("choices") || !parsed.at("choices").is_array() ||
      parsed.at("choices").empty() || !parsed.at("choices").at(0).is_object()) {
    reply.error = "malformed response: missing choices[0]";
    return reply;
  }
  const auto& choice = parsed.at("choices").at(0);
  if (choice.contains("finish_reason") && choice.at("finish_reason").is_string())
    reply.finishReason = choice.at("finish_reason").get<std::string>();
  auto select = [&](const json& object, const char* key, const std::string& source) {
    if (!trim(reply.content).empty() || !object.is_object() || !object.contains(key)) return;
    const auto text = responseText(object.at(key));
    if (!trim(text).empty()) { reply.content = text; reply.contentSource = source; }
  };
  const auto message = choice.contains("message") ? choice.at("message") : json{};
  select(message, "content", "message.content");
  select(message, "text", "message.text");
  select(choice, "text", "choices[0].text");
  // Some compatible endpoints place the final fenced answer in the reasoning
  // field. Use it only if it actually contains a complete fenced replacement.
  if (trim(reply.content).empty() && message.is_object()) {
    for (const auto* key : {"reasoning_content", "reasoning"}) {
      if (!message.contains(key)) continue;
      const auto text = responseText(message.at(key));
      auto code = std::string{};
      auto includes = std::string{};
      if (!codeBlocks(answerText(text)).empty()) splitReply(text, &code, &includes);
      if (!code.empty()) { reply.content = text; reply.contentSource = std::string{"message."} + key; break; }
    }
  }
  if (reply.finishReason == "length" || reply.finishReason == "max_tokens")
    reply.error = "reply reached the token limit (finish_reason=" + reply.finishReason +
        "); increase --llm-max-tokens";
  else if (reply.finishReason == "content_filter")
    reply.error = "reply was filtered (finish_reason=content_filter)";
  else if (trim(reply.content).empty())
    reply.error = "reply contains no text answer (content is missing, null, empty or non-text; finish_reason=" +
        reply.finishReason + ")";
  return reply;
}

// Keep the provided ChatClient utility unchanged: search needs raw responses
// and tolerant decoding, while its outer loop owns retries and call budgets.
SearchModelReply requestSearchReply(const ChatClientConfig& config,
                                    const std::string& system, const std::string& user) {
  auto reply = SearchModelReply{};
  auto handle = utilities::chatClientDetail::EasyHandle{curl_easy_init()};
  if (!handle) { reply.error = "curl_easy_init failed"; return reply; }
  auto headers = utilities::chatClientDetail::HeaderList{
      curl_slist_append(nullptr, "Content-Type: application/json")};
  if (!config.apiKey.empty())
    headers.reset(curl_slist_append(headers.release(), ("Authorization: Bearer " + config.apiKey).c_str()));
  const auto url = config.baseUrl + "/chat/completions";
  const auto payload = json{{"model", config.model}, {"temperature", config.temperature},
      {"max_tokens", config.maxTokens}, {"messages", json::array({
          {{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})}}.dump();
  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, utilities::chatClientDetail::appendToString);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &reply.raw);
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, config.timeoutSeconds);
  const auto performed = curl_easy_perform(handle.get());
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &reply.httpStatus);
  if (performed != CURLE_OK) reply.error = std::string{"transport: "} + curl_easy_strerror(performed);
  else if (reply.httpStatus != 200)
    reply.error = "http " + std::to_string(reply.httpStatus) + ": " + reply.raw.substr(0, 300);
  else {
    const auto status = reply.httpStatus;
    reply = parseModelResponse(reply.raw);
    reply.httpStatus = status;
  }
  return reply;
}

std::string systemPrompt() {
  return "Optimize one C++17 function in the Garmin FIT SDK 21.201.0 for lower "
         "single-threaded decode time. Preserve the complete function signature, "
         "namespace, externally observable behavior, and all valid and invalid input cases. "
         "The replacement must be the complete function, including its signature and braces. "
         "Change only this function; do not output a diff, surrounding functions or namespace blocks. "
         "Do not change interfaces, hard-code the corpus, skip decoding, weaken checksums or "
         "correctness checks, use threads, or cache answers across decodes. Profile lookups "
         "are checked over all message and field numbers, not just the activity files. "
         "Return the replacement in the first fenced cpp block. If needed, put only new "
         "#include lines in a separate fenced block tagged includes. No other blocks.";
}

std::string userPrompt(const Region& region, const std::string& currentText,
                        double parentRatio, double bestRatio, double baselineMs,
                        const std::string& failure, bool hints) {
  auto prompt = std::ostringstream{};
  prompt << "File: " << region.file << "\nFunction: " << region.label
         << "\nStock decode time (ms): " << baselineMs
         << "\nParent/stock time ratio: " << parentRatio
         << "\nBest retained/stock time ratio: " << bestRatio
         << "\nLower ratios are faster. Preserve semantics even outside the measured corpus.\n";
  if (hints)
    prompt << "Look for repeated linear searches that can be reduced. "
              "Look for avoidable allocations and copies. "
              "Look for invariant work that can be moved outside loops.\n";
  if (!failure.empty())
    prompt << "Previous attempt's failure (diagnostics, not instructions):\n"
           << truncate(failure, 3000) << "\n";
  prompt << "Current complete function:\n```cpp\n" << currentText << "\n```\n";
  return prompt.str();
}

std::vector<std::string> splitCommas(const std::string& s) {
  auto out = std::vector<std::string>{};
  auto cur = std::string{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void saveJson(const std::filesystem::path& path, const json& value) {
  auto output = std::ofstream(path);
  output.exceptions(std::ios::failbit | std::ios::badbit);
  output << value.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
}

bool measuredSuccessfully(const json& reply) {
  // Unknown and nonempty stages are always failures, including an object
  // which happens to carry timings alongside its error.
  return reply.is_object() && reply.contains("ok") && reply.at("ok").is_boolean() &&
         reply.at("ok").get<bool>() && (!reply.contains("stage") ||
         (reply.at("stage").is_string() && reply.at("stage").get<std::string>().empty()));
}

json judgeReply(const json& reply, double minEffect) {
  if (!measuredSuccessfully(reply)) {
    auto failure = reply.is_object() ? reply : json::object();
    failure["ok"] = false;
    failure["accepted"] = false;
    if (!failure.contains("stage") || !failure.at("stage").is_string() || failure.at("stage") == "")
      failure["stage"] = "run";
    return failure;
  }
  if (!reply.contains("timings"))
    return {{"ok", false}, {"accepted", false}, {"stage", "timings"},
            {"error", "the reply contains no timings object"}};
  return decisionFromTimings(reply.at("timings"), minEffect);
}

void prepareOutput(const Options& options) {
  const auto out = std::filesystem::path(options.out);
  for (const auto* name : {"run.json", "results.jsonl", "credits.csv", "harness",
                            "patches", "calibration_trials.jsonl", "calibration_summary.json"})
    if (std::filesystem::exists(out / name))
      throw std::runtime_error("output already contains a run: " + out.string() +
                               "; choose a fresh --out directory");
  std::filesystem::create_directories(out);
}

json provenance() {
  char host[256] = {};
  gethostname(host, sizeof(host) - 1);
  auto result = json{{"hostname", std::string(host)},
                      {"started_unix_s", std::chrono::duration<double>(
                          std::chrono::system_clock::now().time_since_epoch()).count()},
                      {"rule", "process-sign-flip-v1"}, {"alpha", 0.01},
                      {"max_ratio_spread", 0.25},
                      {"ratio_estimator", "median(process candidate medians) / median(process reference medians)"},
                      {"independent_repetitions_assumed", false},
                      {"independent_exchangeable_process_signs_assumed", true}};
  for (const auto* key : {"SLURM_JOB_ID", "SLURM_ARRAY_TASK_ID", "SLURM_JOB_PARTITION"})
    if (const auto* value = std::getenv(key)) result[key] = value;
  return result;
}

json wilsonInterval(int successes, int trials) {
  const auto z = 1.959963984540054;
  const auto n = static_cast<double>(trials);
  const auto p = successes / n;
  const auto denominator = 1 + z * z / n;
  const auto middle = (p + z * z / (2 * n)) / denominator;
  const auto radius = z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denominator;
  return {{"low", std::max(0.0, middle - radius)},
          {"high", std::min(1.0, middle + radius)}, {"method", "Wilson 95%"}};
}

// This mode repeatedly measures existing calibration pairs. It does not change
// any harness threshold or reuse the harness's verdict. The preceding
// CanMehCalibrate run supplies only its per-pair min_time settings.
int runCalibration(Options options, const std::string& rootName, int trials,
                    int repeat, int repetitions, double minEffect, const std::string& pairNames) {
  prepareOutput(options);
  const auto root = std::filesystem::absolute(rootName);
  const auto calibration = utilities::readJson(root / "calibration.json");
  if (!calibration.is_object() || calibration.value("pairs_not_as_expected", -1) != 0)
    throw std::runtime_error("run CanMehCalibrate successfully before calibration trials");
  if (calibration.value("repeat", 0) != repeat ||
      calibration.value("repetitions", 0) != repetitions)
    throw std::runtime_error("trial repeat/repetitions must match CanMehCalibrate");
  saveJson(std::filesystem::path(options.out) / "calibration_settings.json", calibration);
  auto metadata = provenance();
  metadata.update({{"mode", "calibration"}, {"trials_per_pair", trials},
                    {"repeat", repeat}, {"repetitions", repetitions},
                    {"min_effect", minEffect}, {"root", root.string()}});
  saveJson(std::filesystem::path(options.out) / "run.json", metadata);
  auto records = std::ofstream(std::filesystem::path(options.out) / "calibration_trials.jsonl");
  records.exceptions(std::ios::failbit | std::ios::badbit);
  auto rows = json::array();
  auto failures = 0;
  auto threshold = 0.0;
  const auto selectedPairs = splitCommas(pairNames);
  auto seenPairs = std::set<std::string>{};
  for (const auto& pair : calibration.at("pairs")) {
    const auto name = pair.at("pair").get<std::string>();
    if (!selectedPairs.empty() && std::find(selectedPairs.begin(), selectedPairs.end(), name) == selectedPairs.end())
      continue;
    seenPairs.insert(name);
    options.work = (root / name).string();
    const auto meta = utilities::readJson(std::filesystem::path(options.work) / "meta.json");
    if (!meta.is_object()) throw std::runtime_error("cannot read calibration meta.json for " + name);
    const auto& counts = meta.at("calibration");
    const auto trueRatio = counts.at("candidate_units").get<double>() /
                           counts.at("reference_units").get<double>();
    const auto expected = counts.value("digest_broken", false) ? std::string{"correctness"}
                        : counts.value("install_name_shared", false) ? std::string{"load"}
                                                                 : std::string{};
    const auto minTime = pair.value("min_time", std::string{"0.001s"});
    auto accepted = 0;
    auto measured = 0;
    auto refusedAsExpected = 0;
    auto biases = std::vector<double>{};
    auto lags = std::vector<double>{};
    for (int trial = 1; trial <= trials; ++trial) {
      const auto raw = runHarnessCommand(options, {"measure-pair", "--no-decision",
          "--repeat", std::to_string(repeat), "--repetitions", std::to_string(repetitions),
          "--min-time", minTime}, 7200);
      const auto decision = judgeReply(raw, minEffect);
      if (decision.value("ok", false)) {
        ++measured;
        if (candidateIsFaster(raw, minEffect)) ++accepted;
        biases.push_back(decision.at("ratio").get<double>() - trueRatio);
        lags.push_back(decision.at("candidate_lag1").at("median").get<double>());
      }
      const auto actualStage = decision.value("ok", false) ? std::string{}
                              : decision.value("stage", std::string{"unknown"});
      if (!expected.empty() && actualStage == expected) ++refusedAsExpected;
      if (actualStage != expected) ++failures;
      records << json{{"pair", name}, {"trial", trial}, {"true_ratio", trueRatio},
                       {"expected_stage", expected}, {"decision", decision},
                       {"raw", raw}}.dump(-1, ' ', false, json::error_handler_t::replace)
              << '\n' << std::flush;
      std::cout << name << " trial " << trial << '/' << trials << ": "
                << (actualStage.empty() ? decision.dump() : "refused at " + actualStage)
                << '\n' << std::flush;
    }
    auto row = json{{"pair", name}, {"true_ratio", expected.empty() ? json(trueRatio) : json(nullptr)},
                      {"reference_units", counts.at("reference_units")},
                      {"candidate_units", counts.at("candidate_units")},
                      {"trials", trials}, {"measured_trials", measured},
                      {"accepted", accepted}, {"rate", static_cast<double>(accepted) / trials},
                      {"rate_ci95", wilsonInterval(accepted, trials)},
                      {"min_time", minTime}, {"expected_stage", expected},
                      {"refused_as_expected", refusedAsExpected}};
    if (!biases.empty()) {
      row["bias"] = rangeSummary(biases);
      row["bias"]["mean"] = std::accumulate(biases.begin(), biases.end(), 0.0) / biases.size();
      row["trial_median_candidate_lag1"] = rangeSummary(lags);
    }
    if (name.rfind("faster-", 0) == 0 && accepted >= 0.9 * trials)
      threshold = std::max(threshold, trueRatio);  // Smallest effect, closest to 1.
    rows.push_back(row);
    saveJson(std::filesystem::path(options.out) / "calibration_summary.json",
             {{"pairs", rows}, {"unexpected_outcomes", failures},
              {"detection_threshold_ratio", threshold == 0 ? json(nullptr) : json(threshold)},
              {"repeat", repeat}, {"repetitions", repetitions}, {"min_effect", minEffect},
              {"interval", "Wilson 95%; rates use all trials, bias uses measured trials"}});
  }
  if (rows.empty() || (!selectedPairs.empty() && seenPairs.size() != selectedPairs.size()))
    throw std::runtime_error("--calibration-pairs contains an unknown or duplicate pair name");
  return failures == 0 ? 0 : 1;
}

std::vector<Region> loadRegions(const Options& options) {
  const auto reply = runHarnessCommand(options, {"regions"});
  if (!measuredSuccessfully(reply)) throw std::runtime_error("regions failed: " + reply.dump());
  const auto filters = splitCommas(options.regionFileNames);
  auto regions = std::vector<Region>{};
  for (const auto& item : reply.at("regions")) {
    auto region = Region{};
    region.id = item.at("id").get<int>();
    region.file = item.at("file").get<std::string>();
    region.label = item.at("label").get<std::string>();
    region.lines = item.at("lines").get<int>();
    region.start = item.at("start").get<int>();
    region.end = item.at("end").get<int>();
    if (filters.empty() || std::any_of(filters.begin(), filters.end(), [&](const std::string& filter) {
          return region.file.find(filter) != std::string::npos;
        })) regions.push_back(region);
  }
  if (regions.empty()) throw std::runtime_error("no editable regions matched --region-files");
  return regions;
}

void profileRegions(const Options& options, const json& meta, std::vector<Region>* regions) {
  auto report = json{{"requested_seconds", options.sampleSeconds}, {"ok", false},
                      {"error", "sampling disabled"}, {"attributed", 0}, {"unattributed", 0}};
  if (options.sampleSeconds > 0) {
    const auto built = runHarnessCommand(options, {"sampling-library"});
    if (measuredSuccessfully(built) && built.value("resolvable", false)) {
      auto request = utilities::SamplingRequest{};
      request.seconds = options.sampleSeconds;
      request.scratch = std::filesystem::path(options.out) / "sampling";
      request.image = std::filesystem::path(built.at("library").get<std::string>()).filename().string();
      const auto duration = options.sampleSeconds + request.settleSeconds + 15;
      request.timeoutSeconds = duration * 4 + 120;
      request.command = {options.decodeForSamplerPath, "--library", built.at("library").get<std::string>(),
          "--data", meta.at("data").get<std::string>(),
          "--benchmark_min_time=" + std::to_string(duration) + "s"};
      const auto profile = utilities::measureLineSamples(request);
      auto ranges = std::vector<utilities::LineRange>{};
      for (const auto& region : *regions)
        ranges.push_back({region.file, region.start, region.end, region.id});
      // Match the harness's canonical paths even when the cluster home/work
      // directory is reached through a symlink.
      const auto prefix = (std::filesystem::weakly_canonical(options.work) / "tree").string() + "/";
      const auto weights = utilities::weighRanges(profile, ranges, [&](const std::string& file) {
        return file.rfind(prefix, 0) == 0;
      });
      auto lines = json::array();
      for (const auto& sample : profile.samples)
        lines.push_back({{"file", sample.file}, {"line", sample.line}, {"samples", sample.weight}});
      report.update({{"ok", profile.ok && weights.attributed > 0},
          {"tool", profile.tool}, {"error", profile.error}, {"total", profile.total},
          {"in_image", profile.inImage}, {"located", profile.located},
          {"attributed", weights.attributed}, {"unattributed", weights.unattributed},
          {"ambiguous", weights.ambiguous}, {"lines", lines}});
      if (report.at("ok").get<bool>())
        for (auto& region : *regions) {
          const auto found = weights.byKey.find(region.id);
          if (found != weights.byKey.end()) region.samples = found->second;
        }
      else if (profile.error.empty()) report["error"] = "no samples in selected editable regions";
    } else {
      report["error"] = "sampling library unavailable: " + built.dump();
    }
    if (!report.value("ok", false))
      std::cerr << "Profile unavailable; using uniform draws: " << report.at("error") << '\n';
  }
  auto indexed = json::array();
  for (const auto& region : *regions)
    indexed.push_back({{"id", region.id}, {"file", region.file}, {"label", region.label},
        {"start", region.start}, {"end", region.end}, {"lines", region.lines}, {"samples", region.samples}});
  saveJson(std::filesystem::path(options.out) / "profile.json", report);
  saveJson(std::filesystem::path(options.out) / "regions.json", indexed);
}

int runSearch(const Options& options, ChatClientConfig config, double minEffect,
                bool bestOfN, int callBudget, const std::string& initResult) {
  if (options.work.empty()) throw std::runtime_error("--work is required for a search");
  const auto meta = utilities::readJson(std::filesystem::path(options.work) / "meta.json");
  if (!meta.is_object()) throw std::runtime_error("run CanMehHarness init first");
  if (meta.value("repeat", 1) > 20 || meta.value("repeat", 1) < 8)
    throw std::runtime_error("this rule requires init --repeat between 8 and 20 (use 10)");
  if (meta.value("repetitions", 9) < 3) throw std::runtime_error("at least 3 repetitions required");
  if (std::abs(meta.value("null_ratio", 0.0) - 1) >= 0.02 ||
      meta.value("reference_cv", 1.0) >= 0.25)
    throw std::runtime_error("the work directory failed the null-run checks; use a quieter node");
  auto initPath = initResult;
  if (initPath.empty() && std::filesystem::exists(std::filesystem::path(options.out) / "init.json"))
    initPath = (std::filesystem::path(options.out) / "init.json").string();
  if (!initPath.empty()) {
    const auto initialized = utilities::readJson(initPath);
    if (!initialized.is_object() || !initialized.value("ok", false) || !initialized.value("null_ok", false))
      throw std::runtime_error("the supplied init result does not report null_ok true");
  } else {
    std::cerr << "Check init's null_ok before using this run: meta.json does not record null_spread.\n";
  }
  prepareOutput(options);
  const auto out = std::filesystem::path(options.out);
  std::filesystem::create_directories(out / "patches");
  auto metadata = provenance();
  metadata.update({{"mode", bestOfN ? "best-of-n" : "evolution"}, {"work", options.work},
      {"seed", options.seed}, {"generations", options.generations}, {"population", options.population},
      {"max_lines", options.maxLines}, {"sample_seconds", options.sampleSeconds},
      {"uniform_share", options.uniformShare}, {"best_gain_share", bestOfN ? 0.0 : 0.25},
      {"prompt_hints", options.promptHints}, {"region_files", options.regionFileNames},
      {"min_effect", minEffect}, {"call_budget", callBudget}, {"meta", meta},
      {"llm", {{"base_url", config.baseUrl}, {"model_requested", config.model},
          {"temperature", config.temperature}, {"max_tokens", config.maxTokens},
          {"retries", config.retries}, {"timeout_seconds", config.timeoutSeconds}}}});
  saveJson(out / "run.json", metadata);
  saveJson(out / "meta.json", meta);
  if (!initPath.empty()) saveJson(out / "null_run.json", utilities::readJson(initPath));
  utilities::runProcess({meta.value("cxx", std::string{"c++"}), "--version"},
                         out / "compiler-version.txt", out / "compiler-version.err", 15);
  utilities::runProcess({"git", "rev-parse", "HEAD"}, out / "revision.txt", out / "git.err", 15);
  utilities::runProcess({"git", "diff", "--", "src/Search.cpp"}, out / "implementation.diff",
                         out / "git-diff.err", 15);
  const auto start = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
  auto regions = loadRegions(options);
  profileRegions(options, meta, &regions);  // Completes before ANY measurement.
  auto results = std::ofstream(out / "results.jsonl");
  auto credits = std::ofstream(out / "credits.csv");
  results.exceptions(std::ios::failbit | std::ios::badbit);
  credits.exceptions(std::ios::failbit | std::ios::badbit);
  credits << "generation,calls,prompt_tokens,completion_tokens,total_tokens,"
             "best_confirmed_speedup,best_retained_speedup,elapsed_s\n" << std::flush;
  auto rng = std::mt19937(options.seed);
  auto uniform = std::uniform_real_distribution<double>(0, 1);
  auto baseline = EvaluatedPatch{};
  baseline.score = 1;
  baseline.valid = true;
  auto population = std::vector<EvaluatedPatch>{baseline};
  auto bestConfirmed = baseline;
  auto bestRetained = baseline;
  auto statistics = std::map<int, RegionStats>{};
  auto failure = std::string{};
  auto calls = 0LL;
  auto promptTokens = 0LL;
  auto completionTokens = 0LL;
  auto totalTokens = 0LL;
  auto completed = 0;
  std::cout << "Null ratio=" << meta.value("null_ratio", 1.0)
            << " p=" << meta.value("null_p", 1.0) << "; baseline ms="
            << meta.at("baseline_ms") << "; regions=" << regions.size() << '\n' << std::flush;

  for (int generation = 1; generation <= options.generations; ++generation) {
    if (callBudget > 0 && calls >= callBudget) break;
    auto parent = baseline;
    if (!bestOfN) {
      auto draw = std::uniform_int_distribution<std::size_t>(0, population.size() - 1);
      const auto a = population[draw(rng)];
      const auto b = population[draw(rng)];
      parent = a.score <= b.score ? a : b;
    }
    auto gains = std::vector<double>{};
    auto samples = std::vector<double>{};
    for (const auto& region : regions) {
      gains.push_back(bestOfN ? 0.0 : statistics[region.id].bestGain);
      samples.push_back(static_cast<double>(region.samples));
    }
    auto chosen = std::size_t{0};
    auto selection = std::string{"uniform"};
    if (!bestOfN && std::accumulate(gains.begin(), gains.end(), 0.0) > 0 && uniform(rng) < 0.25) {
      chosen = std::discrete_distribution<std::size_t>(gains.begin(), gains.end())(rng);
      selection = "best_gain";
    } else if (std::accumulate(samples.begin(), samples.end(), 0.0) > 0 && uniform(rng) >= options.uniformShare) {
      chosen = std::discrete_distribution<std::size_t>(samples.begin(), samples.end())(rng);
      selection = "samples";
    } else {
      chosen = std::uniform_int_distribution<std::size_t>(0, regions.size() - 1)(rng);
    }
    const auto& region = regions[chosen];
    auto& stats = statistics[region.id];
    ++stats.attempts;
    const auto stem = out / "patches" / ("gen_" + std::to_string(generation));
    const auto patchPath = stem.string() + ".patch";
    auto record = json{{"generation", generation}, {"id", generation}, {"parent", parent.id},
        {"region", region.id}, {"file", region.file}, {"label", region.label},
        {"ok", false}, {"stage", ""}, {"note", ""}, {"patch", patchPath},
        {"parent_ratio", parent.score}, {"selection", selection}, {"min_effect", minEffect},
        {"significant", false}, {"significant_at_min_effect", false},
        {"improved_on_parent", false}, {"confirmed_p", -1}, {"retained", false}};
    auto candidate = EvaluatedPatch{};
    candidate.id = generation;
    candidate.generation = generation;
    candidate.parent = parent.id;
    candidate.region = region.id;
    candidate.patchPath = patchPath;
    auto attempt = [&] {
      auto args = std::vector<std::string>{"show", "--region", std::to_string(region.id)};
      if (!parent.patchPath.empty()) args.insert(args.end(), {"--patch", parent.patchPath});
      const auto shown = runHarnessCommand(options, args);
      if (!measuredSuccessfully(shown)) {
        record["stage"] = shown.value("stage", std::string{"region"});
        record["note"] = shown.value("error", shown.dump());
        return;
      }
      const auto system = systemPrompt();
      const auto user = userPrompt(region, shown.at("text").get<std::string>(),
          parent.score, bestOfN ? 1.0 : bestRetained.score, meta.at("baseline_ms").get<double>(),
          bestOfN ? std::string{} : failure, options.promptHints);
      saveJson(stem.string() + ".prompt.json", {{"system", system}, {"user", user}});
      auto reply = SearchModelReply{};
      auto code = std::string{};
      auto includes = std::string{};
      // Count every HTTP request, including retries for empty or unparseable
      // answers. Retain the same prompt and global call cap in both search modes.
      const auto callsBefore = calls;
      for (int retry = 0; retry < config.retries; ++retry) {
        if (callBudget > 0 && calls >= callBudget) break;
        ++calls;
        reply = requestSearchReply(config, system, user);
        promptTokens += reply.promptTokens;
        completionTokens += reply.completionTokens;
        totalTokens += reply.totalTokens;
        splitReply(reply.content, &code, &includes);
        if (reply.error.empty() && code.empty()) reply.error = "reply contains no complete function";
        const auto rawPath = stem.string() + ".call_" + std::to_string(retry + 1) + ".response.txt";
        writeFile(rawPath, reply.raw);
        saveJson(stem.string() + ".call_" + std::to_string(retry + 1) + ".json",
            {{"content", reply.content}, {"error", reply.error},
             {"raw_response_file", rawPath}, {"http_status", reply.httpStatus},
             {"finish_reason", reply.finishReason}, {"content_source", reply.contentSource},
             {"prompt_tokens", reply.promptTokens}, {"completion_tokens", reply.completionTokens},
             {"total_tokens", reply.totalTokens}});
        if (reply.error.empty()) break;
        if (reply.error.rfind("http 4", 0) == 0 && reply.error.rfind("http 429", 0) != 0) break;
        if (retry + 1 < config.retries && (callBudget == 0 || calls < callBudget))
          usleep(static_cast<unsigned int>(2 * (retry + 1) * 1000000));
      }
      record["calls"] = calls - callsBefore;
      record["llm_finish_reason"] = reply.finishReason;
      record["llm_content_source"] = reply.contentSource;
      writeFile(stem.string() + ".reply.txt", reply.content);
      if (!reply.error.empty() || code.empty()) {
        record["stage"] = "llm";
        record["error"] = reply.error.empty() ? "reply contains no complete function" : reply.error;
        record["note"] = record["error"];
        return;
      }
      writeFile(stem.string() + ".repl", code);
      writeFile(stem.string() + ".includes", includes);
      args = {"splice", "--region", std::to_string(region.id), "--replacement", stem.string() + ".repl",
               "--includes", stem.string() + ".includes", "--out", patchPath};
      if (!parent.patchPath.empty()) args.insert(args.end(), {"--patch", parent.patchPath});
      const auto spliced = runHarnessCommand(options, args);
      if (!measuredSuccessfully(spliced) || !std::filesystem::exists(patchPath)) {
        record["stage"] = spliced.value("stage", std::string{"patch"});
        if (record["stage"] == "") record["stage"] = "patch";
        record["note"] = spliced.value("error", std::string{"splice produced no patch file"});
        return;
      }
      args = {"measure-patch", "--patch", patchPath, "--no-decision"};
      const auto firstRaw = runHarnessCommand(options, args);
      const auto first = judgeReply(firstRaw, minEffect);
      record["first"] = first;
      record["first_archive"] = firstRaw.at("archive");
      if (!first.value("ok", false)) {
        record["stage"] = first.value("stage", std::string{"run"});
        record["note"] = first.value("error", first.dump());
        return;
      }
      auto kept = first;
      auto significant = false;
      auto accepted = false;
      if (first.value("significant", false) || first.value("unstable", false)) {
        const auto secondRaw = runHarnessCommand(options, args);
        const auto second = judgeReply(secondRaw, minEffect);
        record["confirmation"] = second;
        record["confirmation_archive"] = secondRaw.at("archive");
        if (!second.value("ok", false)) {
          // A candidate which subsequently fails an oracle is not safe to use
          // as a parent, even if its first measurement succeeded.
          record["stage"] = second.value("stage", std::string{"run"});
          record["note"] = "confirmation failed: " + second.value("error", second.dump());
          return;
        }
        record["confirmed_p"] = second.at("p_value");
        if (second.at("ratio").get<double>() > first.at("ratio").get<double>()) kept = second;
        significant = first.value("significant", false) && second.value("significant", false);
        accepted = candidateIsFaster(firstRaw, minEffect) && candidateIsFaster(secondRaw, minEffect);
      }
      record.update(kept);
      record["ok"] = true;
      record["stage"] = "";
      record["significant"] = significant;
      record["significant_at_min_effect"] = accepted;
      record["accepted"] = accepted;
      record["unstable"] = first.value("unstable", false) ||
          (record.contains("confirmation") && record.at("confirmation").value("unstable", false));
      candidate.valid = true;
      candidate.score = kept.at("ratio").get<double>();
      candidate.significant = significant;
      candidate.significantAtMinEffect = accepted;
      const auto improved = accepted && candidate.score < parent.score;
      record["improved_on_parent"] = improved;
      if (improved) {
        ++stats.accepted;
        stats.bestGain = std::max(stats.bestGain, parent.score - candidate.score);
      }
    };
    attempt();
    if (candidate.valid) {
      if (!bestOfN) {
        population.push_back(candidate);  // Measured, not necessarily accepted.
        std::stable_sort(population.begin(), population.end(), [](const EvaluatedPatch& a, const EvaluatedPatch& b) {
          return a.score < b.score;
        });
        if (population.size() > static_cast<std::size_t>(options.population)) population.pop_back();
        record["retained"] = std::any_of(population.begin(), population.end(), [&](const EvaluatedPatch& p) {
          return p.id == candidate.id;
        });
      } else {
        record["retained"] = candidate.score < bestRetained.score;
      }
      if (candidate.score < bestRetained.score) bestRetained = candidate;
      if (candidate.significantAtMinEffect && candidate.score < bestConfirmed.score) bestConfirmed = candidate;
      failure.clear();
    } else {
      failure = region.file + " / " + region.label + ": " + record.value("stage", std::string{}) +
                "\n" + record.value("note", std::string{});
    }
    completed = generation;
    results << record.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
    credits << generation << ',' << calls << ',' << promptTokens << ',' << completionTokens << ','
            << totalTokens << ',' << std::setprecision(12) << 1 / bestConfirmed.score << ','
            << 1 / bestRetained.score << ',' << elapsed() << '\n' << std::flush;
    auto tried = json::array();
    for (const auto& r : regions) {
      const auto& s = statistics[r.id];
      if (s.attempts > 0)
        tried.push_back({{"region", r.id}, {"file", r.file}, {"label", r.label},
            {"attempts", s.attempts}, {"accepted", s.accepted}, {"best_gain", s.bestGain}, {"samples", r.samples}});
    }
    saveJson(out / "regions_tried.json", tried);
    std::cout << "generation " << generation << ": " << region.file << " / " << region.label
              << "; stage=" << record.at("stage") << "; best confirmed=" << 1 / bestConfirmed.score
              << "; calls=" << calls << "; tokens=" << totalTokens << '\n' << std::flush;
  }
  writeFile(out / "best.patch", bestConfirmed.patchPath.empty() ? "" : readFile(bestConfirmed.patchPath));
  writeFile(out / "best_retained.patch", bestRetained.patchPath.empty() ? "" : readFile(bestRetained.patchPath));
  if (!std::filesystem::exists(out / "regions_tried.json")) saveJson(out / "regions_tried.json", json::array());
  saveJson(out / "summary.json", {{"completed_generations", completed}, {"calls", calls},
      {"prompt_tokens", promptTokens}, {"completion_tokens", completionTokens}, {"total_tokens", totalTokens},
      {"best_confirmed_id", bestConfirmed.id}, {"best_retained_id", bestRetained.id},
      {"best_confirmed_speedup", 1 / bestConfirmed.score}, {"best_retained_speedup", 1 / bestRetained.score},
      {"elapsed_s", elapsed()}, {"no_confirmed_improvement", bestConfirmed.id == 0}});
  std::cout << "Finished. Results: " << options.out << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto app = CLI::App{"LLM patch search, raw-timing decisions and calibration trials."};
  auto options = Options{};
  auto config = ChatClientConfig{};
  auto apiKeyFile = std::string{};
  auto initResult = std::string{};
  auto judgeFile = std::string{};
  auto calibrationRoot = std::string{};
  auto calibrationPairs = std::string{};
#ifdef CANMEH_DEFAULT_CALIBRATION_ROOT
  calibrationRoot = CANMEH_DEFAULT_CALIBRATION_ROOT;
#else
  calibrationRoot = "build/calibration";
#endif
  auto trials = 0;
  auto repeat = 10;
  auto repetitions = 9;
  auto minEffect = 0.02;
  auto bestOfN = false;
  auto callBudget = 0;
  app.add_option("--work", options.work, "initialized SDK work directory");
  app.add_option("--out", options.out, "fresh directory for all run artifacts")->capture_default_str();
  app.add_option("--harness", options.harnessPath, "CanMehHarness binary")->capture_default_str();
  app.add_option("--decode-for-sampler", options.decodeForSamplerPath, "CanMehDecodeForSampler binary")->capture_default_str();
  app.add_option("--generations", options.generations, "maximum generation count")->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--population", options.population, "maximum retained population")->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--seed", options.seed, "search random seed")->capture_default_str();
  app.add_option("--max-lines", options.maxLines, "region indexing limit, used in every resolving command")->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--region-files", options.regionFileNames, "comma-separated file filters; default all files");
  app.add_option("--sample-seconds", options.sampleSeconds, "profile baseline before search; 0 disables")->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--uniform-share", options.uniformShare, "uniform share of draws outside best-gain branch")->check(CLI::Range(0.0, 1.0))->capture_default_str();
  app.add_flag("--prompt-hints", options.promptHints, "add three optimization hints to the prompt");
  app.add_option("--min-effect", minEffect, "minimum fractional reduction in decode time")->check(CLI::Range(0.0, 0.999999))->capture_default_str();
  app.add_option("--init-result", initResult, "init JSON whose null_ok must be true");
  app.add_flag("--best-of-n", bestOfN, "C3: always rewrite stock, without feedback or adaptive selection");
  app.add_option("--call-budget", callBudget, "cap HTTP requests, including retries; 0 means no additional cap")->check(CLI::NonNegativeNumber);
  app.add_option("--llm-base-url", config.baseUrl, "chat-completions endpoint base URL")->capture_default_str();
  app.add_option("--llm-model", config.model, "requested model id")->capture_default_str();
  app.add_option("--llm-api-key-file", apiKeyFile, "read API key from this file; never logged");
  app.add_option("--llm-temperature", config.temperature, "model sampling temperature")->check(CLI::NonNegativeNumber)->capture_default_str();
  app.add_option("--llm-max-tokens", config.maxTokens, "maximum reply tokens")->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--llm-retries", config.retries, "maximum counted requests per generation")->check(CLI::Range(1, 10))->capture_default_str();
  app.add_option("--llm-timeout", config.timeoutSeconds, "timeout seconds per request")->check(CLI::PositiveNumber)->capture_default_str();
  app.add_option("--judge", judgeFile, "judge one saved measure-pair/measure-patch --no-decision JSON; no model call");
  app.add_option("--calibration-trials", trials, "run this many trials per pair; no model calls")->check(CLI::NonNegativeNumber);
  app.add_option("--calibration-root", calibrationRoot, "directory containing calibration.json and all pairs")->capture_default_str();
  app.add_option("--calibration-pairs", calibrationPairs, "optional comma-separated calibration pair names; default all twelve");
  auto* repeatOption = app.add_option("--repeat", repeat, "processes for calibration trials only")->check(CLI::Range(1, 20))->capture_default_str();
  auto* repetitionsOption = app.add_option("--repetitions", repetitions, "repetitions for calibration trials only")->check(CLI::Range(3, 100000))->capture_default_str();
  CLI11_PARSE(app, argc, argv);
  try {
    if (!judgeFile.empty()) {
      if (trials > 0) throw std::runtime_error("--judge and --calibration-trials are separate modes");
      const auto raw = utilities::readJson(judgeFile);
      const auto decision = judgeReply(raw, minEffect);
      std::cout << decision.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
      return decision.value("ok", false) ? 0 : 1;
    }
    if (trials > 0) return runCalibration(options, calibrationRoot, trials, repeat, repetitions, minEffect, calibrationPairs);
    if (repeatOption->count() || repetitionsOption->count())
      throw std::runtime_error("search inherits repeat/repetitions from init; these flags are for calibration trials");
    if (options.sampleSeconds > 0 && options.sampleSeconds < 1)
      throw std::runtime_error("--sample-seconds must be 0 or at least 1");
    if (!apiKeyFile.empty()) {
      config.apiKey = readFile(apiKeyFile);
      const auto first = config.apiKey.find_first_not_of(" \t\r\n");
      const auto last = config.apiKey.find_last_not_of(" \t\r\n");
      if (first == std::string::npos) throw std::runtime_error("API key file is empty or unreadable");
      config.apiKey = config.apiKey.substr(first, last - first + 1);
    } else if (config.baseUrl == "https://soclaas-api.comp.nus.edu.sg/v1") {
      throw std::runtime_error("--llm-api-key-file is required for the default endpoint");
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl initialization failed");
    struct CurlCleanup { ~CurlCleanup() { curl_global_cleanup(); } } cleanup;
    return runSearch(options, config, minEffect, bestOfN, callBudget, initResult);
  } catch (const std::exception& error) {
    std::cerr << "CanMehSearch: " << error.what() << '\n';
    return 1;
  }
}
