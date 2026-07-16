#ifndef __PERFORMANCE_MONITOR_HPP__
#define __PERFORMANCE_MONITOR_HPP__

#include <chrono>
#include <map>
#include <string>
#include <iostream>

class PerformanceMonitor {
public:
    PerformanceMonitor() {}
    ~PerformanceMonitor() {}

    // 开始计时
    void start(const std::string& name) {
        auto now = std::chrono::steady_clock::now();
        start_times_[name] = now;
    }

    // 结束计时并返回耗时（毫秒）
    double end(const std::string& name) {
        auto now = std::chrono::steady_clock::now();
        auto it = start_times_.find(name);
        if (it != start_times_.end()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            times_[name].push_back(duration);
            start_times_.erase(it);
            return duration;
        }
        return 0;
    }

    // 打印所有监控点的平均耗时
    void printStats() {
        std::cout << "\n=== Performance Statistics ===" << std::endl;
        for (const auto& pair : times_) {
            const std::string& name = pair.first;
            const std::vector<double>& values = pair.second;
            if (values.empty()) continue;

            double sum = 0;
            for (double val : values) sum += val;
            double avg = sum / values.size();

            std::cout << name << ": " << avg << " ms (" << values.size() << " samples)" << std::endl;
        }
        std::cout << "==============================" << std::endl;
    }

    // 重置所有监控数据
    void reset() {
        start_times_.clear();
        times_.clear();
    }

private:
    std::map<std::string, std::chrono::steady_clock::time_point> start_times_;
    std::map<std::string, std::vector<double>> times_;
};

#endif /* __PERFORMANCE_MONITOR_HPP__ */