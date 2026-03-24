#pragma once

#include <memory>
#include <functional>

// 前向声明：实现类隐藏
class ThreadPoolManagerImpl;

class ThreadPoolManager final {
public:
    ThreadPoolManager();
    ~ThreadPoolManager();

    // 禁止拷贝/移动
    ThreadPoolManager(const ThreadPoolManager&) = delete;
    ThreadPoolManager& operator=(const ThreadPoolManager&) = delete;
    ThreadPoolManager(ThreadPoolManager&&) = delete;
    ThreadPoolManager& operator=(ThreadPoolManager&&) = delete;

    //非模板接口，用 std::function<void()> 接收任意任务
    void post(std::function<void()> task);

private:
    std::unique_ptr<ThreadPoolManagerImpl> impl_;
};