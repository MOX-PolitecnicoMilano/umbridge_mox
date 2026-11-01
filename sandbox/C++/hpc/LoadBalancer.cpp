/**
 * @file LoadBalancer.cpp
 * @brief Configure and run an UM-Bridge server backed by HPC scheduler jobs.
 *
 * Startup selects a scheduler, discovers model names through a temporary job,
 * and exposes one LoadBalancer proxy per name. See LoadBalancer.md for the
 * complete request lifecycle and the job-script communication contract.
 */
#include "LoadBalancer.hpp"
#include "umbridge.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>


/**
 * @brief Remove URL announcement files left in the communication directory.
 * @param directory Directory to scan; a nonexistent directory is ignored.
 * @throws std::invalid_argument If the path is empty, contains a NUL, or is not a directory.
 * @throws std::filesystem::filesystem_error If directory traversal or removal fails.
 * @note Only regular files matching `url-<digits>.txt` are removed. No check is made
 *       whether another load-balancer process is still using those files.
 */
void clear_url(const std::filesystem::path& directory) {
    load_balancer_detail::require_text(directory.string(), "URL directory");
    if (!std::filesystem::exists(directory)) {
        return;
    }
    if (!std::filesystem::is_directory(directory)) {
        throw std::invalid_argument("The URL path is not a directory: " + directory.string());
    }

    const std::regex url_file_pattern("url-\\d+\\.txt");
    for (auto& file : std::filesystem::directory_iterator(directory)) {
        // Match the complete filename so unrelated files in the directory remain.
        if (std::regex_match(file.path().filename().string(), url_file_pattern)
            && file.is_regular_file()) {
            std::filesystem::remove(file);
        }
    }
}

/**
 * @brief Restart the local HyperQueue server and configure its allocation queue.
 * @details Uses `./hq` and `hq_scripts/allocation_queue.sh` relative to the
 *          process working directory. The shell loop waits for server info to
 *          succeed before invoking the allocation script.
 * @throws std::runtime_error If either executable is unavailable or setup fails.
 * @note The server-stop exit status is ignored because no server may be running.
 *       Other shell command statuses are checked, but starting in the background
 *       does not report the eventual server exit status. The wait has no timeout,
 *       and the `&>` redirection assumes a shell that understands that syntax.
 */
void launch_hq_with_alloc_queue() {
    for (const std::string path : {"./hq", "hq_scripts/allocation_queue.sh"}) {
        load_balancer_detail::validate_readable_file(path);
        if (::access(path.c_str(), X_OK) != 0) {
            throw std::runtime_error("HyperQueue setup file is not executable: " + path);
        }
    }

    // This affects the HQ server selected by the current environment; no server
    // ownership is retained here for automatic shutdown when main finishes.
    std::system("./hq server stop &> /dev/null");

    if (std::system("./hq server start &") != 0) {
        throw std::runtime_error("Failed to start the HyperQueue server.");
    }
    // Wait for the HQ server to start
    if (std::system("until ./hq server info &> /dev/null; do sleep 1; done") != 0) {
        throw std::runtime_error("Failed while waiting for the HyperQueue server.");
    }

    // Create HQ allocation queue
    if (std::system("hq_scripts/allocation_queue.sh") != 0) {
        throw std::runtime_error("Failed to configure the HyperQueue allocation queue.");
    }
}

/**
 * @brief Validate command-line syntax and collect option values.
 * @param args Command-line tokens, normally excluding the executable name.
 * @return Option names and values; the help flag has an empty value.
 * @throws std::invalid_argument If an option is unknown, repeated, malformed,
 * or missing its value. Value options must use the `--name=value` form.
 */
std::map<std::string, std::string> parse_arguments(const std::vector<std::string>& args) {
    std::map<std::string, std::string> options;
    for (const auto& arg : args) {
        std::string name;
        std::string value;
        if (arg == "--help") {
            name = "help";
        } else {
            const auto equals = arg.find('=');
            if (arg.rfind("--", 0) != 0 || equals == std::string::npos) {
                throw std::invalid_argument("Invalid argument '" + arg
                    + "': use --name=value, or --help.");
            }
            name = arg.substr(2, equals - 2);
            if (name != "scheduler" && name != "delay-ms" && name != "port") {
                throw std::invalid_argument("Unknown option: --" + name);
            }
            value = arg.substr(equals + 1);
            if (value.empty()) {
                throw std::invalid_argument("Missing value for --" + name);
            }
        }
        if (!options.emplace(name, value).second) {
            throw std::invalid_argument("Repeated option: --" + name);
        }
    }
    return options;
}

/**
 * @brief Parse a complete decimal integer and enforce an option's allowed range.
 * @throws std::invalid_argument If the value is malformed or outside the range.
 */
int parse_integer_option(const std::string& name, const std::string& value,
                         int minimum, int maximum) {
    // Retain support for an explicit plus sign, but reject whitespace and suffixes.
    const auto first_digit = !value.empty() && (value.front() == '+' || value.front() == '-') ? 1u : 0u;
    if (first_digit == value.size()
        || !std::all_of(value.begin() + first_digit, value.end(),
                        [](char c) { return c >= '0' && c <= '9'; })) {
        throw std::invalid_argument("Invalid integer for --" + name + ": '" + value + "'.");
    }
    int parsed;
    try {
        parsed = std::stoi(value);
    } catch (const std::exception&) {
        throw std::invalid_argument("Value for --" + name + " must be between "
            + std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }
    if (parsed < minimum || parsed > maximum) {
        throw std::invalid_argument("Value for --" + name + " must be between "
            + std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }
    return parsed;
}

/**
 * @brief Print the command synopsis, supported options, and defaults.
 */
void print_help(const std::string& program) {
    std::cout << "Usage: " << program
              << " --scheduler=<hyperqueue|slurm|pbs> [--delay-ms=<milliseconds>] [--port=<port>]\n"
              << "       " << program << " --help\n\n"
              << "Run an UM-Bridge load balancer backed by HPC scheduler jobs.\n\n"
              << "Options:\n"
              << "  --scheduler=<name>  Required scheduler: hyperqueue, slurm, or pbs.\n"
              << "  --delay-ms=<ms>     Delay before each submission, from 0 to "
              << std::numeric_limits<int>::max() << " ms (default: 0).\n"
              << "  --port=<port>       Listening port, from 1 to 65535 (default: 4242).\n"
              << "  --help              Show this help and exit without starting jobs.\n\n"
              << "Value options use --name=value and may be specified only once.\n"
              << "Job scripts and the urls directory are relative to the working directory.\n";
}


/**
 * @brief Assemble scheduler services and serve the discovered UM-Bridge models.
 * @param argc Number of command-line tokens, including the executable name.
 * @param argv Tokens supporting `--scheduler=`, `--delay-ms=`, `--port=`, and `--help`.
 * @return EXIT_SUCCESS for help or a normal server return; EXIT_FAILURE on error.
 * @note Arguments are validated before filesystem cleanup or scheduler setup.
 * Startup failures are reported to standard error. Discovery must finish before
 * the public server starts.
 */
int main(int argc, char* argv[]) {
    try {

        // Process command line args
        const std::vector<std::string> args(argv + 1, argv + argc);
        const auto options = parse_arguments(args);
        if (options.count("help") != 0) {
            print_help(argc > 0 ? argv[0] : "load-balancer");
            return EXIT_SUCCESS;
        }

        // Scheduler used by the load balancer: HyperQueue, SLURM, or the PBS extension.
        const auto scheduler_option = options.find("scheduler");
        // Specifying a scheduler is mandatory since this should be a conscious choice by the user
        if (scheduler_option == options.end()) {
            throw std::invalid_argument("Missing required argument: --scheduler=<hyperqueue|slurm|pbs>");
        }
        const std::string& scheduler = scheduler_option->second;
        if (scheduler != "hyperqueue" && scheduler != "slurm" && scheduler != "pbs") {
            throw std::invalid_argument("Unrecognized value for --scheduler: '" + scheduler
                + "'. Expected hyperqueue, slurm, or pbs.");
        }

        // Each submitter uses a positive delay to serialize sleeps before submission;
        // the scheduler command itself is outside that mutex's scope.
        std::chrono::milliseconds delay = std::chrono::milliseconds::zero();
        if (const auto it = options.find("delay-ms"); it != options.end()) {
            delay = std::chrono::milliseconds(parse_integer_option(
                "delay-ms", it->second, 0, std::numeric_limits<int>::max()));
        }

        // Load balancer port
        int port = 4242;
        if (const auto it = options.find("port"); it != options.end()) {
            port = parse_integer_option("port", it->second, 1, 65535);
        } else {
            std::cout << "Argument --port not set! Using port 4242 as default." << std::endl;
        }

        // All script and communication paths are relative to the working directory,
        // not to the source file or executable. Validate arguments before cleanup.
        const std::string url_directory = "urls";
        clear_url(url_directory);

        // Assemble job manager
        std::unique_ptr<JobSubmitter> job_submitter;
        std::filesystem::path script_dir;
        if (scheduler == "hyperqueue") {
            launch_hq_with_alloc_queue();
            job_submitter = std::make_unique<HyperQueueSubmitter>(delay);
            script_dir = "hq_scripts";
        } else if (scheduler == "slurm") {
            job_submitter = std::make_unique<SlurmSubmitter>(delay);
            script_dir = "slurm_scripts";
        } else if (scheduler == "pbs") {
            job_submitter = std::make_unique<PBSSubmitter>(delay);
            script_dir = "pbs_scripts";
        }

        // Jobs announce their HTTP endpoint through a shared filesystem. The factory
        // creates "urls"; each communicator polls its own job-ID file every 500 ms.
        std::unique_ptr<JobCommunicatorFactory> comm_factory 
            = std::make_unique<FilesystemCommunicatorFactory>(url_directory, std::chrono::milliseconds(500));

        // Prefer job_<model_name>.sh when present; otherwise use job.sh. Discovery
        // always uses job.sh because model names are not known yet.
        JobScriptLocator locator {script_dir, "job.sh", "job_", ".sh"};

        // Move exclusive ownership of scheduler/communication services into a single
        // manager, then share that manager among all exposed model proxies.
        std::shared_ptr<JobManager> job_manager = std::make_shared<CommandJobManager>(
            std::move(job_submitter), std::move(comm_factory), locator);


        // Start a temporary job, query /Info, and cancel that job when discovery
        // returns. The names are a startup snapshot, not continuously refreshed.
        std::vector<std::string> model_names = job_manager->getModelNames();

        // Inform the user about the available models and the job scripts that will be used.
        locator.printModelJobScripts(model_names);    

        // Each proxy stores a name and the shared manager; it does not retain a
        // running backend server between forwarded method calls.
        std::vector<LoadBalancer> LB_vector;
        for (auto model_name : model_names) {
            LB_vector.emplace_back(model_name, job_manager);
        }

        // Build non-owning pointers only after the vector has finished growing:
        // subsequent reallocation would invalidate them. LB_vector owns the proxies
        // and remains alive for the duration of the blocking serveModels call.
        std::vector<umbridge::Model *> LB_ptr_vector(LB_vector.size());
        std::transform(LB_vector.begin(), LB_vector.end(), LB_ptr_vector.begin(),
                       [](LoadBalancer& obj) { return &obj; });

        // Listen on all IPv4 interfaces. enable_parallel=true permits concurrent
        // model calls; error_checks=false skips optional frontend checks that would
        // otherwise launch additional jobs for capability and dimension queries.
        // The called UM-Bridge server still performs some unconditional checks.
        umbridge::serveModels(LB_ptr_vector, "0.0.0.0", port, true, false);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "LoadBalancer: " << error.what() << '\n'
                  << "Use --help for command syntax and options." << std::endl;
        return EXIT_FAILURE;
    }
}
