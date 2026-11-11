// mkw100: runtime messages for text the game's BMG files do not have. See custom_messages.h.
//
// Places: Race.bmg has "1st".."12th" (1301..1312) and nothing past it; results rows of 13th and
// beyond show message PlaceBmg(place) instead, "13th", "21st", ..., with the INF entry (font) of
// 1312. Ids 0x00FF0000 + place reach no BMG file; msg indices 0x7E00 + place stay clear of
// vs_racers_option.cpp's 0x7FFE/0x7FFF.

#include "custom_messages.h"

#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "memory.h"

#include <array>
#include <string>

namespace {

constexpr uint32_t kPlaceBmgBase = 0x00FF0000;
constexpr uint32_t kPlaceIndexBase = 0x7E00;
constexpr uint32_t kFirstPlace = 13;
constexpr uint32_t kLastPlace = PlayerSlots::kMaxPlayers;
constexpr uint32_t kPlaceFontBmg = 1312;      // Race.bmg "12th"
constexpr uint32_t kTextBytes = 16;           // per place, UTF-16BE with terminator
// After vs_racers_option.cpp's label and help strings at kMenuText.
constexpr uint32_t kPlaceText = ArrayOverflow::kMenuText + 0x1000;
static_assert(kPlaceIndexBase + kLastPlace < 0x7FFE);

std::array<uint32_t, kLastPlace + 1> g_fontIndex{};  // msg index of 1312 in the holder that asked
bool g_written = false;

void WriteTexts() {
    if (g_written) return;
    g_written = true;
    for (uint32_t place = kFirstPlace; place <= kLastPlace; ++place) {
        const uint32_t tens = place % 100, ones = place % 10;
        const char* suffix = (tens >= 11 && tens <= 13) ? "th" : ones == 1 ? "st" : ones == 2 ? "nd" : ones == 3 ? "rd" : "th";
        const std::string text = std::to_string(place) + suffix;
        const uint32_t base = kPlaceText + (place - kFirstPlace) * kTextBytes;
        for (uint32_t i = 0; i <= text.size(); ++i) Memory::Write16(base + 2 * i, i < text.size() ? uint8_t(text[i]) : 0);
    }
}

} // namespace

namespace CustomMessages {

uint32_t PlaceBmg(uint32_t place) {
    return place >= kFirstPlace && place <= kLastPlace ? kPlaceBmgBase + place : 0;
}

bool IsCustom(uint32_t bmgId, uint32_t& fontBmg) {
    if (bmgId < kPlaceBmgBase + kFirstPlace || bmgId > kPlaceBmgBase + kLastPlace) return false;
    fontBmg = kPlaceFontBmg;
    return true;
}

uint32_t Index(uint32_t bmgId, uint32_t fontMsgIndex) {
    WriteTexts();
    const uint32_t place = bmgId - kPlaceBmgBase;
    g_fontIndex[place] = fontMsgIndex;
    return kPlaceIndexBase + place;
}

bool Text(uint32_t msgIndex, uint32_t& text) {
    if (msgIndex < kPlaceIndexBase + kFirstPlace || msgIndex > kPlaceIndexBase + kLastPlace) return false;
    text = kPlaceText + (msgIndex - kPlaceIndexBase - kFirstPlace) * kTextBytes;
    return true;
}

bool FontIndex(uint32_t msgIndex, uint32_t& realMsgIndex) {
    if (msgIndex < kPlaceIndexBase + kFirstPlace || msgIndex > kPlaceIndexBase + kLastPlace) return false;
    realMsgIndex = g_fontIndex[msgIndex - kPlaceIndexBase];
    return true;
}

} // namespace CustomMessages
