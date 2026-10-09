// `reload: watch` (SPEC §4.6.2, §11.2 item 8): an input the app rereads
// while it runs.
//
//     docuconf::Watched<docuconf::TlsKeyPair> tls;
//     config.add_file("serving-tls", tls, "Certificate the service serves HTTPS with")
//         .path("/etc/orders/tls");
//     DOCUCONF_PARSE(config, argc, argv);
//
//     std::shared_ptr<const docuconf::TlsKeyPair> pair = tls.current();
//
// current() is safe to call from any thread. At most once per check
// interval (one second by default) it stats the input's files, following
// symlinks, so the symlink Kubernetes swaps when it updates a ConfigMap or
// Secret volume shows up as a change. A changed input is read again and
// goes through every boot check; a value that passes replaces the current
// one, and one that fails is not used: the previous value stays and a
// warning naming the input and the violation (never a secret's content)
// goes to the warning sink.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "errors.hpp"

namespace docuconf {

namespace detail {

/// What `stat` says about each path, following symlinks: device, inode,
/// size and modification and change times, or that the path is missing.
/// Two equal fingerprints mean nothing visible changed.
std::string stat_fingerprint(const std::vector<std::string>& paths);

/// The shared state behind a Watched<T> and its copies.
template <class T>
class WatchCore {
public:
    using Loader = std::function<std::shared_ptr<const T>(std::vector<Violation>&)>;

    /// `fingerprint` is stat_fingerprint(paths), taken before `initial` was
    /// read, so a change made while it was read is seen at the next check.
    WatchCore(std::string name, std::vector<std::string> paths, std::string fingerprint,
              std::shared_ptr<const T> initial, Loader loader, std::function<void(const std::string&)> warn)
        : name_(std::move(name)),
          paths_(std::move(paths)),
          fingerprint_(std::move(fingerprint)),
          value_(std::move(initial)),
          loader_(std::move(loader)),
          warn_(std::move(warn)) {
        next_check_ = now() + interval_.load();
    }

    std::shared_ptr<const T> current() {
        check(false);
        std::shared_lock<std::shared_mutex> lock(value_mutex_);
        return value_;
    }

    bool refresh() { return check(true); }

    std::uint64_t generation() const { return generation_.load(); }

    void set_interval(std::chrono::nanoseconds d) {
        interval_ = d.count() < 0 ? 0 : d.count();
        next_check_ = now() + interval_.load();
    }

private:
    std::string name_;
    std::vector<std::string> paths_;
    std::string fingerprint_;         // of the current value; guarded by check_mutex_
    std::string failed_fingerprint_;  // of the last change that failed its checks
    std::shared_ptr<const T> value_;  // guarded by value_mutex_
    Loader loader_;
    std::function<void(const std::string&)> warn_;
    std::mutex check_mutex_;
    std::shared_mutex value_mutex_;
    std::atomic<std::int64_t> interval_{std::chrono::nanoseconds(std::chrono::seconds(1)).count()};
    std::atomic<std::int64_t> next_check_{0};
    std::atomic<std::uint64_t> generation_{1};

    static std::int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Returns true when a new value replaced the current one. Without
    // `force`, a check that is not due, or that another thread is already
    // making, returns at once: readers never wait for a reload.
    bool check(bool force) {
        std::int64_t t = now();
        std::unique_lock<std::mutex> lock(check_mutex_, std::defer_lock);
        if (force) {
            lock.lock();
        } else {
            if (t < next_check_.load()) return false;
            if (!lock.try_lock()) return false;
            if (t < next_check_.load()) return false;
        }
        next_check_ = t + interval_.load();
        std::string fp = stat_fingerprint(paths_);
        if (fp == fingerprint_ || fp == failed_fingerprint_) return false;
        std::vector<Violation> violations;
        std::shared_ptr<const T> fresh;
        try {
            fresh = loader_(violations);
        } catch (const std::exception& e) {
            violations.push_back(Violation{name_, Code::FileUnreadable, std::string("cannot be read again: ") + e.what()});
        }
        if (!violations.empty() || !fresh) {
            // Warn once per bad change; the next change is checked again.
            failed_fingerprint_ = fp;
            if (warn_) {
                std::string m = name_ + " changed, but the new version was not loaded; keeping the previous one:";
                for (const auto& v : violations) m += "\n  " + v.str();
                warn_(m);
            }
            return false;
        }
        fingerprint_ = std::move(fp);
        failed_fingerprint_.clear();
        {
            std::unique_lock<std::shared_mutex> w(value_mutex_);
            value_ = std::move(fresh);
        }
        ++generation_;
        return true;
    }
};

}  // namespace detail

/// An input declared `reload: watch`: holds its current value and rereads
/// it when its files change. Copies share the same value. Bind a file input
/// to one with Declaration::add_file, or watch a whole contract with
/// Contract::watch. A default-constructed Watched (before the
/// configuration is loaded) holds a default T.
template <class T>
class Watched {
public:
    Watched() = default;
    explicit Watched(std::shared_ptr<detail::WatchCore<T>> core) : core_(std::move(core)) {}

    /// The current value, never null. When a check is due (at most once per
    /// check interval) it first stats the input's files and, if they
    /// changed, reloads them through the boot checks. Thread-safe: callers
    /// never wait for another thread's reload. Keep the returned pointer
    /// for as long as you use the value; a reload never changes it.
    std::shared_ptr<const T> current() const {
        if (!core_) return empty();
        return core_->current();
    }

    /// Checks the files now, whatever the interval. Returns true when a
    /// changed input passed its checks and replaced the current value.
    bool refresh() const { return core_ && core_->refresh(); }

    /// How many values have been loaded: 1 after boot, then one more for
    /// every reload that passed its checks. 0 before the configuration is
    /// loaded.
    std::uint64_t generation() const { return core_ ? core_->generation() : 0; }

    /// How often current() looks for a change; one second by default. The
    /// next check is one interval from now; zero checks on every call.
    template <class R, class P>
    const Watched& check_interval(std::chrono::duration<R, P> d) const {
        if (core_) core_->set_interval(std::chrono::duration_cast<std::chrono::nanoseconds>(d));
        return *this;
    }

private:
    std::shared_ptr<detail::WatchCore<T>> core_;

    static std::shared_ptr<const T> empty() {
        static const std::shared_ptr<const T> e(std::make_shared<T>());
        return e;
    }
};

}  // namespace docuconf
