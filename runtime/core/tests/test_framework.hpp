// SynapseMatrix — C++ Runtime 自研最小测试框架（单头，无第三方依赖）
// ===================================================================
// 为什么不用 GoogleTest：
//   1. 本机/CI 无法保证联网拉取依赖，单头自研零依赖最稳
//   2. Month 3 主题是并发原语，测试框架本身也顺带练 RAII / 宏 / 可变参数
//
// 用法：
//   #include "test_framework.hpp"
//   TEST(组名, 用例名) { EXPECT(条件); }
//   int main() { return RunAllTests(); }
//
// 设计要点（对应 C++ 知识点）：
//   - 静态局部变量的初始化线程安全（C++11 起 magic statics）——注册表用
//   - std::function 存测试函数，lambda 捕获组名/用例名
//   - 宏展开：TEST 把两个参数拼成静态注册；EXPECT 失败时打印文件:行号

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

// 单条测试：名字 + 可调用体
struct TestCase {
    std::string name;                 // "组名.用例名"
    std::function<void()> body;
};

// 全局注册表（magic static：首次调用时线程安全地初始化）
inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> reg;  // 静态局部变量：C++11 起初始化线程安全
    return reg;
}

// 注册一条测试（TEST 宏展开后调用）
inline void Register(const std::string& name, std::function<void()> body) {
    Registry().push_back({name, std::move(body)});
}

// 失败计数：所有 EXPECT 共用，>0 则 RunAllTests 返回失败
inline int& FailureCount() {
    static int count = 0;
    return count;
}

// EXPECT 的实际执行体：条件为假时打印文件:行号并累加失败数
inline void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::printf("  [FAIL] %s:%d  条件不成立: %s\n", file, line, expr);
        ++FailureCount();
    }
}

// 跑全部测试，返回进程退出码（0=全过）
inline int RunAllTests() {
    int total = 0;
    for (const auto& tc : Registry()) {
        ++total;
        const int before = FailureCount();
        tc.body();                                    // 执行用例
        const bool ok = (FailureCount() == before);   // 本用例是否引入新失败
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", tc.name.c_str());
    }
    std::printf("\n%d 个用例，%d 个失败\n", total, FailureCount());
    return FailureCount() == 0 ? 0 : 1;
}

}  // namespace testfw

// 测试注册宏：TEST(Group, Case) 定义匿名命名空间里的静态函数并注册
// 关键点：testfw::Register 的调用发生在 main 之前的静态初始化期，
// 因此不需要手写"测试列表"——加一条测试就是加一个 TEST 块。
#define TEST(Group, Case)                                                        \
    static void Group##_##Case();                                               \
    namespace {                                                                 \
    struct Group##_##Case##_Registrar {                                         \
        Group##_##Case##_Registrar() {                                          \
            testfw::Register(#Group "." #Case, Group##_##Case);                 \
        }                                                                       \
    };                                                                          \
    static Group##_##Case##_Registrar Group##_##Case##_reg;                     \
    }                                                                           \
    static void Group##_##Case()

// 断言宏：失败打印位置与表达式，不中断当前用例（继续跑，便于一次看全失败）
#define EXPECT(cond) testfw::Expect((cond), #cond, __FILE__, __LINE__)

// 测试入口宏：在测试文件末尾调用一次，生成 main
#define TEST_MAIN()                       \
    int main() { return testfw::RunAllTests(); }
