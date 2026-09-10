#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cli_test
{
namespace fs = std::filesystem;
struct TempDir
{
    fs::path path;
    TempDir()
    {
        auto pattern = (fs::temp_directory_path() / "rcv-cli-test-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};
inline std::string read(const fs::path& path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}
inline void write(const fs::path& path, const std::string& text)
{
    std::ofstream out(path);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("fixture write failed: " + path.string());
}
inline std::string binary()
{
    const char* value = std::getenv("RCV_CLI_BINARY");
    return value ? value : "";
}
struct RunResult
{
    int status = -1; // decoded exit code; -1 if terminated by signal
    int signal = 0;
    std::string output;
    std::string error;
};
inline RunResult run(const std::vector<std::string>& args, const fs::path& cwd = {}, rlim_t file_limit = RLIM_INFINITY)
{
    TempDir capture;
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0)
    {
        rlimit no_core{0, 0};
        if (setrlimit(RLIMIT_CORE, &no_core) != 0) _exit(126);
        int out = open((capture.path / "stdout").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int err = open((capture.path / "stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0) _exit(126);
        close(out);
        close(err);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        if (file_limit != RLIM_INFINITY)
        {
            signal(SIGXFSZ, SIG_IGN);
            rlimit limit{file_limit, file_limit};
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(126);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) throw std::runtime_error("waitpid failed");
    RunResult result;
    if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) result.signal = WTERMSIG(status);
    result.output = read(capture.path / "stdout");
    result.error = read(capture.path / "stderr");
    return result;
}
} // namespace cli_test
