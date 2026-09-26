#pragma once

#include <utility>

template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F fn) : fn(std::move(fn)) {}
    ~ScopeGuard() { if (active) fn(); }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

    void dismiss() noexcept { active = false; }

private:
    F fn;
    bool active = true;
};
