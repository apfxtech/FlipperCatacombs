#include "game/Defines.h"
#include "game/Platform.h"
#include "game/Menu.h"
#include "game/Font.h"
#include "game/Game.h"
#include "game/Draw.h"
#include "game/Textures.h"
#include "game/Generated/SpriteTypes.h"
#include "lib/EEPROM.h"

constexpr int EEPROM_ADDR_SCORE = 20; // 20-21
constexpr int EEPROM_ADDR_HIGH = 22; // 22-23

namespace {
constexpr uint8_t MENU_ITEMS_COUNT = 5;
constexpr uint8_t VISIBLE_ROWS = 2;

constexpr uint8_t MENU_FIRST_ROW = 4;
constexpr uint8_t TEXT_X = 24;
constexpr uint8_t CURSOR_X = 16;

static uint8_t Wrap(int v, int n) {
    v %= n;
    if(v < 0) v += n;
    return (uint8_t)v;
}

static uint8_t MaxTop() {
    return (MENU_ITEMS_COUNT > VISIBLE_ROWS) ? (uint8_t)(MENU_ITEMS_COUNT - VISIBLE_ROWS) : 0;
}
}

void Menu::PrintItem(uint8_t idx, uint8_t row) {
    switch(idx) {
    case 0:
        Font::PrintString(PSTR("Play"), row, TEXT_X, COLOUR_WHITE);
        break;
    case 1:
        Font::PrintString(PSTR("Connect"), row, TEXT_X, COLOUR_WHITE);
        break;
    case 2:
        Font::PrintString(PSTR("Sound:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintString(
            Platform::IsAudioEnabled() ? PSTR("on") : PSTR("off"), row, TEXT_X + 28, COLOUR_WHITE);
        break;
    case 3:
        Font::PrintString(PSTR("Score:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintInt(score_, row, TEXT_X + 28, COLOUR_WHITE);
        break;
    case 4:
        Font::PrintString(PSTR("High:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintInt(high_, row, TEXT_X + 28, COLOUR_WHITE);
        break;
    }
}

void Menu::Init() {
    selection = 0;
    topIndex = 0;
    cursorPos = 0;
}

void Menu::Draw() {
    Platform::FillScreen(COLOUR_BLACK);

    Font::PrintString(PSTR("CATACOMBS OF THE DAMNED"), 2, 18, COLOUR_WHITE);
    Font::PrintString(PSTR("by jhhoward && apfxtech"), 7, 18, COLOUR_WHITE);

    for(uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
        uint8_t idx = (uint8_t)(topIndex + row);
        if(idx >= MENU_ITEMS_COUNT) break;
        PrintItem(idx, (uint8_t)(MENU_FIRST_ROW + row));
    }

    Font::PrintString(PSTR(">"), (uint8_t)(MENU_FIRST_ROW + cursorPos), CURSOR_X, COLOUR_WHITE);

    const uint16_t* torchSprite = (Game::globalTickFrame & 4) ? torchSpriteData1 :
                                                                torchSpriteData2;
    Renderer::DrawScaled(torchSprite, 0, 10, 9, 255);
    Renderer::DrawScaled(torchSprite, DISPLAY_WIDTH - 18, 10, 9, 255);
}

void Menu::Tick() {
    static uint8_t lastInput = 0;
    uint8_t input = Platform::GetInput();

    auto syncWindow = [&]() {
        uint8_t maxTop = MaxTop();

        if(selection < topIndex) topIndex = selection;

        uint8_t end = (uint8_t)(topIndex + (VISIBLE_ROWS - 1));
        if(selection > end) {
            int t = (int)selection - (VISIBLE_ROWS - 1);
            if(t < 0) t = 0;
            if(t > maxTop) t = maxTop;
            topIndex = (uint8_t)t;
        }

        cursorPos = (uint8_t)(selection - topIndex);
        if(cursorPos >= VISIBLE_ROWS) cursorPos = (VISIBLE_ROWS - 1);
    };

    if((input & INPUT_DOWN) && !(lastInput & INPUT_DOWN)) {
        uint8_t next = Wrap((int)selection + 1, MENU_ITEMS_COUNT);

        if(cursorPos < (VISIBLE_ROWS - 1)) {
            selection = next;
            cursorPos++;
        } else {
            selection = next;

            if(topIndex < MaxTop()) {
                topIndex++;
            } else {
                topIndex = 0;
                cursorPos = 0;
            }
        }

        syncWindow();
    }

    if((input & INPUT_UP) && !(lastInput & INPUT_UP)) {
        uint8_t prev = Wrap((int)selection - 1, MENU_ITEMS_COUNT);

        if(cursorPos > 0) {
            selection = prev;
            cursorPos--;
        } else {
            selection = prev;

            if(topIndex > 0) {
                topIndex--;
            } else {
                topIndex = MaxTop();
                cursorPos = (VISIBLE_ROWS - 1);
            }
        }

        syncWindow();
    }

    if((input & (INPUT_A | INPUT_B)) && !(lastInput & (INPUT_A | INPUT_B))) {
        switch(selection) {
        case 0:
            Game::StartGame();
            break;
        case 1:
            Game::StartGame();
            break;
        case 2:
            Platform::SetAudioEnabled(!Platform::IsAudioEnabled());
            EEPROM.update(2, Platform::IsAudioEnabled());
            EEPROM.commit();
            break;
        default:
            break;
        }
    }

    lastInput = input;
}

void Menu::ResetTimer() {
    timer = 0;
}

void Menu::TickEnteringLevel() {
    constexpr uint8_t showTime = 30;

    if(timer < showTime) timer++;

    if(timer == showTime && Platform::GetInput() == 0) Game::StartLevel();
}

void Menu::DrawEnteringLevel() {
    Platform::FillScreen(COLOUR_BLACK);
    Font::PrintString(PSTR("Entering floor"), 3, 30, COLOUR_WHITE);
    Font::PrintInt(Game::floor, 3, 90, COLOUR_WHITE);
}

void Menu::TickGameOver() {
    constexpr uint8_t minShowTime = 30;

    if(timer < minShowTime) timer++;

    if(timer == minShowTime && (Platform::GetInput() & (INPUT_A | INPUT_B))) {
        timer++;
    } else if(timer == minShowTime + 1 && Platform::GetInput() == 0) {
        Game::SwitchState(Game::State::Menu);
    }
}

void Menu::DrawGameOver() {
    Platform::FillScreen(COLOUR_BLACK);
    Font::PrintString(PSTR("GAME OVER"), 0, 64 - 18, COLOUR_WHITE);

    switch(Game::stats.killedBy) {
    case EnemyType::Exit:
        Font::PrintString(PSTR("You have left the game."), 1, 18, COLOUR_WHITE);
        break;
    case EnemyType::None:
        Font::PrintString(PSTR("You escaped the catacombs!"), 1, 12, COLOUR_WHITE);
        break;
    case EnemyType::Mage:
        Font::PrintString(PSTR("Killed by a mage on level"), 1, 8, COLOUR_WHITE);
        Font::PrintInt(Game::floor, 1, 112, COLOUR_WHITE);
        break;
    case EnemyType::Skeleton:
        Font::PrintString(PSTR("Killed by a knight on level"), 1, 4, COLOUR_WHITE);
        Font::PrintInt(Game::floor, 1, 116, COLOUR_WHITE);
        break;
    case EnemyType::Bat:
        Font::PrintString(PSTR("Killed by a bat on level"), 1, 10, COLOUR_WHITE);
        Font::PrintInt(Game::floor, 1, 110, COLOUR_WHITE);
        break;
    case EnemyType::Spider:
        Font::PrintString(PSTR("Killed by a spider on level"), 1, 4, COLOUR_WHITE);
        Font::PrintInt(Game::floor, 1, 116, COLOUR_WHITE);
        break;
    }

    Font::PrintString(PSTR("LOOT:"), 2, 20, COLOUR_WHITE);

    constexpr uint8_t firstRow = 21;
    constexpr uint8_t secondRow = 38;

    Renderer::DrawScaled(chestSpriteData, 0, firstRow, 9, 255);
    Font::PrintInt(Game::stats.chestsOpened, 4, 18, COLOUR_WHITE);

    Renderer::DrawScaled(crownSpriteData, 0, secondRow, 9, 255);
    Font::PrintInt(Game::stats.crownsCollected, 6, 18, COLOUR_WHITE);

    Renderer::DrawScaled(scrollSpriteData, 30, firstRow, 9, 255);
    Font::PrintInt(Game::stats.scrollsCollected, 4, 48, COLOUR_WHITE);

    Renderer::DrawScaled(coinsSpriteData, 30, secondRow, 9, 255);
    Font::PrintInt(Game::stats.coinsCollected, 6, 48, COLOUR_WHITE);

    int offset = (Game::globalTickFrame & 8) == 0 ? 32 : 0;
    Font::PrintString(PSTR("KILLS:"), 2, 84, COLOUR_WHITE);

    Renderer::DrawScaled(skeletonSpriteData + offset, 66, firstRow, 9, 255);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Skeleton], 4, 84, COLOUR_WHITE);

    Renderer::DrawScaled(mageSpriteData + offset, 66, secondRow, 9, 255);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Mage], 6, 84, COLOUR_WHITE);

    Renderer::DrawScaled(batSpriteData + offset, 96, firstRow, 9, 255, true);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Bat], 4, 114, COLOUR_WHITE);

    Renderer::DrawScaled(spiderSpriteData + offset, 96, secondRow, 9, 255);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Spider], 6, 114, COLOUR_WHITE);

    // Calculate final score here
    uint16_t finalScore = 0;
    constexpr int finishBonus = 500;
    constexpr int levelBonus = 20;
    constexpr int chestBonus = 15;
    constexpr int crownBonus = 10;
    constexpr int scrollBonus = 8;
    constexpr int coinsBonus = 4;
    constexpr int skeletonKillBonus = 10;
    constexpr int mageKillBonus = 10;
    constexpr int batKillBonus = 5;
    constexpr int spiderKillBonus = 4;

    finalScore += (Game::floor - 1) * levelBonus;

    if(Game::stats.killedBy == EnemyType::None) finalScore += finishBonus;
    finalScore += Game::stats.chestsOpened * chestBonus;
    finalScore += Game::stats.crownsCollected * crownBonus;
    finalScore += Game::stats.scrollsCollected * scrollBonus;
    finalScore += Game::stats.coinsCollected * coinsBonus;
    finalScore += Game::stats.enemyKills[(int)EnemyType::Skeleton] * skeletonKillBonus;
    finalScore += Game::stats.enemyKills[(int)EnemyType::Mage] * mageKillBonus;
    finalScore += Game::stats.enemyKills[(int)EnemyType::Bat] * batKillBonus;
    finalScore += Game::stats.enemyKills[(int)EnemyType::Spider] * spiderKillBonus;

    SetScore(finalScore);
    Font::PrintString(PSTR("FINAL SCORE:"), 7, 20, COLOUR_WHITE);
    Font::PrintInt(finalScore, 7, 72, COLOUR_WHITE);
}

#include <stdio.h>

static inline void DrawEraseTile8x8(int16_t x, int16_t y, const uint8_t* frame8bytes) {
    // frame8bytes: 8 байт, каждый байт = строка 8 пикселей (бит7 слева)
    for(uint8_t row = 0; row < 8; row++) {
        uint8_t rowMask = pgm_read_byte(frame8bytes + row);
        while(rowMask) {
            uint8_t b = (uint8_t)__builtin_ctz((unsigned)rowMask); // индекс младшего 1-бита
            uint8_t col = 7 - b; // т.к. у нас бит7 = левый пиксель
            Platform::PutPixel(x + col, y + row, COLOUR_WHITE); // "erase"
            rowMask &= (uint8_t)(rowMask - 1);
        }
    }
}

void Menu::DrawTransitionFrame(uint8_t frameIndex) {
    const uint8_t w = pgm_read_byte(transitionSet + 0); // 8
    const uint8_t h = pgm_read_byte(transitionSet + 1); // 8
    (void)w;
    (void)h;

    const uint8_t* framePtr = transitionSet + 2 + (uint16_t)frameIndex * 8;
    int16_t tileX = 120;
    int16_t tileY = 56;
    while(true) {
        DrawEraseTile8x8(tileX, tileY, framePtr);

        tileX -= 8;
        if(tileX < 0) {
            tileX = 120;
            tileY -= 8;
            if(tileY < 0) break;
        }
    }
}

void Menu::FadeOut() {
    static uint8_t timer = 0; // 0..29
    constexpr uint8_t totalTime = 32; // 1 секунда
    constexpr uint8_t totalFrames = 8; // 0..7

    uint8_t frameIndex = (uint16_t)timer * totalFrames / totalTime;
    if(frameIndex > totalFrames - 1) frameIndex = totalFrames - 1;
    DrawTransitionFrame(frameIndex);
    timer++;
    if(timer >= totalTime - totalTime / totalFrames) {
        timer = 0;
        Game::SwitchState(Game::State::GameOver);
    }
}

void Menu::ReadScore() {
    score_ = (uint16_t)EEPROM.read(EEPROM_ADDR_SCORE) |
             ((uint16_t)EEPROM.read(EEPROM_ADDR_SCORE + 1) << 8);

    high_ = (uint16_t)EEPROM.read(EEPROM_ADDR_HIGH) |
            ((uint16_t)EEPROM.read(EEPROM_ADDR_HIGH + 1) << 8);
}

void Menu::SetScore(uint16_t score) {
    if (score <= 0) return;
    uint16_t storedHigh = (uint16_t)EEPROM.read(EEPROM_ADDR_HIGH) |
                          ((uint16_t)EEPROM.read(EEPROM_ADDR_HIGH + 1) << 8);

    uint16_t newHigh = storedHigh;
    if(score > newHigh) newHigh = score;

    score_ = score;
    high_ = newHigh;

    EEPROM.update(EEPROM_ADDR_SCORE + 0, (uint8_t)(score & 0xFF));
    EEPROM.update(EEPROM_ADDR_SCORE + 1, (uint8_t)(score >> 8));
    EEPROM.update(EEPROM_ADDR_HIGH + 0, (uint8_t)(newHigh & 0xFF));
    EEPROM.update(EEPROM_ADDR_HIGH + 1, (uint8_t)(newHigh >> 8));
    EEPROM.commit();
}
