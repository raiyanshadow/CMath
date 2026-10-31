#include "BuildSystem.h"

#include "cmath/language/LanguageEngine.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mathmake {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open source file");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) throw std::runtime_error("failed while reading source file");
    return contents.str();
}

std::string findClangxx(const std::string& requested) {
    if (!requested.empty()) return requested;

#ifdef _WIN32
    // _dupenv_s avoids MSVC's deprecated/unsafe getenv warning and returns
    // an owned buffer that must be released with std::free.
    char* environment = nullptr;
    std::size_t environmentSize = 0;
    if (::_dupenv_s(&environment, &environmentSize, "CMATH_CLANGXX") == 0 &&
        environment != nullptr) {
        const std::string value(environment);
        std::free(environment);
        if (!value.empty()) return value;
    } else if (environment != nullptr) {
        std::free(environment);
    }
#else
    if (const char* environment = std::getenv("CMATH_CLANGXX")) {
        if (*environment != '\0') return environment;
    }
#endif

#ifdef CMATH_DEFAULT_CLANGXX_PATH
    return CMATH_DEFAULT_CLANGXX_PATH;
#else
    return "clang++";
#endif
}

#ifdef _WIN32
std::wstring toWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        // argv may use the active Windows code page in a non-UTF-8 console.
        const int fallbackSize = MultiByteToWideChar(CP_ACP, 0, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
        if (fallbackSize <= 0) return std::wstring(text.begin(), text.end());
        std::wstring result(static_cast<std::size_t>(fallbackSize), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), fallbackSize);
        return result;
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

int runProcess(const std::string& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& workingDirectory = {}) {
    std::wstring commandLine = quoteWindowsArgument(toWide(executable));
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(toWide(argument));
    }

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const std::wstring working = workingDirectory.empty() ? L"" : workingDirectory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
        working.empty() ? nullptr : working.c_str(), &startup, &process);
    if (!created) {
        std::cerr << "mathmake: could not start '" << executable
                  << "' (Windows error " << GetLastError() << ").\n";
        return 127;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
#else
int runProcess(const std::string& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& workingDirectory = {}) {
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "mathmake: failed to create child process.\n";
        return 127;
    }
    if (child == 0) {
        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0)
            _exit(126);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        auto executableCopy = executable;
        argv.push_back(executableCopy.data());
        std::vector<std::string> copies = arguments;
        for (auto& argument : copies) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
#endif

void printDiagnostic(const std::filesystem::path& source,
                     const cmath::Diagnostic& diagnostic) {
    std::cerr << source.string() << ':' << diagnostic.location.line << ':'
              << diagnostic.location.column << ": ";
    if (diagnostic.severity == cmath::DiagnosticSeverity::Warning)
        std::cerr << "warning ";
    else if (diagnostic.severity == cmath::DiagnosticSeverity::Note)
        std::cerr << "note ";
    else
        std::cerr << "error ";
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto& note : diagnostic.notes)
        std::cerr << "  note: " << note << '\n';
}

} // namespace

void BuildSystem::printDiagnostics(
    const std::filesystem::path& sourceFile,
    const std::vector<cmath::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        printDiagnostic(sourceFile, diagnostic);
}

int BuildSystem::check(const std::filesystem::path& sourceFile) const {
    std::cerr << "[mathmake] Check: reading " << sourceFile.string() << "\n" << std::flush;
    std::string source;
    try {
        source = readFile(sourceFile);
    } catch (const std::exception& exception) {
        std::cerr << "mathmake: " << sourceFile.string() << ": "
                  << exception.what() << '\n';
        return 2;
    }

    std::cerr << "[mathmake] Check: read " << source.size() << " bytes; compiling\n" << std::flush;
    cmath::LanguageEngine engine;
    const auto compiled = engine.compile(source);
    std::cerr << "[mathmake] Check: frontend returned\n" << std::flush;
    printDiagnostics(sourceFile, compiled.diagnostics);
    if (!compiled.succeeded()) return 1;

    std::cout << "Checked " << sourceFile.filename().string() << " successfully.\n";
    return 0;
}

int BuildSystem::build(const BuildOptions& options) const {
    std::cerr << "[mathmake] Build: starting\n" << std::flush;
    if (options.sourceFile.empty()) {
        std::cerr << "mathmake: no .cmath source file was supplied.\n";
        return 2;
    }
    if (options.sourceFile.extension() != ".cmath") {
        std::cerr << "mathmake: expected a .cmath source file, received '"
                  << options.sourceFile.string() << "'.\n";
        return 2;
    }

    const auto sourcePath = std::filesystem::absolute(options.sourceFile);
    std::string source;
    try {
        source = readFile(sourcePath);
    } catch (const std::exception& exception) {
        std::cerr << "mathmake: " << options.sourceFile.string() << ": "
                  << exception.what() << '\n';
        return 2;
    }

    std::cerr << "[mathmake] Build: read " << source.size() << " bytes; compiling frontend\n" << std::flush;
    cmath::LanguageEngine engine;
    const auto compiled = engine.compile(source);
    std::cerr << "[mathmake] Build: frontend returned\n" << std::flush;
    printDiagnostics(sourcePath, compiled.diagnostics);
    if (!compiled.succeeded()) return 1;

    auto outputFile = options.outputFile;
    if (outputFile.empty()) {
        outputFile = options.sourceFile.parent_path() /
            (options.sourceFile.stem().string() + ".exe");
    } else if (outputFile.extension().empty()) {
        outputFile += ".exe";
    }
    if (outputFile.is_relative()) outputFile = std::filesystem::absolute(outputFile);

    const auto buildDirectory = sourcePath.parent_path() / "build" / "cmath";
    const auto objectFile = buildDirectory /
        (sourcePath.stem().string() + ".obj");

    cmath::LLVMBackend backend;
    cmath::LLVMBackendOptions backendOptions;
    backendOptions.optimization = options.optimization;
    std::cerr << "[mathmake] Build: generating native object " << objectFile.string() << "\n" << std::flush;
    const auto native = backend.emitObjectFile(*compiled.program, objectFile, backendOptions);
    std::cerr << "[mathmake] Build: native backend returned\n" << std::flush;
    printDiagnostics(sourcePath, native.diagnostics);
    if (!native.succeeded) return 1;

    std::error_code outputDirectoryError;
    if (!outputFile.parent_path().empty()) {
        std::filesystem::create_directories(outputFile.parent_path(), outputDirectoryError);
        if (outputDirectoryError) {
            std::cerr << "mathmake: could not create output directory: "
                      << outputDirectoryError.message() << '\n';
            return 1;
        }
    }

    const auto clangxx = findClangxx(options.clangxx);
    std::cout << "Linking " << outputFile.filename().string() << "...\n";
    const std::vector<std::string> linkArguments{
        native.objectFile.string(), "-o", outputFile.string()
    };
    std::cerr << "[mathmake] Build: invoking linker " << clangxx << "\n" << std::flush;
    const int linkResult = runProcess(clangxx, linkArguments, sourcePath.parent_path());
    std::cerr << "[mathmake] Build: linker process returned (" << linkResult << ")\n" << std::flush;
    if (linkResult != 0) {
        std::cerr << "mathmake: linker failed with exit code " << linkResult << ".\n";
        return linkResult;
    }

    std::cout << "Built " << outputFile.string() << " successfully.\n";
    if (options.runAfterBuild) {
        std::cout << "Running " << outputFile.filename().string() << "...\n";
        const auto executable = std::filesystem::absolute(outputFile).string();
        return runProcess(executable, {}, sourcePath.parent_path());
    }
    return 0;
}

} // namespace mathmake
