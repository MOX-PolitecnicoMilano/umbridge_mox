/**
 * @file http-client.cpp
 * @brief Demonstrate evaluation and an optional Jacobian action through UM-Bridge.
 *
 * Connects to the supplied server, lists its models, and accesses "forward".
 * The example uses input {{100.0, 18.0}}, then repeats the evaluation with
 * level=1 and vtk_output=true. Model-specific parameters are intentionally fixed.
 */
// Enable HTTPS in the bundled HTTP client; link with OpenSSL when building.
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "umbridge.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * @brief Format a numeric vector as a comma-separated string without brackets.
 * @tparam T Numeric type accepted by std::to_string.
 * @param vector Values to format in their original order.
 * @return Formatted values, or an empty string for an empty vector.
 */
template<typename T>
std::string to_string(const std::vector<T>& vector) {
  std::string s;
  for (const auto& entry : vector)
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
void validate_address(const std::string& address) {
  if (address.empty() ||
      std::any_of(address.begin(), address.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
    throw std::invalid_argument("Server address must be nonempty and contain no whitespace or control characters.");
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
         (scope + 1 == address.size() || address.find_first_of("/?#@\\%", scope + 1) != std::string::npos))) {
      throw std::invalid_argument("Invalid IPv6 server address.");
    }
    return;
  }

  // UM-Bridge supplies its own endpoint paths, so the address is an origin:
  // no credentials, path, query, or fragment may be appended to it.
  static const std::regex origin(R"((?:https?://)?(?:\[([0-9A-Fa-f:.%_-]+)\]|([^:/?#@\[\]\\]+))(?::([0-9]+))?)");
  std::smatch match;
  if (!std::regex_match(address, match, origin)) {
    throw std::invalid_argument("Expected an HTTP/HTTPS server address or host[:port], without a path, query, or fragment.");
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
    } catch (const std::out_of_range&) {
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
 * @param sizes Expected number of scalars in each block for the same configuration.
 * @param description Operation and data name used in diagnostics.
 * @throws std::runtime_error If the block count or any block length is incorrect.
 * @note Empty blocks are valid when their declared size is zero.
 */
void validate_blocks(const std::vector<std::vector<double>>& blocks,
                     const std::vector<std::size_t>& sizes,
                     const std::string& description) {
  if (blocks.size() != sizes.size()) {
    throw std::runtime_error(description + ": expected " + std::to_string(sizes.size())
                             + " blocks, got " + std::to_string(blocks.size()) + ".");
  }
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    if (blocks[i].size() != sizes[i]) {
      throw std::runtime_error(description + ": block " + std::to_string(i)
                               + " expects " + std::to_string(sizes[i])
                               + " values, got " + std::to_string(blocks[i].size()) + ".");
    }
  }
}

/**
 * @brief Print command syntax, the help option, and the fixed example behavior.
 * @param program Executable name displayed in the synopsis and example.
 */
void print_help(const std::string& program) {
  std::cout << "Usage: " << program << " <address>\n"
            << "       " << program << " --help\n\n"
            << "Arguments:\n"
            << "  <address>  UM-Bridge server: http://host[:port], https://host[:port],\n"
            << "             or host[:port] (HTTP). Ports must be 1 through 65535.\n"
            << "             Default ports are 80 for HTTP and 443 for HTTPS.\n"
            << "             Use brackets around IPv6 addresses when specifying a port.\n"
            << "             Do not include credentials, a path, a query, or a fragment.\n\n"
            << "Options:\n"
            << "  --help     Show this help and exit without connecting to a server.\n\n"
            << "Lists supported models and evaluates 'forward' with input [100, 18].\n"
            << "Repeats evaluation with level=1 and vtk_output=true. If supported,\n"
            << "applies the Jacobian to [1, 4] using input and output block zero.\n\n"
            << "Example: " << program << " http://localhost:4242\n";
}

/**
 * @brief Run the fixed UM-Bridge client example, or display help.
 * @param argc Number of command-line tokens, including the executable name.
 * @param argv Executable name followed by one server address or --help.
 * @return EXIT_SUCCESS after help or successful model operations; EXIT_FAILURE
 * for invalid arguments, incompatible model metadata, or communication errors.
 * @note Exceptions are reported to standard error. Arguments are checked before
 * connecting; model dimensions are checked using each evaluation's configuration.
 */
int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      throw std::invalid_argument("Expected exactly one server address, or --help.");
    }
    const std::string host = argv[1];
    if (host == "--help") {
      print_help(argv[0]);
      return EXIT_SUCCESS;
    }
    if (!host.empty() && host.front() == '-') {
      throw std::invalid_argument("Unknown option: " + host);
    }
    validate_address(host);
    std::cout << "Connecting to host " << host << std::endl;

    // Discovery is kept separate so the complete model list is still printed.
    const std::vector<std::string> models = umbridge::SupportedModels(host);
    std::cout << "Supported models: " << std::endl;
    for (const auto& model : models) {
      std::cout << "  " << model << std::endl;
    }

    // HTTPModel verifies availability and caches the model's capabilities.
    umbridge::HTTPModel client(host, "forward");
    if (!client.SupportsEvaluate()) {
      throw std::runtime_error("Model 'forward' does not support evaluation.");
    }

    // Reuse the default metadata for validation and the optional Jacobian call.
    const auto input_sizes = client.GetInputSizes();
    const auto output_sizes = client.GetOutputSizes();
    std::cout << to_string(input_sizes) << std::endl;
    std::cout << to_string(output_sizes) << std::endl;

    const std::vector<std::vector<double>> inputs {{100.0, 18.0}};
    validate_blocks(inputs, input_sizes, "Default evaluation input");
    if (output_sizes.empty()) {
      throw std::runtime_error("Model 'forward' must declare an output block for this example.");
    }

    const auto outputs = client.Evaluate(inputs);
    validate_blocks(outputs, output_sizes, "Default evaluation output");
    // The metadata and result checks above guarantee that block zero exists.
    std::cout << "Output: " << to_string(outputs[0]) << std::endl;

    json config;
    config["level"] = 1;
    config["vtk_output"] = true;
    // Configuration can change dimensions; ask for its own metadata rather
    // than assuming the default sizes also describe this second evaluation.
    validate_blocks(inputs, client.GetInputSizes(config), "Configured evaluation input");
    const auto configured_output_sizes = client.GetOutputSizes(config);
    const auto configured_outputs = client.Evaluate(inputs, config);
    validate_blocks(configured_outputs, configured_output_sizes, "Configured evaluation output");

    // This action uses the default configuration, just like the first evaluation.
    // Its two-entry direction matches the input block already validated above.
    if (client.SupportsApplyJacobian()) {
      const std::vector<double> jacobian = client.ApplyJacobian(0, 0, inputs, {1.0, 4.0});
      if (jacobian.size() != output_sizes[0]) {
        throw std::runtime_error("Jacobian action size does not match output block zero.");
      }
      std::cout << "Jacobian action: " << to_string(jacobian) << std::endl;
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "http-client: " << error.what() << '\n'
              << "Use --help for command syntax and options." << std::endl;
    return EXIT_FAILURE;
  }
}
