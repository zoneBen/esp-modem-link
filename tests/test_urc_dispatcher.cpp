#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "at_channel/urc_dispatcher.h"

using namespace esp_modem_link::at_channel;

TEST(UrcDispatcherTest, DispatchNoHandlers) {
  UrcDispatcher d;
  EXPECT_NO_THROW(d.Dispatch("+CSQ: 31,99"));
}

TEST(UrcDispatcherTest, SingleHandlerMatch) {
  UrcDispatcher d;
  std::string received_cmd;
  std::string received_args;
  auto h = d.Subscribe("+CSQ", [&](std::string_view cmd, std::string_view args) {
    received_cmd = std::string(cmd);
    received_args = std::string(args);
  });

  d.Dispatch("+CSQ: 31,99");
  EXPECT_EQ(received_cmd, "+CSQ");
  EXPECT_EQ(received_args, "31,99");

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, HandlerNotCalledForUnrelated) {
  UrcDispatcher d;
  bool called = false;
  auto h = d.Subscribe("+CSQ", [&](std::string_view, std::string_view) {
    called = true;
  });

  d.Dispatch("+CREG: 1,1");
  EXPECT_FALSE(called);

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, NoColonLine) {
  UrcDispatcher d;
  std::string received_cmd;
  auto h = d.Subscribe("RING", [&](std::string_view cmd, std::string_view args) {
    received_cmd = std::string(cmd);
    EXPECT_TRUE(args.empty());
  });

  d.Dispatch("RING");
  EXPECT_EQ(received_cmd, "RING");

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, MultipleHandlersSamePrefix) {
  UrcDispatcher d;
  int count = 0;
  auto h1 = d.Subscribe("+CREG", [&](std::string_view, std::string_view) {
    count++;
  });
  auto h2 = d.Subscribe("+CREG", [&](std::string_view, std::string_view) {
    count++;
  });

  d.Dispatch("+CREG: 1,1");
  EXPECT_EQ(count, 2);

  d.Unsubscribe(h1);
  d.Unsubscribe(h2);
}

TEST(UrcDispatcherTest, DifferentPrefixes) {
  UrcDispatcher d;
  bool csq_called = false;
  bool creg_called = false;
  auto h1 = d.Subscribe("+CSQ", [&](std::string_view, std::string_view) {
    csq_called = true;
  });
  auto h2 = d.Subscribe("+CREG", [&](std::string_view, std::string_view) {
    creg_called = true;
  });

  d.Dispatch("+CSQ: 20,99");
  EXPECT_TRUE(csq_called);
  EXPECT_FALSE(creg_called);

  d.Unsubscribe(h1);
  d.Unsubscribe(h2);
}

TEST(UrcDispatcherTest, Unsubscribe) {
  UrcDispatcher d;
  int count = 0;
  auto h = d.Subscribe("+CSQ", [&](std::string_view, std::string_view) {
    count++;
  });

  d.Dispatch("+CSQ: 31,99");
  EXPECT_EQ(count, 1);

  d.Unsubscribe(h);
  d.Dispatch("+CSQ: 30,99");
  EXPECT_EQ(count, 1);
}

TEST(UrcDispatcherTest, UnsubscribeNonExistentHandle) {
  UrcDispatcher d;
  EXPECT_NO_THROW(d.Unsubscribe(99999));
}

TEST(UrcDispatcherTest, HandleUniqueness) {
  UrcDispatcher d;
  auto h1 = d.Subscribe("A", [](std::string_view, std::string_view) {});
  auto h2 = d.Subscribe("A", [](std::string_view, std::string_view) {});
  auto h3 = d.Subscribe("B", [](std::string_view, std::string_view) {});
  EXPECT_NE(h1, h2);
  EXPECT_NE(h2, h3);
  EXPECT_NE(h1, h3);

  d.Unsubscribe(h1);
  d.Unsubscribe(h2);
  d.Unsubscribe(h3);
}

TEST(UrcDispatcherTest, TrimsWhitespace) {
  UrcDispatcher d;
  std::string received_cmd;
  std::string received_args;
  auto h = d.Subscribe("+CSQ", [&](std::string_view cmd, std::string_view args) {
    received_cmd = std::string(cmd);
    received_args = std::string(args);
  });

  d.Dispatch("  +CSQ:  31,99  ");
  EXPECT_EQ(received_cmd, "+CSQ");
  EXPECT_EQ(received_args, "31,99");

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, EmptyLineIgnored) {
  UrcDispatcher d;
  bool called = false;
  auto h = d.Subscribe("", [&](std::string_view, std::string_view) {
    called = true;
  });

  d.Dispatch("");
  EXPECT_FALSE(called);
  d.Dispatch("   ");
  EXPECT_FALSE(called);

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, PrefixPartialMatch) {
  UrcDispatcher d;
  std::string received;
  auto h = d.Subscribe("+MIP", [&](std::string_view cmd, std::string_view) {
    received = std::string(cmd);
  });

  d.Dispatch("+MIPRTCP: 1,5,hello");
  EXPECT_EQ(received, "+MIPRTCP");

  d.Unsubscribe(h);
}

TEST(UrcDispatcherTest, DispatchOrderMultiple) {
  UrcDispatcher d;
  std::vector<int> order;
  auto h1 = d.Subscribe("+CSQ", [&](std::string_view, std::string_view) {
    order.push_back(1);
  });
  auto h2 = d.Subscribe("+CSQ", [&](std::string_view, std::string_view) {
    order.push_back(2);
  });

  d.Dispatch("+CSQ: 31,99");
  ASSERT_EQ(order.size(), 2);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);

  d.Unsubscribe(h1);
  d.Unsubscribe(h2);
}
