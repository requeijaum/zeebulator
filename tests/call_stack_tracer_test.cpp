#include <gtest/gtest.h>

#include "core/control/call_stack_tracer.h"
#include "core/cpu/arm_interpreter.h"

namespace zeebulator {
namespace {

TEST(CallStackTracer, ShadowStackPushPopAndSymbolResolve) {
  CallStackTracer& tracer = CallStackTracer::Instance();
  tracer.Reset();
  tracer.SetEnabled(true);

  tracer.RegisterSymbol(0x00020000, "MainFunction");
  tracer.RegisterSymbol(0x00020100, "SubHelper");

  EXPECT_EQ(tracer.ResolveSymbol(0x00020000), "MainFunction");
  EXPECT_EQ(tracer.ResolveSymbol(0x00020100), "SubHelper");
  EXPECT_EQ(tracer.ResolveSymbol(0x00030000), "0x00030000");

  tracer.OnCall(/*caller_pc=*/0x00020010, /*target_addr=*/0x00020100, /*lr=*/0x00020014, /*sp=*/0x80200000);

  auto stack = tracer.GetShadowStack();
  ASSERT_EQ(stack.size(), 1u);
  EXPECT_EQ(stack[0].pc, 0x00020010u);
  EXPECT_EQ(stack[0].target, 0x00020100u);
  EXPECT_EQ(stack[0].name, "SubHelper");

  tracer.OnReturn(/*current_pc=*/0x00020150, /*lr=*/0x00020014, /*sp=*/0x80200000);
  stack = tracer.GetShadowStack();
  EXPECT_EQ(stack.size(), 0u);
}

TEST(CallStackTracer, TreeViewFormatting) {
  CallStackTracer& tracer = CallStackTracer::Instance();
  tracer.Reset();
  tracer.SetEnabled(true);

  tracer.RegisterSymbol(0x1000, "AppEntry");
  tracer.RegisterSymbol(0x2000, "InitGraphics");
  tracer.RegisterSymbol(0x3000, "AllocBuffer");

  tracer.OnCall(0x1000, 0x2000, 0x1004, 0x8000);
  tracer.OnCall(0x2004, 0x3000, 0x2008, 0x7FF0);
  tracer.OnReturn(0x3010, 0x2008, 0x7FF0);
  tracer.OnReturn(0x2050, 0x1004, 0x8000);

  std::string tree = tracer.FormatCallTree();
  EXPECT_NE(tree.find("InitGraphics"), std::string::npos);
  EXPECT_NE(tree.find("AllocBuffer"), std::string::npos);
  EXPECT_NE(tree.find("└── "), std::string::npos);
}

}  // namespace
}  // namespace zeebulator
