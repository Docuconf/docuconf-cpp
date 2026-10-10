// `reload: watch` (SPEC §4.6.2, §11.2 item 8): an input the app rereads
// while it runs.
//
//     docuconf::Watched<docuconf::TlsKeyPair> tls;
//     config.add_file("serving-tls", tls, "Certificate the service serves HTTPS with")
//         .path("/etc/orders/tls");
//     DOCUCONF_PARSE(config, argc, argv);
//
//     std::shared_ptr<const docuconf::TlsKeyPair> pair = tls.current();
//     tls.on_change([](std::shared_ptr<const docuconf::TlsKeyPair> p) { /* rebuild from *p */ });
//
// current() is safe to call from any thread. At most once per check
// interval (one second by default) it stats the input's files, following
// symlinks, so the symlink Kubernetes swaps when it updates a ConfigMap or
// Secret volume shows up as a change. A changed input is read again and
// goes through every boot check; a value that passes replaces the current
// one, and one that fails is not used: the previous value stays and a
// warning naming the input and the violation (never a secret's content)
// goes to the warning sink. While an on-change hook is registered, a
// background thread makes the same check once per interval, so hooks run
// even when nothing calls current().
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

#include "errors.hpp"

namespace docuconf {

/// A change to a watched input that failed its checks and was not used.
/// It holds no content: only when, which input and the violation codes.
struct RejectedReload {
    std::chrono::system_clock::time_point time;
    /// The input whose checks failed. For Contract::watch, every input
    /// named by a violation, joined with ", ".
    std::string input;
    std::vector<Code> codes;
};

/// The reload state of one watched input, for a health check or a metric.
struct ReloadStatus {
    /// 1 after boot, then one more for every accepted reload; 0 before the
    /// configuration is loaded.
    std::uint64_t generation = 0;
    /// When the last accepted reload replaced the value; nullopt while the
    /// boot value is current.
    std::optional<std::chrono::system_clock::time_point> last_reload;
    /// The last change that failed its checks; cleared when a later change
    /// is accepted.
    std::optional<RejectedReload> last_rejected;
};

namespace detail {

/// What `stat` says about each path, following symlinks: device, inode,
/// size and modification and change times, or that the path is missing.
/// Two equal fingerprints mean nothing visible changed.
std::string stat_fingerprint(const std::vector<std::string>& paths);

/// The name of an exception's type, demangled where the compiler allows.
inline std::string exception_type_name(const std::type_info& t) {
#if defined(__GNUG__)
    int status = 0;
    char* d = abi::__cxa_demangle(t.name(), nullptr, nullptr, &status);
    if (status == 0 && d) {
        std::string out(d);
        std::free(d);
        return out;
    }
    std::free(d);
#endif
    return t.name();
}

/// The shared state behind a Watched<T> and its copies.
template <class T>
class WatchCore : public std::enable_shared_from_this<WatchCore<T>> {
public:
    using Loader = std::function<std::shared_ptr<const T>(std::vector<Violation>&)>;
    using Hook = std::function<void(std::shared_ptr<const T>)>;

    /// `fingerprint` is stat_fingerprint(paths), taken before `initial` was
    /// read, so a change made while it was read is seen at the next check.
    WatchCore(std::string name, std::vector<std::string> paths, std::string fingerprint,
              std::shared_ptr<const T> initial, Loader loader, std::function<void(const std::string&)> warn)
        : name_(std::move(name)),
          paths_(std::move(paths)),
          fingerprint_(std::move(fingerprint)),
          value_(std::move(initial)),
          loader_(std::move(loader)),
          warn_(std::move(warn)),
          bg_(std::make_shared<Background>()) {
        next_check_ = now() + bg_->interval.load();
    }

    WatchCore(const WatchCore&) = delete;
    WatchCore& operator=(const WatchCore&) = delete;

    ~WatchCore() {
        {
            std::lock_guard<std::mutex> lock(bg_->m);
            bg_->stop = true;
        }
        bg_->cv.notify_all();
        if (thread_.joinable()) {
            // The background thread may hold the last reference for a moment.
            if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
            else thread_.join();
        }
    }

    std::shared_ptr<const T> current() {
        check(false);
        std::shared_lock<std::shared_mutex> lock(value_mutex_);
        return value_;
    }

    bool refresh() { return check(true); }

    std::uint64_t generation() const { return generation_.load(); }

    ReloadStatus status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        ReloadStatus s;
        s.generation = generation_.load();
        s.last_reload = last_reload_;
        s.last_rejected = last_rejected_;
        return s;
    }

    void set_interval(std::chrono::nanoseconds d) {
        bg_->interval = d.count() < 0 ? 0 : d.count();
        next_check_ = now() + bg_->interval.load();
        bg_->cv.notify_all();
    }

    std::uint64_t add_hook(Hook fn) {
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(hooks_mutex_);
            id = ++next_hook_id_;
            hooks_.emplace_back(id, std::move(fn));
        }
        {
            std::lock_guard<std::mutex> lock(bg_->m);
            ++bg_->hooks;
            if (!thread_started_) {
                thread_started_ = true;
                std::weak_ptr<WatchCore> weak = this->shared_from_this();
                thread_ = std::thread(&WatchCore::run, weak, bg_);
            }
        }
        bg_->cv.notify_all();
        return id;
    }

    void remove_hook(std::uint64_t id) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(hooks_mutex_);
            auto it = std::find_if(hooks_.begin(), hooks_.end(), [id](const auto& h) { return h.first == id; });
            if (it != hooks_.end()) {
                hooks_.erase(it);
                removed = true;
            }
        }
        if (removed) {
            std::lock_guard<std::mutex> lock(bg_->m);
            --bg_->hooks;
        }
    }

private:
    // What the background thread shares with the core; it outlives the
    // core while the thread finishes.
    struct Background {
        std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        std::size_t hooks = 0;
        std::atomic<std::int64_t> interval{std::chrono::nanoseconds(std::chrono::seconds(1)).count()};
    };

    std::string name_;
    std::vector<std::string> paths_;
    std::string fingerprint_;         // of the current value; guarded by check_mutex_
    std::string failed_fingerprint_;  // of the last change that failed its checks
    std::shared_ptr<const T> value_;  // guarded by value_mutex_
    Loader loader_;
    std::function<void(const std::string&)> warn_;
    // Recursive, so a hook (run with it held) may call current() or refresh().
    std::recursive_mutex check_mutex_;
    std::shared_mutex value_mutex_;
    std::atomic<std::int64_t> next_check_{0};
    std::atomic<std::uint64_t> generation_{1};
    mutable std::mutex status_mutex_;
    std::optional<std::chrono::system_clock::time_point> last_reload_;
    std::optional<RejectedReload> last_rejected_;
    std::mutex hooks_mutex_;
    std::vector<std::pair<std::uint64_t, Hook>> hooks_;
    std::uint64_t next_hook_id_ = 0;
    std::shared_ptr<Background> bg_;
    bool thread_started_ = false;  // guarded by bg_->m
    std::thread thread_;

    static std::int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // The background check: once per interval (at least 10ms apart) while
    // a hook is registered; idle otherwise.
    static void run(std::weak_ptr<WatchCore> weak, std::shared_ptr<Background> bg) {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(bg->m);
                bg->cv.wait(lock, [&] { return bg->stop || bg->hooks > 0; });
                if (bg->stop) return;
                auto period = std::max<std::int64_t>(bg->interval.load(), 10'000'000);
                bg->cv.wait_for(lock, std::chrono::nanoseconds(period), [&] { return bg->stop; });
                if (bg->stop) return;
                if (bg->hooks == 0) continue;
            }
            auto core = weak.lock();
            if (!core) return;
            core->check(false);
        }
    }

    // Returns true when a new value replaced the current one. Without
    // `force`, a check that is not due, or that another thread is already
    // making, returns at once: readers never wait for a reload.
    bool check(bool force) {
        std::int64_t t = now();
        std::unique_lock<std::recursive_mutex> lock(check_mutex_, std::defer_lock);
        if (force) {
            lock.lock();
        } else {
            if (t < next_check_.load()) return false;
            if (!lock.try_lock()) return false;
            if (t < next_check_.load()) return false;
        }
        next_check_ = t + bg_->interval.load();
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
            RejectedReload r{std::chrono::system_clock::now(), {}, {}};
            std::vector<std::string> inputs;
            for (const auto& v : violations) {
                r.codes.push_back(v.code);
                if (std::find(inputs.begin(), inputs.end(), v.input) == inputs.end()) inputs.push_back(v.input);
            }
            for (const auto& i : inputs) r.input += (r.input.empty() ? "" : ", ") + i;
            if (r.input.empty()) r.input = name_;
            {
                std::lock_guard<std::mutex> s(status_mutex_);
                last_rejected_ = std::move(r);
            }
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
            value_ = fresh;
        }
        {
            std::lock_guard<std::mutex> s(status_mutex_);
            last_reload_ = std::chrono::system_clock::now();
            last_rejected_.reset();
            ++generation_;
        }
        notify(fresh);
        return true;
    }

    // Runs every hook with the new value, in registration order, with the
    // check lock held so hooks see reloads in order. A hook that throws is
    // logged by input name and exception type only (what() may hold
    // content), and the others still run.
    void notify(const std::shared_ptr<const T>& fresh) {
        std::vector<Hook> hooks;
        {
            std::lock_guard<std::mutex> lock(hooks_mutex_);
            for (const auto& h : hooks_) hooks.push_back(h.second);
        }
        for (const auto& h : hooks) {
            std::string type;
            try {
                h(fresh);
                continue;
            } catch (const std::exception& e) {
                type = exception_type_name(typeid(e));
            } catch (...) {
                type = "an exception that is not a std::exception";
            }
            if (warn_)
                warn_(name_ + ": an on-change hook threw " + type +
                      "; the new value stays and the other hooks ran");
        }
    }
};

}  // namespace detail

/// Cancels an on-change hook. Copyable; cancel() is idempotent, and does
/// nothing once the watched value is gone. Dropping it does not cancel.
class WatchSubscription {
public:
    WatchSubscription() = default;
    explicit WatchSubscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    /// Removes the hook: it is not called for later reloads. A hook that is
    /// running finishes.
    void cancel() const {
        if (cancel_) cancel_();
    }

private:
    std::function<void()> cancel_;
};

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

    /// generation(), the time of the last accepted reload and the last
    /// rejected change (its time, input and violation codes, never its
    /// content). Empty before the configuration is loaded.
    ReloadStatus reload_status() const { return core_ ? core_->status() : ReloadStatus{}; }

    /// Registers `fn`, called with the new value after a changed input
    /// passes its checks and replaces the current one; never for a change
    /// that fails them. Several hooks run in registration order, on the
    /// thread that made the check: a background thread, which checks once
    /// per check interval while any hook is registered, or a thread in
    /// current() or refresh(). Calls for one input never overlap. A hook
    /// that throws is logged to the warning sink by input name and
    /// exception type; the reload stands and the other hooks run. Before
    /// the configuration is loaded it registers nothing.
    WatchSubscription on_change(std::function<void(std::shared_ptr<const T>)> fn) const {
        if (!core_ || !fn) return WatchSubscription();
        std::uint64_t id = core_->add_hook(std::move(fn));
        std::weak_ptr<detail::WatchCore<T>> weak = core_;
        return WatchSubscription([weak, id] {
            if (auto c = weak.lock()) c->remove_hook(id);
        });
    }

    /// How often current() and the background check look for a change; one
    /// second by default. The next check is one interval from now; zero
    /// checks on every current() call (and every 10ms in the background).
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
