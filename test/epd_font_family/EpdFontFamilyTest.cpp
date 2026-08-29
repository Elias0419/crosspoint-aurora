#include <EpdFontFamily.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

constexpr EpdFontFamily::Style combine(const EpdFontFamily::Style left, const EpdFontFamily::Style right) {
  return static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr auto AUX_COMBINATIONS = std::array{
    combine(EpdFontFamily::REGULAR, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::BOLD, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::ITALIC, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::BOLD_ITALIC, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::UNDERLINE, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::SUP, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::SUB, EpdFontFamily::AUX_FONT),
    combine(EpdFontFamily::RUBY_CONTINUE, EpdFontFamily::AUX_FONT),
};

static_assert([] {
  for (const auto style : AUX_COMBINATIONS) {
    if (!EpdFontFamily::usesAuxFont(style)) return false;
  }
  return true;
}());
static_assert(EpdFontFamily::TEXT_DECORATION_MASK ==
              static_cast<uint8_t>(EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH));
static_assert(!EpdFontFamily::hasTextDecoration(EpdFontFamily::AUX_FONT));

TEST(EpdFontFamilyStyle, AuxiliaryFlagDoesNotChangeFontVariantLookup) {
  const EpdFontData regularData{};
  const EpdFontData boldData{};
  const EpdFontData italicData{};
  const EpdFontData boldItalicData{};
  const EpdFont regular(&regularData);
  const EpdFont bold(&boldData);
  const EpdFont italic(&italicData);
  const EpdFont boldItalic(&boldItalicData);
  const EpdFontFamily family(&regular, &bold, &italic, &boldItalic);

  EXPECT_EQ(family.getData(combine(EpdFontFamily::REGULAR, EpdFontFamily::AUX_FONT)), &regularData);
  EXPECT_EQ(family.getData(combine(EpdFontFamily::BOLD, EpdFontFamily::AUX_FONT)), &boldData);
  EXPECT_EQ(family.getData(combine(EpdFontFamily::ITALIC, EpdFontFamily::AUX_FONT)), &italicData);
  EXPECT_EQ(family.getData(combine(EpdFontFamily::BOLD_ITALIC, EpdFontFamily::AUX_FONT)), &boldItalicData);
}

}  // namespace
