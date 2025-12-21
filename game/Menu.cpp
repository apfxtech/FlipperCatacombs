#include "game/Defines.h"
#include "game/Platform.h"
#include "game/Menu.h"
#include "game/Font.h"
#include "game/Game.h"
#include "game/Draw.h"
#include "game/Generated/SpriteTypes.h"

namespace
{
    constexpr uint8_t MENU_ITEMS_COUNT = 3;
    constexpr uint8_t VISIBLE_ROWS = 2;

    constexpr uint8_t MENU_FIRST_ROW = 4;
    constexpr uint8_t TEXT_X = 24;
    constexpr uint8_t CURSOR_X = 16;

    static void PrintItem(uint8_t idx, uint8_t row)
    {
        switch (idx)
        {
        case 0:
            Font::PrintString(PSTR("Singleplayer"), row, TEXT_X, COLOUR_WHITE);
            break;
        case 1:
            Font::PrintString(PSTR("Multiplayer"), row, TEXT_X, COLOUR_WHITE);
            break;
        case 2:
            Font::PrintString(PSTR("Sound:"), row, TEXT_X, COLOUR_WHITE);
            Font::PrintString(Platform::IsAudioEnabled() ? PSTR("on") : PSTR("off"),
                              row, TEXT_X + 28, COLOUR_WHITE);
            break;
        }
    }

    static uint8_t Wrap(int v, int n)
    {
        v %= n;
        if (v < 0) v += n;
        return (uint8_t)v;
    }

    static uint8_t MaxTop()
    {
        return (MENU_ITEMS_COUNT > VISIBLE_ROWS) ? (uint8_t)(MENU_ITEMS_COUNT - VISIBLE_ROWS) : 0;
    }
}

void Menu::Init()
{
    selection = 0;
    topIndex = 0;
    cursorPos = 0;
}

void Menu::Draw()
{
    Platform::FillScreen(COLOUR_BLACK);

    Font::PrintString(PSTR("CATACOMBS OF THE DAMNED"), 2, 18, COLOUR_WHITE);
    Font::PrintString(PSTR("by jhhoward && apfxtech"), 7, 18, COLOUR_WHITE);

    for (uint8_t row = 0; row < VISIBLE_ROWS; ++row)
    {
        uint8_t idx = (uint8_t)(topIndex + row);
        if (idx >= MENU_ITEMS_COUNT) break;
        PrintItem(idx, (uint8_t)(MENU_FIRST_ROW + row));
    }

    Font::PrintString(PSTR(">"), (uint8_t)(MENU_FIRST_ROW + cursorPos), CURSOR_X, COLOUR_WHITE);

    const uint16_t* torchSprite = (Game::globalTickFrame & 4) ? torchSpriteData1 : torchSpriteData2;
    Renderer::DrawScaled(torchSprite, 0, 10, 9, 255);
    Renderer::DrawScaled(torchSprite, DISPLAY_WIDTH - 18, 10, 9, 255);
}

void Menu::Tick()
{
    static uint8_t lastInput = 0;
    uint8_t input = Platform::GetInput();

    auto syncWindow = [&]()
    {
        uint8_t maxTop = MaxTop();

        if (selection < topIndex)
            topIndex = selection;

        uint8_t end = (uint8_t)(topIndex + (VISIBLE_ROWS - 1));
        if (selection > end)
        {
            int t = (int)selection - (VISIBLE_ROWS - 1);
            if (t < 0) t = 0;
            if (t > maxTop) t = maxTop;
            topIndex = (uint8_t)t;
        }

        cursorPos = (uint8_t)(selection - topIndex);
        if (cursorPos >= VISIBLE_ROWS) cursorPos = (VISIBLE_ROWS - 1);
    };

    if ((input & INPUT_DOWN) && !(lastInput & INPUT_DOWN))
    {
        uint8_t next = Wrap((int)selection + 1, MENU_ITEMS_COUNT);

        if (cursorPos < (VISIBLE_ROWS - 1))
        {
            selection = next;
            cursorPos++;
        }
        else
        {
            selection = next;

            if (topIndex < MaxTop())
            {
                topIndex++;
            }
            else
            {
                topIndex = 0;
                cursorPos = 0;
            }
        }

        syncWindow();
    }

    if ((input & INPUT_UP) && !(lastInput & INPUT_UP))
    {
        uint8_t prev = Wrap((int)selection - 1, MENU_ITEMS_COUNT);

        if (cursorPos > 0)
        {
            selection = prev;
            cursorPos--;
        }
        else
        {
            selection = prev;

            if (topIndex > 0)
            {
                topIndex--;
            }
            else
            {
                topIndex = MaxTop();
                cursorPos = (VISIBLE_ROWS - 1);
            }
        }

        syncWindow();
    }

    if ((input & (INPUT_A | INPUT_B)) && !(lastInput & (INPUT_A | INPUT_B)))
    {
        switch (selection)
        {
        case 0:
            Game::StartGame();
            break;
        case 1:
            Game::StartGame();
            break;
        case 2:
            Platform::SetAudioEnabled(!Platform::IsAudioEnabled());
            break;
        }
    }

    lastInput = input;
}

void Menu::ResetTimer()
{
    timer = 0;
}

void Menu::TickEnteringLevel()
{
    constexpr uint8_t showTime = 30;

    if (timer < showTime)
        timer++;

    if (timer == showTime && Platform::GetInput() == 0)
        Game::StartLevel();
}

void Menu::DrawEnteringLevel()
{
    Platform::FillScreen(COLOUR_BLACK);
    Font::PrintString(PSTR("Entering floor"), 3, 30, COLOUR_WHITE);
    Font::PrintInt(Game::floor, 3, 90, COLOUR_WHITE);
}

void Menu::TickGameOver()
{
    constexpr uint8_t minShowTime = 30;

    if (timer < minShowTime)
        timer++;

    if (timer == minShowTime && (Platform::GetInput() & (INPUT_A | INPUT_B)))
    {
        timer++;
    }
    else if (timer == minShowTime + 1 && Platform::GetInput() == 0)
    {
        Game::SwitchState(Game::State::Menu);
    }
}

void Menu::DrawGameOver()
{
	Platform::FillScreen(COLOUR_BLACK);
	Font::PrintString(PSTR("GAME OVER"), 0, 64 - 18, COLOUR_WHITE);

	switch (Game::stats.killedBy)
	{
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

	///
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

	if (Game::stats.killedBy == EnemyType::None)
		finalScore += finishBonus;
	finalScore += Game::stats.chestsOpened * chestBonus;
	finalScore += Game::stats.crownsCollected * crownBonus;
	finalScore += Game::stats.scrollsCollected * scrollBonus;
	finalScore += Game::stats.coinsCollected * coinsBonus;
	finalScore += Game::stats.enemyKills[(int)EnemyType::Skeleton] * skeletonKillBonus;
	finalScore += Game::stats.enemyKills[(int)EnemyType::Mage] * mageKillBonus;
	finalScore += Game::stats.enemyKills[(int)EnemyType::Bat] * batKillBonus;
	finalScore += Game::stats.enemyKills[(int)EnemyType::Spider] * spiderKillBonus;

	Font::PrintString(PSTR("FINAL SCORE:"), 7, 20, COLOUR_WHITE);
	Font::PrintInt(finalScore, 7, 72, COLOUR_WHITE);
}

#include <stdio.h>

void Menu::FadeOut()
{
	constexpr uint16_t toggleMask = 0x1f6e;
	constexpr int fizzlesPerFrame = 255;
	constexpr uint16_t startValue = 1;

	if (fizzleFade == 0)
	{
		fizzleFade = startValue;
	}

	for (int n = 0; n < fizzlesPerFrame; n++)
	{
		bool lsb = (fizzleFade & 1) != 0;
		fizzleFade >>= 1;
		if (lsb)
		{
			fizzleFade ^= toggleMask;
		}

		uint8_t x = (uint8_t)(fizzleFade & 0x7f);
		uint8_t y = (uint8_t)(fizzleFade >> 7);
		Platform::PutPixel(x, y, COLOUR_WHITE);

		if (fizzleFade == startValue)
		{
			Game::SwitchState(Game::State::GameOver);
			return;
		}
	}

}
