// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h> // For OutputDebugStringW
#endif

#include "common/assert.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/literals.h"

#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/logging/text_formatter.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/threadsafe_queue.h"

namespace Common::Log {

/**
 * Static state as a singleton.
 */
class Impl {
public:
    static Impl& Instance() {
        static Impl backend;
        return backend;
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void PushEntry(Class log_class, Level log_level, const char* filename, unsigned int line_num,
                   const char* function, std::string message) {
        message_queue.Push(
            CreateEntry(log_class, log_level, filename, line_num, function, std::move(message)));
    }

    void AddBackend(std::unique_ptr<Backend> backend) {
        std::lock_guard lock{writing_mutex};
        backends.push_back(std::move(backend));
    }

    void RemoveBackend(std::string_view backend_name) {
        std::lock_guard lock{writing_mutex};

        std::erase_if(backends, [&backend_name](const auto& backend) {
            return backend_name == backend->GetName();
        });
    }

    const Filter& GetGlobalFilter() const {
        return filter;
    }

    void SetGlobalFilter(const Filter& f) {
        filter = f;
    }

    Backend* GetBackend(std::string_view backend_name) {
        const auto it =
            std::find_if(backends.begin(), backends.end(),
                         [&backend_name](const auto& i) { return backend_name == i->GetName(); });
        if (it == backends.end())
            return nullptr;
        return it->get();
    }

private:
    Impl() {
        backend_thread = std::thread([&] {
            Entry entry;
            auto write_logs = [&](Entry& e) {
                std::lock_guard lock{writing_mutex};
                for (const auto& backend : backends) {
                    backend->Write(e);
                }
            };
            while (true) {
                entry = message_queue.PopWait();
                if (entry.final_entry) {
                    break;
                }
                write_logs(entry);
            }

            // Drain the logging queue. Only writes out up to MAX_LOGS_TO_WRITE to prevent a
            // case where a system is repeatedly spamming logs even on close.
            const int MAX_LOGS_TO_WRITE = filter.IsDebug() ? INT_MAX : 100;
            int logs_written = 0;
            while (logs_written++ < MAX_LOGS_TO_WRITE && message_queue.Pop(entry)) {
                write_logs(entry);
            }
        });
    }

    ~Impl() {
        Entry entry;
        entry.final_entry = true;
        message_queue.Push(entry);
        backend_thread.join();
    }

    Entry CreateEntry(Class log_class, Level log_level, const char* filename, unsigned int line_nr,
                      const char* function, std::string message) const {
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        using std::chrono::steady_clock;

        return {
            .timestamp = duration_cast<microseconds>(steady_clock::now() - time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = filename,
            .line_num = line_nr,
            .function = function,
            .message = std::move(message),
            .final_entry = false,
        };
    }

    std::mutex writing_mutex;
    std::thread backend_thread;
    std::vector<std::unique_ptr<Backend>> backends;
    MPSCQueue<Entry> message_queue;
    Filter filter;
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};
};

ConsoleBackend::~ConsoleBackend() = default;

void ConsoleBackend::Write(const Entry& entry) {
    PrintMessage(entry);
}

ColorConsoleBackend::~ColorConsoleBackend() = default;

void ColorConsoleBackend::Write(const Entry& entry) {
    PrintColoredMessage(entry);
}

FileBackend::FileBackend(const std::filesystem::path& filename) {
    auto old_filename = filename;
    old_filename += ".old.txt";

    // Existence checks are done within the functions themselves.
    // We don't particularly care if these succeed or not.
    FS::RemoveFile(old_filename);
    void(FS::RenameFile(filename, old_filename));

    file =
        std::make_unique<FS::IOFile>(filename, FS::FileAccessMode::Write, FS::FileType::TextFile);
}

FileBackend::~FileBackend() = default;

void FileBackend::Write(const Entry& entry) {
    if (!file->IsOpen()) {
        return;
    }

    using namespace Common::Literals;
    // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
    constexpr std::size_t MAX_BYTES_WRITTEN = 100_MiB;
    constexpr std::size_t MAX_BYTES_WRITTEN_EXTENDED = 1_GiB;

    const bool write_limit_exceeded =
        bytes_written > MAX_BYTES_WRITTEN_EXTENDED ||
        (bytes_written > MAX_BYTES_WRITTEN && !Settings::values.extended_logging);

    // Close the file after the write limit is exceeded.
    if (write_limit_exceeded) {
        file->Close();
        return;
    }

    bytes_written += file->WriteString(FormatLogMessage(entry).append(1, '\n'));
    if (entry.log_level >= Level::Error) {
        file->Flush();
    }
}

DebuggerBackend::~DebuggerBackend() = default;

void DebuggerBackend::Write(const Entry& entry) {
#ifdef _WIN32
    ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry).append(1, '\n')).c_str());
#endif
}

void SetGlobalFilter(const Filter& filter) {
    Impl::Instance().SetGlobalFilter(filter);
}

void AddBackend(std::unique_ptr<Backend> backend) {
    Impl::Instance().AddBackend(std::move(backend));
}

void RemoveBackend(std::string_view backend_name) {
    Impl::Instance().RemoveBackend(backend_name);
}

Backend* GetBackend(std::string_view backend_name) {
    return Impl::Instance().GetBackend(backend_name);
}

void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename,
                       unsigned int line_num, const char* function, const char* format,
                       const fmt::format_args& args) {
    auto& instance = Impl::Instance();
    const auto& filter = instance.GetGlobalFilter();
    if (!filter.CheckMessage(log_class, log_level))
        return;

    instance.PushEntry(log_class, log_level, filename, line_num, function,
                       fmt::vformat(format, args));
}
} // namespace Common::Log
