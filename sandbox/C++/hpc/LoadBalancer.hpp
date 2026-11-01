/**
 * @file LoadBalancer.hpp
 * @brief Scheduler-backed UM-Bridge model proxies and job-lifetime helpers.
 *
 * The layers below submit a model server, discover its URL through a shared
 * filesystem, and forward one model operation while owning the associated job.
 * See LoadBalancer.md for the full startup and request walkthrough.
 */
#pragma once

#include "umbridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <regex>
#include <sstream>
#include <thread>
#include <sys/wait.h>

namespace load_balancer_detail {

inline void require_text(const std::string& value, const std::string& description) {
    if (value.empty() || value.find('\0') != std::string::npos) {
        throw std::invalid_argument(description + " must be nonempty and contain no NUL characters.");
    }
}

inline void validate_polling_cycle(std::chrono::milliseconds polling_cycle) {
    if (polling_cycle <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Polling interval must be positive.");
    }
}

inline void validate_submission_delay(std::chrono::milliseconds submission_delay) {
    if (submission_delay < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Submission delay must be nonnegative.");
    }
}

inline void validate_readable_file(const std::filesystem::path& path) {
    require_text(path.string(), "File path");
    if (!std::filesystem::is_regular_file(path) || !std::ifstream(path).is_open()) {
        throw std::runtime_error("Expected a readable regular file: '" + path.string() + "'.");
    }
}

inline void validate_directory(const std::filesystem::path& path) {
    require_text(path.string(), "Directory path");
    if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error("Expected a directory: '" + path.string() + "'.");
    }
}

inline void validate_model_name(const std::string& name) {
    require_text(name, "Model name");
    if (name.find_first_of("/\\\r\n") != std::string::npos) {
        throw std::invalid_argument("Model name must not contain path separators or line breaks.");
    }
}

inline void validate_job_id(const std::string& id) {
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("Expected a numeric scheduler job ID.");
    }
}

// These submitters concatenate raw paths/values into shell fragments. Reject
// characters that need quoting rather than letting the shell reinterpret them.
inline void validate_shell_word(const std::string& value, const std::string& description) {
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; }) ||
        value.find_first_of("'\"\\`$;&|<>(){}[]*?~") != std::string::npos) {
        throw std::invalid_argument(description + " contains characters requiring shell quoting.");
    }
}

inline void validate_submission(const std::string& job_script,
                                const std::map<std::string, std::string>& env,
                                bool quoted_environment, bool comma_separated) {
    validate_readable_file(job_script);
    validate_shell_word(job_script, "Job script path");
    if (job_script.front() == '-' || job_script.front() == '#') {
        throw std::invalid_argument("Job script path must not start with '-' or '#'; use an explicit directory.");
    }
    static const std::regex env_name("[A-Za-z_][A-Za-z0-9_]*");
    for (const auto& [key, value] : env) {
        if (!std::regex_match(key, env_name)) {
            throw std::invalid_argument("Invalid environment variable name: '" + key + "'.");
        }
        if (value.find('\0') != std::string::npos ||
            (comma_separated && value.find(',') != std::string::npos)) {
            throw std::invalid_argument("Environment value for '" + key + "' cannot be represented by this scheduler.");
        }
        if (!quoted_environment) {
            validate_shell_word(value, "Environment value for '" + key + "'");
        }
    }
}

inline void validate_input_index(unsigned int index,
                                 const std::vector<std::vector<double>>& inputs) {
    if (index >= inputs.size()) {
        throw std::invalid_argument("Derivative input-block index is out of range.");
    }
}

inline void validate_direction(unsigned int index,
                               const std::vector<std::vector<double>>& inputs,
                               const std::vector<double>& vec) {
    validate_input_index(index, inputs);
    if (vec.size() != inputs[index].size()) {
        throw std::invalid_argument("Direction vector size must match the selected input block.");
    }
}

} // namespace load_balancer_detail

/**
 * @brief Execute a POSIX shell command and collect its standard output.
 * @param command Complete shell command, including any required quoting.
 * @return Standard output exactly as read, including trailing newlines.
 * @throws std::invalid_argument If the command is empty or contains a NUL.
 * @throws std::runtime_error If opening, reading, closing, or executing fails.
 * @warning Arguments must be trusted: this function does not escape shell syntax.
 * @note Standard error is not captured.
 */
std::string get_command_output(const std::string& command) {
    load_balancer_detail::require_text(command, "Shell command");
    // The deleter closes the pipe and waits on normal return and stack unwinding.
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), &pclose);

    if (!pipe) {
        std::string error_msg = "Failed to run command: " + command + "\n"
                              + "popen failed with error: " + std::strerror(errno) + "\n"; 
        throw std::runtime_error(error_msg);
    }

    // Buffer size can be small and is largely unimportant since most commands we use only return a single line.
    std::array<char, 128> buffer;
    std::string output;
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), pipe.get())) {
        output.append(buffer.data(), count);
    }

    if (std::ferror(pipe.get())) {
        throw std::runtime_error("Failed to read output from command: " + command);
    }
    const int status = pclose(pipe.release());
    if (status == -1) {
        throw std::runtime_error("Failed to close command pipe: " + std::string(std::strerror(errno)));
    }
    if (!WIFEXITED(status)) {
        throw std::runtime_error("Command terminated abnormally: " + command);
    }
    if (WEXITSTATUS(status) != 0) {
        throw std::runtime_error("Command failed with exit status " + std::to_string(WEXITSTATUS(status)) + ": " + command);
    }

    return output;
}

/**
 * @brief Poll for a filesystem entry to appear.
 * @param file_path Path expected to be created by the submitted job.
 * @param polling_cycle Positive delay between existence checks.
 * @note There is no timeout, scheduler-status check, or file-content check.
 * @throws std::invalid_argument If the path or polling interval is invalid.
 * @throws std::filesystem::filesystem_error If checking the path fails.
 */
void wait_for_file(const std::filesystem::path& file_path, std::chrono::milliseconds polling_cycle) {
    load_balancer_detail::require_text(file_path.string(), "File path");
    load_balancer_detail::validate_polling_cycle(polling_cycle);
    while (!std::filesystem::exists(file_path)) {
        std::this_thread::sleep_for(polling_cycle);
    }
}

/**
 * @brief Read the first line of a file, excluding its newline.
 * @param file_path File containing a model server URL.
 * @return Nonempty first line.
 * @throws std::runtime_error If the file is not readable and regular or has no first line.
 * @note The line is not validated as a URL and later lines are ignored.
 */
std::string read_line_from_file(const std::filesystem::path& file_path) {
    load_balancer_detail::validate_readable_file(file_path);
    std::ifstream file(file_path);

    if (!file.is_open()) {
        std::string error_msg = "Unable to open file: '" + file_path.string() + "'\n";
        throw std::runtime_error(error_msg);
    }

    std::string line;
    if (!std::getline(file, line) || line.empty()) {
        throw std::runtime_error("Unable to read a nonempty first line from file: '" + file_path.string() + "'.");
    }

    return line;
}

/**
 * @brief Remove one final newline character, if present.
 * @param[in,out] s String to modify; other whitespace is preserved.
 */
void remove_trailing_newline(std::string& s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
}

/**
 * @brief Assemble a shell command from an executable, options, and target.
 * @warning Fragments are concatenated verbatim; callers supply safe shell quoting.
 */
struct Command {
    std::string exec; ///< Executable and optional subcommand, such as "./hq submit".
    std::vector<std::string> options; ///< Shell fragments inserted before the target.
    std::string target; ///< Final shell fragment, normally the job script path.

    /**
     * @brief Append one option fragment without escaping it.
     * @param option Option text, which may itself contain spaces.
     */
    void addOption(const std::string& option) {
        load_balancer_detail::require_text(option, "Command option");
        options.push_back(option);
    }

    /**
     * @brief Join the command fragments with single separating spaces.
     * @return Shell command ready for get_command_output().
     */
    std::string toString() const {
        load_balancer_detail::require_text(exec, "Command executable");
        load_balancer_detail::require_text(target, "Command target");
        std::string result = exec;
        for (const std::string& s : options)
        {
            load_balancer_detail::require_text(s, "Command option");
            result += " " + s;
        }
        result += " " + target;

        return result;
    }
};


/**
 * @brief Own a submitted scheduler job and its cancellation responsibility.
 *
 * Concrete subclasses submit in their constructors and request cancellation in
 * their destructors (resource acquisition is initialization, or RAII). Copying
 * or moving the object is forbidden to avoid multiple owners cancelling one job;
 * transfer a std::unique_ptr<Job> instead.
 */
class Job {
public:
    /**
     * @brief Construct an empty base object.
     */
    Job() = default;
    /**
     * @brief Disable copying of uniquely owned resource state.
     * @param other Object that must not be copied.
     */
    Job(Job& other) = delete;
    /**
     * @brief Disable moving the object; move its owning pointer instead.
     * @param other Object that must not be moved.
     */
    Job(Job&& other) = delete;
    /**
     * @brief Disable copy assignment of resource state.
     * @param other Object that must not be copied.
     */
    Job& operator=(Job& other) = delete;
    /**
     * @brief Disable move assignment; transfer the owning pointer instead.
     * @param other Object that must not be moved.
     */
    Job& operator=(Job&& other) = delete;
    /**
     * @brief Allow derived cleanup through a base-class owning pointer.
     */
    virtual ~Job() = default;

    /**
     * @brief Return the scheduler identifier used for cancellation and URL lookup.
     * @return Identifier retained by the concrete job implementation.
     */
    virtual std::string getJobId() const = 0;
};

/**
 * @brief Submit and own a HyperQueue task using the local ./hq executable.
 * @note The executable path is fixed relative to the current working directory.
 */
class HyperQueueJob : public Job {
public:
    /**
     * @brief Submit a job with ./hq submit and retain its reported identifier.
     * @param options Scheduler-specific command fragments.
     * @param target Path to the job script, already quoted if necessary.
     * @throws std::runtime_error If submission fails or returns no valid job ID.
     * @note Quiet output is used as the ID after removing one trailing newline;
     * the remaining output must consist of decimal digits.
     */
    HyperQueueJob(const std::vector<std::string>& options, const std::string& target) {
        Command command {"./hq submit", options, target};

        // Makes HQ output "<job id>\n"
        command.addOption("--output-mode=quiet");
        id = get_command_output(command.toString());

        remove_trailing_newline(id);
        load_balancer_detail::validate_job_id(id);
    }

    /**
     * @brief Request cancellation with ./hq job cancel; the exit status is ignored.
     */
    ~HyperQueueJob() override {
        std::system(("./hq job cancel " + id).c_str());
    }

    /**
     * @copydoc Job::getJobId
     */
    std::string getJobId() const override {
        return id;
    }
    
private:
    std::string id; ///< Validated numeric scheduler ID.
};

/**
 * @brief Submit a SLURM batch job and request scancel when its owner is destroyed.
 */
class SlurmJob : public Job {
public:
    /**
     * @brief Submit a job with sbatch and retain its reported identifier.
     * @param options Scheduler-specific command fragments.
     * @param target Path to the job script, already quoted if necessary.
     * @throws std::runtime_error If submission fails or returns no valid job ID.
     * @note Only fully matching numeric IDs, optionally followed by a semicolon
     * and cluster name, are accepted. The last matching line wins and the cluster
     * suffix is discarded. If no line matches, construction fails.
     */
    SlurmJob(const std::vector<std::string>& options, const std::string& target) {
        Command command {"sbatch", options, target};

        // Makes SLURM output "<job id>[;<cluster name>]\n"
        command.addOption("--parsable");
        std::string output = get_command_output(command.toString());

	std::regex job_id_regex(R"(^(\d+)(?:;[a-zA-Z0-9_-]+)?$)");
	std::istringstream stream(output);
    	std::string line;

    	while (std::getline(stream, line)) {
		std::smatch match;
		if (std::regex_match(line, match, job_id_regex)) {
			id = match[1];
                }
        }
	remove_trailing_newline(id);
        load_balancer_detail::validate_job_id(id);
    }

    /**
     * @brief Request cancellation with scancel; the exit status is ignored.
     */
    ~SlurmJob() override {
        std::system(("scancel " + id).c_str());
    }

    /**
     * @copydoc Job::getJobId
     */
    std::string getJobId() const override {
        return id;
    }
    
private:
    std::string id; ///< Validated numeric scheduler ID.
};

/**
 * @brief Submit a PBS batch job and request qdel when its owner is destroyed.
 */
class PBSJob : public Job {
public:
    /**
     * @brief Submit a job with qsub and retain its reported identifier.
     * @param options Scheduler-specific command fragments.
     * @param target Path to the job script, already quoted if necessary.
     * @throws std::runtime_error If submission fails or returns no valid job ID.
     * @note Only fully matching numeric IDs, optionally followed by a dot and a
     * server suffix containing letters, digits, underscores, or hyphens, are accepted.
     * The last matching line wins and the suffix is discarded. Other PBS ID formats
     * are not recognized; if no line matches, construction fails.
     */
    PBSJob(const std::vector<std::string>& options, const std::string& target) {
        Command command {"qsub", options, target};

        // PBS commonly reports "<job id>.<server>"; only the numeric part is kept.
        std::string output = get_command_output(command.toString());

	std::regex job_id_regex(R"(^(\d+)(?:\.[a-zA-Z0-9_-]+)?$)");
	std::istringstream stream(output);
    	std::string line;

    	while (std::getline(stream, line)) {
		std::smatch match;
		if (std::regex_match(line, match, job_id_regex)) {
			id = match[1];
                }
        }
	remove_trailing_newline(id);
        load_balancer_detail::validate_job_id(id);
    }

    /**
     * @brief Request cancellation with qdel; the exit status is ignored.
     */
    ~PBSJob() override {
        std::system(("qdel " + id).c_str());
    }

    /**
     * @copydoc Job::getJobId
     */
    std::string getJobId() const override {
        return id;
    }
    
private:
    std::string id; ///< Validated numeric scheduler ID.
};


/**
 * @brief Scheduler-independent factory for jobs and their startup environment.
 */
class JobSubmitter {
public:
    /**
     * @brief Destroy a submitter through the abstract interface.
     */
    virtual ~JobSubmitter() = default;

    /**
     * @brief Submit a script and transfer ownership of its scheduler job.
     * @param job_script Readable regular script path; built-in submitters require a path without shell metacharacters.
     * @param env Environment variables passed to the script.
     * @return Unique owner whose destruction requests job cancellation.
     * @note Implementations may block for a configured submission delay.
     * Built-in submitters reject invalid environment names and values that their
     * command encoding cannot represent without shell expansion or list splitting.
     */
    virtual std::unique_ptr<Job> submit(const std::string& job_script, const std::map<std::string, std::string>& env) = 0;
};

/**
 * @brief Create HyperQueue jobs with scheduler-specific environment options.
 * @note The mutex serializes the optional sleeps only. Submission commands run
 * after the lock is released, so this is not a strict scheduler rate limiter.
 */
class HyperQueueSubmitter : public JobSubmitter {
public:
    /**
     * @brief Store the delay applied before each submission.
     * @param submission_delay Nonnegative sleep duration; zero disables it.
     */
    explicit HyperQueueSubmitter(std::chrono::milliseconds submission_delay) 
    : submission_delay(submission_delay) {
        load_balancer_detail::validate_submission_delay(submission_delay);
    }

    /**
     * @copydoc JobSubmitter::submit
     */
    std::unique_ptr<Job> submit(const std::string& job_script, const std::map<std::string, std::string>& env) override {
        load_balancer_detail::validate_submission(job_script, env, false, false);
        // Serialize the waiting intervals; the lock ends before the command below.
        if (submission_delay > std::chrono::milliseconds::zero()) {
            std::lock_guard lock(submission_mutex);
            std::this_thread::sleep_for(submission_delay);
        }

        // Later sequential submissions receive lower priorities. The atomic load
        // and increment are separate, so concurrent calls can share a priority.
        std::vector<std::string> options = env_to_options(env);
        options.emplace_back("--priority=-" + std::to_string(job_count));

        std::unique_ptr<Job> job = std::make_unique<HyperQueueJob>(options, job_script);
        job_count++;
        return job;
    }
private:
    /**
     * @brief Convert each entry to a separate --env KEY=VALUE shell fragment.
     * @param env Variables to export to the job.
     * @return One unquoted option fragment per environment variable.
     */
    std::vector<std::string> env_to_options(const std::map<std::string, std::string>& env) const {
        std::vector<std::string> options;
        options.reserve(env.size());

        for (const auto& [key, val] : env) {
            options.push_back("--env " + key + "=" + val);
        }
        return options;
    }

    std::chrono::milliseconds submission_delay = std::chrono::milliseconds::zero(); ///< Optional pre-submission pause.
    std::mutex submission_mutex; ///< Protects only the sleep interval.

    std::atomic<int32_t> job_count = 0; ///< Counter used to assign increasingly negative priorities.
};

/**
 * @brief Create SLURM jobs with scheduler-specific environment options.
 * @note The mutex serializes the optional sleeps only. Submission commands run
 * after the lock is released, so this is not a strict scheduler rate limiter.
 */
class SlurmSubmitter : public JobSubmitter {
public:
    /**
     * @brief Store the delay applied before each submission.
     * @param submission_delay Nonnegative sleep duration; zero disables it.
     */
    SlurmSubmitter(std::chrono::milliseconds submission_delay) 
    : submission_delay(submission_delay) {
        load_balancer_detail::validate_submission_delay(submission_delay);
    }

    /**
     * @copydoc JobSubmitter::submit
     */
    std::unique_ptr<Job> submit(const std::string& job_script, const std::map<std::string, std::string>& env) override {
        load_balancer_detail::validate_submission(job_script, env, false, true);
        // Serialize the waiting intervals; the lock ends before the command below.
        if (submission_delay > std::chrono::milliseconds::zero()) {
            std::lock_guard lock(submission_mutex);
            std::this_thread::sleep_for(submission_delay);
        }

        // Submit job
        std::vector<std::string> options = env_to_options(env);
        std::unique_ptr<Job> job = std::make_unique<SlurmJob>(options, job_script);
        return job;
    }
private:
    /**
     * @brief Combine variables into one --export=ALL,KEY=VALUE,... option.
     * @param env Variables to export to the job.
     * @return One unquoted option that also preserves the submission environment.
     */
    std::vector<std::string> env_to_options(const std::map<std::string, std::string>& env) const {
        // ALL requests inheritance of the submission environment, plus these entries.
        std::string env_option = "--export=ALL";

        for (const auto& [key, val] : env) {
            env_option += "," + key + "=" + val;
        }

        return {env_option};
    }

    std::chrono::milliseconds submission_delay = std::chrono::milliseconds::zero(); ///< Optional pre-submission pause.
    std::mutex submission_mutex; ///< Protects only the sleep interval.
};

/**
 * @brief Create PBS jobs with scheduler-specific environment options.
 * @note The mutex serializes the optional sleeps only. Submission commands run
 * after the lock is released, so this is not a strict scheduler rate limiter.
 */
class PBSSubmitter : public JobSubmitter {
public:
    /**
     * @brief Store the delay applied before each submission.
     * @param submission_delay Nonnegative sleep duration; zero disables it.
     */
    PBSSubmitter(std::chrono::milliseconds submission_delay) 
    : submission_delay(submission_delay) {
        load_balancer_detail::validate_submission_delay(submission_delay);
    }

    /**
     * @copydoc JobSubmitter::submit
     */
    std::unique_ptr<Job> submit(const std::string& job_script, const std::map<std::string, std::string>& env) override {
        load_balancer_detail::validate_submission(job_script, env, true, true);
        // Serialize the waiting intervals; the lock ends before the command below.
        if (submission_delay > std::chrono::milliseconds::zero()) {
            std::lock_guard lock(submission_mutex);
            std::this_thread::sleep_for(submission_delay);
        }

        // Submit job
        std::vector<std::string> options = env_to_options(env);
        std::unique_ptr<Job> job = std::make_unique<PBSJob>(options, job_script);
        return job;
    }
private:
    /**
     * @brief Combine variables into the single shell-quoted argument of qsub -v.
     * @param env Variables to export to the job.
     * @return The -v option and its argument, or no options for an empty map.
     * @note Shell quoting preserves spaces and quotes, but does not escape commas
     * for PBS's own comma-separated variable syntax.
     */
    std::vector<std::string> env_to_options(const std::map<std::string, std::string>& env) const {
        if (env.empty()) {
            return {};
        }

        std::string env_option;
        for (const auto& [key, val] : env) {
            if (!env_option.empty()) {
                env_option += ",";
            }
            env_option += key + "=" + val;
        }

        // Protect the whole comma-separated list from the shell. An embedded
        // quote closes the quoted string, emits an escaped quote, and reopens it.
        std::string quoted_env_option = "'";
        for (char character : env_option) {
            if (character == '\'') {
                quoted_env_option += "'\\''";
            } else {
                quoted_env_option += character;
            }
        }
        quoted_env_option += "'";

        return {"-v", quoted_env_option};
    }

    std::chrono::milliseconds submission_delay = std::chrono::milliseconds::zero(); ///< Optional pre-submission pause.
    std::mutex submission_mutex; ///< Protects only the sleep interval.
};


/**
 * @brief Exchange startup information with one submitted model server.
 *
 * getInitMessage() provides environment variables for the job script;
 * getModelUrl() receives the server address after submission. Instances own
 * communication state and cannot be copied or moved; use std::unique_ptr.
 */
class JobCommunicator {
public:
    /**
     * @brief Construct an empty base object.
     */
    JobCommunicator() = default;
    /**
     * @brief Disable copying of uniquely owned resource state.
     * @param other Object that must not be copied.
     */
    JobCommunicator(JobCommunicator& other) = delete;
    /**
     * @brief Disable moving the object; move its owning pointer instead.
     * @param other Object that must not be moved.
     */
    JobCommunicator(JobCommunicator&& other) = delete;
    /**
     * @brief Disable copy assignment of resource state.
     * @param other Object that must not be copied.
     */
    JobCommunicator& operator=(JobCommunicator& other) = delete;
    /**
     * @brief Disable move assignment; transfer the owning pointer instead.
     * @param other Object that must not be moved.
     */
    JobCommunicator& operator=(JobCommunicator&& other) = delete;
    /**
     * @brief Allow derived cleanup through a base-class owning pointer.
     */
    virtual ~JobCommunicator() = default;

    /**
     * @brief Prepare startup variables that tell a job how to publish its URL.
     * @return Environment entries consumed by the job script.
     */
    virtual std::map<std::string, std::string> getInitMessage() = 0;

    /**
     * @brief Obtain the model server address associated with a submitted job.
     * @param job_id Scheduler identifier returned by Job::getJobId().
     * @return Server URL suitable for constructing an umbridge::HTTPModel.
     */
    virtual std::string getModelUrl(const std::string& job_id) = 0;
};

/**
 * @brief Create independent communication state for each submitted job.
 */
class JobCommunicatorFactory {
public:
    /**
     * @brief Allow destruction through the factory interface.
     */
    virtual ~JobCommunicatorFactory() = default;

    /**
     * @brief Allocate one communicator for a single job startup.
     * @return Exclusive ownership of the new communicator.
     */
    virtual std::unique_ptr<JobCommunicator> create() = 0;
};

/**
 * @brief Receive a server URL through a file on a shared filesystem.
 *
 * The job must publish its address in `file_dir/url-<job_id>.txt`. The script and
 * load balancer must agree on the directory, ID spelling, and file format.
 * The communicator removes its URL file when destroyed; it does not own the job.
 */
class FilesystemCommunicator : public JobCommunicator {
public:
    /**
     * @brief Store the shared directory and polling interval for one job.
     * @param file_dir Directory visible to both the load balancer and job script.
     * @param polling_cycle Positive delay between URL-file existence checks.
     * @throws std::invalid_argument If the path is empty or the polling interval is nonpositive.
     * @throws std::runtime_error If file_dir is not an existing directory.
     */
    FilesystemCommunicator(std::filesystem::path file_dir, std::chrono::milliseconds polling_cycle) 
    : file_dir(std::move(file_dir)), polling_cycle(polling_cycle) {
        load_balancer_detail::validate_polling_cycle(polling_cycle);
        load_balancer_detail::validate_directory(this->file_dir);
    }

    /**
     * @brief Remove the URL file if a lookup has recorded its path.
     * @note Cleanup uses the nonthrowing overload so failures do not terminate unwinding.
     */
    ~FilesystemCommunicator() override {
        if(!file_path.empty()) {
            std::error_code error;
            std::filesystem::remove(file_path, error);
            if (error) {
                std::cerr << "Unable to remove URL file '" << file_path.string()
                          << "': " << error.message() << '\n';
            }
        }
    }

    /**
     * @brief Export the directory where the job must publish its URL.
     * @return One entry named UMBRIDGE_LOADBALANCER_COMM_FILEDIR.
     */
    std::map<std::string, std::string> getInitMessage() override {
        std::map<std::string, std::string> msg {{"UMBRIDGE_LOADBALANCER_COMM_FILEDIR", file_dir.string()}};
        return msg;
    }

    /**
     * @brief Wait for the job's URL file and read its first line.
     * @param job_id Scheduler ID used in the URL filename.
     * @return Nonempty first line without whitespace or control characters;
     * other address validation is left to the HTTP client.
     * @throws std::runtime_error If the ID or announcement file is invalid.
     * @note Waiting has no timeout; existence alone does not prove that writing
     * is complete or that the HTTP server is ready.
     */
    std::string getModelUrl(const std::string& job_id) override {
        load_balancer_detail::validate_job_id(job_id);
        file_path = file_dir / getUrlFileName(job_id);

        std::cout << "Waiting for URL file: " << file_path.string() << std::endl;
        wait_for_file(file_path, polling_cycle);

        // Invalid/empty announcements fail promptly; reading is not retried.
        std::string url = read_line_from_file(file_path);
        if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
            throw std::runtime_error("Model URL must not contain whitespace or control characters: '" + file_path.string() + "'.");
        }
        return url;
    }

private:
    /**
     * @brief Apply the URL-file naming convention shared with the job script.
     * @param job_id Scheduler ID with the same spelling used by the publisher.
     * @return Filename `url-<job_id>.txt`, without its parent directory.
     */
    std::string getUrlFileName(const std::string& job_id) const {
        return "url-" + job_id + ".txt";
    }

    std::filesystem::path file_dir; ///< Shared directory passed to the job script.
    std::filesystem::path file_path; ///< URL file owned once getModelUrl() starts.

    std::chrono::milliseconds polling_cycle; ///< Interval between existence checks.
};

/**
 * @brief Create per-job communicators that share one directory and poll interval.
 */
class FilesystemCommunicatorFactory : public JobCommunicatorFactory {
public:
    /**
     * @brief Initialize the shared URL directory if it does not already exist.
     * @param file_dir Communication directory; parent directories must already exist.
     * @param polling_cycle Positive delay between file-existence checks.
     * @throws std::invalid_argument If the path is empty or the polling interval is nonpositive.
     * @throws std::filesystem::filesystem_error If directory creation fails.
     */
    FilesystemCommunicatorFactory(std::filesystem::path file_dir, std::chrono::milliseconds polling_cycle)
    : file_dir(file_dir), polling_cycle(polling_cycle) {
        load_balancer_detail::require_text(file_dir.string(), "Communication directory");
        load_balancer_detail::validate_polling_cycle(polling_cycle);
        std::filesystem::create_directory(file_dir);
        load_balancer_detail::validate_directory(file_dir);
    }

    /**
     * @copydoc JobCommunicatorFactory::create
     */
    std::unique_ptr<JobCommunicator> create() override {
        return std::make_unique<FilesystemCommunicator>(file_dir, polling_cycle);
    }

private:
    std::filesystem::path file_dir; ///< Directory reused by all created communicators.

    std::chrono::milliseconds polling_cycle; ///< Polling interval copied into each communicator.
};


/**
 * @brief Resolve a model-specific job script with a common default fallback.
 */
struct JobScriptLocator {
    /**
     * @brief Prefer the named model's script, otherwise select the default.
     * @param model_name Name inserted between model_prefix and model_suffix.
     * @return First existing model-specific or default script path.
     * @throws std::runtime_error If neither candidate exists.
     * @note Scripts must be readable regular files; executable permission is not required.
     */
    std::filesystem::path selectJobScript(const std::string& model_name) {
        load_balancer_detail::validate_model_name(model_name);
        validate();
        std::filesystem::path script_default = script_dir / script_default_name;
        std::filesystem::path script_model_specific = script_dir / (model_prefix + model_name + model_suffix);

        // Use model specific job script if available, default otherwise.
        if (std::filesystem::exists(script_model_specific)) {
            load_balancer_detail::validate_readable_file(script_model_specific);
            return script_model_specific;
        } else if (std::filesystem::exists(script_default)) {
            load_balancer_detail::validate_readable_file(script_default);
            return script_default;
        } else {
            std::string error_msg = "Job script not found: Check that file '" + script_default.string() + "' exists.\n";
            throw std::runtime_error(error_msg);
        }
    }

    /**
     * @brief Form the default script path used during model discovery.
     * @return Path to a readable regular default script. Throws if it is unavailable.
     */
    std::filesystem::path getDefaultJobScript() {
        validate();
        const auto script = script_dir / script_default_name;
        load_balancer_detail::validate_readable_file(script);
        return script;
    }

    /**
     * @brief Print models and their selected scripts in alphabetical order.
     * @param model_names Names copied locally so sorting does not affect the caller.
     * @throws std::runtime_error If a model has no available script.
     */
    void printModelJobScripts(std::vector<std::string> model_names) {
        const std::string section_start_delimiter = "==============================MODEL INFO==============================";
        const std::string section_end_delimiter   = "======================================================================";
        
        // Sort the model names in alphabetical order for cleaner output.
        std::sort(model_names.begin(), model_names.end());

        std::cout << section_start_delimiter << std::endl;

        std::cout << "Available models and corresponding job-scripts:\n";
        for (const std::string& model_name : model_names) {
            std::filesystem::path used_job_script = selectJobScript(model_name);
            std::cout << "* Model '" << model_name << "' --> '" << used_job_script.string() << "'" << std::endl;
        }
        std::cout << std::endl;

        std::cout << section_end_delimiter << std::endl;
    }


    std::filesystem::path script_dir; ///< Directory containing scheduler-specific scripts.

    std::string script_default_name; ///< Fallback filename, normally job.sh.

    std::string model_prefix; ///< Prefix before a model name, normally job_.
    std::string model_suffix; ///< Suffix after a model name, normally .sh.

private:
    void validate() const {
        load_balancer_detail::validate_directory(script_dir);
        load_balancer_detail::require_text(script_default_name, "Default job script filename");
        for (const auto& component : {script_default_name, model_prefix, model_suffix}) {
            if (component.find_first_of("/\\\r\n") != std::string::npos ||
                component.find('\0') != std::string::npos) {
                throw std::invalid_argument("Job script filenames must not contain path separators, NULs, or line breaks.");
            }
        }
    }
};


/**
 * @brief Abstract provider of exclusively owned model access on an HPC system.
 */
class JobManager {
public:
    /**
     * @brief Allow concrete managers to be destroyed through this interface.
     */
    virtual ~JobManager() = default;

    /**
     * @brief Acquire a model together with responsibility for its resources.
     * @param model_name Name of the model to access.
     * @return Exclusive model owner that releases its resources when destroyed.
     */
    virtual std::unique_ptr<umbridge::Model> requestModelAccess(const std::string& model_name) = 0;

    /**
     * @brief Discover the models that the load balancer should expose.
     * @return Names advertised by a model server; discovery may itself require a job.
     */
    virtual std::vector<std::string> getModelNames() = 0;
};


/**
 * @brief Couple a model proxy to the scheduler job that keeps its server alive.
 *
 * Every operation delegates unchanged to the wrapped model. The owned model is
 * destroyed before the owned Job because members are destroyed in reverse
 * declaration order; destruction of the Job then requests cancellation.
 */
class JobModel : public umbridge::Model {
public:
    /**
     * @brief Take ownership of a running job and its model proxy.
     * @param job Scheduler job whose lifetime must cover all proxy operations.
     * @param model Non-null model proxy; its name initializes the base Model.
     * @pre Both pointers identify the resources belonging to the same server.
     * @throws std::invalid_argument If either pointer is null.
     */
    JobModel(std::unique_ptr<Job> job, std::unique_ptr<umbridge::Model> model)
    : umbridge::Model(model ? model->GetName() : throw std::invalid_argument("JobModel requires a non-null model.")),
      job(std::move(job)), model(std::move(model)) {
        if (!this->job) {
            throw std::invalid_argument("JobModel requires a non-null job.");
        }
    }

    /**
     * @brief Return the number of scalars in each model input block.
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return Sizes of the input blocks for this configuration.
     */
    std::vector<std::size_t> GetInputSizes(const json &config_json = json::parse("{}")) const override {
        return model->GetInputSizes(config_json);
    }

    /**
     * @brief Return the number of scalars in each model output block.
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return Sizes of the output blocks for this configuration.
     */
    std::vector<std::size_t> GetOutputSizes(const json &config_json = json::parse("{}")) const override {
        return model->GetOutputSizes(config_json);
    }

    /**
     * @brief Forward a model evaluation with all input blocks.
     * @param inputs Input vectors in the order defined by GetInputSizes().
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return Output vectors in the order defined by GetOutputSizes().
     */
    std::vector<std::vector<double>> Evaluate(const std::vector<std::vector<double>> &inputs, 
                                              json config_json = json::parse("{}")) override {
        return model->Evaluate(inputs, config_json);
    }

    /**
     * @brief Apply the transpose of a Jacobian block to an output sensitivity.
     * @param outWrt Zero-based output-block index.
     * @param inWrt Zero-based input-block index to differentiate with respect to.
     * @param inputs Input blocks at which to evaluate the derivative.
     * @param sens Sensitivity vector with the size of output block outWrt.
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return J(outWrt, inWrt)^T * sens, with the size of input block inWrt.
     * @throws std::invalid_argument If inWrt does not identify a supplied input block.
     */
    std::vector<double> Gradient(unsigned int outWrt,
                                 unsigned int inWrt,
                                 const std::vector<std::vector<double>> &inputs,
                                 const std::vector<double> &sens,
                                 json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_input_index(inWrt, inputs);
        return model->Gradient(outWrt, inWrt, inputs, sens, config_json);
    }

    /**
     * @brief Apply a Jacobian block to an input-space direction.
     * @param outWrt Zero-based output-block index.
     * @param inWrt Zero-based input-block index to differentiate with respect to.
     * @param inputs Input blocks at which to evaluate the derivative.
     * @param vec Direction vector with the size of input block inWrt.
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return J(outWrt, inWrt) * vec, with the size of output block outWrt.
     * @throws std::invalid_argument If inWrt or the direction vector size is invalid.
     */
    std::vector<double> ApplyJacobian(unsigned int outWrt,
                                      unsigned int inWrt,
                                      const std::vector<std::vector<double>> &inputs,
                                      const std::vector<double> &vec,
                                      json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_direction(inWrt, inputs, vec);
        return model->ApplyJacobian(outWrt, inWrt, inputs, vec, config_json);
    }

    /**
     * @brief Apply a sensitivity-weighted Hessian block to a direction.
     * @param outWrt Zero-based output-block index weighted by sens.
     * @param inWrt1 Zero-based input-block index of the returned gradient.
     * @param inWrt2 Zero-based input-block index of the direction vec.
     * @param inputs Input blocks at which to evaluate the derivative.
     * @param sens Output sensitivity, held fixed while differentiating.
     * @param vec Direction vector with the size of input block inWrt2.
     * @param config_json Model-specific configuration forwarded unchanged.
     * @return Directional derivative of Gradient(outWrt, inWrt1, inputs, sens)
     * with respect to input block inWrt2 along vec, sized like input block inWrt1.
     * @throws std::invalid_argument If either input index or the direction vector size is invalid.
     */
    std::vector<double> ApplyHessian(unsigned int outWrt,
                                     unsigned int inWrt1,
                                     unsigned int inWrt2,
                                     const std::vector<std::vector<double>> &inputs,
                                     const std::vector<double> &sens,
                                     const std::vector<double> &vec,
                                     json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_input_index(inWrt1, inputs);
        load_balancer_detail::validate_direction(inWrt2, inputs, vec);
        return model->ApplyHessian(outWrt, inWrt1, inWrt2, inputs, sens, vec, config_json);
    }

    /**
     * @brief Query whether the wrapped model implements Evaluate.
     * @return The capability reported by the wrapped model.
     */
    bool SupportsEvaluate() override {
        return model->SupportsEvaluate();
    }
    /**
     * @brief Query whether the wrapped model implements Gradient.
     * @return The capability reported by the wrapped model.
     */
    bool SupportsGradient() override {
        return model->SupportsGradient();
    }
    /**
     * @brief Query whether the wrapped model implements ApplyJacobian.
     * @return The capability reported by the wrapped model.
     */
    bool SupportsApplyJacobian() override {
        return model->SupportsApplyJacobian();
    }
    /**
     * @brief Query whether the wrapped model implements ApplyHessian.
     * @return The capability reported by the wrapped model.
     */
    bool SupportsApplyHessian() override {
        return model->SupportsApplyHessian();
    }

private:
    std::unique_ptr<Job> job; ///< Kept alive until after the model proxy is destroyed.
    std::unique_ptr<umbridge::Model> model; ///< Proxy used for every forwarded operation.
};

/**
 * @brief Implement model access by submitting scripts and connecting over HTTP.
 *
 * The submitter selects the scheduler, the communicator factory selects the URL
 * exchange mechanism, and the locator selects scripts. Each access submits a
 * fresh job; this manager does not maintain a pool of persistent servers.
 */
class CommandJobManager : public JobManager {
public:
    /**
     * @brief Assemble the scheduler, startup communication, and script policy.
     * @param job_submitter Non-null scheduler factory; ownership is transferred.
     * @param job_comm_factory Non-null communicator factory; ownership is transferred.
     * @param locator Script-selection configuration copied into the manager.
     * @throws std::invalid_argument If either service pointer is null.
     */
    CommandJobManager(
        std::unique_ptr<JobSubmitter> job_submitter, 
        std::unique_ptr<JobCommunicatorFactory> job_comm_factory,
        JobScriptLocator locator) 
        : job_submitter(std::move(job_submitter)), job_comm_factory(std::move(job_comm_factory)), locator(std::move(locator)) {
        if (!this->job_submitter || !this->job_comm_factory) {
            throw std::invalid_argument("CommandJobManager requires a non-null submitter and communicator factory.");
        }
    }

    /**
     * @brief Start a server and return a proxy that owns its scheduler job.
     * @param model_name Model to select and request from the new server.
     * @return JobModel owning both an HTTPModel proxy and the submitted Job.
     * @note URL-file cleanup occurs before return; the job remains alive in the result.
     * Submission, communication, and HTTP initialization failures propagate; local
     * owners perform their usual cleanup during stack unwinding.
     */
    std::unique_ptr<umbridge::Model> requestModelAccess(const std::string& model_name) override {
        std::filesystem::path job_script = locator.selectJobScript(model_name);
        std::unique_ptr<JobCommunicator> comm = job_comm_factory->create();
        if (!comm) {
            throw std::runtime_error("Communicator factory returned a null communicator.");
        }
        std::unique_ptr<Job> job = job_submitter->submit(job_script, comm->getInitMessage());
        if (!job) {
            throw std::runtime_error("Job submitter returned a null job.");
        }
        std::string url = comm->getModelUrl(job->getJobId());
        // Construction performs HTTP discovery and reads supported operations;
        // publishing a URL alone therefore does not guarantee successful access.
        auto model = std::make_unique<umbridge::HTTPModel>(url, model_name);
        // Transfer the job to the returned proxy. The local communicator can now
        // remove its handshake file without stopping the model server.
        return std::make_unique<JobModel>(std::move(job), std::move(model));
    }

    /**
     * @brief Use a temporary default-script job to discover available models.
     * @return Names returned by the server's /Info endpoint.
     * @note Normal return requests job cancellation before removing the URL file;
     * the discovery server is not reused by later model operations.
     */
    std::vector<std::string> getModelNames() override 
    {
        std::filesystem::path job_script = locator.getDefaultJobScript();
        std::unique_ptr<JobCommunicator> comm = job_comm_factory->create();
        if (!comm) {
            throw std::runtime_error("Communicator factory returned a null communicator.");
        }
        std::unique_ptr<Job> job = job_submitter->submit(job_script, comm->getInitMessage());
        if (!job) {
            throw std::runtime_error("Job submitter returned a null job.");
        }
        std::string url = comm->getModelUrl(job->getJobId());
        // Materialize the names before local owners cancel the discovery job.
        return umbridge::SupportedModels(url);
    }
    
private:
    std::unique_ptr<JobSubmitter> job_submitter; ///< Scheduler-specific job factory.
    std::unique_ptr<JobCommunicatorFactory> job_comm_factory; ///< Per-job URL exchange factory.
    JobScriptLocator locator; ///< Default and model-specific script policy.
};


/**
 * @brief Expose one named UM-Bridge model by acquiring a job for each operation.
 *
 * Each model operation overridden below obtains a proxy from the shared manager,
 * forwards its arguments, and releases that proxy on normal return or stack
 * unwinding. With CommandJobManager this includes a new job even for size and capability
 * queries. No result, metadata, or running-server cache is maintained here.
 */
class LoadBalancer : public umbridge::Model {
public:
    /**
     * @brief Bind one public model name to a shared model-access provider.
     * @param name Model name advertised by this proxy.
     * @param job_manager Non-null manager shared with other LoadBalancer instances.
     * @throws std::invalid_argument If the model name is invalid or the manager is null.
     */
    LoadBalancer(std::string name, std::shared_ptr<JobManager> job_manager) 
    : umbridge::Model(name), job_manager(job_manager) {
        load_balancer_detail::validate_model_name(name);
        if (!this->job_manager) {
            throw std::invalid_argument("LoadBalancer requires a non-null job manager.");
        }
    }

    /**
     * @copydoc JobModel::GetInputSizes
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<std::size_t> GetInputSizes(const json &config_json = json::parse("{}")) const override {
        auto model = request_model();
        return model->GetInputSizes(config_json);
    }

    /**
     * @copydoc JobModel::GetOutputSizes
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<std::size_t> GetOutputSizes(const json &config_json = json::parse("{}")) const override {
        auto model = request_model();
        return model->GetOutputSizes(config_json);
    }

    /**
     * @copydoc JobModel::Evaluate
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<std::vector<double>> Evaluate(const std::vector<std::vector<double>> &inputs, 
                                              json config_json = json::parse("{}")) override {
        // This local owner keeps the remote server alive until the result has
        // been obtained; destruction also requests cancellation during stack unwinding.
        auto model = request_model();
        return model->Evaluate(inputs, config_json);
    }

    /**
     * @copydoc JobModel::Gradient
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<double> Gradient(unsigned int outWrt,
                                 unsigned int inWrt,
                                 const std::vector<std::vector<double>> &inputs,
                                 const std::vector<double> &sens,
                                 json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_input_index(inWrt, inputs);
        auto model = request_model();
        return model->Gradient(outWrt, inWrt, inputs, sens, config_json);
    }

    /**
     * @copydoc JobModel::ApplyJacobian
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<double> ApplyJacobian(unsigned int outWrt,
                                      unsigned int inWrt,
                                      const std::vector<std::vector<double>> &inputs,
                                      const std::vector<double> &vec,
                                      json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_direction(inWrt, inputs, vec);
        auto model = request_model();
        return model->ApplyJacobian(outWrt, inWrt, inputs, vec, config_json);
    }

    /**
     * @copydoc JobModel::ApplyHessian
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    std::vector<double> ApplyHessian(unsigned int outWrt,
                                     unsigned int inWrt1,
                                     unsigned int inWrt2,
                                     const std::vector<std::vector<double>> &inputs,
                                     const std::vector<double> &sens,
                                     const std::vector<double> &vec,
                                     json config_json = json::parse("{}")) override {
        load_balancer_detail::validate_input_index(inWrt1, inputs);
        load_balancer_detail::validate_direction(inWrt2, inputs, vec);
        auto model = request_model();
        return model->ApplyHessian(outWrt, inWrt1, inWrt2, inputs, sens, vec, config_json);
    }

    /**
     * @copydoc JobModel::SupportsEvaluate
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    bool SupportsEvaluate() override {
        auto model = request_model();
        return model->SupportsEvaluate();
    }
    /**
     * @copydoc JobModel::SupportsGradient
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    bool SupportsGradient() override {
        auto model = request_model();
        return model->SupportsGradient();
    }
    /**
     * @copydoc JobModel::SupportsApplyJacobian
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    bool SupportsApplyJacobian() override {
        auto model = request_model();
        return model->SupportsApplyJacobian();
    }
    /**
     * @copydoc JobModel::SupportsApplyHessian
     * @note Acquires a fresh model owner for this call and releases it afterward.
     */
    bool SupportsApplyHessian() override {
        auto model = request_model();
        return model->SupportsApplyHessian();
    }

private:
    std::unique_ptr<umbridge::Model> request_model() const {
        auto model = job_manager->requestModelAccess(name);
        if (!model) {
            throw std::runtime_error("Job manager returned a null model.");
        }
        return model;
    }

    std::shared_ptr<JobManager> job_manager; ///< Shared scheduling policy, not a shared running model.
};
