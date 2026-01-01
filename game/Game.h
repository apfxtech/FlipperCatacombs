#pragma once

#include <stdint.h>
#include "game/Defines.h"
#include "game/Player.h"
#include "game/Enemy.h"
#include "game/Menu.h"

struct Stats {
    EnemyType killedBy;
    uint8_t enemyKills[(int)EnemyType::NumEnemyTypes];
    uint8_t chestsOpened;
    uint8_t crownsCollected;
    uint8_t scrollsCollected;
    uint8_t coinsCollected;

    void Reset();
};

class Game {
public:
    static Menu menu;
    static uint8_t globalTickFrame;

    enum class State : uint8_t {
        Menu,
        EnteringLevel,
        InGame,
        GameOver,
        FadeOut,

        EstablishingNetwork,
        SendSyncMessage,
        RecvSyncMessage
    };

    static void Init();
    static bool Tick();
    static void Draw();

    static bool IsInMenu();
    static void GoToMenu();

    static void StartGame();
    static void StartGameStory();
    static void StartGameServer();

    static void StartLevel();
    static void NextLevel();
    static void GameOver();
    static void Respawn();

    static void SwitchState(State newState);

    static void ShowMessage(const char* message);

    static Player& GetLocalPlayer();
    static Player& GetRemotePlayer();

    static bool IsOnlineEnabled();

    static Player players[2];
    static Player& player;

    static const char* displayMessage;
    static uint8_t displayMessageTime;
    static uint8_t floor;

    static Stats stats;
    static uint8_t localPlayerId;

private:
    static void InitOffline();
    static void InitOnline();

    static void TickInGameOffline();
    static bool TickInGameOnline();

    static void ConnectToNetwork();
    static void SyncNetwork();

    static bool TickOffline();
    static bool TickOnline();

    static void DrawOffline();
    static void DrawOnline();

    static State state;
    static bool onlineEnabled;
    static char localNetworkToken;
};
