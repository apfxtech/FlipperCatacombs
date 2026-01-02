#include "game/Defines.h"
#include "game/Platform.h"
#include "game/Menu.h"
#include "game/Font.h"
#include "game/Game.h"
#include "game/Draw.h"
#include "game/Textures.h"
#include "game/Generated/SpriteTypes.h"
#include "game/Map.h"
#include "game/FixedMath.h"
#include "lib/EEPROM.h"

#include <stdio.h>
#include <string.h>

constexpr int EEPROM_BASE_ADDR = 20;

namespace {
constexpr uint8_t MENU_ITEMS_COUNT = 4;
constexpr uint8_t VISIBLE_ROWS = 2;

constexpr uint8_t MENU_FIRST_ROW = 4;
constexpr uint8_t TEXT_X = 18;
constexpr uint8_t CURSOR_X = 10;

constexpr uint8_t SPLASH_TIME_TICKS = 45;
static uint8_t splashTimer = 0;
static bool splashActive = true;

static uint8_t Wrap(int v, int n) {
    v %= n;
    if(v < 0) v += n;
    return (uint8_t)v;
}

static uint8_t MaxTop() {
    return (MENU_ITEMS_COUNT > VISIBLE_ROWS) ? (uint8_t)(MENU_ITEMS_COUNT - VISIBLE_ROWS) : 0;
}
void DrawMenuRoom() {
    const int16_t leftWall = 1 * CELL_SIZE;
    const int16_t rightWall = 4 * CELL_SIZE;
    const int16_t topWall = 1 * CELL_SIZE;
    const int16_t bottomWall = 4 * CELL_SIZE;

    Renderer::camera.x = (int16_t)((leftWall + rightWall) / 2);
    Renderer::camera.y = (int16_t)((topWall + bottomWall) / 2);

    static uint8_t ang = 0;
    static uint8_t acc = 0;
    acc = (uint8_t)(acc + 85);
    if(acc < 85) ang = (uint8_t)(ang + 1);
    Renderer::camera.angle = ang;

    Renderer::camera.tilt = 0;
    Renderer::camera.bob = 0;

    Renderer::numBufferSlicesFilled = 0;
    Renderer::numQueuedDrawables = 0;

    for(uint8_t n = 0; n < DISPLAY_WIDTH; n++) {
        Renderer::wBuffer[n] = 0;
        Renderer::horizonBuffer[n] = HORIZON;
    }

    Renderer::camera.cellX = Renderer::camera.x / CELL_SIZE;
    Renderer::camera.cellY = Renderer::camera.y / CELL_SIZE;

    Renderer::camera.rotCos = FixedCos(-Renderer::camera.angle);
    Renderer::camera.rotSin = FixedSin(-Renderer::camera.angle);
    Renderer::camera.clipCos = FixedCos(-Renderer::camera.angle + CLIP_ANGLE);
    Renderer::camera.clipSin = FixedSin(-Renderer::camera.angle + CLIP_ANGLE);

    Renderer::DrawBackground();

#if WITH_IMAGE_TEXTURES
    const uint16_t* texture = wallTextureData;
#elif WITH_VECTOR_TEXTURES
    const uint8_t* texture = vectorTexture0;
#endif

    const int16_t x0 = leftWall;
    const int16_t y0 = topWall;
    const int16_t x1 = rightWall;
    const int16_t y1 = bottomWall;

    const bool edgeLeft = true;
    const bool edgeRight = true;
    const bool shadeEdge = false;

#if WITH_TEXTURES
    Renderer::DrawWall(texture, x0, y0, x1, y0, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(texture, x1, y0, x1, y1, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(texture, x1, y1, x0, y1, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(texture, x0, y1, x0, y0, edgeLeft, edgeRight, shadeEdge);
#else
    Renderer::DrawWall(x0, y0, x1, y0, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(x1, y0, x1, y1, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(x1, y1, x0, y1, edgeLeft, edgeRight, shadeEdge);
    Renderer::DrawWall(x0, y1, x0, y0, edgeLeft, edgeRight, shadeEdge);
#endif
}

}

void Menu::PrintItem(uint8_t idx, uint8_t row) {
    switch(idx) {
    case 0:
        Font::PrintString(PSTR("Play"), row, TEXT_X, COLOUR_WHITE);
        break;
    case 1:
        Font::PrintString(PSTR("Sound:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintString(
            Platform::IsAudioEnabled() ? PSTR("on") : PSTR("off"), row, TEXT_X + 28, COLOUR_WHITE);
        break;
    case 2:
        Font::PrintString(PSTR("Score:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintInt(score_, row, TEXT_X + 28, COLOUR_WHITE);
        break;
    case 3:
        Font::PrintString(PSTR("High:"), row, TEXT_X, COLOUR_WHITE);
        Font::PrintInt(high_, row, TEXT_X + 28, COLOUR_WHITE);
        break;
    }
}

void Menu::Init() {
    selection = 0;
    topIndex = 0;
    cursorPos = 0;

    splashTimer = 0;
    splashActive = true;
}

void Menu::Draw() {
    DrawMenuRoom();

    if(splashActive) {
        Font::PrintString(PSTR("FLIPPER GAME"), 2, 42, COLOUR_WHITE);
        Font::PrintString(PSTR("JHHOWARD & APFXTECH"), 4, 26, COLOUR_WHITE);
        Font::PrintString(PSTR("PRESENT"), 6, 52, COLOUR_WHITE);
        return;
    }

    Font::PrintString(PSTR("CATACOMBS OF THE DAMNED"), 2, 18, COLOUR_WHITE);

    for(uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
        uint8_t idx = (uint8_t)(topIndex + row);
        if(idx >= MENU_ITEMS_COUNT) break;
        PrintItem(idx, (uint8_t)(MENU_FIRST_ROW + row));
    }

    const int animOffset = ((Game::globalTickFrame & 8) == 0) ? 32 : 0;
    const uint8_t enemyShow = (uint8_t)((Game::globalTickFrame / 64) & 3);
    const uint16_t* enemySprite = nullptr;
    const uint16_t* lootSprite = nullptr;
    bool flip = false;
    uint8_t color = COLOUR_WHITE;
    uint8_t num1 = 0;
    uint8_t num2 = 0;

    switch(enemyShow) {
    case 0:
        lootSprite = chestSpriteData;
        enemySprite = skeletonSpriteData;
        num1 = vars_[0];
        num2 = vars_[4];
        break;
    case 1:
        lootSprite = scrollSpriteData;
        enemySprite = mageSpriteData;
        num1 = vars_[2];
        num2 = vars_[5];
        break;
    case 2:
        lootSprite = coinsSpriteData;
        enemySprite = batSpriteData;
        num1 = vars_[3];
        num2 = vars_[6];
        flip = true;
        color = COLOUR_BLACK;
        break;
    case 3:
        lootSprite = crownSpriteData;
        enemySprite = spiderSpriteData;
        num1 = vars_[1];
        num2 = vars_[7];
        break;
    }

    const uint16_t* torchSprite = (Game::globalTickFrame & 4) ? torchSpriteData1 :
                                                                torchSpriteData2;
    Renderer::DrawScaled(lootSprite, 66, 29, 9, 255);
    Renderer::DrawScaled(enemySprite + animOffset, 96, 30, 9, 255, flip, color);
    Renderer::DrawScaled(torchSprite, 0, 10, 9, 255);
    Renderer::DrawScaled(torchSprite, DISPLAY_WIDTH - 18, 10, 9, 255);
    Font::PrintInt(num1, MENU_FIRST_ROW + 1, 86, COLOUR_WHITE);
    Font::PrintInt(num2, MENU_FIRST_ROW + 1, 116, COLOUR_WHITE);

    Font::PrintString(PSTR(">"), (uint8_t)(MENU_FIRST_ROW + cursorPos), CURSOR_X, COLOUR_WHITE);
}

void Menu::DrawEnteringLevel() {
    DrawMenuRoom();
    Font::PrintString(PSTR("Entering floor"), 3, 30, COLOUR_BLACK);
    Font::PrintInt(Game::floor, 3, 90, COLOUR_BLACK);
}

void Menu::DrawGameOver() {
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

    Platform::FillScreen(COLOUR_WHITE);
    Font::PrintString(PSTR("GAME OVER"), 0, 12, COLOUR_BLACK);
    Font::PrintString(PSTR("FINAL SCORE:"), 0, 56, COLOUR_BLACK);
    Font::PrintInt(finalScore, 0, 106, COLOUR_BLACK);

    switch(Game::stats.killedBy) {
    case EnemyType::Exit:
        Font::PrintString(PSTR("You have left the game."), 1, 18, COLOUR_BLACK);
        break;
    case EnemyType::None:
        Font::PrintString(PSTR("You escaped the catacombs!"), 1, 12, COLOUR_BLACK);
        break;
    case EnemyType::Mage:
        Font::PrintString(PSTR("Killed by a mage on level"), 1, 8, COLOUR_BLACK);
        Font::PrintInt(Game::floor, 1, 112, COLOUR_BLACK);
        break;
    case EnemyType::Skeleton:
        Font::PrintString(PSTR("Killed by a knight on level"), 1, 4, COLOUR_BLACK);
        Font::PrintInt(Game::floor, 1, 116, COLOUR_BLACK);
        break;
    case EnemyType::Bat:
        Font::PrintString(PSTR("Killed by a bat on level"), 1, 10, COLOUR_BLACK);
        Font::PrintInt(Game::floor, 1, 110, COLOUR_BLACK);
        break;
    case EnemyType::Spider:
        Font::PrintString(PSTR("Killed by a spider on level"), 1, 4, COLOUR_BLACK);
        Font::PrintInt(Game::floor, 1, 116, COLOUR_BLACK);
        break;
    }

    constexpr uint8_t firstRow = 21;
    constexpr uint8_t secondRow = 38;

    int offset = (Game::globalTickFrame & 8) == 0 ? 32 : 0;

    Renderer::DrawScaled(chestSpriteData, 0, firstRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.chestsOpened, 4, 18, COLOUR_BLACK);

    Renderer::DrawScaled(crownSpriteData, 0, secondRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.crownsCollected, 6, 18, COLOUR_BLACK);

    Renderer::DrawScaled(scrollSpriteData, 30, firstRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.scrollsCollected, 4, 48, COLOUR_BLACK);

    Renderer::DrawScaled(coinsSpriteData, 30, secondRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.coinsCollected, 6, 48, COLOUR_BLACK);

    Renderer::DrawScaled(skeletonSpriteData + offset, 66, firstRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Skeleton], 4, 84, COLOUR_BLACK);

    Renderer::DrawScaled(mageSpriteData + offset, 66, secondRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Mage], 6, 84, COLOUR_BLACK);

    Renderer::DrawScaled(batSpriteData + offset, 96, firstRow, 9, 255, true, COLOUR_BLACK);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Bat], 4, 114, COLOUR_BLACK);

    Renderer::DrawScaled(spiderSpriteData + offset, 96, secondRow, 9, 255, false, COLOUR_WHITE);
    Font::PrintInt(Game::stats.enemyKills[(int)EnemyType::Spider], 6, 114, COLOUR_BLACK);

    vars_[0] = Game::stats.chestsOpened;
    vars_[1] = Game::stats.crownsCollected;
    vars_[2] = Game::stats.scrollsCollected;
    vars_[3] = Game::stats.coinsCollected;
    vars_[4] = Game::stats.enemyKills[(int)EnemyType::Skeleton];
    vars_[5] = Game::stats.enemyKills[(int)EnemyType::Mage];
    vars_[6] = Game::stats.enemyKills[(int)EnemyType::Bat];
    vars_[7] = Game::stats.enemyKills[(int)EnemyType::Spider];

    SetScore(finalScore);
}

void Menu::Tick() {
    static uint8_t lastInput = 0;
    uint8_t input = Platform::GetInput();

    if(splashActive) {
        if(splashTimer < SPLASH_TIME_TICKS) splashTimer++;
        if(splashTimer >= SPLASH_TIME_TICKS) splashActive = false;
        lastInput = input;
        return;
    }

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

void Menu::TickEnteringLevel() {
    constexpr uint8_t showTime = 30;
    if(timer < showTime) timer++;
    if(timer == showTime && Platform::GetInput() == 0) {
        Game::SwitchState(Game::State::TransitionToLevel);
    }
}

void Menu::TickGameOver() {
    constexpr uint8_t minShowTime = 60;

    if(timer < minShowTime) timer++;

    if(timer == minShowTime && (Platform::GetInput() & (INPUT_A | INPUT_B))) {
        timer++;
    } else if(timer == minShowTime + 1 && Platform::GetInput() == 0) {
        Game::SwitchState(Game::State::Menu);
    }
}

static inline void DrawEraseTile8x8(int16_t x, int16_t y, const uint8_t* frame8bytes) {
    for(uint8_t row = 0; row < 8; row++) {
        uint8_t rowMask = pgm_read_byte(frame8bytes + row);
        while(rowMask) {
            uint8_t b = (uint8_t)__builtin_ctz((unsigned)rowMask);
            uint8_t col = 7 - b;
            Platform::PutPixel(x + col, y + row, COLOUR_WHITE);
            rowMask &= (uint8_t)(rowMask - 1);
        }
    }
}

void Menu::DrawTransitionFrame(uint8_t frameIndex) {
    const uint8_t w = pgm_read_byte(transitionSet + 0);
    const uint8_t h = pgm_read_byte(transitionSet + 1);
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

void Menu::ResetTimer() {
    timer = 0;
}

static constexpr uint8_t TOTAL_TIME = 26;
static constexpr uint8_t TOTAL_FRAMES = 8;

void Menu::RunTransition(Menu* menu, uint8_t& t, TransitionNextFn next) {
    uint8_t frameIndex = (uint16_t)t * TOTAL_FRAMES / TOTAL_TIME;
    if(frameIndex >= TOTAL_FRAMES) frameIndex = TOTAL_FRAMES - 1;
    menu->DrawTransitionFrame(frameIndex);
    if(frameIndex >= TOTAL_FRAMES - 2) {
        t = 0;
        next();
        return;
    }
    ++t;
}

void Menu::FadeOut() {
    static uint8_t t = 0;
    RunTransition(this, t, +[]() { Game::SwitchState(Game::State::GameOver); });
}

void Menu::TransitionToLevel() {
    static uint8_t t = 0;
    RunTransition(this, t, +[]() { Game::StartLevel(); });
}

void Menu::ReadScore() {
    uint8_t addr = EEPROM_BASE_ADDR;

    score_ = (uint16_t)EEPROM.read(addr) | ((uint16_t)EEPROM.read(addr + 1) << 8);
    addr += 2;
    high_ = (uint16_t)EEPROM.read(addr) | ((uint16_t)EEPROM.read(addr + 1) << 8);
    addr += 2;

    for(int i = 0; i < 8; i++) {
        vars_[i] = EEPROM.read(addr++);
    }
}

void Menu::SetScore(uint16_t score) {
    if(score == 0) return;

    static unsigned long lastSaveTime = 0;
    unsigned long now = furi_get_tick();

    if(lastSaveTime != 0 && (now - lastSaveTime) < 2000) {
        score_ = score;
        return;
    }

    lastSaveTime = now;

    uint16_t storedHigh = (uint16_t)EEPROM.read(EEPROM_BASE_ADDR + 2) |
                          ((uint16_t)EEPROM.read(EEPROM_BASE_ADDR + 3) << 8);

    uint16_t newHigh = (score > storedHigh) ? score : storedHigh;

    score_ = score;
    high_ = newHigh;

    uint8_t addr = EEPROM_BASE_ADDR;

    EEPROM.update(addr++, (uint8_t)(score & 0xFF));
    EEPROM.update(addr++, (uint8_t)(score >> 8));
    EEPROM.update(addr++, (uint8_t)(newHigh & 0xFF));
    EEPROM.update(addr++, (uint8_t)(newHigh >> 8));

    for(int i = 0; i < 8; i++) {
        EEPROM.update(addr++, vars_[i]);
    }

    EEPROM.commit();
}
