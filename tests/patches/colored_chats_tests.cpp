#include "../../patches/colored_chats/messages.hpp"

#include <catch2/catch_test_macros.hpp>

namespace chat = fc::core_plugin::colored_chats;

TEST_CASE("Chat colors follow the message channel with admin precedence") {
    constexpr chat::Colors colors{0xffdf'df20, 0xff20'df20, 0xffd8'd8d8, 0xff10'f9ca};

    CHECK(chat::message_color(colors, 7, 0, false) == 0xffdf'df20);
    CHECK(chat::message_color(colors, 7, 0, true) == 0xffdf'df20);
    CHECK(chat::message_color(colors, 7, 1, false) == 0xff20'df20);
    CHECK(chat::message_color(colors, 7, 1, true) == 0xff10'f9ca);
    CHECK(chat::message_color(colors, 7, 2, true) == 0xffdf'df20);
    CHECK(chat::message_color(colors, chat::kAdminSender, 0, false) == 0xffd8'd8d8);
    CHECK(chat::message_color(colors, chat::kAdminSender, 1, true) == 0xffd8'd8d8);

    constexpr chat::Colors custom{0xffaa'bbcc, colors.team, colors.admin, colors.buddy};
    CHECK(chat::message_color(custom, 7, 0, false) == 0xffaa'bbcc);
    CHECK(chat::message_color(custom, 7, 0, true) == 0xffaa'bbcc);
}
