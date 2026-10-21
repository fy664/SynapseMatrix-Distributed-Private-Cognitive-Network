// SynapseMatrix — 单测：Task 基础语义
// ====================================
// 覆盖：
//   1. 构造后 valid() 为真（有可执行体）
//   2. operator() 真正执行了任务体（副作用可见）
//   3. 空函数体构造时 valid() 为假
//   4. 元数据（id/priority）正确存取

#include "test_framework.hpp"

#include "runtime/task.hpp"

using synapse::runtime::Priority;
using synapse::runtime::Task;

TEST(Task, ValidAfterConstruction) {
    Task t([] {}, "t1", Priority::High);
    EXPECT(t.valid());
    EXPECT(t.id() == "t1");
    EXPECT(t.priority() == Priority::High);
}

TEST(Task, OperatorInvokesBody) {
    int side_effect = 0;
    Task t([&] { side_effect = 42; });
    t();                      // 执行任务体
    EXPECT(side_effect == 42);
}

TEST(Task, EmptyBodyIsInvalid) {
    Task t(std::function<void()>{});   // 显式空函数对象
    EXPECT(!t.valid());                // 无任务体
}

TEST(Task, DefaultPriorityIsNormal) {
    Task t([] {});
    EXPECT(t.priority() == Priority::Normal);
}

TEST_MAIN()
