/**
 * @file http-client.cpp
 * @brief Evaluate independent UM-Bridge sessions concurrently.
 * @details main() launches one asynchronous evaluate_job() per input, collects
 * every result by job index, then prints outputs after all workers finish.
 * Each worker owns an HTTPModel and a SessionCloser; its backend session is
 * released independently of when the main thread consumes the worker's future.
 * See docs/http-client.tex for the architecture and scheduler interaction.
 */
// Enable HTTPS in the bundled HTTP client; link with OpenSSL when building.
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "umbridge.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <vector>

/**
 * @brief Format a numeric vector as a comma-separated string without brackets.
 * @tparam T Numeric type accepted by std::to_string.
 * @param vector Values to format in their original order.
 * @return Formatted values, or an empty string for an empty vector.
 */
template <typename T> std::string to_string(const std::vector<T> &vector) {
  std::string s;
  for (const auto &entry : vector)
    s += (s.empty() ? "" : ",") + std::to_string(entry);
  return s;
}

/**
 * @brief Validate a server address before passing it to the HTTP library.
 * @param address HTTP/HTTPS origin or a host with an optional decimal port.
 * @throws std::invalid_argument If the address is empty, contains unsupported
 * URL components or whitespace, or specifies a port outside 1 through 65535.
 * @note Scheme-less hosts use HTTP. Bare IPv6 addresses retain the library's
 * default-port behavior; bracketed addresses can specify an explicit port.
 * Hostname resolution and connectivity are checked by the HTTP library.
 */
void validate_address(const std::string &address) {
  if (address.empty() ||
      std::any_of(address.begin(), address.end(),
                  [](unsigned char c) { return c <= 32 || c == 127; })) {
    throw std::invalid_argument("Server address must be nonempty and contain "
                                "no whitespace or control characters.");
  }

  // The library also accepts bare IPv6 literals, including scoped addresses.
  // Multiple colons in this form are part of the host, not a port separator.
  if (address.find("://") == std::string::npos && address.front() != '[' &&
      std::count(address.begin(), address.end(), ':') > 1) {
    const auto scope = address.find('%');
    const std::string literal = address.substr(0, scope);
    in6_addr parsed{};
    if (inet_pton(AF_INET6, literal.c_str(), &parsed) != 1 ||
        (scope != std::string::npos &&
         (scope + 1 == address.size() ||
          address.find_first_of("/?#@\\%", scope + 1) != std::string::npos))) {
      throw std::invalid_argument("Invalid IPv6 server address.");
    }
    return;
  }

  // UM-Bridge supplies its own endpoint paths, so the address is an origin:
  // no credentials, path, query, or fragment may be appended to it.
  static const std::regex origin(
      R"((?:https?://)?(?:\[([0-9A-Fa-f:.%_-]+)\]|([^:/?#@\[\]\\]+))(?::([0-9]+))?)");
  std::smatch match;
  if (!std::regex_match(address, match, origin)) {
    throw std::invalid_argument(
        "Expected an HTTP/HTTPS server address or host[:port], without a path, "
        "query, or fragment.");
  }
  if (match[1].matched) {
    in6_addr parsed{};
    const std::string literal = match[1].str();
    if (inet_pton(AF_INET6, literal.c_str(), &parsed) != 1) {
      throw std::invalid_argument("Invalid bracketed IPv6 server address.");
    }
  }
  if (match[3].matched) {
    int port = 0;
    try {
      port = std::stoi(match[3].str());
    } catch (const std::out_of_range &) {
      throw std::invalid_argument("Server port must be between 1 and 65535.");
    }
    if (port < 1 || port > 65535) {
      throw std::invalid_argument("Server port must be between 1 and 65535.");
    }
  }
}

/**
 * @brief Check block counts and dimensions against model metadata.
 * @param blocks Input or output vectors to inspect without modifying them.
 * @param sizes Expected number of scalars in each block for the same
 * configuration.
 * @param description Operation and data name used in diagnostics.
 * @throws std::runtime_error If the block count or any block length is
 * incorrect.
 * @note Empty blocks are valid when their declared size is zero.
 */
void validate_blocks(const std::vector<std::vector<double>> &blocks,
                     const std::vector<std::size_t> &sizes,
                     const std::string &description) {
  if (blocks.size() != sizes.size()) {
    throw std::runtime_error(description + ": expected " +
                             std::to_string(sizes.size()) + " blocks, got " +
                             std::to_string(blocks.size()) + ".");
  }
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    if (blocks[i].size() != sizes[i]) {
      throw std::runtime_error(description + ": block " + std::to_string(i) +
                               " expects " + std::to_string(sizes[i]) +
                               " values, got " +
                               std::to_string(blocks[i].size()) + ".");
    }
  }
}

/**
 * @brief Generate a random identifier for an independent model session.
 * @return A 32-character lowercase hexadecimal string, including leading zeros.
 * @details Four independently drawn 32-bit values provide 128 random bits.
 * Collisions are unlikely but uniqueness is not guaranteed. All generator state
 * is local to this call, so workers do not share a random-number engine.
 * @throws std::exception If the random source or string allocation fails.
 */
std::string create_session_id() {
  std::random_device random;
  std::uniform_int_distribution<std::uint32_t> distribution;
  std::ostringstream session_id;
  session_id << std::hex << std::setfill('0');
  for (int i = 0; i < 4; ++i) {
    session_id << std::setw(8) << distribution(random);
  }
  return session_id.str();
}

/**
 * @brief Scope guard that attempts to close a borrowed HTTPModel's session.
 * @details Declare the guard after the model so reverse destruction order keeps
 * the model alive during cleanup. The guard also runs when evaluation throws.
 * Copying is forbidden to avoid duplicate guards for the same session.
 * @note Move operations are not implicitly generated because the class declares
 * a destructor and copy operations. A defaulted move constructor would leave
 * both guards bound to the same model: destroying the source could close the
 * session while the destination still needs it. A defaulted move assignment
 * would be deleted because a reference member cannot be rebound. Keep this
 * stack-local guard nonmovable; safe ownership transfer would require custom
 * move operations that disarm the source, rather than defaulted operations.
 * @note This guard owns neither the model nor its scheduler job. Closing the
 * session asks the load balancer to release its backend job. Forced process
 * termination does not run this destructor.
 */
class SessionCloser {
public:
  /**
   * @brief Bind cleanup to an existing model without sending a request.
   * @param client Model whose session should be closed when this scope ends.
   * @pre client must outlive this guard.
   */
  explicit SessionCloser(umbridge::HTTPModel &client) : client(client) {}

  /** @brief Disable copying to prevent duplicate session-cleanup owners.
   * @param other Guard that cannot be copied.
   */
  SessionCloser(const SessionCloser &other) = delete;
  /** @brief Disable assignment to keep the original model binding.
   * @param other Guard that cannot be assigned.
   * @return No value; this operation is deleted.
   */
  SessionCloser &operator=(const SessionCloser &other) = delete;

  /**
   * @brief Attempt session cleanup on normal return or exception unwinding.
   * @details Reports standard exceptions to stderr instead of propagating them.
   * After a successful explicit CloseSession(), this call is a no-op in the
   * local UM-Bridge implementation. If explicit closure failed, it retries.
   */
  ~SessionCloser() {
    try {
      client.CloseSession();
    } catch (const std::exception &error) {
      std::osyncstream(std::cerr) << "http-client: failed to close model session: "
                << error.what() << std::endl;
    }
  }

private:
  umbridge::HTTPModel &client; ///< Borrowed model; must outlive the guard.
};

/// @brief Output blocks; each inner vector contains one model output block.
using ModelOutput = std::vector<std::vector<double>>;
/// @brief Input blocks; each inner vector contains one model input block.
using ModelInput = std::vector<std::vector<double>>;

/**
 * @brief Evaluate one input in an independently owned model session.
 * @param host Validated server origin, normally pointing to the load balancer.
 * @param model_name Name of the model to evaluate, currently "forward".
 * @param inputs Input blocks owned by this worker; each inner vector contains one model input block.
 * @param i Zero-based job index used only for progress diagnostics.
 * @return Validated output blocks after successful explicit session closure.
 * @throws std::runtime_error If evaluation is unsupported, metadata or block
 * dimensions are incompatible, or a model operation or session closure fails.
 * @throws std::exception If session-ID creation or model construction fails.
 * @note Prints progress messages, but leaves numerical result printing to main().
 * Exceptions are transported to main() through the asynchronous task's future.
 */
ModelOutput evaluate_job(const std::string &host, const std::string &model_name,
                         ModelInput const &inputs, std::size_t i) {
  // Workers can log concurrently. osyncstream emits each message as one unit.
  std::osyncstream(std::cout) << "Creating client " << i << std::endl;
  // Construct inside the task: no HTTP client is shared or moved in a growing
  // vector. The unique session ID keeps this worker's backend independent.
  umbridge::HTTPModel client(host, model_name, false, httplib::Headers(),
                             create_session_id());
  // Reverse destruction order guarantees the guard runs while client is alive.
  SessionCloser session_closer(client);
  std::osyncstream(std::cout) << "Created client " << i << std::endl;
  if (!client.SupportsEvaluate()) {
    throw std::runtime_error("Model " + model_name + " does not support evaluation.");
  }

  // Query dimensions for the same default configuration used by Evaluate().
  // Reusing this client/session makes all requests target the same backend.
  const auto input_sizes = client.GetInputSizes();
  const auto output_sizes = client.GetOutputSizes();
  if (output_sizes.empty()) {
    throw std::runtime_error("Model 'forward' must declare an output block.");
  }
  validate_blocks(inputs, input_sizes, "Default evaluation input");
  auto outputs = client.Evaluate(inputs);
  validate_blocks(outputs, output_sizes, "Default evaluation output");
  // Release the PBS slot in this worker, before waiting for any other job.
  client.CloseSession();
  return outputs;
}

/**
 * @brief Print command syntax, defaults, and concurrent evaluation behavior.
 * @param program Executable name to include in the usage and example commands.
 */
void print_help(const std::string &program) {
  std::cout << "Usage: " << program << " <address> [num_jobs]\n"
            << "       " << program << " --help\n\n"
            << "Evaluate 'forward' in independent concurrent sessions.\n"
            << "  <address>  HTTP/HTTPS origin, or host[:port], without a path.\n"
            << "  num_jobs   Positive number of jobs (default: 5).\n\n"
            << "Job i uses input [100+i, 18]. Outputs are collected and printed\n"
            << "in job order after all jobs finish. LoadBalancer's --max-pbs-jobs limits\n"
            << "the number of outstanding backend PBS jobs.\n\n"
            << "Example: " << program << " http://localhost:4242 50\n";
}

/**
 * @brief Launch independent evaluations, collect their results, and print them.
 * @param argc Number of command-line tokens, including the executable name.
 * @param argv Server origin and optional positive job count, or --help.
 * @return EXIT_SUCCESS for help or when every job succeeds; EXIT_FAILURE for
 * argument/launch errors or any failed job.
 * @details All tasks are launched before result collection begins. Every future
 * is observed even after a job fails. Successful numerical results are printed
 * in job order only after every worker has finished. Progress and error messages
 * may appear earlier. Job i receives input {{100+i, 18}}.
 * @note Exceptions from argument parsing and task launch are reported here;
 * worker exceptions are handled separately for each future. No exception is
 * intentionally propagated to the caller.
 */
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      print_help(argv[0]);
      return EXIT_SUCCESS;
    }
    if (argc != 2 && argc != 3) {
      throw std::invalid_argument("Expected a server address and optional num_jobs.");
    }
    const std::string host = argv[1];
    if (!host.empty() && host.front() == '-') {
      throw std::invalid_argument("Unknown option: " + host);
    }
    validate_address(host);
    int num_jobs = 5;
    if (argc == 3) {
      const std::string count = argv[2];
      if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("num_jobs must be a positive integer.");
      }
      num_jobs = std::stoi(count);
      if (num_jobs < 1) {
        throw std::invalid_argument("num_jobs must be a positive integer.");
      }
    }
    std::cout << "Connecting to host " << host << std::endl;

    const std::string model_name = "forward";
    std::vector<std::future<ModelOutput>> jobs;
    jobs.reserve(num_jobs);
    // launch::async requires concurrent execution rather than deferred calls
    // from get(). Arguments are copied into each task, so the loop's inputs can
    // go out of scope safely. Futures store results/exceptions, not HTTPModels.
    for (int i = 0; i < num_jobs; ++i) {
      ModelInput inputs = {{100.0 + static_cast<double>(i), 18.0}};
      jobs.emplace_back(std::async(std::launch::async, evaluate_job,
                                   host, model_name, inputs, i));
    }

    // Keep results indexed by job, even though jobs finish out of order.
    // Observe every future so one failure does not hide other job failures.
    // Waiting for job 0 first does not serialize workers: all are already running
    // and each closes its own session without waiting for this collection loop.
    std::vector<ModelOutput> outputs(num_jobs);
    bool failed = false;
    for (int i = 0; i < num_jobs; ++i) {
      try {
        // get() waits if needed, transfers the result, or rethrows the worker's
        // exception. Each future is consumed exactly once by the main thread.
        outputs[i] = jobs[i].get();
      } catch (const std::exception &error) {
        std::osyncstream(std::cerr) << "Job " << i << " failed: "
                                   << error.what() << std::endl;
        failed = true;
      }
    }
    // All futures have been observed: every worker has finished and attempted
    // session cleanup. Successful workers have explicitly closed their sessions.
    // Only main accesses outputs, so this vector needs no mutex. A failed job's
    // entry stays empty; successful jobs must have at least one output block.
    // Print successful results only now, including every output block.
    for (int i = 0; i < num_jobs; ++i) {
      if (outputs[i].empty()) {
        continue; // Failed jobs were reported above and have no result.
      }
      std::cout << "Job " << i << " output: ";
      for (std::size_t block = 0; block < outputs[i].size(); ++block) {
        if (block != 0) {
          std::cout << " | ";
        }
        std::cout << to_string(outputs[i][block]);
      }
      std::cout << '\n';
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::osyncstream(std::cerr) << "http-client: " << error.what() << '\n'
                               << "Use --help for command syntax and options." << std::endl;
    return EXIT_FAILURE;
  }
}
