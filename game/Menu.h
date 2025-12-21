#pragma once
#include <stdint.h>

class Menu
{
public:
    void Init();
    void Draw();
    void Tick();
	
    void TickEnteringLevel();
    void DrawEnteringLevel();

    void TickGameOver();
    void DrawGameOver();

    void ResetTimer();
    void FadeOut();

private:
    uint8_t selection = 0;
    uint8_t topIndex  = 0;
    uint8_t cursorPos = 0;

    union
    {
        uint16_t timer;
        uint16_t fizzleFade;
    };
};
