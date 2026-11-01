#pragma once
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace test_support {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Runner {
public:
    void operator()(const std::string& name, const std::function<void()>& body) {
        try { body(); ++passed_; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) {
            ++failed_; std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    int finish() const {
        std::cout << passed_ << " passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }
private:
    int passed_{};
    int failed_{};
};
} // namespace test_support
